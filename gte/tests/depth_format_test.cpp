/// PixelFormat-Completion-Plan + depth-attachment plumbing — GPU integration test.
///
/// Depth testing has never actually functioned in this engine, for three
/// independent reasons that this test pins down so they cannot regress:
///
///   1. There were no depth `PixelFormat` entries at all (the enum shipped five
///      color formats), so a depth texture could not even be named.
///   2. `GERenderPassDescriptor::DepthStencilAttachment` had nowhere to put a
///      depth texture, and the Metal backend "resolved" that by binding the COLOR
///      render target as the depth attachment — a BGRA8 color texture is not a
///      depth format, so this could never work.
///   3. The Metal pipeline never set `depthAttachmentPixelFormat`, which leaves a
///      depth-enabled pipeline silently inert (no validation error; depth simply
///      never tests).
///
/// What is asserted here, against whichever backend the executable was linked for:
///
///   1. `pixelFormatInfo` reports the right structural facts — byte counts, the
///      depth aspect, and compressed block shape (a compressed format must report
///      bytesPerTexel == 0 so a naive width*height*bpt never yields a plausible
///      but wrong size).
///   2. A depth-format texture can be created with `RenderTargetAndDepthStencil`
///      usage. (On Metal a depth texture may not carry ShaderWrite usage — Metal
///      rejects it — so this fails outright if that is not special-cased.)
///   3. A float G-buffer texture (RGBA32Float / RGBA16Float) can be created. World
///      positions and signed normals do not survive an 8-bit unorm, so a G-buffer
///      is impossible without these.
///   4. `makeTextureRenderTarget` accepts a `depthTexture` and hands it back on the
///      render target.
///   5. NEGATIVE: handing a COLOR-format texture in as `depthTexture` is rejected,
///      rather than being accepted and failing later inside the render pass.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GE.h>
#include <omegaGTE/GETexture.h>
#include <omegaGTE/GERenderTarget.h>
#include "GTETestEntryPoint.h"

#include <cstdio>

using namespace OmegaGTE;

