/// Mesh-Shader-Plan Phase 4c.4 / 4c.5 — backend-independent mesh-shader
/// integration test. Lives at the shared `gte/tests/` level so other backends
/// pick it up when their Phase 4 lands. Today only the Metal CMakeLists wires
/// it in (D3D12 has its own AppKit-free `MeshAndRaytracingTest`, Vulkan's
/// Phase 4a is pending). Headless: renders to an offscreen 8x8 RGBA8 target,
/// no window.
///
/// Two phases:
///
///   A. No-input meshlet (Phase 4c.4 baseline). A single hardcoded NDC triangle,
///      pure R/G/B vertex colors → center pixel ≈ (85, 85, 85, 255). Proves
///      the full chain: OmegaSL → MSL → `metal` toolchain → `.metallib` →
///      `MTLMeshRenderPipelineDescriptor` → `drawMeshThreadgroups` →
///      rasterizer → fragment → texture → CPU readback.
///
///   B. Buffer-bound meshlet (Phase 4c.5 — mesh-stage resource binding).
///      Binds a CPU-built buffer of `{float4 pos, float4 color}` per vertex
///      via `bindResourceAtVertexShader`, which on a mesh-variant PSO now
///      routes to Metal's `setMeshBuffer:` (4c.5.1). The shader reads from
///      the bound buffer and emits a solid-green triangle → center pixel ≈
///      (0, 200, 0, 255). A specific color (not a per-channel "all
///      contributed" check) so a silent bind failure — Metal returning
///      zeros / leftover state — would show up as a different readback.
///
/// PASS criterion: both phases produce the expected center-pixel color
/// within tolerance.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <cstdint>
#include <cstdlib>  // std::abs
#include <cstring>
#include <functional>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

/// Two mesh shaders sharing one fragment shader and one inter-stage struct.
///
/// `meshFunc` (Phase A): hardcoded NDC triangle with pure R/G/B vertex
/// colors. No input resources.
///
/// `meshBufFunc` (Phase B): same triangle geometry, but per-vertex pos +
/// color are pulled from a bound `buffer<MeshVertexIn>` at slot 0. The CPU
/// side fills the buffer with a uniform green so the readback is a specific,
/// recognizable color that distinguishes "buffer reached the mesh stage"
/// from "buffer was silently dropped".
///
/// `#requires(MESH_SHADERS)` exercises the runtime preprocessor (the
/// runtime `OmegaSLCompiler` now runs `omegasl::Preprocessor` on every
/// source string, matching the offline `omegaslc` flow). The directive
/// also belt-and-suspenders the C++-side `enumerateDevices` feature
/// check below — on a device that somehow exposes the runtime entry
/// point without the mesh-shader bit, the loader rejects the library
/// rather than the test silently dispatching nothing.
OmegaCommon::String kShaders = R"(

#requires(MESH_SHADERS)

struct MeshletVertex internal {
    float4 pos   : Position;
    float4 color : TexCoord;
};

struct MeshVertexIn {
    float4 pos;
    float4 color;
};

buffer<MeshVertexIn> vertBuf : 0;

mesh(max_vertices=3, max_primitives=1, topology=triangle)
void meshFunc(uint3 tid : GlobalThreadID,
              out vertices MeshletVertex verts[3],
              out indices  uint3         tris[1]){
    verts[0].pos   = float4(-0.8, -0.8, 0.0, 1.0);
    verts[0].color = float4( 1.0,  0.0, 0.0, 1.0);
    verts[1].pos   = float4( 0.8, -0.8, 0.0, 1.0);
    verts[1].color = float4( 0.0,  1.0, 0.0, 1.0);
    verts[2].pos   = float4( 0.0,  0.8, 0.0, 1.0);
    verts[2].color = float4( 0.0,  0.0, 1.0, 1.0);
    tris[0] = uint3(0, 1, 2);
}

