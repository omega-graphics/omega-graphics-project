#include "GED3D12.h"
#include "omegaGTE/GERenderTarget.h"
#include "GED3D12CommandQueue.h"
#include "GED3D12Texture.h"
#include <cstdint>

#ifndef OMEGAGTE_D3D12_GED3D12RENDERTARGET_H
#define OMEGAGTE_D3D12_GED3D12RENDERTARGET_H

_NAMESPACE_BEGIN_
    class GED3D12NativeRenderTarget : public GENativeRenderTarget {
        GED3D12Engine *engine;
        ComPtr<IDXGISwapChain3> swapChain;
        SharedHandle<GECommandQueue> presentQueue_;
        std::uint64_t traceResourceId = 0;
        PixelFormat colorFormat_;
        // DXGI format used for the swap-chain buffer storage. For SRGB
        // pixel formats on a FLIP-model swap chain this is the non-SRGB
        // typeless-compatible variant; the SRGB form is only used for the
        // RTV view.
        DXGI_FORMAT bufferDxgiFormat_;
        DXGI_FORMAT rtvDxgiFormat_;

        // Phase F-G (UIView-Render-Redesign): full ResizeBuffers + RTV
        // recreation to `bufferW x bufferH` — the expensive
        // commitToGPUAndWait + DXGI realloc. The bucketed `resizeSwapChain`
        // calls this only when the live size crosses a bucket boundary;
        // otherwise it presents a sub-region via `SetSourceSize` with no
        // realloc. Returns false if the back-buffers could not be recreated.
        bool reallocBackBuffers(unsigned bufferW, unsigned bufferH);
    public:
        HWND hwnd;
        void *getSwapChain() override;
        void resizeSwapChain(unsigned int width, unsigned int height) override;
        void waitForGPU() override;
        void waitForFence(SharedHandle<GEFence> & fence) override;
        SharedHandle<GECommandQueue> presentQueue() const override { return presentQueue_; }
        void present() override;
        PixelFormat pixelFormat() override { return colorFormat_; }
        // Color only — a swap chain has no depth surface, so there is no DSV
        // heap here. Depth lives on a GETextureRenderTarget's depthTexture.
        ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
        unsigned frameIndex;
        std::vector<ID3D12Resource *> renderTargets;

        // Per-back-buffer resource state, parallel to `renderTargets`. The
        // back-buffer's PRESENT<->RENDER_TARGET transition cannot be gated on
        // the per-command-buffer `firstRenderPass` flag: a single presented
        // frame can span several command buffers (the compositor suspends the
        // frame for every content-cache capture / blur scratch pass and resumes
        // it on a fresh buffer, each of which is `firstRenderPass`). Tracking
        // the actual state here — set in startRenderPass, cleared in present()
        // — makes the transition idempotent across those buffers. Assumes the
        // non-MSAA native swap chain that makeNativeRenderTarget builds. Seeded
        // to PRESENT (== COMMON, the GetBuffer initial state).
        std::vector<D3D12_RESOURCE_STATES> renderTargetStates;

        // Phase F-G: the LIVE (window) backing dims the OS presents — the
        // [0,0,sourceWidth_,sourceHeight_] source region set via
        // IDXGISwapChain2::SetSourceSize. The back-buffer itself is allocated
        // at a >= power-of-two bucket and is grow-only, so most resize ticks
        // are a cheap SetSourceSize rather than a ResizeBuffers GPU stall.
        // These also drive the viewport / scissor Y-flip in
        // GED3D12CommandBuffer so content top-aligns into the presented source
        // region. In the legacy (non-bucketed) path they track the exact
        // back-buffer size, so the flip is unchanged there. Seeded from the
        // creation back-buffer size; kept in step by resizeSwapChain.
        unsigned sourceWidth_  = 0;
        unsigned sourceHeight_ = 0;
        GED3D12NativeRenderTarget(IDXGISwapChain3 * swapChain,
                                 ID3D12DescriptorHeap * descriptorHeapForRenderTarget,
                                 SharedHandle<GECommandQueue> presentQueue,
                                 unsigned frameIndex,
                                 ID3D12Resource *const *renderTargets,
                                 size_t renderTargetViewCount,
                                 HWND hwnd,
                                 PixelFormat colorFormat,
                                 DXGI_FORMAT bufferDxgiFormat,
                                 DXGI_FORMAT rtvDxgiFormat);
        ~GED3D12NativeRenderTarget() override;
    };

    class GED3D12TextureRenderTarget : public GETextureRenderTarget {
        GED3D12Engine *engine;
        std::uint64_t traceResourceId = 0;
    public:
        SharedHandle<GED3D12Texture> texture;
        explicit GED3D12TextureRenderTarget(SharedHandle<GED3D12Texture> texture);

        ~GED3D12TextureRenderTarget() override;

        SharedHandle<GETexture> underlyingTexture() override;
    };
_NAMESPACE_END_

#endif
