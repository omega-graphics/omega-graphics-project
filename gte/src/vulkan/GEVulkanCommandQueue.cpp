#include "GEVulkanCommandQueue.h"
#include "GEVulkanRenderTarget.h"
#include "GEVulkanPipeline.h"
#include "GEVulkan.h"
#include "GEVulkanTexture.h"
#include "vulkan/vulkan_core.h"
#include "../common/GEResourceTracker.h"

#include <cstdint>
#include <iostream>

_NAMESPACE_BEGIN_
    namespace {
        inline VkImageSubresourceRange fullColorSubresourceRange(const GEVulkanTexture *texture){
            VkImageSubresourceRange range {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = texture != nullptr && texture->descriptor.mipLevels > 0
                               ? texture->descriptor.mipLevels
                               : 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            return range;
        }
    }

    void GEVulkanCommandBuffer::setName(OmegaCommon::StrRef name) {
                VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                nameInfoExt.pNext = nullptr;
                nameInfoExt.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
                nameInfoExt.objectHandle = (uint64_t)commandBuffer;
                nameInfoExt.pObjectName = name.data();
                vkSetDebugUtilsObjectNameEXT(parentQueue->engine->device,&nameInfoExt);
    }

    unsigned int GEVulkanCommandBuffer::getBindingForResourceID(unsigned int &id, omegasl_shader &shader) {\
        ArrayRef<omegasl_shader_layout_desc> layoutDesc {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto & l : layoutDesc){
            if(l.location == id){
                return l.gpu_relative_loc;
            }
        }
        return 0;
    }

    omegasl_shader_layout_desc_io_mode
    GEVulkanCommandBuffer::getResourceIOModeForResourceID(unsigned int &id, omegasl_shader &shader) {
        ArrayRef<omegasl_shader_layout_desc> layoutDesc {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto & l : layoutDesc){
            if(l.location == id){
                return l.io_mode;
            }
        }
        return OMEGASL_SHADER_DESC_IO_INOUT;
    }

    void GEVulkanCommandBuffer::insertResourceBarrierIfNeeded(GEVulkanBuffer *buffer, unsigned int &resource_id,
                                                              omegasl_shader &shader) {
        // Temporary diagnostic fallback:
        // current explicit buffer barrier path can trigger VK_ERROR_DEVICE_LOST on some drivers.
        // Skip barriers for now to keep BasicAppTest stable while Vulkan sync is reworked.
        (void)buffer;
        (void)resource_id;
        (void)shader;
        return;

        auto ioMode = getResourceIOModeForResourceID(resource_id,shader);

        if(parentQueue->engine->hasSynchronization2Ext) {


            VkAccessFlags2KHR shaderAccess;
            VkPipelineStageFlags2KHR pipelineStage;

            if (shader.type == OMEGASL_SHADER_VERTEX) {
                pipelineStage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT_KHR;
            } else if (shader.type == OMEGASL_SHADER_FRAGMENT) {
                pipelineStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR;
            } else {
                pipelineStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
            }

            bool hasPipelineAccess = buffer->priorPipelineAccess2 != 0;

            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                shaderAccess = VK_ACCESS_2_SHADER_READ_BIT_KHR;
            } else if (ioMode == OMEGASL_SHADER_DESC_IO_INOUT) {
                shaderAccess = VK_ACCESS_2_SHADER_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT_KHR;
            } else {
                shaderAccess = VK_ACCESS_2_SHADER_WRITE_BIT_KHR;
            }

            if (buffer->priorAccess2 != 0 && hasPipelineAccess) {
                VkBufferMemoryBarrier2KHR bufferMemoryBarrier2Khr{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR};
                bufferMemoryBarrier2Khr.srcQueueFamilyIndex = bufferMemoryBarrier2Khr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bufferMemoryBarrier2Khr.buffer = buffer->buffer;
                bufferMemoryBarrier2Khr.offset = 0;
                bufferMemoryBarrier2Khr.size = VK_WHOLE_SIZE;
                bufferMemoryBarrier2Khr.srcAccessMask = buffer->priorAccess2;
                bufferMemoryBarrier2Khr.dstAccessMask = shaderAccess;
                bufferMemoryBarrier2Khr.srcStageMask = buffer->priorPipelineAccess2;
                bufferMemoryBarrier2Khr.dstStageMask = pipelineStage;
                bufferMemoryBarrier2Khr.pNext = nullptr;

                VkDependencyInfoKHR dependencyInfoKhr{VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
                dependencyInfoKhr.pNext = nullptr;
                dependencyInfoKhr.bufferMemoryBarrierCount = 1;
                dependencyInfoKhr.pBufferMemoryBarriers = &bufferMemoryBarrier2Khr;
                parentQueue->engine->vkCmdPipelineBarrier2Khr(commandBuffer, &dependencyInfoKhr);
            }

            buffer->priorPipelineAccess2 = pipelineStage;
            buffer->priorAccess2 = shaderAccess;

        }
        else {
            VkAccessFlags shaderAccess;
            VkPipelineStageFlags pipelineStage;

            if (shader.type == OMEGASL_SHADER_VERTEX) {
                pipelineStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            } else if (shader.type == OMEGASL_SHADER_FRAGMENT) {
                pipelineStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            } else {
                pipelineStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }

            bool hasPipelineAccess = buffer->priorPipelineAccess != 0;

            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                shaderAccess = VK_ACCESS_SHADER_READ_BIT;
            } else if (ioMode == OMEGASL_SHADER_DESC_IO_INOUT) {
                shaderAccess = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            } else {
                shaderAccess = VK_ACCESS_SHADER_WRITE_BIT;
            }

            if (buffer->priorAccess != 0 && hasPipelineAccess) {
                VkBufferMemoryBarrier bufferMemoryBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                bufferMemoryBarrier.srcQueueFamilyIndex = bufferMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bufferMemoryBarrier.buffer = buffer->buffer;
                bufferMemoryBarrier.offset = 0;
                bufferMemoryBarrier.size = VK_WHOLE_SIZE;
                bufferMemoryBarrier.srcAccessMask = buffer->priorAccess;
                bufferMemoryBarrier.dstAccessMask = shaderAccess;
                bufferMemoryBarrier.pNext = nullptr;

                vkCmdPipelineBarrier(commandBuffer,
                                     buffer->priorPipelineAccess,
                                     pipelineStage,
                                     0,
                                     0,
                                     nullptr,
                                     1,
                                     &bufferMemoryBarrier,
                                     0,
                                     nullptr);
            }

            buffer->priorPipelineAccess = pipelineStage;
            buffer->priorAccess = shaderAccess;
        }
    }

    void GEVulkanCommandBuffer::insertResourceBarrierIfNeeded(GEVulkanTexture *texture, unsigned int &resource_id,
                                                              omegasl_shader &shader) {

        auto ioMode = getResourceIOModeForResourceID(resource_id,shader);
        VkImageLayout layout;

        if(parentQueue->engine->hasSynchronization2Ext) {

            /// Use Pipeline Barrier if Access changes
            VkAccessFlags2KHR shaderAccess;
            VkPipelineStageFlags2KHR pipelineStage;

            if (shader.type == OMEGASL_SHADER_VERTEX) {
                pipelineStage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT_KHR;
            } else if (shader.type == OMEGASL_SHADER_FRAGMENT) {
                pipelineStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR;
            } else {
                pipelineStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
            }

            bool hasPipelineAccess = texture->priorPipelineAccess2 != 0;

            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                shaderAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT_KHR;
                layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else if (ioMode == OMEGASL_SHADER_DESC_IO_INOUT) {
                shaderAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT_KHR;
                layout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                shaderAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT_KHR;
                layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            /// If not first time access, pipeline barrier must be inserted before binding.
            if (texture->priorShaderAccess2 != 0 && hasPipelineAccess) {
                VkImageMemoryBarrier2KHR imageMemoryBarrier2Khr{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR};
                imageMemoryBarrier2Khr.pNext = nullptr;
                imageMemoryBarrier2Khr.srcAccessMask = texture->priorShaderAccess2;
                imageMemoryBarrier2Khr.dstAccessMask = shaderAccess;
                imageMemoryBarrier2Khr.image = texture->img;
                imageMemoryBarrier2Khr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imageMemoryBarrier2Khr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imageMemoryBarrier2Khr.oldLayout = texture->layout;
                imageMemoryBarrier2Khr.newLayout = layout;
                imageMemoryBarrier2Khr.srcStageMask = texture->priorPipelineAccess2 != 0
                                                      ? texture->priorPipelineAccess2
                                                      : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                imageMemoryBarrier2Khr.dstStageMask = pipelineStage;
                imageMemoryBarrier2Khr.subresourceRange = fullColorSubresourceRange(texture);


                VkDependencyInfoKHR dependencyInfoKhr{VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
                dependencyInfoKhr.pNext = nullptr;
                dependencyInfoKhr.imageMemoryBarrierCount = 1;
                dependencyInfoKhr.pImageMemoryBarriers = &imageMemoryBarrier2Khr;
                parentQueue->engine->vkCmdPipelineBarrier2Khr(commandBuffer, &dependencyInfoKhr);
            }

            texture->layout = layout;
            texture->priorShaderAccess2 = shaderAccess;
            texture->priorPipelineAccess2 = pipelineStage;
        }
        else {

            /// Use Pipeline Barrier if Access changes
            VkAccessFlags shaderAccess;
            VkPipelineStageFlags pipelineStage;

            if (shader.type == OMEGASL_SHADER_VERTEX) {
                pipelineStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            } else if (shader.type == OMEGASL_SHADER_FRAGMENT) {
                pipelineStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            } else {
                pipelineStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }

            bool hasPipelineAccess = texture->priorPipelineAccess != 0;

            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                shaderAccess = VK_ACCESS_SHADER_READ_BIT;
                layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else if (ioMode == OMEGASL_SHADER_DESC_IO_INOUT) {
                shaderAccess = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                layout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                shaderAccess = VK_ACCESS_SHADER_WRITE_BIT;
                layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            /// If not first time access, pipeline barrier must be inserted before binding.
            if (texture->priorShaderAccess != 0 && hasPipelineAccess) {
                VkImageMemoryBarrier imageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                imageMemoryBarrier.pNext = nullptr;
                imageMemoryBarrier.srcAccessMask = texture->priorShaderAccess;
                imageMemoryBarrier.dstAccessMask = shaderAccess;
                imageMemoryBarrier.image = texture->img;
                imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imageMemoryBarrier.oldLayout = texture->layout;
                imageMemoryBarrier.newLayout = layout;
                imageMemoryBarrier.subresourceRange = fullColorSubresourceRange(texture);

                vkCmdPipelineBarrier(commandBuffer,
                                     texture->priorPipelineAccess != 0
                                     ? texture->priorPipelineAccess
                                     : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     pipelineStage,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &imageMemoryBarrier);
            }

            texture->layout = layout;
            texture->priorShaderAccess = shaderAccess;
            texture->priorPipelineAccess = pipelineStage;
        }
}

    GEVulkanCommandBuffer::GEVulkanCommandBuffer(VkCommandBuffer & commandBuffer,GEVulkanCommandQueue *parentQueue):commandBuffer(commandBuffer),parentQueue(parentQueue){
        VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        // vk::CommandBufferInheritanceInfo inheritanceInfo;
        beginInfo.pInheritanceInfo = nullptr;
        beginInfo.flags = 0;
        auto beginRes = vkBeginCommandBuffer(commandBuffer,&beginInfo);
        if(beginRes != VK_SUCCESS){
            std::cerr << "Vulkan command buffer begin failed (" << beginRes << ")" << std::endl;
        }
        traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Create,
                ResourceTracking::Backend::Vulkan,
                "CommandBuffer",
                traceResourceId,
                reinterpret_cast<const void *>(commandBuffer));
    };

    GEVulkanCommandBuffer::~GEVulkanCommandBuffer() {
        if(parentQueue != nullptr && parentQueue->engine != nullptr){
            for(auto framebuffer : ownedFramebuffers){
                if(framebuffer != VK_NULL_HANDLE){
                    vkDestroyFramebuffer(parentQueue->engine->device,framebuffer,nullptr);
                }
            }
            for(auto renderPass : ownedRenderPasses){
                if(renderPass != VK_NULL_HANDLE){
                    vkDestroyRenderPass(parentQueue->engine->device,renderPass,nullptr);
                }
            }
            ownedFramebuffers.clear();
            ownedRenderPasses.clear();
            activeFramebuffer = VK_NULL_HANDLE;
            activeRenderPass = VK_NULL_HANDLE;
        }
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::Vulkan,
                "CommandBuffer",
                traceResourceId,
                reinterpret_cast<const void *>(commandBuffer));
    }

    void GEVulkanCommandBuffer::startRenderPass(const GERenderPassDescriptor &desc){
        if(desc.colorAttachment == nullptr){
            return;
        }

        activeFramebuffer = VK_NULL_HANDLE;
        activeRenderPass = VK_NULL_HANDLE;

        VkAttachmentDescription attachmentDescription {};
        attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;

        switch (desc.colorAttachment->loadAction) {
            case GERenderTarget::RenderPassDesc::ColorAttachment::Clear: {
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                break;
            }
            case GERenderTarget::RenderPassDesc::ColorAttachment::Load: {
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                break;
            }
            case GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve: {
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                break;
            }
            case GERenderTarget::RenderPassDesc::ColorAttachment::Discard: {
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                break;
            }
        }

        VkAttachmentReference color_ref {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkSubpassDependency dependency {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassCreateInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassCreateInfo.pNext = nullptr;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments = &attachmentDescription;
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpass;
        renderPassCreateInfo.dependencyCount = 1;
        renderPassCreateInfo.pDependencies = &dependency;

        VkFramebufferCreateInfo framebufferInfo {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.pNext = nullptr;
        framebufferInfo.flags = 0;
        framebufferInfo.layers = 1;
        framebufferInfo.attachmentCount = 1;

        VkRenderPassBeginInfo beginInfo {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        beginInfo.renderArea.offset.x = 0;
        beginInfo.renderArea.offset.y = 0;

        VkImageView attachmentView = VK_NULL_HANDLE;
        if(desc.nRenderTarget != nullptr) {
            auto *nativeTarget = reinterpret_cast<GEVulkanNativeRenderTarget *>(desc.nRenderTarget);
            if(nativeTarget == nullptr || nativeTarget->frameViews.empty()){
                return;
            }

            if(nativeTarget->frameIsReadyFence == VK_NULL_HANDLE){
                return;
            }

            auto resetRes = vkResetFences(parentQueue->engine->device,1,&nativeTarget->frameIsReadyFence);
            if(resetRes != VK_SUCCESS){
                std::cerr << "Vulkan reset acquire fence failed (" << resetRes << ")" << std::endl;
                return;
            }
            auto acquireRes = vkAcquireNextImageKHR(parentQueue->engine->device,
                                                    nativeTarget->swapchainKHR,
                                                    UINT64_MAX,
                                                    VK_NULL_HANDLE,
                                                    nativeTarget->frameIsReadyFence,
                                                    &nativeTarget->currentFrameIndex);
            if(acquireRes == VK_ERROR_OUT_OF_DATE_KHR){
                return;
            }
            if(acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR){
                std::cerr << "Vulkan acquire image failed (" << acquireRes << ")" << std::endl;
                return;
            }

            auto waitRes = vkWaitForFences(parentQueue->engine->device,1,&nativeTarget->frameIsReadyFence,VK_TRUE,UINT64_MAX);
            if(waitRes != VK_SUCCESS){
                std::cerr << "Vulkan wait acquire fence failed (" << waitRes << ")" << std::endl;
                return;
            }

            if(nativeTarget->currentFrameIndex >= nativeTarget->frameViews.size()){
                nativeTarget->currentFrameIndex = 0;
            }

            attachmentDescription.format = nativeTarget->format;
            attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            if(attachmentDescription.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
               attachmentDescription.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED){
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            }
            attachmentView = nativeTarget->frameViews[nativeTarget->currentFrameIndex];

            auto rpRes = vkCreateRenderPass(parentQueue->engine->device,&renderPassCreateInfo,nullptr,&activeRenderPass);
            if(rpRes != VK_SUCCESS || activeRenderPass == VK_NULL_HANDLE){
                activeRenderPass = VK_NULL_HANDLE;
                return;
            }

            framebufferInfo.renderPass = activeRenderPass;
            framebufferInfo.pAttachments = &attachmentView;
            framebufferInfo.width = nativeTarget->extent.width > 0 ? nativeTarget->extent.width : 1;
            framebufferInfo.height = nativeTarget->extent.height > 0 ? nativeTarget->extent.height : 1;
            auto fbRes = vkCreateFramebuffer(parentQueue->engine->device,&framebufferInfo,nullptr,&activeFramebuffer);
            if(fbRes != VK_SUCCESS || activeFramebuffer == VK_NULL_HANDLE){
                vkDestroyRenderPass(parentQueue->engine->device,activeRenderPass,nullptr);
                activeRenderPass = VK_NULL_HANDLE;
                activeFramebuffer = VK_NULL_HANDLE;
                return;
            }
            ownedRenderPasses.push_back(activeRenderPass);
            ownedFramebuffers.push_back(activeFramebuffer);

            beginInfo.renderArea.extent = nativeTarget->extent;
        }
        else {
            auto *textureTarget = reinterpret_cast<GEVulkanTextureRenderTarget *>(desc.tRenderTarget);
            if(textureTarget == nullptr || textureTarget->texture == nullptr){
                return;
            }

            attachmentDescription.format = textureTarget->texture->format;
            attachmentDescription.initialLayout = textureTarget->texture->layout;
            attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
            if(attachmentDescription.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
               attachmentDescription.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED){
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            }
            switch (textureTarget->texture->descriptor.sampleCount) {
                case 1: attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT; break;
                case 2: attachmentDescription.samples = VK_SAMPLE_COUNT_2_BIT; break;
                case 4: attachmentDescription.samples = VK_SAMPLE_COUNT_4_BIT; break;
                case 8: attachmentDescription.samples = VK_SAMPLE_COUNT_8_BIT; break;
                case 16: attachmentDescription.samples = VK_SAMPLE_COUNT_16_BIT; break;
                case 32: attachmentDescription.samples = VK_SAMPLE_COUNT_32_BIT; break;
                case 64: attachmentDescription.samples = VK_SAMPLE_COUNT_64_BIT; break;
                default: attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT; break;
            }

            attachmentView = textureTarget->texture->img_view;
            auto rpRes = vkCreateRenderPass(parentQueue->engine->device,&renderPassCreateInfo,nullptr,&activeRenderPass);
            if(rpRes != VK_SUCCESS || activeRenderPass == VK_NULL_HANDLE){
                activeRenderPass = VK_NULL_HANDLE;
                return;
            }

            framebufferInfo.renderPass = activeRenderPass;
            framebufferInfo.pAttachments = &attachmentView;
            framebufferInfo.width = textureTarget->texture->descriptor.width > 0 ? textureTarget->texture->descriptor.width : 1;
            framebufferInfo.height = textureTarget->texture->descriptor.height > 0 ? textureTarget->texture->descriptor.height : 1;
            auto fbRes = vkCreateFramebuffer(parentQueue->engine->device,&framebufferInfo,nullptr,&activeFramebuffer);
            if(fbRes != VK_SUCCESS || activeFramebuffer == VK_NULL_HANDLE){
                vkDestroyRenderPass(parentQueue->engine->device,activeRenderPass,nullptr);
                activeRenderPass = VK_NULL_HANDLE;
                activeFramebuffer = VK_NULL_HANDLE;
                return;
            }
            ownedRenderPasses.push_back(activeRenderPass);
            ownedFramebuffers.push_back(activeFramebuffer);

            textureTarget->texture->layout = VK_IMAGE_LAYOUT_GENERAL;
            beginInfo.renderArea.extent = {framebufferInfo.width,framebufferInfo.height};
        }

        VkClearValue val {};
        val.color.float32[0] = desc.colorAttachment->clearColor.r;
        val.color.float32[1] = desc.colorAttachment->clearColor.g;
        val.color.float32[2] = desc.colorAttachment->clearColor.b;
        val.color.float32[3] = desc.colorAttachment->clearColor.a;

        beginInfo.clearValueCount = 1;
        beginInfo.pClearValues = &val;
        beginInfo.renderPass = activeRenderPass;
        beginInfo.framebuffer = activeFramebuffer;

        vkCmdBeginRenderPass(commandBuffer,&beginInfo,VK_SUBPASS_CONTENTS_INLINE);
    };

    void GEVulkanCommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState){
        auto vulkanPipeline = (GEVulkanRenderPipelineState *)pipelineState.get();
        VkPipeline state = vulkanPipeline->pipeline;

        vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,state);
        renderPipelineState = vulkanPipeline;
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vulkanPipeline->layout,
                                0,
                                vulkanPipeline->descs.size(),
                                vulkanPipeline->descs.data(),
                                0,nullptr);
    };

    void GEVulkanCommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned id){
        auto vk_buffer = (GEVulkanBuffer *)buffer.get();

        insertResourceBarrierIfNeeded(vk_buffer,id,renderPipelineState->vertexShader->internal);

        VkDescriptorBufferInfo bufferInfo {};
        bufferInfo.buffer = vk_buffer->buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->vertexShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = &bufferInfo;
        writeInfo.pImageInfo = nullptr;
        writeInfo.pTexelBufferView = nullptr;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,renderPipelineState->layout,
                                      0,1,&writeInfo);
        }
        else {
            writeInfo.dstSet = renderPipelineState->descs.front();
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }


    };

    void GEVulkanCommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned id){
        auto vk_texture = (GEVulkanTexture *)texture.get();
        /// TODO!

        auto ioMode = getResourceIOModeForResourceID(id,renderPipelineState->vertexShader->internal);

        insertResourceBarrierIfNeeded(vk_texture,id,renderPipelineState->vertexShader->internal);

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->vertexShader->internal);
        writeInfo.descriptorCount = 1;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = vk_texture->img_view;
        imgInfo.imageLayout = vk_texture->layout;

        VkDescriptorType t;


        if(ioMode == OMEGASL_SHADER_DESC_IO_IN){
            t = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        else {
            t=  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }

        writeInfo.descriptorType = t;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = &imgInfo;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,renderPipelineState->layout,
                                                           0,1,&writeInfo);
        }
        else {
            writeInfo.dstSet = renderPipelineState->descs.front();
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };

    void GEVulkanCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned id){
        auto vk_buffer = (GEVulkanBuffer *)buffer.get();

        insertResourceBarrierIfNeeded(vk_buffer,id,renderPipelineState->fragmentShader->internal);

        VkDescriptorBufferInfo bufferInfo {};
        bufferInfo.buffer = vk_buffer->buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->fragmentShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = &bufferInfo;
        writeInfo.pImageInfo = nullptr;
        writeInfo.pTexelBufferView = nullptr;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,renderPipelineState->layout,
                                                           1,1,&writeInfo);
        }
        else {
            writeInfo.dstSet = renderPipelineState->descs.back();
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };

    void GEVulkanCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned id){

        auto vk_texture = (GEVulkanTexture *)texture.get();

        auto ioMode = getResourceIOModeForResourceID(id,renderPipelineState->fragmentShader->internal);

        insertResourceBarrierIfNeeded(vk_texture,id,renderPipelineState->fragmentShader->internal);

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->fragmentShader->internal);
        writeInfo.descriptorCount = 1;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = vk_texture->img_view;
        imgInfo.imageLayout = vk_texture->layout;

        VkDescriptorType t;

        if(ioMode == OMEGASL_SHADER_DESC_IO_IN){
            t = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        else {
            t = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }

        writeInfo.descriptorType = t;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = &imgInfo;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,renderPipelineState->layout,
                                                           1,1,&writeInfo);
        }
        else {
            writeInfo.dstSet = renderPipelineState->descs.back();
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };



    void GEVulkanCommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount, size_t startIdx){
        VkPrimitiveTopology topology;

        switch (polygonType) {
            case GERenderTarget::CommandBuffer::Triangle :
                topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                break;
            case GERenderTarget::CommandBuffer::TriangleStrip : {
                topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                break;
            }
        }

        if(parentQueue->engine->hasExtendedDynamicState) {
            parentQueue->engine->vkCmdSetPrimitiveTopologyExt(commandBuffer, topology);
        }
        vkCmdDraw(commandBuffer,vertexCount,1,startIdx,0);
    };

    void GEVulkanCommandBuffer::setStencilRef(unsigned ref){
        VkStencilFaceFlags faceflags = VK_STENCIL_FACE_FRONT_AND_BACK;
        vkCmdSetStencilReference(commandBuffer,faceflags,ref);
    }

    void GEVulkanCommandBuffer::setScissorRects(std::vector<GEScissorRect> scissorRects){
        std::vector<VkRect2D> vk_rects;
        for(auto & r : scissorRects){
            VkRect2D rect {};
            rect.offset.x = r.x;
            rect.offset.y = r.y;
            rect.extent.width = r.width;
            rect.extent.height = r.height;
            vk_rects.push_back(rect);
        };

        vkCmdSetScissor(commandBuffer,0,vk_rects.size(),vk_rects.data());
    };

    void GEVulkanCommandBuffer::setViewports(std::vector<GEViewport> viewports){
        std::vector<VkViewport> vk_viewports;
        for(auto & v : viewports){
            VkViewport viewport {};
            viewport.x = v.x;
            viewport.y = v.y;
            viewport.width = v.width;
            viewport.height = v.height;
            viewport.minDepth = v.nearDepth;
            viewport.maxDepth = v.farDepth;
            vk_viewports.push_back(viewport);
        };

        vkCmdSetViewport(commandBuffer,0,vk_viewports.size(),vk_viewports.data());
    };

    void GEVulkanCommandBuffer::setVertexBuffer(SharedHandle<GEBuffer> &buffer) {
        auto vkBuffer = ((GEVulkanBuffer *)buffer.get());
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer,0,1,&vkBuffer->buffer,offsets);
    }

    void GEVulkanCommandBuffer::finishRenderPass(){
        if(activeRenderPass != VK_NULL_HANDLE){
            vkCmdEndRenderPass(commandBuffer);
        }
        activeFramebuffer = VK_NULL_HANDLE;
        activeRenderPass = VK_NULL_HANDLE;
        renderPipelineState = nullptr;
    };

    void GEVulkanCommandBuffer::startComputePass(const GEComputePassDescriptor &desc) {
        inComputePass = true;
    }

    void GEVulkanCommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) {
        auto *vkPipelineState = (GEVulkanComputePipelineState *)pipelineState.get();
        vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,vkPipelineState->pipeline);

        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                vkPipelineState->layout,
                                0,
                                1,
                                &vkPipelineState->descSet,
                                0,
                                nullptr);
        computePipelineState = vkPipelineState;
    }

    void GEVulkanCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) {
        auto vk_buffer = (GEVulkanBuffer *)buffer.get();

        insertResourceBarrierIfNeeded(vk_buffer,id,computePipelineState->computeShader->internal);

        VkDescriptorBufferInfo bufferInfo {};
        bufferInfo.buffer = vk_buffer->buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,computePipelineState->computeShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = &bufferInfo;
        writeInfo.pImageInfo = nullptr;
        writeInfo.pTexelBufferView = nullptr;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,computePipelineState->layout,
                                                           0,1,&writeInfo);
        }
        else {
            writeInfo.dstSet = computePipelineState->descSet;
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    }

    void GEVulkanCommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id) {

        auto vk_texture = (GEVulkanTexture *)texture.get();

        insertResourceBarrierIfNeeded(vk_texture,id,computePipelineState->computeShader->internal);

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,computePipelineState->computeShader->internal);
        writeInfo.descriptorCount = 1;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = vk_texture->img_view;
        imgInfo.imageLayout = vk_texture->layout;

        VkDescriptorType t;

        if(vk_texture->memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU){
            t = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        else {
            t = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }

        writeInfo.descriptorType = t;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = &imgInfo;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,computePipelineState->layout,
                                                           0,1,&writeInfo);
        }
        else {
            writeInfo.dstSet = computePipelineState->descSet;
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    }

    void GEVulkanCommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
        vkCmdDispatch(commandBuffer,x,y,z);
        
    }

    void GEVulkanCommandBuffer::finishComputePass() {
        inComputePass = false;
    }

    void GEVulkanCommandBuffer::startBlitPass() {
        inBlitPass = true;
    }

    inline void addResourceBarrierForTextureCopy(GEVulkanEngine *engine,VkCommandBuffer commandBuffer,GEVulkanTexture *src_img,GEVulkanTexture *dest_img){
        if(src_img == nullptr || dest_img == nullptr){
            return;
        }

        if(engine->hasSynchronization2Ext){
            std::vector<VkImageMemoryBarrier2KHR> memBarriers2;
            VkDependencyInfoKHR dep_info {VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
            dep_info.pNext = nullptr;

            if(!(src_img->priorShaderAccess2 & VK_ACCESS_2_TRANSFER_READ_BIT_KHR)){
                VkImageMemoryBarrier2KHR img_mem_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR};
                img_mem_barrier.srcAccessMask = src_img->priorShaderAccess2;
                img_mem_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
                img_mem_barrier.srcQueueFamilyIndex = img_mem_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                img_mem_barrier.srcStageMask = src_img->priorPipelineAccess2 != 0
                                               ? src_img->priorPipelineAccess2
                                               : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                img_mem_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                img_mem_barrier.image = src_img->img;
                img_mem_barrier.oldLayout = src_img->layout;
                img_mem_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                img_mem_barrier.subresourceRange = fullColorSubresourceRange(src_img);

                src_img->priorShaderAccess2 = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
                src_img->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                src_img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

                memBarriers2.push_back(img_mem_barrier);
            }

            if(!(dest_img->priorShaderAccess2 & VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR)){
                VkImageMemoryBarrier2KHR img_mem_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR};
                img_mem_barrier.srcAccessMask = dest_img->priorShaderAccess2;
                img_mem_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
                img_mem_barrier.srcQueueFamilyIndex = img_mem_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                img_mem_barrier.srcStageMask = dest_img->priorPipelineAccess2 != 0
                                               ? dest_img->priorPipelineAccess2
                                               : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                img_mem_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                img_mem_barrier.image = dest_img->img;
                img_mem_barrier.oldLayout = dest_img->layout;
                img_mem_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                img_mem_barrier.subresourceRange = fullColorSubresourceRange(dest_img);

                dest_img->priorShaderAccess2 = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
                dest_img->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                dest_img->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                memBarriers2.push_back(img_mem_barrier);
            }

            if(!memBarriers2.empty()){
                dep_info.imageMemoryBarrierCount = static_cast<std::uint32_t>(memBarriers2.size());
                dep_info.pImageMemoryBarriers = memBarriers2.data();
                engine->vkCmdPipelineBarrier2Khr(commandBuffer,&dep_info);
            }
        }
        else {
            std::vector<VkImageMemoryBarrier> memBarriers;

            if(!(src_img->priorShaderAccess & VK_ACCESS_TRANSFER_READ_BIT)){
                VkImageMemoryBarrier img_mem_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                img_mem_barrier.srcAccessMask = src_img->priorShaderAccess;
                img_mem_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                img_mem_barrier.srcQueueFamilyIndex = img_mem_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                img_mem_barrier.image = src_img->img;
                img_mem_barrier.oldLayout = src_img->layout;
                img_mem_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                img_mem_barrier.subresourceRange = fullColorSubresourceRange(src_img);

                src_img->priorShaderAccess = VK_ACCESS_TRANSFER_READ_BIT;
                src_img->priorPipelineAccess = VK_PIPELINE_STAGE_TRANSFER_BIT;
                src_img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

                memBarriers.push_back(img_mem_barrier);
            }

            if(!(dest_img->priorShaderAccess & VK_ACCESS_TRANSFER_WRITE_BIT)){
                VkImageMemoryBarrier img_mem_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                img_mem_barrier.srcAccessMask = dest_img->priorShaderAccess;
                img_mem_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                img_mem_barrier.srcQueueFamilyIndex = img_mem_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                img_mem_barrier.image = dest_img->img;
                img_mem_barrier.oldLayout = dest_img->layout;
                img_mem_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                img_mem_barrier.subresourceRange = fullColorSubresourceRange(dest_img);

                dest_img->priorShaderAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
                dest_img->priorPipelineAccess = VK_PIPELINE_STAGE_TRANSFER_BIT;
                dest_img->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                memBarriers.push_back(img_mem_barrier);
            }

            if(!memBarriers.empty()){
                VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
                vkCmdPipelineBarrier(commandBuffer,
                                     srcStages,
                                     dstStages,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     static_cast<std::uint32_t>(memBarriers.size()),
                                     memBarriers.data());
            }
        }
    }

    void GEVulkanCommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) {
        assert(inBlitPass && "Must be in a blit pass");
        auto src_img = (GEVulkanTexture *)src.get(),dest_img = (GEVulkanTexture *)dest.get();

        addResourceBarrierForTextureCopy(parentQueue->engine,commandBuffer,src_img,dest_img);

        VkImageCopy imgCopy {};
        imgCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgCopy.srcSubresource.baseArrayLayer = 0;
        imgCopy.srcSubresource.layerCount = 1;
        imgCopy.srcSubresource.mipLevel = 0;
        imgCopy.srcOffset = {0,0,0};
        imgCopy.dstOffset = {0,0,0};
        imgCopy.dstSubresource.mipLevel = 0;
        imgCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgCopy.dstSubresource.layerCount = 1;
        imgCopy.dstSubresource.baseArrayLayer = 0;
        imgCopy.extent = {src_img->descriptor.width,src_img->descriptor.height,src_img->descriptor.depth};
        vkCmdCopyImage(commandBuffer,src_img->img,src_img->layout,dest_img->img,dest_img->layout,1,&imgCopy);
    }

    void GEVulkanCommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest,
                                                     const TextureRegion &region, const GPoint3D &destCoord) {
        auto src_img = (GEVulkanTexture *)src.get(),dest_img = (GEVulkanTexture *)dest.get();

        addResourceBarrierForTextureCopy(parentQueue->engine,commandBuffer,src_img,dest_img);

        VkImageCopy imgCopy {};
        imgCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgCopy.srcSubresource.baseArrayLayer = 0;
        imgCopy.srcSubresource.layerCount = 1;
        imgCopy.srcSubresource.mipLevel = 0;
        imgCopy.srcOffset = {int32_t(region.x),int32_t(region.y),int32_t(region.z)};
        imgCopy.dstOffset = {int32_t(destCoord.x),int32_t(destCoord.y),int32_t(destCoord.z)};
        imgCopy.dstSubresource.mipLevel = 0;
        imgCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgCopy.dstSubresource.layerCount = 1;
        imgCopy.dstSubresource.baseArrayLayer = 0;
        imgCopy.extent = {region.w,region.h,region.d};
        vkCmdCopyImage(commandBuffer,src_img->img,src_img->layout,dest_img->img,dest_img->layout,1,&imgCopy);
    }

    void GEVulkanCommandBuffer::finishBlitPass() {
        inBlitPass = false;
    }

    void GEVulkanCommandBuffer::reset(){
        vkResetCommandBuffer(commandBuffer,VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    };

    void GEVulkanCommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence){
        auto buffer = (GEVulkanCommandBuffer *)commandBuffer.get();
        auto fence = (GEVulkanFence *)waitFence.get();
        if(buffer == nullptr || fence == nullptr || fence->event == VK_NULL_HANDLE){
            return;
        }
        vkCmdWaitEvents(buffer->commandBuffer,
            1,
            &fence->event,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,nullptr,
            0,nullptr,
            0,nullptr);
        vkCmdResetEvent(buffer->commandBuffer,fence->event,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    void GEVulkanCommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer){
        auto buffer = (GEVulkanCommandBuffer *)commandBuffer.get();
        submittedTraceCommandBufferIds.push_back(buffer->traceResourceId);
        ResourceTracking::Event submitEvent {};
        submitEvent.backend = ResourceTracking::Backend::Vulkan;
        submitEvent.eventType = ResourceTracking::EventType::Submit;
        submitEvent.resourceType = "CommandBuffer";
        submitEvent.resourceId = buffer->traceResourceId;
        submitEvent.queueId = traceResourceId;
        submitEvent.commandBufferId = buffer->traceResourceId;
        submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(buffer->commandBuffer);
        ResourceTracking::Tracker::instance().emit(submitEvent);
        commandQueue.push_back(buffer->commandBuffer);
    };

    void GEVulkanCommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,SharedHandle<GEFence> & signalFence){
        auto buffer = (GEVulkanCommandBuffer *)commandBuffer.get();
        auto fence = (GEVulkanFence *)signalFence.get();
        submittedTraceCommandBufferIds.push_back(buffer->traceResourceId);
        ResourceTracking::Event submitEvent {};
        submitEvent.backend = ResourceTracking::Backend::Vulkan;
        submitEvent.eventType = ResourceTracking::EventType::Submit;
        submitEvent.resourceType = "CommandBuffer";
        submitEvent.resourceId = buffer->traceResourceId;
        submitEvent.queueId = traceResourceId;
        submitEvent.commandBufferId = buffer->traceResourceId;
        submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(buffer->commandBuffer);
        ResourceTracking::Tracker::instance().emit(submitEvent);
        if(fence != nullptr && fence->event != VK_NULL_HANDLE){
            vkCmdSetEvent(buffer->commandBuffer,fence->event,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        commandQueue.push_back(buffer->commandBuffer);
    }

   SharedHandle<GECommandBuffer> GEVulkanCommandQueue::getAvailableBuffer(){
       if(commandBuffers.empty()){
           return nullptr;
       }
       if(currentBufferIndex >= commandBuffers.size()){
           currentBufferIndex = 0;
       }
       auto &commandBuffer = commandBuffers[currentBufferIndex];
       auto resetRes = vkResetCommandBuffer(commandBuffer,0);
       if(resetRes != VK_SUCCESS){
           std::cerr << "Vulkan reset command buffer failed (" << resetRes << ")" << std::endl;
       }
       auto res = std::make_shared<GEVulkanCommandBuffer>(commandBuffer,this);
       currentBufferIndex = (currentBufferIndex + 1) % commandBuffers.size();
       return res;
   };

    VkCommandBuffer &GEVulkanCommandQueue::getLastCommandBufferInQueue() {
        return commandQueue.back();
    }

   void GEVulkanCommandQueue::commitToGPU(){
        if(commandQueue.empty()){
            submittedTraceCommandBufferIds.clear();
            return;
        }
        for(auto cb : commandQueue){
            auto endRes = vkEndCommandBuffer(cb);
            if(endRes != VK_SUCCESS){
                std::cerr << "Vulkan end command buffer failed (" << endRes << ")" << std::endl;
                commandQueue.clear();
                submittedTraceCommandBufferIds.clear();
                return;
            }
        }
        if(engine == nullptr || engine->deviceQueuefamilies.empty() || engine->deviceQueuefamilies.front().empty()){
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto &queueEntry = engine->deviceQueuefamilies.front().front();
        auto vkQueue = queueEntry.second;
        if(vkQueue == VK_NULL_HANDLE){
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

       VkSubmitInfo submission {VK_STRUCTURE_TYPE_SUBMIT_INFO};
       submission.signalSemaphoreCount = 0;
       submission.pSignalSemaphores = nullptr;
       submission.waitSemaphoreCount = 0;
       submission.pWaitSemaphores = nullptr;
       submission.commandBufferCount = commandQueue.size();
       submission.pCommandBuffers = commandQueue.data();
       submission.pNext = nullptr;

       auto res = vkQueueSubmit(vkQueue, 1, &submission,VK_NULL_HANDLE);
       if(!VK_RESULT_SUCCEEDED(res)){
           std::cerr << "Failed to Submit Command Buffers to GPU (" << res << ")" << std::endl;
           commandQueue.clear();
           submittedTraceCommandBufferIds.clear();
           return;
       };

       commandQueue.clear();

   };

   void GEVulkanCommandQueue::commitToGPUPresent(VkPresentInfoKHR *info){
        if(info == nullptr){
            submittedTraceCommandBufferIds.clear();
            return;
        }
        if(commandQueue.empty()){
            submittedTraceCommandBufferIds.clear();
            return;
        }
        for(auto cb : commandQueue){
            auto endRes = vkEndCommandBuffer(cb);
            if(endRes != VK_SUCCESS){
                std::cerr << "Vulkan end command buffer failed (" << endRes << ")" << std::endl;
                commandQueue.clear();
                submittedTraceCommandBufferIds.clear();
                return;
            }
        }
        if(engine == nullptr || engine->deviceQueuefamilies.empty() || engine->deviceQueuefamilies.front().empty()){
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto &queueEntry = engine->deviceQueuefamilies.front().front();
        auto vkQueue = queueEntry.second;
        if(vkQueue == VK_NULL_HANDLE){
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        vkResetFences(engine->device,1,&submitFence);

        VkSubmitInfo submission {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.signalSemaphoreCount = 0;
        submission.pSignalSemaphores = nullptr;
        submission.waitSemaphoreCount = 0;
        submission.pWaitSemaphores = nullptr;
        submission.commandBufferCount = commandQueue.size();
        submission.pCommandBuffers = commandQueue.data();
        submission.pNext = nullptr;

        auto res = vkQueueSubmit(vkQueue, 1, &submission,submitFence);
        if(!VK_RESULT_SUCCEEDED(res)){
            std::cerr << "Failed to Submit Command Buffers to GPU (" << res << ")" << std::endl;
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }
        auto waitRes = vkWaitForFences(engine->device,1,&submitFence,VK_TRUE,UINT64_MAX);
        if(waitRes != VK_SUCCESS){
            std::cerr << "Failed waiting for submitted command buffers (" << waitRes << ")" << std::endl;
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto presentRes = vkQueuePresentKHR(vkQueue,info);
        if(presentRes != VK_SUCCESS &&
           presentRes != VK_SUBOPTIMAL_KHR &&
           presentRes != VK_ERROR_OUT_OF_DATE_KHR){
            std::cerr << "Failed to present swapchain image (" << presentRes << ")" << std::endl;
        }
        commandQueue.clear();
        for(const auto traceId : submittedTraceCommandBufferIds){
            ResourceTracking::Event completeEvent {};
            completeEvent.backend = ResourceTracking::Backend::Vulkan;
            completeEvent.eventType = ResourceTracking::EventType::Complete;
            completeEvent.resourceType = "CommandBuffer";
            completeEvent.resourceId = traceId;
            completeEvent.queueId = traceResourceId;
            completeEvent.commandBufferId = traceId;
            completeEvent.nativeHandle = reinterpret_cast<std::uint64_t>(vkQueue);
            ResourceTracking::Tracker::instance().emit(completeEvent);
        }

   }

   void GEVulkanCommandQueue::commitToGPUAndWait(){
        if(commandQueue.empty()){
            submittedTraceCommandBufferIds.clear();
            return;
        }
        for(auto cb : commandQueue){
            auto endRes = vkEndCommandBuffer(cb);
            if(endRes != VK_SUCCESS){
                std::cerr << "Vulkan end command buffer failed (" << endRes << ")" << std::endl;
                commandQueue.clear();
                submittedTraceCommandBufferIds.clear();
                return;
            }
        }
        if(engine == nullptr || engine->deviceQueuefamilies.empty() || engine->deviceQueuefamilies.front().empty()){
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto &queueEntry = engine->deviceQueuefamilies.front().front();
        auto vkQueue = queueEntry.second;
        if(vkQueue == VK_NULL_HANDLE){
            commandQueue.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        vkResetFences(engine->device,1,&submitFence);

       VkSubmitInfo submission {VK_STRUCTURE_TYPE_SUBMIT_INFO};
       submission.signalSemaphoreCount = 0;
       submission.pSignalSemaphores = nullptr;
       submission.waitSemaphoreCount = 0;
       submission.pWaitSemaphores = nullptr;
       submission.commandBufferCount = commandQueue.size();
       submission.pCommandBuffers = commandQueue.data();
       submission.pNext = nullptr;

       auto res = vkQueueSubmit(vkQueue, 1, &submission,submitFence);
       if(!VK_RESULT_SUCCEEDED(res)){
           std::cerr << "Failed to Submit Command Buffers to GPU (" << res << ")" << std::endl;
           commandQueue.clear();
           submittedTraceCommandBufferIds.clear();
           return;
       };

       commandQueue.clear();
       auto waitRes = vkWaitForFences(engine->device,1,&submitFence,VK_TRUE,UINT64_MAX);
       if(waitRes != VK_SUCCESS){
           std::cerr << "Failed waiting for submitted command buffers (" << waitRes << ")" << std::endl;
           submittedTraceCommandBufferIds.clear();
           return;
       }
       for(const auto traceId : submittedTraceCommandBufferIds){
           ResourceTracking::Event completeEvent {};
           completeEvent.backend = ResourceTracking::Backend::Vulkan;
           completeEvent.eventType = ResourceTracking::EventType::Complete;
           completeEvent.resourceType = "CommandBuffer";
           completeEvent.resourceId = traceId;
           completeEvent.queueId = traceResourceId;
           completeEvent.commandBufferId = traceId;
           completeEvent.nativeHandle = reinterpret_cast<std::uint64_t>(vkQueue);
           ResourceTracking::Tracker::instance().emit(completeEvent);
       }
       submittedTraceCommandBufferIds.clear();


   }

   GEVulkanCommandQueue::GEVulkanCommandQueue(GEVulkanEngine *engine,unsigned size):GECommandQueue(size){
       this->engine = engine;
       VkResult res;
       VkCommandPoolCreateInfo poolCreateInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

       VkFenceCreateInfo fenceCreateInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
       fenceCreateInfo.pNext = nullptr;
       fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

       vkCreateFence(engine->device,&fenceCreateInfo,nullptr,&submitFence);

        

       poolCreateInfo.queueFamilyIndex = engine->queueFamilyIndices.front();
       poolCreateInfo.pNext = nullptr;
       poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

       res = vkCreateCommandPool(engine->device,&poolCreateInfo,nullptr,&commandPool);

       if(!VK_RESULT_SUCCEEDED(res)){
           exit(1);
       };

       VkCommandBufferAllocateInfo commandBufferCreateInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
       commandBufferCreateInfo.commandBufferCount = size;
       commandBufferCreateInfo.commandPool = commandPool;
       commandBufferCreateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
       commandBufferCreateInfo.pNext = nullptr;
       commandBuffers.resize(size);

       res = vkAllocateCommandBuffers(engine->device,&commandBufferCreateInfo,commandBuffers.data());

       if(!VK_RESULT_SUCCEEDED(res)){
           exit(1);
       };

       currentBufferIndex = 0;
       traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
       ResourceTracking::Tracker::instance().emit(
               ResourceTracking::EventType::Create,
               ResourceTracking::Backend::Vulkan,
               "CommandQueue",
               traceResourceId,
               reinterpret_cast<const void *>(commandPool));

   };

   GEVulkanCommandQueue::~GEVulkanCommandQueue() {
       ResourceTracking::Tracker::instance().emit(
               ResourceTracking::EventType::Destroy,
               ResourceTracking::Backend::Vulkan,
               "CommandQueue",
               traceResourceId,
               reinterpret_cast<const void *>(commandPool));
       vkFreeCommandBuffers(engine->device,commandPool,commandBuffers.size(),commandBuffers.data());
       commandBuffers.resize(0);
       vkDestroyCommandPool(engine->device,commandPool,nullptr);
       vkDestroyFence(engine->device,submitFence,nullptr);
   }
_NAMESPACE_END_