[in vertBuf]
mesh(max_vertices=3, max_primitives=1, topology=triangle)
void meshBufFunc(uint3 tid : GlobalThreadID,
                 out vertices MeshletVertex verts[3],
                 out indices  uint3         tris[1]){
    verts[0].pos   = vertBuf[0u].pos;
    verts[0].color = vertBuf[0u].color;
    verts[1].pos   = vertBuf[1u].pos;
    verts[1].color = vertBuf[1u].color;
    verts[2].pos   = vertBuf[2u].pos;
    verts[2].color = vertBuf[2u].color;
    tris[0] = uint3(0, 1, 2);
}

fragment float4 fragFunc(MeshletVertex raster){
    return raster.color;
}

)";

constexpr unsigned kRTSize = 8;

SharedHandle<GETexture> makeRenderTargetTexture(GTE &gte) {
    TextureDescriptor d{};
    d.kind = TextureKind::Tex2D;
    d.usage = GETexture::RenderTarget;
    d.pixelFormat = PixelFormat::RGBA8Unorm;
    d.width = kRTSize;
    d.height = kRTSize;
    d.storage_opts = GPUOnly;
    return gte.graphicsEngine->makeTexture(d);
}

SharedHandle<GETexture> makeReadbackTexture(GTE &gte) {
    TextureDescriptor d{};
    d.kind = TextureKind::Tex2D;
    d.usage = GETexture::FromGPU;
    d.pixelFormat = PixelFormat::RGBA8Unorm;
    d.width = kRTSize;
    d.height = kRTSize;
    d.storage_opts = Shared;
    return gte.graphicsEngine->makeTexture(d);
}

SharedHandle<GERenderPipelineState> makeMeshPipe(GTE &gte,
                                                 SharedHandle<GTEShaderLibrary> &lib,
                                                 const char *meshFuncName,
                                                 const char *label) {
    MeshPipelineDescriptor desc{};
    desc.name = label;
    desc.meshFunc     = lib->shaders[meshFuncName];
    desc.fragmentFunc = lib->shaders["fragFunc"];
    desc.colorPixelFormats = {PixelFormat::RGBA8Unorm};
    desc.depthAndStencilDesc = {false, false};
    desc.cullMode = RasterCullMode::None;
    desc.triangleFillMode = TriangleFillMode::Solid;
    desc.rasterSampleCount = 1;
    return gte.graphicsEngine->makeMeshPipelineState(desc);
}

/// Approximate-byte compare for the center-pixel checks. Interpolation
/// across 8x8 isn't pixel-perfect; channel ordering also depends on the
/// readback format convention (RGBA8 vs BGRA8 — the sibling sampler-bind
/// test handles the same caveat).
bool approx(std::uint8_t got, std::uint8_t want, int tol = 30) {
    return std::abs(int(got) - int(want)) <= tol;
}

/// Render one mesh dispatch into `rt`, copy to `readback`, return the
/// center pixel. The `bindMesh` callback receives the command buffer
/// AFTER `setRenderPipelineState` so it can attach mesh-stage resources
/// for Phase B (no-op for Phase A).
void renderAndReadCenter(GTE &gte,
                         SharedHandle<GECommandQueue> &queue,
                         SharedHandle<GERenderPipelineState> &pipeline,
                         SharedHandle<GETexture> &rt,
                         SharedHandle<GETextureRenderTarget> &target,
                         SharedHandle<GETexture> &readback,
                         const std::function<void(SharedHandle<GECommandBuffer> &)> &bindMesh,
                         std::uint8_t outRGBA[4]) {
    using ColorAttachment = GERenderPassDescriptor::ColorAttachment;
    auto cb = queue->getAvailableBuffer();

    GERenderPassDescriptor rp{};
    rp.tRenderTarget = target.get();
    rp.colorAttachments.push_back(ColorAttachment({0.f, 0.f, 0.f, 1.f}, ColorAttachment::Clear));
    rp.depthStencilAttachment.disabled = true;

    GEViewport vp{0, 0, (float)kRTSize, (float)kRTSize, 0, 1.f};
    GEScissorRect sr{0, 0, (float)kRTSize, (float)kRTSize};

    cb->startRenderPass(rp);
    cb->setRenderPipelineState(pipeline);
    cb->setViewports({vp});
    cb->setScissorRects({sr});
    bindMesh(cb);
    cb->drawMeshTasks(1, 1, 1);
    cb->finishRenderPass();

    cb->startBlitPass();
    cb->copyTextureToTexture(rt, readback);
    cb->finishBlitPass();

    queue->submitCommandBuffer(cb);
    queue->commitToGPUAndWait();

    std::vector<std::uint8_t> pixels(kRTSize * kRTSize * 4, 0);
    readback->getBytes(pixels.data(), kRTSize * 4);
    const unsigned cx = kRTSize / 2, cy = kRTSize / 2;
    const std::uint8_t *p = &pixels[(cy * kRTSize + cx) * 4];
    std::memcpy(outRGBA, p, 4);
}

