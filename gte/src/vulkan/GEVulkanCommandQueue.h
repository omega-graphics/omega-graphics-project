#include "GEVulkan.h"
#include "omegaGTE/GECommandQueue.h"

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

        GEVulkanRenderPipelineState *renderPipelineState = nullptr;
        GEVulkanComputePipelineState *computePipelineState = nullptr;

        friend class GEVulkanCommandQueue;

        bool inBlitPass = false;
        bool inComputePass = false;

        unsigned getBindingForResourceID(unsigned & id,omegasl_shader & shader);

        omegasl_shader_layout_desc_io_mode getResourceIOModeForResourceID(unsigned & id,omegasl_shader & shader);

        void insertResourceBarrierIfNeeded(GEVulkanTexture *texture,unsigned & resource_id,omegasl_shader & shader);
        void insertResourceBarrierIfNeeded(GEVulkanBuffer *buffer,unsigned & resource_id,omegasl_shader & shader);
    public:
        // void waitForFence(SharedHandle<GEFence> &fence, unsigned int val) override;
        // void signalFence(SharedHandle<GEFence> &fence, unsigned int val) override;

        void startRenderPass(const GERenderPassDescriptor &desc) override;

        void setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) override;

        void setScissorRects(std::vector<GEScissorRect> scissorRects) override;

        void setViewports(std::vector<GEViewport> viewports) override;

        void bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned index) override;

        void bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned index) override;

        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned index) override;

        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned  index) override;

        void setVertexBuffer(SharedHandle<GEBuffer> &buffer) override;

        void drawPolygons(RenderPassDrawPolygonType polygonType, unsigned vertexCount, size_t startIdx) override;

        void finishRenderPass() override;

        void startComputePass(const GEComputePassDescriptor &desc) override;
        void setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) override;
        void bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) override;
        void bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id) override;
        void dispatchThreads(unsigned int x, unsigned int y, unsigned int z) override;
        void finishComputePass() override;

        void startBlitPass() override;
        void copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) override;
        void copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest, const TextureRegion &region, const GPoint3D &destCoord) override;
        void finishBlitPass() override;
        void reset() override;

        void setName(OmegaCommon::StrRef name) override;

        void *native() override {
            return (void *)commandBuffer;
        }

        GEVulkanCommandBuffer(VkCommandBuffer & commandBuffer,GEVulkanCommandQueue *parentQueue);
        ~GEVulkanCommandBuffer() override = default;
    };

    class GEVulkanCommandQueue : public GECommandQueue {
        GEVulkanEngine *engine;
        VkCommandPool commandPool;

        VkFence submitFence;

        OmegaCommon::Vector<VkCommandBuffer> commandBuffers;

        OmegaCommon::Vector<VkCommandBuffer> commandQueue;
        unsigned currentBufferIndex;
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
            nameInfoExt.objectHandle = commandPool;
            nameInfoExt.pObjectName = name.data();
            vkSetDebugUtilsObjectNameEXT(engine->device,&nameInfoExt);
        };

        void *native() override {
            return (void *)commandPool;
        }
        GEVulkanCommandQueue(GEVulkanEngine *engine,unsigned size);
        ~GEVulkanCommandQueue() override;
    };
_NAMESPACE_END_

#endif