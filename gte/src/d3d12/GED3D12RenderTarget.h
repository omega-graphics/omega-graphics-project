#include "GED3D12.h"
#include "omegaGTE/GERenderTarget.h"
#include "GED3D12CommandQueue.h"
#include "GED3D12Texture.h"

#ifndef OMEGAGTE_D3D12_GED3D12RENDERTARGET_H
#define OMEGAGTE_D3D12_GED3D12RENDERTARGET_H

_NAMESPACE_BEGIN_
    class GED3D12NativeRenderTarget : public GENativeRenderTarget {
        GED3D12Engine *engine;
        ComPtr<IDXGISwapChain3> swapChain;
        SharedHandle<GECommandQueue> commandQueue;
    public:
        HWND hwnd;
        void *getSwapChain() override;
        SharedHandle<CommandBuffer> commandBuffer() override;
        void commitAndPresent() override;
        void notifyCommandBuffer(SharedHandle<CommandBuffer> & cb,SharedHandle<GEFence> & waitFence) override;
        void submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer) override;
        void submitCommandBuffer(SharedHandle<CommandBuffer> & cb,SharedHandle<GEFence> & signalFence) override;
         ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
         ComPtr<ID3D12DescriptorHeap> dsvDescHeap;
          unsigned frameIndex;
        std::vector<ID3D12Resource *> renderTargets;
        void *nativeCommandQueue() override{
            return commandQueue->native();
        }
        GED3D12NativeRenderTarget(IDXGISwapChain3 * swapChain,
                                 ID3D12DescriptorHeap * descriptorHeapForRenderTarget,
                                 ID3D12DescriptorHeap * dsvDescHeap,
                                 SharedHandle<GECommandQueue> commandQueue,
                                 unsigned frameIndex,
                                 ID3D12Resource *const *renderTargets,
                                 size_t renderTargetViewCount,HWND hwnd);
    };

    class GED3D12TextureRenderTarget : public GETextureRenderTarget {
        GED3D12Engine *engine;
        SharedHandle<GED3D12CommandQueue> commandQueue;
    public:
        SharedHandle<GED3D12Texture> texture;
        explicit GED3D12TextureRenderTarget(
                SharedHandle<GED3D12Texture> texture,
                SharedHandle<GECommandQueue> & commandQueue);

        void commit() override;
        SharedHandle<CommandBuffer> commandBuffer() override;
        void *nativeCommandQueue() override;
        void notifyCommandBuffer(SharedHandle<CommandBuffer> & cb,SharedHandle<GEFence> & waitFence) override;
        void submitCommandBuffer(SharedHandle<CommandBuffer> & cb) override;
        void submitCommandBuffer(SharedHandle<CommandBuffer> & cb,SharedHandle<GEFence> & signalFence) override;
        SharedHandle<GETexture> underlyingTexture() override;
    };
_NAMESPACE_END_

#endif