namespace {

int failures = 0;

void expect(bool cond, const char *what) {
    if (cond) {
        std::printf("PASS: %s\n", what);
    } else {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

template <typename EngineT>
SharedHandle<GETexture> makeTex(EngineT &engine,
                                PixelFormat fmt,
                                GETexture::GETextureUsage usage) {
    TextureDescriptor d{};
    d.storage_opts = GPUOnly;
    d.usage = usage;
    d.pixelFormat = fmt;
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.kind = TextureKind::Tex2D;
    return engine->makeTexture(d);
}

} // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc; (void)argv;

    // ── 1. pixelFormatInfo — pure, no device needed ──────────────────
    {
        const auto rgba8 = pixelFormatInfo(PixelFormat::RGBA8Unorm);
        expect(rgba8.bytesPerTexel == 4 && rgba8.channelCount == 4 && !rgba8.isDepthStencil(),
               "pixelFormatInfo(RGBA8Unorm) = 4B, 4ch, color");

        const auto rgba32f = pixelFormatInfo(PixelFormat::RGBA32Float);
        expect(rgba32f.bytesPerTexel == 16 && !rgba32f.isDepthStencil(),
               "pixelFormatInfo(RGBA32Float) = 16B, color");

        const auto d32 = pixelFormatInfo(PixelFormat::D32Float);
        expect(d32.bytesPerTexel == 4 &&
               d32.aspect == PixelFormatInfo::Aspect::Depth &&
               d32.isDepthStencil(),
               "pixelFormatInfo(D32Float) = 4B, Depth aspect");

        const auto d32s8 = pixelFormatInfo(PixelFormat::D32Float_S8Uint);
        expect(d32s8.aspect == PixelFormatInfo::Aspect::DepthStencil && d32s8.isDepthStencil(),
               "pixelFormatInfo(D32Float_S8Uint) = DepthStencil aspect");

        // A compressed format must NOT report a per-texel size — a caller doing
        // width*height*bytesPerTexel has to get 0, not a plausible wrong answer.
        const auto bc7 = pixelFormatInfo(PixelFormat::BC7_RGBA_Unorm);
        expect(bc7.isCompressed && bc7.bytesPerTexel == 0 &&
               bc7.blockWidth == 4 && bc7.blockHeight == 4 && bc7.blockBytes == 16,
               "pixelFormatInfo(BC7) = compressed, 4x4 block, 16B, bytesPerTexel 0");

        const auto srgb = pixelFormatInfo(PixelFormat::RGBA8Unorm_SRGB);
        expect(srgb.isSRGB, "pixelFormatInfo(RGBA8Unorm_SRGB).isSRGB");
    }

    // ── device-backed checks ────────────────────────────────────────
    auto gte = OmegaGTE::InitWithDefaultDevice();
    auto engine = gte.graphicsEngine;
    if (!engine) {
        std::printf("FAIL: engine creation returned null\n");
        return 1;
    }

    // ── 2. depth texture ────────────────────────────────────────────
    auto depthTex = makeTex(engine, PixelFormat::D32Float,
                            GETexture::RenderTargetAndDepthStencil);
    expect(depthTex != nullptr,
           "makeTexture(D32Float, RenderTargetAndDepthStencil) is non-null");
    if (depthTex) {
        expect(pixelFormatInfo(depthTex->getPixelFormat()).isDepthStencil(),
               "created depth texture reports a depth aspect");
    }

    // ── 3. float G-buffer textures ──────────────────────────────────
    auto posTex = makeTex(engine, PixelFormat::RGBA32Float, GETexture::RenderTarget);
    expect(posTex != nullptr, "makeTexture(RGBA32Float, RenderTarget) is non-null");
    auto nrmTex = makeTex(engine, PixelFormat::RGBA16Float, GETexture::RenderTarget);
    expect(nrmTex != nullptr, "makeTexture(RGBA16Float, RenderTarget) is non-null");

    // ── 4. render target carrying a depth surface ───────────────────
    if (posTex && depthTex) {
        TextureRenderTargetDescriptor rtDesc{};
        rtDesc.renderToExistingTexture = true;
        rtDesc.texture = posTex;
        rtDesc.depthTexture = depthTex;
        rtDesc.region = TextureRegion{0, 0, 0, 64, 64, 1};

        auto rt = engine->makeTextureRenderTarget(rtDesc);
        expect(rt != nullptr, "makeTextureRenderTarget with a depthTexture is non-null");
        if (rt) {
            expect(rt->depthTexture == depthTex,
                   "render target hands back the depth texture it was given");
        }
    }

    // ── 5. NEGATIVE: a color texture may not serve as depth ─────────
    if (posTex && nrmTex) {
        TextureRenderTargetDescriptor badDesc{};
        badDesc.renderToExistingTexture = true;
        badDesc.texture = posTex;
        badDesc.depthTexture = nrmTex;   // RGBA16Float — a COLOR format
        badDesc.region = TextureRegion{0, 0, 0, 64, 64, 1};

        auto badRt = engine->makeTextureRenderTarget(badDesc);
        expect(badRt == nullptr,
               "[Negative] a color-format texture is rejected as depthTexture");
    }

    // ── 6. NOTE: a native render target is COLOR ONLY ────────────────
    //
    // `NativeRenderTargetDescriptor::allowDepthStencilTesting` is DELETED. No API
    // gives a swapchain a depth buffer, and both backends previously "supported"
    // the flag by aliasing the COLOR back buffer as the depth view — silently
    // broken. There is now no way to ask for it: the house standard is to render
    // 3D offscreen into a GETextureRenderTarget that owns a depthTexture, then
    // blit/resolve to the drawable. Enabling a render pass's depthStencilAttachment
    // against a native target is a caller-contract violation, reported by
    // startRenderPass (DEBUG_CRITICAL) on Metal and D3D12; not asserted here
    // because a real drawable needs a window.
    std::printf("NOTE: allowDepthStencilTesting is deleted — a native render target is "
                "color only (render offscreen + blit).\n");

    std::printf(failures == 0 ? "depth/format test PASSED\n" : "depth/format test FAILED\n");
    return failures == 0 ? 0 : 1;
}