/// Build the Phase-B buffer: three vertices, each `{float4 pos, float4
/// color}`. Same triangle geometry as Phase A but with a uniform green
/// color so the readback color is distinct (rules out the case where the
/// bind silently failed and we got stale Phase A pixels). Stride = 32B
/// (two float4 = already a 16-byte multiple, no trailing padding needed).
SharedHandle<GEBuffer> makeMeshInputBuffer(GTE &gte, SharedHandle<GEBufferWriter> &w) {
    const size_t stride = omegaSLStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT4});
    auto buf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, 3 * stride, stride});
    w->setOutputBuffer(buf);

    auto writeVert = [&](float x, float y) {
        auto pos = FVec<4>::Create();
        pos[0][0] = x; pos[1][0] = y; pos[2][0] = 0.f; pos[3][0] = 1.f;
        auto col = FVec<4>::Create();
        // Solid green — alpha 1.0. Same color at all three vertices so the
        // interpolated center is exactly that green (modulo readback channel
        // order). Distinct from Phase A's R/G/B-per-vertex mid-grey.
        col[0][0] = 0.f; col[1][0] = 0.8f; col[2][0] = 0.f; col[3][0] = 1.f;
        w->structBegin();
        w->writeFloat4(pos);
        w->writeFloat4(col);
        w->structEnd();
        w->sendToBuffer();
    };
    writeVert(-0.8f, -0.8f);
    writeVert( 0.8f, -0.8f);
    writeVert( 0.0f,  0.8f);
    w->flush();
    return buf;
}

bool runPhaseA(GTE &gte,
               SharedHandle<GTEShaderLibrary> &lib,
               SharedHandle<GECommandQueue> &queue) {
    auto pipeline = makeMeshPipe(gte, lib, "meshFunc", "MeshShaderTest.PhaseA");
    if (!pipeline) {
        std::cerr << "[PhaseA] FAIL: makeMeshPipelineState returned null\n";
        return false;
    }
    std::cout << "[PhaseA] makeMeshPipelineState -> live PSO\n";

    auto rt = makeRenderTargetTexture(gte);
    auto readback = makeReadbackTexture(gte);
    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = rt;
    auto target = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    std::uint8_t out[4];
    renderAndReadCenter(gte, queue, pipeline, rt, target, readback,
                        [](SharedHandle<GECommandBuffer> & /*cb*/) {},
                        out);

    std::cout << "[PhaseA] center pixel = ("
              << int(out[0]) << "," << int(out[1]) << "," << int(out[2]) << "," << int(out[3]) << ")\n";

    // Pure R, G, B at three vertices → center interpolates to ~ (85, 85, 85).
    // PASS criterion is "all three channels contributed and alpha is intact"
    // — proves the mesh shader ran AND emitted a primitive covering the
    // center AND the fragment interpolated.
    const bool allThreeContributed = (out[0] >= 20 && out[1] >= 20 && out[2] >= 20);
    const bool alphaOk = (out[3] >= 200);
    const bool ok = allThreeContributed && alphaOk;
    std::cout << (ok ? "[PhaseA] PASS" : "[PhaseA] FAIL") << "\n";
    return ok;
}

