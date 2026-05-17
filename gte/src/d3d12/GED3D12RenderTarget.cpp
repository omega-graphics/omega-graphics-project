#include "GED3D12RenderTarget.h"
#include "../common/GEResourceTracker.h"
#include <iostream>
#include <vector>

_NAMESPACE_BEGIN_
    GED3D12NativeRenderTarget::GED3D12NativeRenderTarget(
        IDXGISwapChain3 * swapChain,
        ID3D12DescriptorHeap * descriptorHeapForRenderTarget,
        ID3D12DescriptorHeap * dsvDescHeap,
        SharedHandle<GECommandQueue> presentQueue,
        unsigned frameIndex,
        ID3D12Resource *const *renderTargets,
        size_t renderTargetViewCount,HWND hwnd,
        PixelFormat colorFormat,
        DXGI_FORMAT bufferDxgiFormat,
        DXGI_FORMAT rtvDxgiFormat): swapChain(swapChain),

                                                 presentQueue_(std::move(presentQueue)),
                                                 colorFormat_(colorFormat),
                                                 bufferDxgiFormat_(bufferDxgiFormat),
                                                 rtvDxgiFormat_(rtvDxgiFormat),
                                                 hwnd(hwnd),
                                                   rtvDescHeap(descriptorHeapForRenderTarget),
                                                 dsvDescHeap(dsvDescHeap),
                                                  frameIndex(frameIndex),
                                                 renderTargets(renderTargets,renderTargets + renderTargetViewCount){
        traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Create,
                ResourceTracking::Backend::D3D12,
                "NativeRenderTarget",
                traceResourceId,
                this->swapChain.Get());
    };

    GED3D12NativeRenderTarget::~GED3D12NativeRenderTarget(){
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::D3D12,
                "NativeRenderTarget",
                traceResourceId,
                swapChain.Get());
        // Back-buffer pointers arrive at +1 from `IDXGISwapChain3::GetBuffer`
        // and were never wrapped in ComPtrs — release them here so the swap
        // chain can actually be torn down.
        for(auto *r : renderTargets) if(r) r->Release();
        renderTargets.clear();
    }

    void *GED3D12NativeRenderTarget::getSwapChain(){
        return (void *)swapChain.Get();
    };

    void GED3D12NativeRenderTarget::resizeSwapChain(unsigned int width, unsigned int height) {
        if (width == 0 || height == 0) return;
        if (!renderTargets.empty() && renderTargets[0] != nullptr) {
            D3D12_RESOURCE_DESC d = renderTargets[0]->GetDesc();
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                static_cast<unsigned>(d.Width) == width && d.Height == height)
                return;
        }
        ComPtr<ID3D12Device> device;
        if (!renderTargets.empty() && renderTargets[0] != nullptr) {
            if (FAILED(renderTargets[0]->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
                return;
        } else
            return;
        auto queue = (GED3D12CommandQueue *)presentQueue_.get();
        if (queue != nullptr)
            queue->commitToGPUAndWait();
        for (auto *r : renderTargets)
            if (r != nullptr) r->Release();
        renderTargets.clear();
        HRESULT hr = swapChain->ResizeBuffers(2, width, height, bufferDxgiFormat_, 0);
        if (FAILED(hr)) return;
        const UINT rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle(rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = rtvDxgiFormat_;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        rtvDesc.Texture2D.PlaneSlice = 0;
        const bool needsExplicitRtvDesc = bufferDxgiFormat_ != rtvDxgiFormat_;
        for (unsigned i = 0; i < 2; i++) {
            ComPtr<ID3D12Resource> backBuffer;
            hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
            if (FAILED(hr)) return;
            device->CreateRenderTargetView(backBuffer.Get(),
                                           needsExplicitRtvDesc ? &rtvDesc : nullptr,
                                           rtvCpuHandle);
            rtvCpuHandle.Offset(1, rtvDescSize);
            renderTargets.push_back(backBuffer.Detach());
        }
        frameIndex = swapChain->GetCurrentBackBufferIndex();
    }

    void GED3D12NativeRenderTarget::waitForGPU() {
        auto queue = (GED3D12CommandQueue *)presentQueue_.get();
        if (queue != nullptr)
            queue->commitToGPUAndWait();
    }

    void GED3D12NativeRenderTarget::waitForFence(SharedHandle<GEFence> & fence) {
        auto *q = static_cast<GED3D12CommandQueue *>(presentQueue_.get());
        if (q == nullptr) return;
        std::uint64_t value = fence->getLastSignaledValue();
        if (value > 0)
            q->waitForFence(fence, value);
    }

    void GED3D12NativeRenderTarget::present(){
        HRESULT hr;
        auto queue = (GED3D12CommandQueue *)presentQueue_.get();
        const auto presentedCommandBufferId = queue != nullptr ? queue->lastSubmittedCommandBufferTraceId() : 0;

        // The caller is expected to have left the back-buffer in
        // RENDER_TARGET state on this queue. The PRESENT barrier rides
        // on whatever command list is convenient:
        //   - If the queue still has pending (un-committed) lists, append
        //     to the last one — cheap, no extra CB allocation.
        //   - If the caller already called `commitToGPU` before `present()`
        //     (the post queue-decoupling pattern in
        //     `BackendRenderTargetContext::commit`), `commandLists` is
        //     empty; allocate a fresh CB so the transition still runs on
        //     this queue ahead of Present1.
        if(queue == nullptr){
            return;
        }

        auto commandList = queue->getLastCommandList();
        SharedHandle<GECommandBuffer> barrierCb;
        if(commandList == nullptr){
            barrierCb = queue->getAvailableBuffer();
            if(barrierCb != nullptr){
                commandList = static_cast<ID3D12GraphicsCommandList6 *>(barrierCb->native());
            }
        }
        if(commandList != nullptr){
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
            commandList->ResourceBarrier(1,&barrier);
        }
        if(barrierCb != nullptr){
            queue->submitCommandBuffer(barrierCb);
        }
        queue->commitToGPU();
        DXGI_PRESENT_PARAMETERS params {};
        params.DirtyRectsCount = NULL;
        params.pDirtyRects = NULL;
        params.pScrollOffset = NULL;
        params.pScrollRect = NULL;
        hr = swapChain->Present1(1,0,&params);
        if(FAILED(hr) || hr == DXGI_STATUS_OCCLUDED){
            std::cout << "[GED3D12Engine_Internal] - Failed to Present SwapChain hr=0x"
                      << std::hex << static_cast<unsigned long>(hr) << std::dec;

            if(hr == DXGI_STATUS_OCCLUDED){
                std::cout << " (DXGI_STATUS_OCCLUDED)";
            }
            else if(commandList != nullptr) {
                ComPtr<ID3D12Device> device;
                HRESULT devHr = commandList->GetDevice(IID_PPV_ARGS(&device));
                if(SUCCEEDED(devHr) && device != nullptr){
                    const HRESULT removedReason = device->GetDeviceRemovedReason();
                    if(removedReason != S_OK){
                        std::cout << " deviceRemoved=0x"
                                  << std::hex << static_cast<unsigned long>(removedReason) << std::dec;
                    }

                    ComPtr<ID3D12InfoQueue> infoQueue;
                    if(SUCCEEDED(device.As(&infoQueue)) && infoQueue != nullptr){
                        const UINT64 msgCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
                        if(msgCount > 0){
                            const UINT64 maxDump = 8;
                            const UINT64 first = msgCount > maxDump ? (msgCount - maxDump) : 0;
                            std::cout << " d3d12Messages=" << msgCount;
                            for(UINT64 i = first; i < msgCount; i++){
                                SIZE_T msgLength = 0;
                                if(FAILED(infoQueue->GetMessage(i,nullptr,&msgLength)) || msgLength == 0){
                                    continue;
                                }
                                std::vector<char> storage(msgLength);
                                auto *msg = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                                if(SUCCEEDED(infoQueue->GetMessage(i,msg,&msgLength)) && msg != nullptr && msg->pDescription){
                                    std::cout << "\n  [D3D12Message] " << msg->pDescription;
                                }
                            }
                            infoQueue->ClearStoredMessages();
                        }
                    }
                }
            }
            std::cout << std::endl;
        }
        else
            frameIndex = swapChain->GetCurrentBackBufferIndex();

        ResourceTracking::Event presentEvent {};
        presentEvent.backend = ResourceTracking::Backend::D3D12;
        presentEvent.eventType = ResourceTracking::EventType::Present;
        presentEvent.resourceType = "NativeRenderTarget";
        presentEvent.resourceId = traceResourceId;
        presentEvent.queueId = queue->traceId();
        presentEvent.commandBufferId = presentedCommandBufferId;
        presentEvent.nativeHandle = reinterpret_cast<std::uint64_t>(swapChain.Get());
        ResourceTracking::Tracker::instance().emit(presentEvent);
        queue->clearSubmittedTraceCommandBufferIds();
    };

    GED3D12TextureRenderTarget::GED3D12TextureRenderTarget(SharedHandle<GED3D12Texture> texture)
                                                           : engine(nullptr),
                                                           texture(std::move(texture)) {
        traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Create,
                ResourceTracking::Backend::D3D12,
                "TextureRenderTarget",
                traceResourceId,
                this->texture != nullptr ? this->texture->resource.Get() : nullptr);

    }

    GED3D12TextureRenderTarget::~GED3D12TextureRenderTarget(){
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::D3D12,
                "TextureRenderTarget",
                traceResourceId,
                texture != nullptr ? texture->resource.Get() : nullptr);
    }

    SharedHandle<GETexture> GED3D12TextureRenderTarget::underlyingTexture() {
        return std::static_pointer_cast<GETexture>(texture);
    }

_NAMESPACE_END_
