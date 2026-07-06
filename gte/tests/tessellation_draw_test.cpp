/// OmegaSL §16 Phase E — tessellated-draw integration test (Metal runtime).
///
/// Exercises the full `startTessRenderPass` / `drawPatches` path end to end: a
/// hull/domain/fragment tessellation pipeline draws a single triangle patch to
/// an offscreen RGBA8 texture, and the center pixel is read back to confirm the
/// tessellator + domain + fragment stages actually rasterized the patch.
///
/// The hull passes its 3 control points through and emits unit tessellation
/// factors (the base triangle, no subdivision — enough to prove the pipeline
/// runs and rasterizes). The domain interpolates the control-point clip
/// positions by the barycentric `DomainLocation`, so the drawn triangle is the
/// control-point triangle; the fragment shades it solid green. The control
/// points span the center of the target, so the center pixel must come back
/// green (not the black clear).
///
/// Metal-only today: only Metal drives tessellation as a deferred
/// compute-then-render pass. D3D12 / Vulkan run HS/DS inside the graphics
/// pipeline; their `startTessRenderPass` / `drawPatches` land in the
/// D3D12/Vulkan Phase-E pass.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GETexture.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

constexpr unsigned kRTSize = 64;

OmegaCommon::String kShaders = R"(

struct ControlPointIn { float4 pos; };
struct ControlPointOut { float4 pos; };
struct DomainOut internal { float4 pos : Position; };
struct TriPatchConstants internal {
    float edges[3]  : TessFactor;
    float inside[1] : InsideTessFactor;
};

buffer<ControlPointIn> controlPoints : 0;
buffer<ControlPointOut> hullOut : 1;

TriPatchConstants triConstants() {
    TriPatchConstants p;
    p.edges[0] = 1.0; p.edges[1] = 1.0; p.edges[2] = 1.0;
    p.inside[0] = 1.0;
    return p;
}

[in controlPoints, out hullOut]
hull(domain=tri, partitioning=integer, outputtopology=triangle_cw, outputcontrolpoints=3, patchfn=triConstants)
ControlPointOut triHull(uint vid : VertexID) {
    ControlPointOut o;
    o.pos = controlPoints[vid].pos;
    return o;
}

[in controlPoints]
domain(domain=tri)
DomainOut triDomain(float3 loc : DomainLocation) {
    DomainOut o;
    o.pos = controlPoints[0].pos * loc.x
          + controlPoints[1].pos * loc.y
          + controlPoints[2].pos * loc.z;
    return o;
}

fragment float4 triFragment(DomainOut r) {
    return float4(0.0, 1.0, 0.0, 1.0);
}

)";

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

}  // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc;
    (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    // ── Tessellation pipeline (hull + domain + fragment) ───────────────────
    RenderPipelineDescriptor pd{};
    pd.name = "tessDraw";
    pd.hullFunc = lib->shaders["triHull"];
    pd.domainFunc = lib->shaders["triDomain"];
    pd.fragmentFunc = lib->shaders["triFragment"];
    pd.patchControlPoints = 3;
    pd.colorPixelFormats = {PixelFormat::RGBA8Unorm};
    pd.depthAndStencilDesc = {false, false};
    pd.cullMode = RasterCullMode::None;
    pd.triangleFillMode = TriangleFillMode::Solid;
    pd.rasterSampleCount = 1;
    // Control-point struct layout for the domain's patch_control_point stage-in
    // (float4 pos at offset 0, one buffer slot, 16-byte stride).
    VertexBufferLayout bl{};
    bl.stride = 16;
    bl.stepFunction = VertexStepFunction::PerVertex;
    bl.stepRate = 1;
    pd.vertexInputDescriptor.bufferLayouts.push_back(bl);
    VertexAttribute at{};
    at.bufferIndex = 0;
    at.offset = 0;
    at.format = VertexFormat::Float4;
    at.shaderLocation = 0;
    pd.vertexInputDescriptor.attributes.push_back(at);

    if (!pd.hullFunc || !pd.domainFunc || !pd.fragmentFunc) {
        std::cerr << "FAIL: tessellation shaders not found in library\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeRenderPipelineState(pd);
    if (!pipeline) {
        std::cerr << "FAIL: tessellation pipeline creation failed\n";
        OmegaGTE::Close(gte);
        return 1;
    }

    // ── Input control points: a triangle spanning the target center ────────
    const float kCP[3][4] = {
        {-0.8f, -0.8f, 0.f, 1.f},
        { 0.8f, -0.8f, 0.f, 1.f},
        { 0.0f,  0.8f, 0.f, 1.f},
    };
    BufferDescriptor cpDesc{};
    cpDesc.usage = BufferDescriptor::Upload;
    cpDesc.len = 3 * 16;
    cpDesc.objectStride = 16;
    auto cpBuf = gte.graphicsEngine->makeBuffer(cpDesc);
    {
        auto writer = GEBufferWriter::Create();
        writer->setOutputBuffer(cpBuf);
        for (unsigned i = 0; i < 3; ++i) {
            auto v = FVec<4>::Create();
            for (unsigned k = 0; k < 4; ++k) v[k][0] = kCP[i][k];
            writer->structBegin();
            writer->writeFloat4(v);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }

    // ── Render target + readback texture ───────────────────────────────────
    auto rt = makeRenderTargetTexture(gte);
    auto readback = makeReadbackTexture(gte);
    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = rt;
    auto target = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);

    // ── Deferred tessellation draw ─────────────────────────────────────────
    using ColorAttachment = GERenderPassDescriptor::ColorAttachment;
    auto cb = queue->getAvailableBuffer();
    GERenderPassDescriptor rp{};
    rp.tRenderTarget = target.get();
    rp.colorAttachments.push_back(ColorAttachment({0.f, 0.f, 0.f, 1.f}, ColorAttachment::Clear));
    rp.depthStencilAttachment.disabled = true;

    cb->startTessRenderPass(rp);
    cb->setRenderPipelineState(pipeline);
    cb->drawPatches(1, cpBuf, 0);
    cb->finishRenderPass();

    cb->startBlitPass();
    cb->copyTextureToTexture(rt, readback);
    cb->finishBlitPass();

    queue->submitCommandBuffer(cb);
    queue->commitToGPUAndWait();

    // ── Read back the center pixel ─────────────────────────────────────────
    std::vector<std::uint8_t> pixels(kRTSize * kRTSize * 4, 0);
    readback->getBytes(pixels.data(), kRTSize * 4);
    const unsigned cx = kRTSize / 2, cy = kRTSize / 2;
    const std::uint8_t *p = &pixels[(cy * kRTSize + cx) * 4];
    std::cout << "center pixel = (" << int(p[0]) << "," << int(p[1]) << "," << int(p[2]) << "," << int(p[3]) << ")\n";

    // Green is channel index 1 in both RGBA8 and BGRA8, so the readback channel
    // order does not matter: the tessellated triangle shaded solid green must
    // dominate the center (vs the black clear).
    const bool ok = (p[1] >= 200) && (p[0] < 60) && (p[2] < 60);

    OmegaGTE::Close(gte);
    std::cout << (ok ? "PASS: tessellation draw" : "FAIL: tessellation draw") << "\n";
    return ok ? 0 : 1;
}
