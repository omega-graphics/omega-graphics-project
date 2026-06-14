#include "omegaGTE/GECommandQueue.h"

#include "D3D12DescriptorRing.h"

#include <memory>
#include "GED3D12.h"
#include <cstdint>
#include <vector>
#include <utility>

#ifndef OMEGAGTE_GED3D12COMMANDQUEUE_H
#define OMEGAGTE_GED3D12COMMANDQUEUE_H

_NAMESPACE_BEGIN_
    class GED3D12CommandQueue;
    class GED3D12RenderPipelineState;
    class GED3D12ComputePipelineState;
    class GED3D12TextureRenderTarget;
    class GED3D12NativeRenderTarget;

    class GED3D12CommandBuffer : public GECommandBuffer {
        ComPtr<ID3D12GraphicsCommandList6> commandList;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        GED3D12CommandQueue *parentQueue;
        /// CommandQueue-Typed-Pool Phase 3 — slot in the parent queue's
        /// pool this buffer was checked out from. UINT32_MAX (default)
        /// means the buffer wasn't from a tracked pool slot; the submit
        /// paths skip stamping in that case so a defensive fallback path
        /// can still issue an ad-hoc buffer without confusing recycler.
        std::uint32_t poolSlot = UINT32_MAX;
        bool inComputePass;
        bool inBlitPass;
        bool inRenderPass;
        bool firstRenderPass = true;
        bool closed = false;
        std::uint64_t traceResourceId = 0;

        /// G.5.1 D3D12 completion-wiring follow-up — the WTK side sets this
        /// on the final frame command buffer (via `setCompletionHandler`)
        /// right before submit so its pooled scratch buffers can be recycled
        /// once the GPU finishes the frame. The owning queue moves the
        /// handler out at submit time (`stageCompletionHandlerFrom`) and
        /// fires it from `pollCompletions` when the queue's `retentionFence`
        /// reaches the value signaled for this buffer's `ExecuteCommandLists`.
        /// Empty when no handler is registered (the common non-frame CB case).
        GECommandBufferCompletionHandler completionHandler;

        friend class GED3D12CommandQueue;

        struct {
            GED3D12NativeRenderTarget *native = nullptr;
            GED3D12TextureRenderTarget *texture = nullptr;
        } currentTarget;

        GED3D12RenderPipelineState *currentRenderPipeline = nullptr;
        GED3D12ComputePipelineState *currentComputePipeline = nullptr;

        D3D12_ROOT_SIGNATURE_DESC1 * currentRootSignature = nullptr;

        // Extension 8 — D3D12 allows at most one CBV/SRV/UAV heap and one
        // SAMPLER heap bound at a time. Track the last-bound heap of each
        // type so a sampler bind can keep the resource heap live (and vice
        // versa); `rebindDescriptorHeaps` sets both in one SetDescriptorHeaps
        // call. nullptr means "none bound yet".
        ID3D12DescriptorHeap *currentResourceDescHeap = nullptr;  // CBV/SRV/UAV
        ID3D12DescriptorHeap *currentSamplerDescHeap = nullptr;   // SAMPLER

       friend class GED3D12CommandQueue;

       unsigned getRootParameterIndexOfResource(unsigned id,omegasl_shader &shader);
       /// Extension 8 — (re)bind whichever of the resource / sampler heaps are
       /// set, so both types can be visible to a single draw / dispatch.
       void rebindDescriptorHeaps();
       D3D12_RESOURCE_STATES getRequiredResourceStateForResourceID(unsigned & id,omegasl_shader &shader);
       /// Loud (always-on) diagnostic + debug assert: a resource bound for use
       /// inside a live render pass needed a state transition that D3D12 forbids
       /// between BeginRenderPass/EndRenderPass (mirrors the Vulkan backend's
       /// VUID-vkCmdPipelineBarrier2-pDependencies-02285 guard). The transition
       /// is then skipped rather than recorded; the log points at the frontend
       /// bug — a resource sampled in a pass must reach its required state
       /// before the pass begins.
       void reportTransitionInsideRenderPass(const char *resourceKind,
                                             D3D12_RESOURCE_STATES fromState,
                                             D3D12_RESOURCE_STATES toState) const;
       /// Combine a runtime swizzle override with the shader layout's
       /// `swizzle_desc` (texture-swizzle proposal §4 precedence rule).
       TextureSwizzle resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader &shader);
    public:

        bool multisampleResolvePass = false;

        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW wstr(name.data());
            commandList->SetName(wstr.GetString());
        }

        void *native() override {
            return (void *)commandList.Get();
        }

        void startBlitPass() override;
        void copyTextureToTexture(SharedHandle<GETexture> & src,SharedHandle<GETexture> & dest) override;
        void copyTextureToTexture(SharedHandle<GETexture> & src,SharedHandle<GETexture> & dest,const TextureRegion & region,const GPoint3D & destCoord) override;
        void copyBufferToBuffer(SharedHandle<GEBuffer> & src,SharedHandle<GEBuffer> & dest,
                                size_t size, size_t srcOffset, size_t destOffset) override;
        void copyBufferToTexture(SharedHandle<GEBuffer> & src,SharedHandle<GETexture> & dest,
                                 size_t bytesPerRow, size_t bytesPerImage,
                                 const TextureRegion & destRegion, size_t srcBufferOffset) override;
        void copyTextureToBuffer(SharedHandle<GETexture> & src,SharedHandle<GEBuffer> & dest,
                                 size_t bytesPerRow, size_t bytesPerImage,
                                 const TextureRegion & srcRegion, size_t destBufferOffset) override;
        void generateMipmaps(SharedHandle<GETexture> & texture) override;
        void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                              SharedHandle<GETexture> & src,
                              SharedHandle<GETexture> & dest) override;
        void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                              SharedHandle<GETexture> & src,
                              SharedHandle<GETexture> & dest,
                              const TextureRegion & srcRegion,
                              const TextureRegion & destRegion) override;
        void fillBuffer(SharedHandle<GEBuffer> & buffer, uint32_t value,
                        size_t offset, size_t size) override;
        void finishBlitPass() override;
        /// Raytracing Funcs
        void beginAccelStructPass() override;
        void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, const GEAccelerationStructDescriptor &desc) override;
        void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest) override;
        void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc) override;
        void finishAccelStructPass() override;

        void startRenderPass(const GERenderPassDescriptor &desc) override;
        void setVertexBuffer(SharedHandle<GEBuffer> &buffer) override;
        void setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) override;
        void bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned int id,
                                        const TextureSwizzle & swizzle) override;
        void bindResourceAtVertexShader(SharedHandle<GESamplerState> &sampler, unsigned int id) override;
        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned int id,
                                          const TextureSwizzle & swizzle) override;
        void bindResourceAtFragmentShader(SharedHandle<GESamplerState> &sampler, unsigned int id) override;
        void setRenderConstants(const void *data, unsigned size, unsigned offset) override;
        void setStencilRef(unsigned int ref) override;
       
        void drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount, size_t startIdx) override;
        void setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType) override;
        void drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                 unsigned indexCount, size_t startIndex,
                                 int baseVertex) override;
        void drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                   unsigned vertexCount, size_t startIdx,
                                   unsigned instanceCount, unsigned firstInstance) override;
        void drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                           unsigned indexCount, size_t startIndex,
                                           int baseVertex, unsigned instanceCount,
                                           unsigned firstInstance) override;
        void drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                   SharedHandle<GEBuffer> & argumentBuffer,
                                   size_t argumentBufferOffset) override;
        void drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                          SharedHandle<GEBuffer> & argumentBuffer,
                                          size_t argumentBufferOffset) override;
        /// Mesh-Shader-Plan Phase 3 — public-API stub. Feature-gates,
        /// logs + returns. Phase 4b lands `commandList->DispatchMesh`.
        void drawMeshTasks(uint32_t groupCountX,
                           uint32_t groupCountY,
                           uint32_t groupCountZ) override;
        void setViewports(std::vector<GEViewport> viewports) override;
        void setScissorRects(std::vector<GEScissorRect> scissorRects) override;
        void finishRenderPass() override;

        void startComputePass(const GEComputePassDescriptor &desc) override;
        void setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) override;
        void bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id,
                                         const TextureSwizzle & swizzle) override;
        void bindResourceAtComputeShader(SharedHandle<GESamplerState> &sampler, unsigned int id) override;
        void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct, unsigned int id) override;
        void setComputeConstants(const void *data, unsigned size, unsigned offset) override;
        void dispatchRays(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreads(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                           size_t argumentBufferOffset) override;
        void finishComputePass() override;

        GED3D12CommandBuffer(ID3D12GraphicsCommandList6 *commandList,ID3D12CommandAllocator *commandAllocator,GED3D12CommandQueue *parentQueue);
        void reset() override;
        /// Store a callback fired (exactly once) by the owning queue when this
        /// command buffer's GPU work completes. Matches the Metal contract;
        /// see `completionHandler` above for the D3D12 fence-poll mechanism.
        void setCompletionHandler(const GECommandBufferCompletionHandler & handler) override;

        ~GED3D12CommandBuffer() override;
    };

    class GED3D12CommandQueue : public GECommandQueue {
        GED3D12Engine *engine;

        // Pending submit batch: command lists queued by submitCommandBuffer
        // calls and not yet handed to ExecuteCommandLists, plus the
        // SharedHandles / descriptor heaps they depend on. At flush time
        // these are moved into the engine retention queue with a fence gate
        // for the just-issued submit; they are NOT released inline.
        std::vector<ID3D12GraphicsCommandList6 *> commandLists;
        std::vector<SharedHandle<GECommandBuffer>> retainedCommandBuffers;
        std::vector<ComPtr<ID3D12DescriptorHeap>> retainedDescriptorHeaps;
        ComPtr<ID3D12CommandQueue> commandQueue;

        // Monotonic fence for retention gating. Distinct from `fence` below,
        // which is the binary-valued fence used by commitToGPUAndWait. Each
        // ExecuteCommandLists is paired with Signal(retentionFence,
        // ++nextSubmitValue); the captured value drives the FenceGate handed
        // to the engine retention queue.
        ComPtr<ID3D12Fence> retentionFence;
        std::uint64_t       nextSubmitValue = 0;

        // Build a gate that returns true once the next submit's signal value
        // (i.e. ++nextSubmitValue) has been reached on retentionFence. Must be
        // called immediately before issuing the corresponding Signal().
        Retention::FenceGate gateForNextSubmit();

    public:
        // Snapshot a gate that signals once every command list already
        // submitted to this queue has retired on the GPU. Captures the
        // current `nextSubmitValue` — later submits do NOT move the gate.
        // Cheap; safe to call from any thread. Used by texture / sampler
        // destructors to defer descriptor-slot frees behind GPU completion.
        Retention::FenceGate gateForRetiredSubmissions() const;
    private:

        // Move every accumulated retained command buffer / descriptor heap
        // into the engine retention queue under `gate`, then clear the local
        // pending vectors.
        void flushPendingRetentionUnder(const Retention::FenceGate &gate);

        // G.5.1 D3D12 completion-wiring follow-up. Mirrors the pendingSlots →
        // stampPendingSlots lifecycle: a command buffer's completion handler
        // is *staged* at submit time (its list is queued but not yet executed)
        // and *gated* to a concrete retentionFence value once the
        // ExecuteCommandLists that runs it is issued. `pollCompletions` fires
        // and drops every gated handler whose value the GPU has reached.
        // All three are touched only on the queue's owning (compositor) thread
        // — the same thread that submits, commits, and calls getAvailableBuffer
        // — so no locking is needed; the cross-thread hand-off the plan warns
        // about only applies to the waiter-thread alternative, not this poll.
        std::vector<GECommandBufferCompletionHandler>                            stagedCompletionHandlers_;
        std::vector<std::pair<std::uint64_t, GECommandBufferCompletionHandler>>  gatedCompletionHandlers_;
        // Move `cb`'s registered completion handler (if any) into the staged
        // list and clear it off the buffer. No-op when `cb` has no handler.
        void stageCompletionHandlerFrom(GED3D12CommandBuffer *cb);
        // Promote every staged handler to gated at `signalValue` (the
        // retentionFence value just signaled for the Execute that ran them).
        void gateStagedCompletions(std::uint64_t signalValue);
        // Fire + drop every gated handler whose retentionFence value the GPU
        // has reached. Cheap (one GetCompletedValue); safe to call at any
        // drain point. Reports Error to fired handlers if the device was lost.
        void pollCompletions();

        ComPtr<ID3D12Fence> fence;

        HANDLE cpuEvent;
        std::uint64_t traceResourceId = 0;
        std::vector<std::uint64_t> submittedTraceCommandBufferIds;

        bool multiQueueSync = false;

        unsigned currentCount;

        // CommandQueue-Typed-Pool Phase 3 — growable pool of
        // (allocator, list) pairs with per-slot in-flight tracking via
        // `retentionFence`. `poolAllocators[i]` and `poolLists[i]`
        // co-own the allocator/list backing pool slot i;
        // `poolSubmissionIndex[i]` holds the `retentionFence` value at
        // which that slot was last handed to `ExecuteCommandLists`. The
        // recycler reuses a slot once `retentionFence->GetCompletedValue()`
        // catches up.
        std::vector<ComPtr<ID3D12CommandAllocator>>      poolAllocators;
        std::vector<ComPtr<ID3D12GraphicsCommandList6>>  poolLists;
        std::vector<std::uint64_t>                       poolSubmissionIndex;
        // Slot indices parallel to `commandLists`. At submit/commit time
        // these are stamped into `poolSubmissionIndex` so getAvailableBuffer
        // knows when each slot is recyclable. UINT32_MAX entries
        // correspond to lists that weren't from a tracked pool slot
        // (defensive — shouldn't happen in normal use).
        std::vector<std::uint32_t>                       pendingSlots;
        std::uint32_t                                    currentBufferIndex = 0;
        std::uint32_t                                    initialBufferHint  = 0;
        bool                                             poolGrowthWarned   = false;

        D3D12_COMMAND_LIST_TYPE listType = D3D12_COMMAND_LIST_TYPE_DIRECT;

        // Shared-Descriptor-Heap-Plan Phase 3 — per-queue transient
        // descriptor ring for one-shot dispatches (tessellation,
        // generateMipmaps). Bump-only, recycled when this queue's
        // retentionFence retires the submission that recorded the slots.
        // Initial size 4096 — easily covers a typical frame's
        // generateMipmaps + tessellation traffic on a single window.
        std::unique_ptr<D3D12DescriptorRing> transientRing;

        // Tag `poolSubmissionIndex[slot]` for every entry of `pendingSlots`
        // and clear `pendingSlots`. Call after Signal(retentionFence,
        // nextSubmitValue) on any code path that successfully submitted
        // the pending batch.
        void stampPendingSlots(std::uint64_t signalValue);
        // Try to allocate one more pool slot (allocator + list) on
        // demand. Returns the new slot index on success, UINT32_MAX on
        // failure or at the hard ceiling (256). Soft-warns once when the
        // pool first crosses 4× the initial hint.
        std::uint32_t growPoolOnce();

        friend class GED3D12Engine;
        friend class GED3D12CommandBuffer;
    public:
        void setName(OmegaCommon::StrRef name) override{
            ATL::CStringW wstr(name.data());
            commandQueue->SetName(wstr);
        }

        void *native() override {
            return (void *)commandQueue.Get();
        }
        /// Phase 3 (Shared-Descriptor-Heap-Plan) — exposed for external
        /// dispatch paths that aren't `GED3D12CommandBuffer` (currently
        /// `D3D12TEContext::d3d12GpuDispatch`, which records its own
        /// command list against the raw `ID3D12CommandQueue` but still
        /// wants to reuse the queue's transient descriptor ring).
        D3D12DescriptorRing *getTransientRing() const { return transientRing.get(); }
        std::uint64_t traceId() const {
            return traceResourceId;
        }
        std::uint64_t lastSubmittedCommandBufferTraceId() const {
            if(submittedTraceCommandBufferIds.empty()){
                return 0;
            }
            return submittedTraceCommandBufferIds.back();
        }
        void clearSubmittedTraceCommandBufferIds() {
            submittedTraceCommandBufferIds.clear();
        }
        ID3D12GraphicsCommandList6 * getLastCommandList();
        void commitToGPU() override;
        void commitToGPUAndWait() override;
        void notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &signalFence) override;
        void signalFence(SharedHandle<GEFence> & fence) override;
        void waitForFence(SharedHandle<GEFence> & fence, std::uint64_t value) override;
        SharedHandle<GECommandBuffer> getAvailableBuffer() override;
        /// Builds a real `D3D12_COMMAND_QUEUE_DESC` from `desc` (Type →
        /// `D3D12_COMMAND_LIST_TYPE_*`, Priority →
        /// `D3D12_COMMAND_QUEUE_PRIORITY_*`) and applies `desc.label` via
        /// `ID3D12Object::SetName`. Realtime priority silently downgrades
        /// to HIGH if the device denies it (no entitlement). This is the
        /// only ctor after the Phase 4 legacy-overload retirement; build
        /// a default-initialized `GECommandQueueDesc` for the historical
        /// "universal queue with N pool slots" use case.
        GED3D12CommandQueue(GED3D12Engine *engine,const GECommandQueueDesc & desc);
        ~GED3D12CommandQueue() override;
    };
_NAMESPACE_END_
#endif
