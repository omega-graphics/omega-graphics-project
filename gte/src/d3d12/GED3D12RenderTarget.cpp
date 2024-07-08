#include "GED3D12RenderTarget.h"

_NAMESPACE_BEGIN_
    GED3D12NativeRenderTarget::GED3D12NativeRenderTarget(
        IDXGISwapChain3 * swapChain,
        ID3D12DescriptorHeap * descriptorHeapForRenderTarget,
        ID3D12DescriptorHeap * dsvDescHeap,
        SharedHandle<GECommandQueue> commandQueue,
        unsigned frameIndex,
        ID3D12Resource *const *renderTargets,
        size_t renderTargetViewCount,HWND hwnd): swapChain(swapChain),

                                                 commandQueue(commandQueue),                                                 
                                                 hwnd(hwnd),
                                                   rtvDescHeap(descriptorHeapForRenderTarget),
                                                 dsvDescHeap(dsvDescHeap),
                                                  frameIndex(frameIndex),
                                                 renderTargets(renderTargets,renderTargets + renderTargetViewCount){
        
    };

    void *GED3D12NativeRenderTarget::getSwapChain(){
        return (void *)swapChain.Get();
    };

    void
    GED3D12NativeRenderTarget::notifyCommandBuffer(SharedHandle<CommandBuffer> &cb, SharedHandle<GEFence> &waitFence) {
        commandQueue->notifyCommandBuffer(cb->commandBuffer,waitFence);
    }

    void GED3D12NativeRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer){
        commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
    };

    void GED3D12NativeRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &cb,
                                                        SharedHandle<GEFence> &signalFence) {
        commandQueue->submitCommandBuffer(cb->commandBuffer,signalFence);
    }

    SharedHandle<GERenderTarget::CommandBuffer> GED3D12NativeRenderTarget::commandBuffer(){
//         std::ostringstream ss;
// //        ss << "About to Get Buffer" << commandQueue << std::endl;
//         //  MessageBoxA(GetForegroundWindow(),ss.str().c_str(),"NOTE",MB_OK);

//         auto commandBuffer = commandQueue->getAvailableBuffer();
//         //  MessageBoxA(GetForegroundWindow(),"Got Buffer","NOTE",MB_OK);
        return std::shared_ptr<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer((GERenderTarget *)this,CommandBuffer::GERTType::Native,commandQueue->getAvailableBuffer()));
    };

    void GED3D12NativeRenderTarget::commitAndPresent(){
        HRESULT hr;
        auto queue = (GED3D12CommandQueue *)commandQueue.get();

        auto commandList = queue->getLastCommandList();

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);

        commandList->ResourceBarrier(1,&barrier);
        /// Close all Command Lists and Commit.
        commandQueue->commitToGPU();
        /// NOTE: Maybe a Fence here to prevent gpu timing problems.
        DXGI_PRESENT_PARAMETERS params {};
        params.DirtyRectsCount = NULL;
        params.pDirtyRects = NULL;
        params.pScrollOffset = NULL;
        params.pScrollRect = NULL;
        hr = swapChain->Present1(1,0,&params);
        if(FAILED(hr) || hr == DXGI_STATUS_OCCLUDED){
            // MessageBOx
            DEBUG_STREAM("Failed to Present SwapChain");
        }
        else
            frameIndex = swapChain->GetCurrentBackBufferIndex();
    };

    GED3D12TextureRenderTarget::GED3D12TextureRenderTarget(SharedHandle<GED3D12Texture> texture,
                                                           SharedHandle<GECommandQueue> & commandQueue)
                                                           : engine(nullptr),
                                                           commandQueue(std::dynamic_pointer_cast<GED3D12CommandQueue>(commandQueue)),texture(texture) {

    }

    SharedHandle<GERenderTarget::CommandBuffer> GED3D12TextureRenderTarget::commandBuffer(){
        return SharedHandle<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,CommandBuffer::GERTType::Texture,commandQueue->getAvailableBuffer()));
    };

    void GED3D12TextureRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &cb) {
        commandQueue->submitCommandBuffer(cb->commandBuffer);
    }

    void GED3D12TextureRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &cb,
                                                         SharedHandle<GEFence> &signalFence) {
        commandQueue->submitCommandBuffer(cb->commandBuffer,signalFence);
    }

    void
    GED3D12TextureRenderTarget::notifyCommandBuffer(SharedHandle<CommandBuffer> &cb, SharedHandle<GEFence> &waitFence) {
        commandQueue->notifyCommandBuffer(cb->commandBuffer,waitFence);
    }

    void GED3D12TextureRenderTarget::commit(){
        // HRESULT hr;
        commandQueue->commitToGPU();
        /// TODO: Fence.
    };

    void *GED3D12TextureRenderTarget::nativeCommandQueue() {
        return commandQueue->native();
    }

    SharedHandle<GETexture> GED3D12TextureRenderTarget::underlyingTexture() {
        return std::static_pointer_cast<GETexture>(texture);
    }

_NAMESPACE_END_