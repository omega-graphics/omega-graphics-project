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
    public:
        HWND hwnd;
        void *getSwapChain() override;
        void resizeSwapChain(unsigned int width, unsigned int height) override;
        void waitForGPU() override;
        void waitForFence(SharedHandle<GEFence> & fence) override;
        SharedHandle<GECommandQueue> presentQueue() const override { return presentQueue_; }
        void present() override;
        ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
        ComPtr<ID3D12DescriptorHeap> dsvDescHeap;
        unsigned frameIndex;
        std::vector<ID3D12Resource *> renderTargets;
        GED3D12NativeRenderTarget(IDXGISwapChain3 * swapChain,
                                 ID3D12DescriptorHeap * descriptorHeapForRenderTarget,
                                 ID3D12DescriptorHeap * dsvDescHeap,
                                 SharedHandle<GECommandQueue> presentQueue,
                                 unsigned frameIndex,
                                 ID3D12Resource *const *renderTargets,
                                 size_t renderTargetViewCount,HWND hwnd);
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
