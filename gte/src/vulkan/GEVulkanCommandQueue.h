#include "GEVulkan.h"
#include "omegaGTE/GECommandQueue.h"
#include <cstdint>

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
        OmegaCommon::Vector<VkDescriptorSet> retiredFallbackSets;
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

        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned index) override;

        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned  index,
                                          const TextureSwizzle & swizzle) override;

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

        VkFence submitFence;

        // Per-queue timeline semaphore signaled with a monotonic value at every
        // vkQueueSubmit. Backs FenceGate via vkGetSemaphoreCounterValue. The
        // engine retention queue dereferences this through the gate closures
        // it stores; the semaphore itself is destroyed in releaseNative()
        // after the engine has drained pending entries.
        VkSemaphore   retentionTimeline = VK_NULL_HANDLE;
        std::uint64_t nextSubmitValue   = 0;

        OmegaCommon::Vector<VkCommandBuffer> commandBuffers;

        OmegaCommon::Vector<VkCommandBuffer> commandQueue;
        // SharedHandles to the GEVulkanCommandBuffers queued by
        // submitCommandBuffer. At vkQueueSubmit each is moved into
        // engine->retentionQueue under the submit's gate, along with any
        // render passes / framebuffers / tracked resources it owns.
        OmegaCommon::Vector<SharedHandle<GECommandBuffer>> pendingRetainedBuffers;
        OmegaCommon::Vector<std::uint64_t> submittedTraceCommandBufferIds;
        unsigned currentBufferIndex;

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
        GEVulkanCommandQueue(GEVulkanEngine *engine,unsigned size);
        void releaseNative();
        ~GEVulkanCommandQueue() override;
    };
_NAMESPACE_END_

#endif
