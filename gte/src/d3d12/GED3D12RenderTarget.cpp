#include "GED3D12RenderTarget.h"
#include "../common/GEResourceTracker.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace {
    // Smallest power of two >= v (v <= 1 maps to 1).
    unsigned nextPow2(unsigned v){
        if(v <= 1) return 1u;
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
        return v + 1u;
    }

    // Phase F-G bucket policy: power-of-two bucket >= x, clamped to
    // [kMinBucket, kMaxBucket]. For x above the cap the bucket is clamped below
    // x, so callers take max(bucketDim(x), x) to guarantee the allocation
    // always covers the live size (an exact, un-bucketed alloc above the cap).
    unsigned bucketDim(unsigned x){
        constexpr unsigned kMinBucket = 256u;
        constexpr unsigned kMaxBucket = 8192u;   // modern max 2D texture dim
        unsigned b = nextPow2(x);
        if(b < kMinBucket) b = kMinBucket;
        if(b > kMaxBucket) b = kMaxBucket;
        return b;
    }

    // OMEGAWTK_BUCKETED_SWAPCHAIN gates the Phase F-G bucketed swap-chain path
    // at runtime. The build-time default is the CMake option
    // OMEGAGTE_ENABLE_BUCKETED_RENDER_TARGET (OFF while the path is under test,
    // so the default build uses the legacy exact-size ResizeBuffers-per-tick
    // behavior). The env var overrides the build default in either direction
    // ("1"/"on"/"true" to enable, "0"/"off"/"false" to disable), so one binary
    // can A/B the path. Read once.
    bool bucketedSwapChainEnabled(){
        static const bool enabled = [](){
#ifdef OMEGAGTE_BUCKETED_RENDER_TARGET_ENABLED
            constexpr bool kBuildDefault = true;
#else
            constexpr bool kBuildDefault = false;
#endif
            const char * v = std::getenv("OMEGAWTK_BUCKETED_SWAPCHAIN");
            if(v == nullptr) return kBuildDefault;
            return !(std::strcmp(v, "0") == 0 ||
                     std::strcmp(v, "off") == 0 ||
                     std::strcmp(v, "false") == 0);
        }();
        return enabled;
    }
}

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
        // Phase F-G: seed the live source dims from the creation back-buffer
        // size (== window client size at creation). resizeSwapChain keeps them
        // in step with the presented region thereafter.
        if(!this->renderTargets.empty() && this->renderTargets[0] != nullptr){
            auto d = this->renderTargets[0]->GetDesc();
            sourceWidth_  = static_cast<unsigned>(d.Width);
            sourceHeight_ = static_cast<unsigned>(d.Height);
        }
        // Back buffers come out of IDXGISwapChain::GetBuffer in COMMON/PRESENT.
        renderTargetStates.assign(this->renderTargets.size(), D3D12_RESOURCE_STATE_PRESENT);
        traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Create,
                ResourceTracking::Backend::D3D12,
                "NativeRenderTarget",
                traceResourceId,
                this->swapChain.Get());
        DEBUG_INFO(DEBUG_DOMAIN_RENDERTGT, "NativeRenderTarget created: id=" << traceResourceId
                   << " " << sourceWidth_ << "x" << sourceHeight_);
    };

    GED3D12NativeRenderTarget::~GED3D12NativeRenderTarget(){
        // D3D12 final-release of back buffers / swap chain is only safe when
        // no GPU work on the present queue still references them. The
        // Compositor's global waitForGPUIdle only covers the main rendering
        // queue; this target owns a dedicated present queue (e.g.
        // "WTK::DCVisualBinder presentQueue") whose pending Present1 /
        // barrier work has to be drained explicitly here, before the
        // renderTargets[] Release()s and the swapChain ComPtr drop below.
        waitForGPU();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::D3D12,
                "NativeRenderTarget",
                traceResourceId,
                swapChain.Get());
        DEBUG_INFO(DEBUG_DOMAIN_RENDERTGT, "NativeRenderTarget destroyed: id=" << traceResourceId);
        // Back-buffer pointers arrive at +1 from `IDXGISwapChain3::GetBuffer`
        // and were never wrapped in ComPtrs — release them here so the swap
        // chain can actually be torn down.
        for(auto *r : renderTargets) if(r) r->Release();
        renderTargets.clear();
    }

    void *GED3D12NativeRenderTarget::getSwapChain(){
        return (void *)swapChain.Get();
    };

    bool GED3D12NativeRenderTarget::reallocBackBuffers(unsigned bufferW, unsigned bufferH) {
        if (bufferW == 0 || bufferH == 0) return false;
        ComPtr<ID3D12Device> device;
        if (!renderTargets.empty() && renderTargets[0] != nullptr) {
            if (FAILED(renderTargets[0]->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
                return false;
        } else
            return false;
        // The realloc frees the back-buffers the present queue's in-flight
        // command lists bake in, so drain that queue first (sole-owner
        // contract documented on BackendRenderTargetContext::beginFrame).
        auto queue = (GED3D12CommandQueue *)presentQueue_.get();
        if (queue != nullptr)
            queue->commitToGPUAndWait();
        for (auto *r : renderTargets)
            if (r != nullptr) r->Release();
        renderTargets.clear();
        HRESULT hr = swapChain->ResizeBuffers(2, bufferW, bufferH, bufferDxgiFormat_, 0);
        if (FAILED(hr)) return false;
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
            if (FAILED(hr)) return false;
            device->CreateRenderTargetView(backBuffer.Get(),
                                           needsExplicitRtvDesc ? &rtvDesc : nullptr,
                                           rtvCpuHandle);
            rtvCpuHandle.Offset(1, rtvDescSize);
            renderTargets.push_back(backBuffer.Detach());
        }
        // Freshly (re)allocated back buffers are in COMMON/PRESENT again.
        renderTargetStates.assign(renderTargets.size(), D3D12_RESOURCE_STATE_PRESENT);
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        return true;
    }

    void GED3D12NativeRenderTarget::resizeSwapChain(unsigned int width, unsigned int height) {
        if (width == 0 || height == 0) return;
        if (renderTargets.empty() || renderTargets[0] == nullptr) return;

        const D3D12_RESOURCE_DESC bb = renderTargets[0]->GetDesc();
        const unsigned curBufW = static_cast<unsigned>(bb.Width);
        const unsigned curBufH = static_cast<unsigned>(bb.Height);

        if (bucketedSwapChainEnabled()) {
            // Phase F-G: keep the back-buffer at a >= power-of-two bucket and
            // present only the live [0,0,width,height] sub-region via
            // IDXGISwapChain2::SetSourceSize. DXGI_SCALING_STRETCH (set at
            // creation) scales that source region to the window; because the
            // source size == the window's physical client size, the result is
            // 1:1 (crisp, no blur). ResizeBuffers — the expensive
            // commitToGPUAndWait + realloc — fires ONLY when the live size
            // grows past the current buffer; every in-bucket resize tick is a
            // cheap SetSourceSize. The buffer is grow-only: a shrink keeps the
            // larger buffer, so a shrink-then-grow stays in one bucket.
            // (Assumes the non-MSAA native path, SampleDesc.Count == 1, which
            // is how makeNativeRenderTarget builds the swap chain. MSAA-on-
            // native would also need its resolve source bucketed; not used.)
            if (width <= curBufW && height <= curBufH) {
                HRESULT sr = swapChain->SetSourceSize(width, height);
                if (SUCCEEDED(sr)) {
                    sourceWidth_  = width;
                    sourceHeight_ = height;
                    DEBUG_INFO(DEBUG_DOMAIN_RENDERTGT, "Swapchain SetSourceSize: rt=" << traceResourceId
                               << " " << width << "x" << height
                               << " (buffer " << curBufW << "x" << curBufH << ")");
                    return;
                }
                // Unexpected (source <= buffer should always succeed) — fall
                // through to a real resize so we never present a stale region.
            }
            // Grow: live exceeds the current buffer. Realloc to a fresh bucket
            // covering the new live size. max(bucketDim(x), x) handles the
            // above-cap case as an exact, un-bucketed allocation.
            const unsigned bufW = std::max(curBufW, std::max(bucketDim(width),  width));
            const unsigned bufH = std::max(curBufH, std::max(bucketDim(height), height));
            if (!reallocBackBuffers(bufW, bufH)) return;
            HRESULT sr = swapChain->SetSourceSize(width, height);
            sourceWidth_  = width;
            sourceHeight_ = height;
            ResourceTracking::Tracker::instance().emit(
                    ResourceTracking::EventType::ResizeRebuild,
                    ResourceTracking::Backend::D3D12,
                    "NativeRenderTarget",
                    traceResourceId,
                    swapChain.Get());
            DEBUG_INFO(DEBUG_DOMAIN_RENDERTGT, "Swapchain rebuild: rt=" << traceResourceId
                         << " bucket " << bufW << "x" << bufH
                         << ", SetSourceSize " << width << "x" << height
                         << (SUCCEEDED(sr) ? "" : " (SetSourceSize FAILED)"));
            return;
        }

        // Legacy (non-bucketed) path: exact-size ResizeBuffers per tick. Skip
        // when the buffer already matches. sourceWidth_/sourceHeight_ track the
        // exact buffer size here, so the viewport Y-flip is unchanged.
        if (bb.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            curBufW == width && curBufH == height)
            return;
        if (reallocBackBuffers(width, height)) {
            sourceWidth_  = width;
            sourceHeight_ = height;
        }
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
        if(commandList != nullptr && frameIndex < renderTargetStates.size()
           && renderTargetStates[frameIndex] != D3D12_RESOURCE_STATE_PRESENT){
            // Transition from the tracked state (RENDER_TARGET after a frame's
            // first startRenderPass). Skipping when already PRESENT avoids a
            // spurious PRESENT->PRESENT barrier on a frame that never drew to
            // the back buffer.
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTargets[frameIndex], renderTargetStates[frameIndex], D3D12_RESOURCE_STATE_PRESENT);
            commandList->ResourceBarrier(1,&barrier);
            renderTargetStates[frameIndex] = D3D12_RESOURCE_STATE_PRESENT;
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
            // Accumulate the (variable-length) present-failure diagnostic into one
            // string so it lands as a single gated DEBUG_ERROR line rather than a
            // chain of raw std::cout writes. The macro adds the
            // "[GED3D12Engine_Internal] - ERROR RENDERTGT" prefix and trailing
            // newline, so the message itself starts at "Failed to Present …".
            std::ostringstream oss;
            oss << "Failed to Present SwapChain hr=0x"
                << std::hex << static_cast<unsigned long>(hr) << std::dec;

            if(hr == DXGI_STATUS_OCCLUDED){
                oss << " (DXGI_STATUS_OCCLUDED)";
            }
            else if(commandList != nullptr) {
                ComPtr<ID3D12Device> device;
                HRESULT devHr = commandList->GetDevice(IID_PPV_ARGS(&device));
                if(SUCCEEDED(devHr) && device != nullptr){
                    const HRESULT removedReason = device->GetDeviceRemovedReason();
                    if(removedReason != S_OK){
                        oss << " deviceRemoved=0x"
                            << std::hex << static_cast<unsigned long>(removedReason) << std::dec;
                    }

                    ComPtr<ID3D12InfoQueue> infoQueue;
                    if(SUCCEEDED(device.As(&infoQueue)) && infoQueue != nullptr){
                        const UINT64 msgCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
                        if(msgCount > 0){
                            const UINT64 maxDump = 8;
                            const UINT64 first = msgCount > maxDump ? (msgCount - maxDump) : 0;
                            oss << " d3d12Messages=" << msgCount;
                            for(UINT64 i = first; i < msgCount; i++){
                                SIZE_T msgLength = 0;
                                if(FAILED(infoQueue->GetMessage(i,nullptr,&msgLength)) || msgLength == 0){
                                    continue;
                                }
                                std::vector<char> storage(msgLength);
                                auto *msg = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                                if(SUCCEEDED(infoQueue->GetMessage(i,msg,&msgLength)) && msg != nullptr && msg->pDescription){
                                    oss << "\n  [D3D12Message] " << msg->pDescription;
                                }
                            }
                            infoQueue->ClearStoredMessages();
                        }
                    }
                }
            }
            DEBUG_ERROR(DEBUG_DOMAIN_RENDERTGT, oss.str());
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
        DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "Present: rt=" << traceResourceId);
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
        DEBUG_INFO(DEBUG_DOMAIN_RENDERTGT, "TextureRenderTarget created: id=" << traceResourceId);
    }

    GED3D12TextureRenderTarget::~GED3D12TextureRenderTarget(){
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::D3D12,
                "TextureRenderTarget",
                traceResourceId,
                texture != nullptr ? texture->resource.Get() : nullptr);
        DEBUG_INFO(DEBUG_DOMAIN_RENDERTGT, "TextureRenderTarget destroyed: id=" << traceResourceId);
    }

    SharedHandle<GETexture> GED3D12TextureRenderTarget::underlyingTexture() {
        return std::static_pointer_cast<GETexture>(texture);
    }

_NAMESPACE_END_
