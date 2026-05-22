/// Extension 8 §8.6 — runtime sampler-state binding, GPU integration test.
/// Backend-independent: uses only the OmegaGTE public API + runtime OmegaSL
/// compilation, so the same source builds and runs on Metal, Vulkan, and
/// D3D12. Headless (renders to an offscreen texture, no window).
///
/// Phases (default invocation, returns 0 on success / 1 on failure):
///   A. Runtime sampler + texture, no static samplers anywhere. Render a
///      full-screen quad sampling a solid-color source texture into an
///      offscreen RGBA8 target, read it back, assert the source color survived
///      the runtime sampler bind.
///   B. Mixed pipeline: one `static sampler2d` + one runtime `sampler2d`, two
///      source textures. The fragment routes the static-sampled texel into R
///      and the runtime-sampled texel into G, so the readback proves BOTH the
///      baked static slot and the runtime-bound slot delivered their texture.
///
/// Negative invocation (argv[1] == "negative"): bind a sampler to a
/// non-sampler slot during a live render pass. `validateSamplerBindKind`
/// rejects it and the bind method asserts — the process aborts in a debug
/// build. Registered in CMake with WILL_FAIL so the abort counts as a pass.
/// (The deterministic, build-type-independent validation coverage lives in
/// sampler_validation_test.cpp.)

#include <OmegaGTE.h>
#include <omegaGTE/GTEShader.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct QuadVertex {
    float4 pos;
    float2 uv;
};

struct QuadRaster internal {
    float4 pos : Position;
    float2 uv : TexCoord;
};

buffer<QuadVertex> quadBuf : 0;

[in quadBuf]
vertex QuadRaster quadVertex(uint v_id : VertexID){
    QuadVertex v = quadBuf[v_id];
    QuadRaster r = { v.pos, v.uv };
    return r;
}

// Resource declarations are library-global, so every slot across all shaders
// needs a distinct `: N` location (quadBuf owns 0).

// Phase A — runtime (non-static) sampler + texture, no static samplers.
texture2d srcTex : 1;
sampler2d srcSampler : 2;

[in srcTex, in srcSampler]
fragment float4 sampleFragment(QuadRaster r){
    return sample(srcSampler, srcTex, r.uv);
}

// Phase B — mixed: a baked static sampler and a runtime-bound sampler.
texture2d texA : 3;
static sampler2d bakedSampler(filter=linear);
texture2d texB : 4;
sampler2d boundSampler : 5;

[in texA, in bakedSampler, in texB, in boundSampler]
fragment float4 mixedFragment(QuadRaster r){
    float4 a = sample(bakedSampler, texA, r.uv);
    float4 b = sample(boundSampler, texB, r.uv);
    return float4(a[0], b[1], 0.0, 1.0);
}

)";

constexpr unsigned kRTSize = 8;

// Quad vertex struct stride. Mirrors the BlitTest layout: float4 pos + float2
// uv, padded to a 16-byte multiple (the array stride the GPU expects), so we
// write an explicit trailing float2 of padding per vertex.
size_t quadStructStride() {
    return omegaSLStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT2, OMEGASL_FLOAT2});
}

void writeQuadVertex(SharedHandle<GEBufferWriter> &w, float x, float y, float u, float v) {
    auto pos = FVec<4>::Create();
    pos[0][0] = x; pos[1][0] = y; pos[2][0] = 0.f; pos[3][0] = 1.f;
    auto uv = FVec<2>::Create();
    uv[0][0] = u; uv[1][0] = v;
    auto pad = FVec<2>::Create();
    pad[0][0] = 0.f; pad[1][0] = 0.f;
    w->structBegin();
    w->writeFloat4(pos);
    w->writeFloat2(uv);
    w->writeFloat2(pad);
    w->structEnd();
    w->sendToBuffer();
}

// Full-NDC quad (2 triangles, 6 verts). Orientation is irrelevant here because
// the source textures are solid colors.
SharedHandle<GEBuffer> makeQuadBuffer(GTE &gte, SharedHandle<GEBufferWriter> &w) {
    const size_t stride = quadStructStride();
    auto buf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, 6 * stride, stride});
    w->setOutputBuffer(buf);
    writeQuadVertex(w, -1.f, -1.f, 0.f, 0.f);
    writeQuadVertex(w, -1.f,  1.f, 0.f, 1.f);
    writeQuadVertex(w,  1.f,  1.f, 1.f, 1.f);
    writeQuadVertex(w, -1.f, -1.f, 0.f, 0.f);
    writeQuadVertex(w,  1.f,  1.f, 1.f, 1.f);
    writeQuadVertex(w,  1.f, -1.f, 1.f, 0.f);
    w->flush();
    return buf;
}

