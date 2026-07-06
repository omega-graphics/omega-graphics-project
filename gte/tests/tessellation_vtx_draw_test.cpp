/// OmegaSL §16 Phase G — tessellated-draw integration test for the mandatory
/// vertex-stage model (`vertex(tess=true)`).
///
/// Exercises the full `startTessRenderPass` / `drawPatches` path end to end on
/// the standard vertex → hull → domain → fragment tessellation dataflow: a
/// vertex stage reads the input control points and outputs them, the hull
/// passes them through and emits unit tessellation factors (the base triangle,
/// no subdivision), the domain interpolates the post-hull control points by the
/// barycentric `DomainLocation`, and the fragment shades the patch solid green.
/// The patch spans the center of an offscreen RGBA8 target, so the center pixel
/// must read back green (not the black clear) once the tessellator + domain +
/// fragment stages have rasterized it.
///
/// Vulkan-first (§16 Phase G): on Vulkan the HS/DS run inside the one graphics
/// pipeline (vertex → tess-control → tess-evaluation → fragment). Metal's
/// runtime still uses the interim SSBO-hull model (`tessellation_draw_test.cpp`)
/// until it migrates to the compute-before-hull vertex stage; D3D12 lands with
/// its Phase-E runtime. When Metal/D3D12 migrate, this backend-independent
/// source can be registered for them too and the interim test retired.

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
struct ControlPointOut internal { float4 pos : Position; };
struct DomainOut internal { float4 pos : Position; };
struct TriPatchConstants internal {
    float edges[3]  : TessFactor;
    float inside[1] : InsideTessFactor;
};

buffer<ControlPointIn> controlPoints : 0;

TriPatchConstants triConstants() {
    TriPatchConstants p;
    p.edges[0] = 1.0; p.edges[1] = 1.0; p.edges[2] = 1.0;
    p.inside[0] = 1.0;
    return p;
}

[in controlPoints]
vertex(tess=true)
ControlPointOut triVertex(uint vid : VertexID) {
    ControlPointOut o;
    o.pos = controlPoints[vid].pos;
    return o;
}

hull(domain=tri, partitioning=integer, outputtopology=triangle_cw, outputcontrolpoints=3, patchfn=triConstants)
ControlPointOut triHull(ControlPointOut cp[3], uint vid : VertexID) {
    ControlPointOut o;
    o.pos = cp[vid].pos;
    return o;
}

domain(domain=tri)
DomainOut triDomain(ControlPointOut cp[3], float3 loc : DomainLocation) {
    DomainOut o;
    o.pos = cp[0].pos * loc.x
          + cp[1].pos * loc.y
          + cp[2].pos * loc.z;
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

    // Skip cleanly on a device that cannot honor tessellation (matches the
    // mesh-shader test's feature-gate behavior), so the test doesn't false-fail
    // on hardware without the capability.
    bool tessSupported = false;
    for (auto &dev : OmegaGTE::enumerateDevices()) {
        if (dev && dev->features.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_TESSELLATION_SHADER)) {
            tessSupported = true;
            break;
        }
    }
    if(!tessSupported){
        std::cout << "SKIP: device does not advertise GTEDEVICE_FEATURE_TESSELLATION_SHADER\n";
        OmegaGTE::Close(gte);
        return 0;
    }

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    // ── Tessellation pipeline (vertex + hull + domain + fragment) ───────────
    RenderPipelineDescriptor pd{};
    pd.name = "tessVtxDraw";
    pd.vertexFunc = lib->shaders["triVertex"];
    pd.hullFunc = lib->shaders["triHull"];
    pd.domainFunc = lib->shaders["triDomain"];
    pd.fragmentFunc = lib->shaders["triFragment"];
    pd.patchControlPoints = 3;
    pd.colorPixelFormats = {PixelFormat::RGBA8Unorm};
    pd.depthAndStencilDesc = {false, false};
    pd.cullMode = RasterCullMode::None;
    pd.triangleFillMode = TriangleFillMode::Solid;
    pd.rasterSampleCount = 1;
    // No vertexInputDescriptor: the vertex stage pulls its control points from
    // the `buffer<ControlPointIn>` storage buffer by VertexID, not from a
    // vertex-attribute buffer, so the pipeline has no vertex-input state.

    if (!pd.vertexFunc || !pd.hullFunc || !pd.domainFunc || !pd.fragmentFunc) {
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

    GEViewport vp{0, 0, (float)kRTSize, (float)kRTSize, 0, 1.f};
    GEScissorRect sr{0, 0, (float)kRTSize, (float)kRTSize};

    cb->startTessRenderPass(rp);
    cb->setRenderPipelineState(pipeline);
    cb->setViewports({vp});
    cb->setScissorRects({sr});
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
    std::cout << (ok ? "PASS: tessellation vtx draw" : "FAIL: tessellation vtx draw") << "\n";
    return ok ? 0 : 1;
}
