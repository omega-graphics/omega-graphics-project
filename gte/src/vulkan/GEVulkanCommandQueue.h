#include "GEVulkan.h"
#include "omegaGTE/GECommandQueue.h"
#include <cstdint>
#include <utility>

#ifndef OMEGAGTE_VULKAN_GEVULKANCOMMANDQUEUE_H
#define OMEGAGTE_VULKAN_GEVULKANCOMMANDQUEUE_H


_NAMESPACE_BEGIN_
    class GEVulkanCommandQueue;
    class GEVulkanRenderPipelineState;
    class GEVulkanComputePipelineState;
    class GEVulkanTexture;
    class GEVulkanBuffer;

    class GEVulkanCommandBuffer : public GECommandBuffer {
        GEVulkanCommandQueue *parentQueue;
        VkCommandBuffer & commandBuffer;
        std::uint64_t traceResourceId = 0;
        /// CommandQueue-Typed-Pool Phase 3 — index of the pool slot this
        /// buffer was checked out from. The queue uses this to map a
        /// submitted buffer back to the slot in `commandBuffers[]` /
        /// `commandBufferSubmissionIndex[]` so the slot's last-submitted
        /// counter can be stamped at vkQueueSubmit time. UINT32_MAX
        /// (default) means "not from a tracked pool slot" — defensive,
        /// shouldn't fire in normal use.
        std::uint32_t poolSlot = UINT32_MAX;

        GEVulkanRenderPipelineState *renderPipelineState = nullptr;
        GEVulkanComputePipelineState *computePipelineState = nullptr;
        VkRenderPass activeRenderPass = VK_NULL_HANDLE;
        VkFramebuffer activeFramebuffer = VK_NULL_HANDLE;
        OmegaCommon::Vector<VkRenderPass> ownedRenderPasses;
        OmegaCommon::Vector<VkFramebuffer> ownedFramebuffers;

        // Resources bound by encoders during recording. Kept alive until the
        // submitted work is gated to a queue retention value at submit time;
        // each tracked resource then has the submit's gate appended to its
        // pendingGates so its destructor can defer vmaDestroyBuffer/Image.
        OmegaCommon::Vector<SharedHandle<GEBuffer>>  trackedBuffers;
        OmegaCommon::Vector<SharedHandle<GETexture>> trackedTextures;
        void trackBuffer(const SharedHandle<GEBuffer> &b);
        void trackTexture(const SharedHandle<GETexture> &t);
        friend class GEVulkanCommandQueue;

        /// Deferred render pass begin: barriers issued between startRenderPass
        /// and drawPolygons execute outside the render pass instance, avoiding
        /// Vulkan spec violations for image layout transitions.
        bool renderPassBeginDeferred = false;
        VkRenderPassBeginInfo deferredBeginInfo {};
        OmegaCommon::Vector<VkClearValue> deferredClearValues;

        // Deferred descriptor-set bind: NVIDIA's driver snapshots descriptor
        // contents at vkCmdBindDescriptorSets time; updating the set afterward
        // is spec-legal but the snapshot is stale. Defer the bind to draw time
        // so it runs after all vkUpdateDescriptorSets calls.
        bool descriptorSetsBindPending = false;

        // Fallback (no-push-descriptor) per-pipeline-state descriptor set
        // ring. setRenderPipelineState() lazily allocates one fresh set per
        // descriptor slot from the pipeline's pool and stages them here so
        // bindResource* writes don't touch sets that may still be bound to
        // an in-flight command buffer. Cleared (and freed back to the
        // pool) when the command buffer is destroyed or reset.
        OmegaCommon::Vector<VkDescriptorSet> fallbackDescriptorSets;
        // Retired sets carry their origin pool: a command buffer that switches
        // pipelines accumulates sets from multiple per-pipeline pools, and
        // vkFreeDescriptorSets requires every set in the batch to belong to
        // the pool being freed against (VUID-vkFreeDescriptorSets-pDescriptorSets-parent).
        OmegaCommon::Vector<std::pair<VkDescriptorPool, VkDescriptorSet>> retiredFallbackSets;
        VkDescriptorPool fallbackDescriptorPool = VK_NULL_HANDLE;
        bool fallbackPoolExhausted = false;
        // True once the current fallback ring slots have been bound to a
        // draw via bindDescriptorSetsIfPending. The next bindResource*
        // write must retire them and acquire fresh slots — updating a set
        // that's still bound to a recorded draw violates the spec.
        bool fallbackSetsCommitted = false;

        VkDescriptorSet acquireOrUpdateFallbackSet(unsigned slotIndex);
        void releaseFallbackDescriptorSets();
        void resetFallbackDescriptorSetsForNewPipeline();

        void beginRenderPassIfDeferred();
        void bindDescriptorSetsIfPending();

        /// True when a command recorded right now would land INSIDE an active
        /// render pass instance: the deferred begin has been flushed (first draw
        /// happened) and the pass has not yet been ended. Resource barriers are
        /// illegal here (VUID-vkCmdPipelineBarrier2-pDependencies-02285) — the
        /// frontend must split the pass (finishRenderPass + restart with
        /// LoadPreserve) before any texture layout transition. Note that a pass
        /// whose begin is still deferred reports false: barriers issued then are
        /// recorded before vkCmdBeginRenderPass and are legal.
        bool isInsideRenderPassInstance() const {
            return activeRenderPass != VK_NULL_HANDLE && !renderPassBeginDeferred;
        }
        /// Loud (always-on) diagnostic + debug assert for the contract violation
        /// above: a real layout transition was requested for `texture` while a
        /// render pass instance is live. The barrier is then skipped rather than
        /// recorded, so we never emit invalid Vulkan; the log points at the
        /// frontend bug (a pass that should have been split).
        void reportBarrierInsideRenderPass(GEVulkanTexture *texture,
                                           VkImageLayout oldLayout,
                                           VkImageLayout newLayout,
                                           const omegasl_shader &shader) const;

        friend class GEVulkanCommandQueue;

        bool inBlitPass = false;
        bool inComputePass = false;

        VkIndexType pendingIndexType = VK_INDEX_TYPE_UINT32;

        void applyTopologyIfDynamic(RenderPassDrawPolygonType polygonType);

        unsigned getBindingForResourceID(unsigned & id,omegasl_shader & shader);

        omegasl_shader_layout_desc_io_mode getResourceIOModeForResourceID(unsigned & id,omegasl_shader & shader);

        /// §2.4 — pick the buffer descriptor type for a resource slot from the
        /// shader layout: VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER for a `uniform<T>`
        /// slot (OMEGASL_SHADER_UNIFORM_DESC), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        /// otherwise. The shader layout is authoritative for binding type.
        VkDescriptorType getBufferDescriptorTypeForResourceID(unsigned & id,omegasl_shader & shader);

        /// Combine a runtime swizzle override with the shader layout's
        /// `swizzle_desc` per the precedence rule in
        /// `gte/docs/texture-swizzle-proposal.md` §4: runtime override
        /// wins when non-identity, otherwise the layout default applies.
        TextureSwizzle resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader & shader);

        void insertResourceBarrierIfNeeded(GEVulkanTexture *texture,unsigned & resource_id,omegasl_shader & shader);
        void insertResourceBarrierIfNeeded(GEVulkanBuffer *buffer,unsigned & resource_id,omegasl_shader & shader);
    public:
        void setStencilRef(unsigned int ref) override;

        void startRenderPass(const GERenderPassDescriptor &desc) override;

        void setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) override;

        void setScissorRects(std::vector<GEScissorRect> scissorRects) override;

        void setViewports(std::vector<GEViewport> viewports) override;

        void bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned index) override;

        void bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned index,
                                        const TextureSwizzle & swizzle) override;

        void bindResourceAtVertexShader(SharedHandle<GESamplerState> &sampler, unsigned index) override;

        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned index) override;

        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned  index,
                                          const TextureSwizzle & swizzle) override;

        void bindResourceAtFragmentShader(SharedHandle<GESamplerState> &sampler, unsigned index) override;

        void setRenderConstants(const void *data, unsigned size, unsigned offset) override;

        void setVertexBuffer(SharedHandle<GEBuffer> &buffer) override;

        void drawPolygons(RenderPassDrawPolygonType polygonType, unsigned vertexCount, size_t startIdx) override;

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
        /// logs + returns. Phase 4a wires `vkCmdDrawMeshTasksEXT`.
        void drawMeshTasks(uint32_t groupCountX,
                           uint32_t groupCountY,
                           uint32_t groupCountZ) override;

        void finishRenderPass() override;

        void beginAccelStructPass() override;
        void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, const GEAccelerationStructDescriptor &desc) override;
        void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest) override;
        void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc) override;
        void finishAccelStructPass() override;
        void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct, unsigned int id) override;
        void dispatchRays(unsigned int x, unsigned int y, unsigned int z) override;

        void startComputePass(const GEComputePassDescriptor &desc) override;
        void setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) override;
        void bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id,
                                         const TextureSwizzle & swizzle) override;
        void bindResourceAtComputeShader(SharedHandle<GESamplerState> &sampler, unsigned int id) override;
        void setComputeConstants(const void *data, unsigned size, unsigned offset) override;
        void dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreads(unsigned int x, unsigned int y, unsigned int z) override;
        void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                           size_t argumentBufferOffset) override;
        void finishComputePass() override;

        void startBlitPass() override;
        void copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) override;
        void copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest, const TextureRegion &region, const GPoint3D &destCoord) override;
        void copyBufferToBuffer(SharedHandle<GEBuffer> &src, SharedHandle<GEBuffer> &dest,
                                size_t size, size_t srcOffset, size_t destOffset) override;
        void copyBufferToTexture(SharedHandle<GEBuffer> &src, SharedHandle<GETexture> &dest,
                                 size_t bytesPerRow, size_t bytesPerImage,
                                 const TextureRegion &destRegion, size_t srcBufferOffset) override;
        void copyTextureToBuffer(SharedHandle<GETexture> &src, SharedHandle<GEBuffer> &dest,
                                 size_t bytesPerRow, size_t bytesPerImage,
                                 const TextureRegion &srcRegion, size_t destBufferOffset) override;
        void generateMipmaps(SharedHandle<GETexture> &texture) override;
        void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                              SharedHandle<GETexture> & src,
                              SharedHandle<GETexture> & dest) override;
        void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                              SharedHandle<GETexture> & src,
                              SharedHandle<GETexture> & dest,
                              const TextureRegion & srcRegion,
                              const TextureRegion & destRegion) override;
        void fillBuffer(SharedHandle<GEBuffer> &buffer, uint32_t value,
                         size_t offset, size_t size) override;
        void finishBlitPass() override;
        void reset() override;

        void setName(OmegaCommon::StrRef name) override;

        void *native() override {
            return (void *)commandBuffer;
        }

        GEVulkanCommandBuffer(VkCommandBuffer & commandBuffer,GEVulkanCommandQueue *parentQueue);
        ~GEVulkanCommandBuffer() override;
    };

    class GEVulkanCommandQueue : public GECommandQueue {
        GEVulkanEngine *engine;
        VkCommandPool commandPool;
        std::uint64_t traceResourceId = 0;
        bool nativeReleased_ = false;

        // CommandQueue-Typed-Pool Phase 2 — the (familyIndex, VkQueue) pair
        // this queue is bound to. Resolved at construction by
        // `VulkanQueueFamilies::Pick` and the engine's family lookup; used
        // by every submit path instead of grabbing
        // `engine->deviceQueuefamilies.front().front()` so multi-queue
        // designs (graphics + async-compute + DMA) actually hit different
        // VkQueues. `nativeQueue == VK_NULL_HANDLE` means construction
        // failed and the submit paths early-return without touching the
        // queue.
        std::uint32_t boundFamilyIndex = 0;
        VkQueue       nativeQueue      = VK_NULL_HANDLE;

        VkFence submitFence;

        // Per-queue timeline semaphore signaled with a monotonic value at every
        // vkQueueSubmit. Backs FenceGate via vkGetSemaphoreCounterValue. The
        // engine retention queue dereferences this through the gate closures
        // it stores; the semaphore itself is destroyed in releaseNative()
        // after the engine has drained pending entries.
        VkSemaphore   retentionTimeline = VK_NULL_HANDLE;
        std::uint64_t nextSubmitValue   = 0;

        OmegaCommon::Vector<VkCommandBuffer> commandBuffers;
        /// CommandQueue-Typed-Pool Phase 3 — last submission counter at
        /// which `commandBuffers[i]` was handed to `vkQueueSubmit`.
        /// `getAvailableBuffer()` recycles a slot once
        /// `vkGetSemaphoreCounterValue(retentionTimeline)` reaches this
        /// value. Zero means "never submitted" — that slot is free for
        /// the first cycle without any GPU wait. Sized in lockstep with
        /// `commandBuffers` by every grow / shrink.
        OmegaCommon::Vector<std::uint64_t> commandBufferSubmissionIndex;
        /// CommandQueue-Typed-Pool Phase 3 — initial pool size hint from
        /// `desc.maxBufferCount`. Sticky once construction completes so
        /// the soft-warn threshold (4× hint) and the per-call pool growth
        /// can refer to the same baseline even after the pool has grown.
        std::uint32_t initialBufferHint = 0;
        /// CommandQueue-Typed-Pool Phase 3 — sticky flag so the
        /// "pool grew past 4× the hint" warning fires exactly once per
        /// queue. Without this the soft warning would spam the log
        /// every time `getAvailableBuffer()` allocates a fresh slot.
        bool poolGrowthWarned = false;

        OmegaCommon::Vector<VkCommandBuffer> commandQueue;
        /// CommandQueue-Typed-Pool Phase 3 — pool-slot indices parallel
        /// to `commandQueue`. When `commitToGPU` calls vkQueueSubmit, the
        /// new submission counter value is stamped onto
        /// `commandBufferSubmissionIndex[pendingSlots[i]]` for every i.
        /// UINT32_MAX entries correspond to buffers that weren't checked
        /// out via `getAvailableBuffer()` (defensive — shouldn't happen
        /// in normal use).
        OmegaCommon::Vector<std::uint32_t> pendingSlots;
        // SharedHandles to the GEVulkanCommandBuffers queued by
        // submitCommandBuffer. At vkQueueSubmit each is moved into
        // engine->retentionQueue under the submit's gate, along with any
        // render passes / framebuffers / tracked resources it owns.
        OmegaCommon::Vector<SharedHandle<GECommandBuffer>> pendingRetainedBuffers;
        OmegaCommon::Vector<std::uint64_t> submittedTraceCommandBufferIds;
        unsigned currentBufferIndex;

        /// CommandQueue-Typed-Pool Phase 3 — after a successful
        /// vkQueueSubmit + nextSubmitValue bump, write `signalValue` into
        /// `commandBufferSubmissionIndex[slot]` for every entry of
        /// `pendingSlots`, then clear `pendingSlots`. UINT32_MAX entries
        /// (defensive — buffer not from getAvailableBuffer) are skipped.
        /// MUST be called inside any commit path that successfully
        /// submitted, paired with the corresponding `commandQueue.clear()`.
        void stampPendingSlots(std::uint64_t signalValue);

        Retention::FenceGate gateForNextSubmit();
        // Build a VkTimelineSemaphoreSubmitInfo signaling ++nextSubmitValue,
        // wire it into `submission.pNext`, and attach the timeline semaphore
        // to `submission.signalSemaphores`. The provided storage must outlive
        // the vkQueueSubmit call.
        void prepareSubmitWithRetentionSignal(VkSubmitInfo &submission,
                                              VkTimelineSemaphoreSubmitInfo &timelineInfo,
                                              VkSemaphore &signalSlot,
                                              std::uint64_t &signalValueSlot);
        // Move every accumulated pending command buffer (and the resources +
        // owned render passes / framebuffers each one carries) into the engine
        // retention queue under `gate`, then clear the local pending vector.
        void flushPendingRetentionUnder(const Retention::FenceGate &gate);

        friend class GEVulkanCommandBuffer;
    public:
        void notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &signalFence) override;
        void commitToGPU() override;
        void commitToGPUPresent(VkPresentInfoKHR * info);
        void commitToGPUAndWait() override;
        VkCommandBuffer &getLastCommandBufferInQueue();
        SharedHandle<GECommandBuffer> getAvailableBuffer() override;

        void setName(OmegaCommon::StrRef name) override {
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_COMMAND_POOL;
            nameInfoExt.objectHandle = (uint64_t)commandPool;
            nameInfoExt.pObjectName = name.data();
            vkSetDebugUtilsObjectNameEXT(engine->device,&nameInfoExt);
        };

        void *native() override {
            return (void *)commandPool;
        }
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
        GEVulkanEngine *getEngine() const { return engine; }
        /// Consults `VulkanQueueFamilies::Pick` to resolve `desc.type`
        /// (with the documented fallback ladder) into the VkQueue / family
        /// this queue will submit on. `desc.label` is plumbed through
        /// `VK_EXT_debug_utils` when the extension is available. Returns
        /// a queue with `nativeQueue == VK_NULL_HANDLE` if the engine has
        /// no families that satisfy the request and `desc.requireDedicated`
        /// is set — callers see this as a `nullptr` from
        /// `OmegaGraphicsEngine::makeCommandQueue`. This is the only ctor
        /// after the Phase 4 legacy-overload retirement; build a
        /// default-initialized `GECommandQueueDesc` for the historical
        /// "universal queue with N pool slots" use case.
        GEVulkanCommandQueue(GEVulkanEngine *engine, const GECommandQueueDesc & desc);
        /// The VkQueue this queue is bound to. May be `VK_NULL_HANDLE` if
        /// construction failed (caller should treat as nullptr).
        VkQueue getNativeQueue() const { return nativeQueue; }
        /// The Vulkan queue family index this queue is bound to.
        std::uint32_t getBoundFamilyIndex() const { return boundFamilyIndex; }
        void releaseNative();
        ~GEVulkanCommandQueue() override;
    };
_NAMESPACE_END_

#endif