// Solid-color RGBA8 source texture (uploadable from the CPU).
SharedHandle<GETexture> makeSolidTexture(GTE &gte, std::uint8_t r, std::uint8_t g,
                                         std::uint8_t b, std::uint8_t a) {
    constexpr unsigned w = 2, h = 2;
    TextureDescriptor d{};
    d.kind = TextureKind::Tex2D;
    d.usage = GETexture::ToGPU;
    d.pixelFormat = PixelFormat::RGBA8Unorm;
    d.width = w;
    d.height = h;
    d.storage_opts = Shared;
    auto tex = gte.graphicsEngine->makeTexture(d);
    std::vector<std::uint8_t> px(w * h * 4);
    for (unsigned i = 0; i < w * h; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = a;
    }
    tex->copyBytes(px.data(), w * 4);
    return tex;
}

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

SharedHandle<GESamplerState> makeSampler(GTE &gte) {
    SamplerDescriptor sd{};
    sd.filter = SamplerDescriptor::Filter::Linear;
    sd.uAddressMode = SamplerDescriptor::AddressMode::ClampToEdge;
    sd.vAddressMode = SamplerDescriptor::AddressMode::ClampToEdge;
    sd.wAddressMode = SamplerDescriptor::AddressMode::ClampToEdge;
    return gte.graphicsEngine->makeSamplerState(sd);
}

SharedHandle<GERenderPipelineState> makePipeline(GTE &gte, SharedHandle<GTEShaderLibrary> &lib,
                                                 const char *fragName) {
    RenderPipelineDescriptor desc{};
    desc.vertexFunc = lib->shaders["quadVertex"];
    desc.fragmentFunc = lib->shaders[fragName];
    desc.colorPixelFormats = {PixelFormat::RGBA8Unorm};
    desc.depthAndStencilDesc = {false, false};
    desc.cullMode = RasterCullMode::None;
    desc.triangleFillMode = TriangleFillMode::Solid;
    desc.rasterSampleCount = 1;
    return gte.graphicsEngine->makeRenderPipelineState(desc);
}