bool runPhaseB(GTE &gte,
               SharedHandle<GTEShaderLibrary> &lib,
               SharedHandle<GECommandQueue> &queue,
               SharedHandle<GEBufferWriter> &writer) {
    auto pipeline = makeMeshPipe(gte, lib, "meshBufFunc", "MeshShaderTest.PhaseB");
    if (!pipeline) {
        std::cerr << "[PhaseB] FAIL: makeMeshPipelineState returned null\n";
        return false;
    }
    std::cout << "[PhaseB] makeMeshPipelineState -> live PSO\n";

    auto vertBuf = makeMeshInputBuffer(gte, writer);
    auto rt = makeRenderTargetTexture(gte);
    auto readback = makeReadbackTexture(gte);
    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = rt;
    auto target = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    std::uint8_t out[4];
    renderAndReadCenter(gte, queue, pipeline, rt, target, readback,
                        [&](SharedHandle<GECommandBuffer> &cb) {
                            // bindResourceAtVertexShader → routed to
                            // setMeshBuffer: by Phase 4c.5.1 because the
                            // bound PSO's isMesh flag is true. Slot 0
                            // matches the `: 0` annotation on
                            // `buffer<MeshVertexIn>` in the shader.
                            cb->bindResourceAtVertexShader(vertBuf, 0);
                        },
                        out);

    std::cout << "[PhaseB] center pixel = ("
              << int(out[0]) << "," << int(out[1]) << "," << int(out[2]) << "," << int(out[3]) << ")\n";

    // Solid green from the bound buffer → center pixel ~ (0, 200, 0, 255).
    // G never reorders; R and B may swap with the backend's readback format,
    // so accept either order for R/B being near 0 (mirrors the sampler-bind
    // test's R/B-ordering caveat). G near 200 isolates the buffer-bind
    // contribution — if the bind silently failed we'd get the clear-black
    // (0, 0, 0, 255) or whatever Metal happened to leave in the slot.
    const bool greenStrong = approx(out[1], 200);
    const bool redBlueBothLow = (out[0] <= 30 && out[2] <= 30);
    const bool alphaOk = (out[3] >= 200);
    const bool ok = greenStrong && redBlueBothLow && alphaOk;
    std::cout << (ok ? "[PhaseB] PASS" : "[PhaseB] FAIL") << "\n";
    return ok;
}

}  // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc; (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto enumerateRes = OmegaGTE::enumerateDevices();
    bool meshSupported = false;
    for (auto &dev : enumerateRes) {
        if (dev && dev->features.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_MESH_SHADER)) {
            meshSupported = true;
            break;
        }
    }
    std::cout << "[MeshShaderTest] GTEDEVICE_FEATURE_MESH_SHADER = "
              << (meshSupported ? "YES" : "NO") << "\n";
    if (!meshSupported) {
        std::cout << "[MeshShaderTest] SKIP: device does not advertise mesh-shader support\n";
        OmegaGTE::Close(gte);
        return 0;
    }

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);
    if (!lib) {
        std::cerr << "[MeshShaderTest] FAIL: loadShaderLibraryRuntime returned null\n";
        OmegaGTE::Close(gte);
        return 1;
    }

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 8;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto writer = GEBufferWriter::Create();

    const bool a = runPhaseA(gte, lib, queue);
    const bool b = runPhaseB(gte, lib, queue, writer);

    // Negative: `#include` in runtime-compiled source must be rejected by
    // the preprocessor (no file-system context at runtime). Compile a
    // source that only contains an `#include` and assert the resulting
    // library has no shaders — the preprocessor's hasErrors() path skips
    // parsing for that source, so nothing makes it into the library.
    OmegaCommon::String includeSrc = R"(
#include "nonexistent.omegasl"
)";
    auto neg = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(includeSrc)});
    auto negLib = gte.graphicsEngine->loadShaderLibraryRuntime(neg);
    const bool includeRejected = (!negLib) || negLib->shaders.empty();
    std::cout << (includeRejected
                  ? "[Negative] PASS: #include rejected by runtime preprocessor"
                  : "[Negative] FAIL: #include was not rejected")
              << "\n";

    const int rc = (a && b && includeRejected) ? 0 : 1;
    std::cout << (rc == 0 ? "PASS: mesh shader test" : "FAIL: mesh shader test") << "\n";

    OmegaGTE::Close(gte);
    return rc;
}