// Render the quad with `bind` recording the per-phase fragment resources, then
// copy the target into a readback texture and pull the center pixel.
void renderAndReadCenter(GTE &gte, SharedHandle<GECommandQueue> &queue,
                         SharedHandle<GERenderPipelineState> &pipeline,
                         SharedHandle<GEBuffer> &quadBuffer,
                         SharedHandle<GETexture> &rt,
                         SharedHandle<GETextureRenderTarget> &target,
                         SharedHandle<GETexture> &readback,
                         const std::function<void(SharedHandle<GECommandBuffer> &)> &bind,
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
    cb->bindResourceAtVertexShader(quadBuffer, 0);
    bind(cb);
    cb->drawPolygons(GECommandBuffer::Triangle, 6, 0);
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

bool approx(std::uint8_t got, std::uint8_t want, int tol = 4) {
    return std::abs(int(got) - int(want)) <= tol;
}

// Phase A — runtime sampler delivers the source texture's solid color.
bool runPhaseA(GTE &gte, SharedHandle<GTEShaderLibrary> &lib,
               SharedHandle<GECommandQueue> &queue, SharedHandle<GEBufferWriter> &writer) {
    auto pipeline = makePipeline(gte, lib, "sampleFragment");
    if (!pipeline) { std::cerr << "[PhaseA] pipeline creation failed\n"; return false; }

    auto quad = makeQuadBuffer(gte, writer);
    auto src = makeSolidTexture(gte, 51, 102, 153, 255);
    auto sampler = makeSampler(gte);
    auto rt = makeRenderTargetTexture(gte);
    auto readback = makeReadbackTexture(gte);

    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = rt;
    auto target = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    std::uint8_t out[4];
    renderAndReadCenter(gte, queue, pipeline, quad, rt, target, readback,
                        [&](SharedHandle<GECommandBuffer> &cb) {
                            cb->bindResourceAtFragmentShader(src, 1);
                            cb->bindResourceAtFragmentShader(sampler, 2);
                        },
                        out);

    // The runtime sampler delivered the source texel iff all four source
    // channel values survived. G and A never reorder; R and B may be swapped by
    // the backend's pixel-format readback convention (e.g. RGBA8 vs BGRA8), so
    // accept either order for the R/B pair — channel order is orthogonal to
    // sampler binding.
    const bool gaOk = approx(out[1], 102) && approx(out[3], 255);
    const bool rbOk = (approx(out[0], 51) && approx(out[2], 153)) ||
                      (approx(out[0], 153) && approx(out[2], 51));
    const bool ok = gaOk && rbOk;
    std::cout << "[PhaseA] center pixel = (" << int(out[0]) << "," << int(out[1]) << ","
              << int(out[2]) << "," << int(out[3]) << ") expected {R,B}={51,153} G=102 A=255 -> "
              << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}

// Phase B — static slot drives R, runtime-bound slot drives G. A failure of the
// runtime bind would leave G near zero.
bool runPhaseB(GTE &gte, SharedHandle<GTEShaderLibrary> &lib,
               SharedHandle<GECommandQueue> &queue, SharedHandle<GEBufferWriter> &writer) {
    auto pipeline = makePipeline(gte, lib, "mixedFragment");
    if (!pipeline) { std::cerr << "[PhaseB] pipeline creation failed\n"; return false; }

    auto quad = makeQuadBuffer(gte, writer);
    auto texA = makeSolidTexture(gte, 255, 0, 0, 255);   // static sampler reads this -> R
    auto texB = makeSolidTexture(gte, 0, 255, 0, 255);   // runtime sampler reads this -> G
    auto sampler = makeSampler(gte);
    auto rt = makeRenderTargetTexture(gte);
    auto readback = makeReadbackTexture(gte);

    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = rt;
    auto target = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    std::uint8_t out[4];
    renderAndReadCenter(gte, queue, pipeline, quad, rt, target, readback,
                        [&](SharedHandle<GECommandBuffer> &cb) {
                            cb->bindResourceAtFragmentShader(texA, 3);
                            cb->bindResourceAtFragmentShader(texB, 4);
                            cb->bindResourceAtFragmentShader(sampler, 5);  // runtime slot
                        },
                        out);

    // mixedFragment outputs (texA.r via static sampler, texB.g via runtime
    // sampler, 0, 1) = (255, 255, 0, 255). G (index 1) never reorders, so it
    // isolates the RUNTIME-bound sampler's contribution (texB green). The
    // static red lands in R or B depending on readback channel order: require
    // one of them ~255 and the other ~0.
    const bool runtimeOk = approx(out[1], 255);
    const bool staticOk = (approx(out[0], 255) && approx(out[2], 0)) ||
                          (approx(out[0], 0) && approx(out[2], 255));
    const bool ok = runtimeOk && staticOk;
    std::cout << "[PhaseB] center pixel = (" << int(out[0]) << "," << int(out[1]) << ","
              << int(out[2]) << "," << int(out[3]) << ") runtime(G)~255 && static(R|B)~255 -> "
              << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}

// Negative — bind a sampler to slot 1, which `sampleFragment` declared as a
// texture2d. The guard rejects it and the bind asserts (debug). Process aborts.
void runNegative(GTE &gte, SharedHandle<GTEShaderLibrary> &lib,
                 SharedHandle<GECommandQueue> &queue, SharedHandle<GEBufferWriter> &writer) {
    using ColorAttachment = GERenderPassDescriptor::ColorAttachment;
    auto pipeline = makePipeline(gte, lib, "sampleFragment");
    auto quad = makeQuadBuffer(gte, writer);
    auto sampler = makeSampler(gte);
    auto rt = makeRenderTargetTexture(gte);

    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = rt;
    auto target = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    auto cb = queue->getAvailableBuffer();
    GERenderPassDescriptor rp{};
    rp.tRenderTarget = target.get();
    rp.colorAttachments.push_back(ColorAttachment({0.f, 0.f, 0.f, 1.f}, ColorAttachment::Clear));
    rp.depthStencilAttachment.disabled = true;
    cb->startRenderPass(rp);
    cb->setRenderPipelineState(pipeline);
    std::cerr << "[Negative] binding sampler to texture slot 1 — expecting an assertion\n";
    cb->bindResourceAtFragmentShader(sampler, 1);  // slot 1 is a texture2d -> reject + assert
    cb->finishRenderPass();
    std::cerr << "[Negative] bind was NOT rejected (asserts disabled?)\n";
}

// Convert the negative case's debug assertion (abort -> SIGABRT) into a clean
// non-zero exit, so CTest's WILL_FAIL reliably inverts it to a pass instead of
// reporting a raw "subprocess aborted" crash.
extern "C" void onAbortExit(int) { std::_Exit(70); }

}  // namespace

int main(int argc, const char *argv[]) {
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);
    auto queue = gte.graphicsEngine->makeCommandQueue(8);
    auto writer = GEBufferWriter::Create();

    int rc = 0;
    if (argc > 1 && std::strcmp(argv[1], "negative") == 0) {
        std::signal(SIGABRT, onAbortExit);
        runNegative(gte, lib, queue, writer);
        // Reached only if the assertion did not fire (e.g. NDEBUG build): the
        // bind was skipped without aborting. Report success so a non-WILL_FAIL
        // run still passes; the WILL_FAIL registration is gated to debug builds.
        rc = 0;
    } else {
        const bool a = runPhaseA(gte, lib, queue, writer);
        const bool b = runPhaseB(gte, lib, queue, writer);
        rc = (a && b) ? 0 : 1;
        std::cout << (rc == 0 ? "PASS: sampler bind test" : "FAIL: sampler bind test") << "\n";
    }

    OmegaGTE::Close(gte);
    return rc;
}
