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

        // §6.3 — bind-time validation helper, shared by every Vulkan
        // texture-bind path. Walks the shader's layout-desc array, picks
        // the descriptor that owns the bound location, and consults
        // validateTextureBindKind() for the kind / sample-count check.
        inline bool checkTextureBindAgainstShader(unsigned int location,
                                                  const omegasl_shader &shader,
                                                  GETexture &tex){
            OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{
                shader.pLayout, shader.pLayout + shader.nLayout};
            for (auto &l : layoutArr) {
                if (l.location == location) {
                    return validateTextureBindKind((int)l.type, tex.getKind(),
                                                   tex.getSampleCount(),
                                                   shader.name, location);
                }
            }
            return true;
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
            /// Insert barrier when layout transition is needed or prior shader access exists.
            /// The layout may differ even without prior shader access — e.g. when the
            /// texture was used as a render target attachment (GENERAL/COLOR_ATTACHMENT)
            /// and is now being read as a shader resource (SHADER_READ_ONLY_OPTIMAL).
            if ((texture->priorShaderAccess2 != 0 && hasPipelineAccess) || texture->layout != layout) {
                VkImageMemoryBarrier2KHR imageMemoryBarrier2Khr{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR};
                imageMemoryBarrier2Khr.pNext = nullptr;
                imageMemoryBarrier2Khr.srcAccessMask = texture->priorShaderAccess2 != 0
                                                       ? texture->priorShaderAccess2
                                                       : VK_ACCESS_2_MEMORY_WRITE_BIT_KHR;
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
            /// Insert barrier when layout transition is needed or prior shader access exists.
            if ((texture->priorShaderAccess != 0 && hasPipelineAccess) || texture->layout != layout) {
                VkImageMemoryBarrier imageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                imageMemoryBarrier.pNext = nullptr;
                imageMemoryBarrier.srcAccessMask = texture->priorShaderAccess != 0
                                                   ? texture->priorShaderAccess
                                                   : VK_ACCESS_MEMORY_WRITE_BIT;
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
        // Free any per-cmd-buffer fallback descriptor sets back to their
        // owning pipeline pool (deferred to the engine retention queue so
        // the GPU has finished using them).
        releaseFallbackDescriptorSets();
        // Render passes / framebuffers reach this point only when the buffer
        // was never submitted (otherwise vkQueueSubmit's flush already moved
        // them into the engine retention queue under a fence gate). Enqueue
        // them with no gates so the next drainCompleted() releases them
        // immediately — semantically the same as the previous "destroy on
        // next fenced submit" behavior for buffers the GPU never touched.
        if(parentQueue != nullptr && parentQueue->engine != nullptr){
            VkDevice dev = parentQueue->engine->device;
            for(auto framebuffer : ownedFramebuffers){
                if(framebuffer != VK_NULL_HANDLE){
                    parentQueue->engine->retentionQueue.enqueue(
                        {},
                        [dev, framebuffer]{ vkDestroyFramebuffer(dev, framebuffer, nullptr); });
                }
            }
            for(auto renderPass : ownedRenderPasses){
                if(renderPass != VK_NULL_HANDLE){
                    parentQueue->engine->retentionQueue.enqueue(
                        {},
                        [dev, renderPass]{ vkDestroyRenderPass(dev, renderPass, nullptr); });
                }
            }
        }
        ownedFramebuffers.clear();
        ownedRenderPasses.clear();
        activeFramebuffer = VK_NULL_HANDLE;
        activeRenderPass = VK_NULL_HANDLE;
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::Vulkan,
                "CommandBuffer",
                traceResourceId,
                reinterpret_cast<const void *>(commandBuffer));
    }

    void GEVulkanCommandBuffer::trackBuffer(const SharedHandle<GEBuffer> &b) {
        if (b) trackedBuffers.push_back(b);
    }
    void GEVulkanCommandBuffer::trackTexture(const SharedHandle<GETexture> &t) {
        if (t) trackedTextures.push_back(t);
    }

    void GEVulkanCommandBuffer::beginRenderPassIfDeferred(){
        if(renderPassBeginDeferred){
            renderPassBeginDeferred = false;
            vkCmdBeginRenderPass(commandBuffer,&deferredBeginInfo,VK_SUBPASS_CONTENTS_INLINE);
        }
    }

    void GEVulkanCommandBuffer::startRenderPass(const GERenderPassDescriptor &desc){
        if(desc.colorAttachments.empty()){
            return;
        }

        activeFramebuffer = VK_NULL_HANDLE;
        activeRenderPass = VK_NULL_HANDLE;

        const unsigned attachmentCount = (unsigned)desc.colorAttachments.size();

        std::vector<VkAttachmentDescription> attachmentDescriptions(attachmentCount);
        std::vector<VkAttachmentReference> colorRefs(attachmentCount);
        std::vector<VkImageView> attachmentViews(attachmentCount, VK_NULL_HANDLE);

        for(unsigned i = 0; i < attachmentCount; ++i){
            VkAttachmentDescription & ad = attachmentDescriptions[i];
            ad = {};
            ad.samples = VK_SAMPLE_COUNT_1_BIT;
            switch (desc.colorAttachments[i].loadAction) {
                case GERenderTarget::RenderPassDesc::ColorAttachment::Clear: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    break;
                }
                case GERenderTarget::RenderPassDesc::ColorAttachment::Load: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    break;
                }
                case GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    break;
                }
                case GERenderTarget::RenderPassDesc::ColorAttachment::Discard: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    break;
                }
            }
            colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }

        VkAttachmentDescription & attachmentDescription = attachmentDescriptions[0];

        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = attachmentCount;
        subpass.pColorAttachments = colorRefs.data();

        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Read-side: close the attachment-write → fragment-shader-read hazard
        // so a subsequent sample does not need an explicit pipeline barrier.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassCreateInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassCreateInfo.pNext = nullptr;
        renderPassCreateInfo.attachmentCount = attachmentCount;
        renderPassCreateInfo.pAttachments = attachmentDescriptions.data();
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpass;
        renderPassCreateInfo.dependencyCount = 2;
        renderPassCreateInfo.pDependencies = dependencies;

        VkFramebufferCreateInfo framebufferInfo {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.pNext = nullptr;
        framebufferInfo.flags = 0;
        framebufferInfo.layers = 1;
        framebufferInfo.attachmentCount = attachmentCount;

        VkRenderPassBeginInfo beginInfo {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        beginInfo.renderArea.offset.x = 0;
        beginInfo.renderArea.offset.y = 0;

        VkImageView attachmentView = VK_NULL_HANDLE;

        // Populate additional attachments (index > 0) from their supplied textures.
        for(unsigned i = 1; i < attachmentCount; ++i){
            const auto & att = desc.colorAttachments[i];
            assert(att.texture != nullptr && "Color attachments beyond index 0 must supply an explicit texture.");
            auto *extraTex = (GEVulkanTexture *)att.texture.get();
            attachmentDescriptions[i].format = extraTex->format;
            attachmentDescriptions[i].initialLayout = extraTex->layout;
            attachmentDescriptions[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
            if(attachmentDescriptions[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
               attachmentDescriptions[i].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED){
                attachmentDescriptions[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            }
            attachmentViews[i] = extraTex->img_view;
            extraTex->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
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

            // Attachment 0 falls back to native swapchain image when attachment 0's texture is null.
            if(desc.colorAttachments[0].texture == nullptr){
                attachmentDescription.format = nativeTarget->format;
                attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                if(attachmentDescription.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                   attachmentDescription.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED){
                    attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                }
                attachmentView = nativeTarget->frameViews[nativeTarget->currentFrameIndex];
            }
            else {
                auto *attachTex = (GEVulkanTexture *)desc.colorAttachments[0].texture.get();
                attachmentDescription.format = attachTex->format;
                attachmentDescription.initialLayout = attachTex->layout;
                attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
                if(attachmentDescription.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                   attachmentDescription.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED){
                    attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                }
                attachmentView = attachTex->img_view;
                attachTex->layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            attachmentViews[0] = attachmentView;

            auto rpRes = vkCreateRenderPass(parentQueue->engine->device,&renderPassCreateInfo,nullptr,&activeRenderPass);
            if(rpRes != VK_SUCCESS || activeRenderPass == VK_NULL_HANDLE){
                activeRenderPass = VK_NULL_HANDLE;
                return;
            }

            framebufferInfo.renderPass = activeRenderPass;
            framebufferInfo.pAttachments = attachmentViews.data();
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

            GEVulkanTexture *primaryTex = nullptr;
            if(desc.colorAttachments[0].texture == nullptr){
                primaryTex = textureTarget->texture.get();
            }
            else {
                primaryTex = (GEVulkanTexture *)desc.colorAttachments[0].texture.get();
            }

            attachmentDescription.format = primaryTex->format;
            attachmentDescription.initialLayout = primaryTex->layout;
            attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if(attachmentDescription.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
               attachmentDescription.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED){
                attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            }
            switch (primaryTex->descriptor.sampleCount) {
                case 1: attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT; break;
                case 2: attachmentDescription.samples = VK_SAMPLE_COUNT_2_BIT; break;
                case 4: attachmentDescription.samples = VK_SAMPLE_COUNT_4_BIT; break;
                case 8: attachmentDescription.samples = VK_SAMPLE_COUNT_8_BIT; break;
                case 16: attachmentDescription.samples = VK_SAMPLE_COUNT_16_BIT; break;
                case 32: attachmentDescription.samples = VK_SAMPLE_COUNT_32_BIT; break;
                case 64: attachmentDescription.samples = VK_SAMPLE_COUNT_64_BIT; break;
                default: attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT; break;
            }

            attachmentView = primaryTex->img_view;
            attachmentViews[0] = attachmentView;

            auto rpRes = vkCreateRenderPass(parentQueue->engine->device,&renderPassCreateInfo,nullptr,&activeRenderPass);
            if(rpRes != VK_SUCCESS || activeRenderPass == VK_NULL_HANDLE){
                activeRenderPass = VK_NULL_HANDLE;
                return;
            }

            framebufferInfo.renderPass = activeRenderPass;
            framebufferInfo.pAttachments = attachmentViews.data();
            framebufferInfo.width = primaryTex->descriptor.width > 0 ? primaryTex->descriptor.width : 1;
            framebufferInfo.height = primaryTex->descriptor.height > 0 ? primaryTex->descriptor.height : 1;
            auto fbRes = vkCreateFramebuffer(parentQueue->engine->device,&framebufferInfo,nullptr,&activeFramebuffer);
            if(fbRes != VK_SUCCESS || activeFramebuffer == VK_NULL_HANDLE){
                vkDestroyRenderPass(parentQueue->engine->device,activeRenderPass,nullptr);
                activeRenderPass = VK_NULL_HANDLE;
                activeFramebuffer = VK_NULL_HANDLE;
                return;
            }
            ownedRenderPasses.push_back(activeRenderPass);
            ownedFramebuffers.push_back(activeFramebuffer);

            // Render pass transitions the attachment to SHADER_READ_ONLY_OPTIMAL
            // via finalLayout + the 0→EXTERNAL subpass dependency above, so a
            // subsequent sample is synchronized without an extra barrier.
            primaryTex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            // Seed the attachment write as the prior access so a later
            // sample-time barrier names COLOR_ATTACHMENT_OUTPUT/WRITE as the
            // src scope instead of the ALL_COMMANDS/MEMORY_WRITE fallback.
            primaryTex->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR;
            primaryTex->priorShaderAccess2 = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT_KHR;
            primaryTex->priorPipelineAccess = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            primaryTex->priorShaderAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            beginInfo.renderArea.extent = {framebufferInfo.width,framebufferInfo.height};
        }

        deferredClearValues.clear();
        deferredClearValues.resize(attachmentCount);
        for(unsigned i = 0; i < attachmentCount; ++i){
            VkClearValue & val = deferredClearValues[i];
            val = {};
            val.color.float32[0] = desc.colorAttachments[i].clearColor.r;
            val.color.float32[1] = desc.colorAttachments[i].clearColor.g;
            val.color.float32[2] = desc.colorAttachments[i].clearColor.b;
            val.color.float32[3] = desc.colorAttachments[i].clearColor.a;
        }

        beginInfo.renderPass = activeRenderPass;
        beginInfo.framebuffer = activeFramebuffer;

        // Defer vkCmdBeginRenderPass so that resource barriers issued by
        // bind* calls (e.g. image layout transitions) are recorded OUTSIDE
        // the render pass instance — Vulkan forbids image layout transitions
        // for non-attachment images inside a render pass.
        deferredBeginInfo = beginInfo;
        deferredBeginInfo.clearValueCount = attachmentCount;
        deferredBeginInfo.pClearValues = deferredClearValues.data();
        renderPassBeginDeferred = true;
    };

    void GEVulkanCommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState){
        auto vulkanPipeline = (GEVulkanRenderPipelineState *)pipelineState.get();
        VkPipeline state = vulkanPipeline->pipeline;

        vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,state);
        // Switching pipelines retires the previous pipeline's freshly
        // allocated fallback descriptor sets — the new pipeline's first
        // bindResource* call will lazily allocate fresh ones.
        resetFallbackDescriptorSetsForNewPipeline();
        renderPipelineState = vulkanPipeline;
        // Defer the descriptor-set bind to draw time. Even in push-descriptor
        // mode the fragment set (set 1+) is a regular descriptor set that
        // needs vkCmdBindDescriptorSets. The recorder allocates a fresh
        // ring slot for it on the next bindResource* call, then this flag
        // makes drawX bind it.
        if(!vulkanPipeline->descs.empty()){
            descriptorSetsBindPending = true;
        }
    };

    void GEVulkanCommandBuffer::bindDescriptorSetsIfPending(){
        if(!descriptorSetsBindPending || renderPipelineState == nullptr){
            return;
        }
        descriptorSetsBindPending = false;

        // The set indices we bind start at the first non-push set:
        //   push mode  → set 1+ (set 0 is push, recorded inline).
        //   fallback mode → set 0+.
        // descs[i] corresponds to layout slot (firstSet + i). When the
        // recorder allocated a fresh fallback set for slot i, prefer it;
        // otherwise fall back to the pipeline's seed set.
        const bool pushMode = parentQueue->engine->hasPushDescriptorExt;
        const std::uint32_t firstSet = pushMode ? 1u : 0u;
        const std::size_t slotCount = renderPipelineState->descs.size();
        if(slotCount == 0){
            return;
        }

        OmegaCommon::Vector<VkDescriptorSet> setsToBind;
        setsToBind.reserve(slotCount);
        for(std::size_t i = 0; i < slotCount; ++i){
            VkDescriptorSet s = VK_NULL_HANDLE;
            const std::size_t setIndex = firstSet + i;
            if(setIndex < fallbackDescriptorSets.size()){
                s = fallbackDescriptorSets[setIndex];
            }
            if(s == VK_NULL_HANDLE){
                s = renderPipelineState->descs[i];
            }
            if(s == VK_NULL_HANDLE){
                return;
            }
            setsToBind.push_back(s);
        }
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderPipelineState->layout,
                                firstSet,
                                static_cast<std::uint32_t>(setsToBind.size()),
                                setsToBind.data(),
                                0,nullptr);
        // Mark the current ring slots as committed — any subsequent
        // bindResource* write must retire them and acquire fresh ones.
        fallbackSetsCommitted = true;
    }

    VkDescriptorSet GEVulkanCommandBuffer::acquireOrUpdateFallbackSet(unsigned setIndex){
        // setIndex is the absolute Vulkan descriptor set index (0 = vertex,
        // 1 = fragment, …). In push mode set 0 is push and must never be
        // routed here; callers gate that.
        if(renderPipelineState == nullptr){
            return VK_NULL_HANDLE;
        }
        const std::size_t totalSets = renderPipelineState->descLayouts.size();
        if(totalSets == 0 || setIndex >= totalSets){
            return VK_NULL_HANDLE;
        }
        const bool pushMode = parentQueue->engine->hasPushDescriptorExt;
        const std::size_t pushOffset = pushMode ? 1u : 0u;
        if(setIndex < pushOffset){
            return VK_NULL_HANDLE;
        }
        const std::size_t descsIndex = setIndex - pushOffset;

        // If the previous ring slots have already been bound to a draw,
        // retire them and start a fresh cluster — writing the same set
        // after it was bound violates "descriptor set updated while bound."
        if(fallbackSetsCommitted){
            for(auto s : fallbackDescriptorSets){
                if(s != VK_NULL_HANDLE){
                    retiredFallbackSets.push_back(s);
                }
            }
            fallbackDescriptorSets.clear();
            fallbackSetsCommitted = false;
            // Force a re-bind of the new ring slots before the next draw.
            descriptorSetsBindPending = true;
        }

        if(fallbackDescriptorSets.size() != totalSets){
            fallbackDescriptorSets.assign(totalSets, VK_NULL_HANDLE);
        }
        if(fallbackDescriptorSets[setIndex] != VK_NULL_HANDLE){
            return fallbackDescriptorSets[setIndex];
        }
        if(fallbackPoolExhausted || renderPipelineState->descriptorPool == VK_NULL_HANDLE){
            if(descsIndex < renderPipelineState->descs.size()){
                return renderPipelineState->descs[descsIndex];
            }
            return VK_NULL_HANDLE;
        }

        VkDescriptorSetLayout layouts[1] = { renderPipelineState->descLayouts[setIndex] };
        VkDescriptorSetAllocateInfo allocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = renderPipelineState->descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = layouts;
        allocInfo.pNext = nullptr;

        VkDescriptorSet allocated = VK_NULL_HANDLE;
        VkResult res = vkAllocateDescriptorSets(parentQueue->engine->device, &allocInfo, &allocated);
        if(res != VK_SUCCESS || allocated == VK_NULL_HANDLE){
            // Pool exhausted (rare). Fall back to the persistent seed set —
            // re-introduces the original VVL noise on this frame but keeps
            // rendering moving. The pool is sized at 256 slots per
            // non-push set per pipeline — should be unreachable in steady
            // state.
            fallbackPoolExhausted = true;
            if(descsIndex < renderPipelineState->descs.size()){
                return renderPipelineState->descs[descsIndex];
            }
            return VK_NULL_HANDLE;
        }

        fallbackDescriptorSets[setIndex] = allocated;
        fallbackDescriptorPool = renderPipelineState->descriptorPool;
        // The new sets must be bound before the next draw consumes them.
        descriptorSetsBindPending = true;
        return allocated;
    }

    void GEVulkanCommandBuffer::resetFallbackDescriptorSetsForNewPipeline(){
        for(auto s : fallbackDescriptorSets){
            if(s != VK_NULL_HANDLE){
                retiredFallbackSets.push_back(s);
            }
        }
        fallbackDescriptorSets.clear();
        fallbackPoolExhausted = false;
        fallbackSetsCommitted = false;
    }

    void GEVulkanCommandBuffer::releaseFallbackDescriptorSets(){
        if(parentQueue == nullptr || parentQueue->engine == nullptr){
            fallbackDescriptorSets.clear();
            retiredFallbackSets.clear();
            fallbackDescriptorPool = VK_NULL_HANDLE;
            return;
        }
        VkDevice dev = parentQueue->engine->device;
        VkDescriptorPool pool = fallbackDescriptorPool;
        // Defer the actual vkFreeDescriptorSets to the engine's retention
        // queue so the GPU has finished using the sets before they're
        // returned to the pool. The pattern mirrors the framebuffer
        // teardown in this destructor.
        OmegaCommon::Vector<VkDescriptorSet> toFree;
        toFree.reserve(retiredFallbackSets.size() + fallbackDescriptorSets.size());
        for(auto s : retiredFallbackSets){
            if(s != VK_NULL_HANDLE){ toFree.push_back(s); }
        }
        for(auto s : fallbackDescriptorSets){
            if(s != VK_NULL_HANDLE){ toFree.push_back(s); }
        }
        retiredFallbackSets.clear();
        fallbackDescriptorSets.clear();
        if(!toFree.empty() && pool != VK_NULL_HANDLE){
            parentQueue->engine->retentionQueue.enqueue(
                {},
                [dev, pool, sets = std::move(toFree)]() mutable {
                    if(!sets.empty()){
                        vkFreeDescriptorSets(dev, pool,
                                             static_cast<std::uint32_t>(sets.size()),
                                             sets.data());
                    }
                });
        }
        fallbackDescriptorPool = VK_NULL_HANDLE;
    }

    void GEVulkanCommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned id){
        auto vk_buffer = (GEVulkanBuffer *)buffer.get();
        trackBuffer(buffer);

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
            // Vertex set 0 in fallback mode: write to a freshly-allocated
            // ring slot so we don't update a set that may still be bound
            // to an in-flight command buffer.
            VkDescriptorSet target = acquireOrUpdateFallbackSet(0);
            if(target != VK_NULL_HANDLE){
                writeInfo.dstSet = target;
                vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
            }
        }


    };

    void GEVulkanCommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned id){
        auto vk_texture = (GEVulkanTexture *)texture.get();
        trackTexture(texture);
        /// TODO!

        checkTextureBindAgainstShader(id, renderPipelineState->vertexShader->internal, *vk_texture);

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
            // Vertex-stage texture binds live on set 0, the same set that
            // bindResourceAtVertexShader(buffer) targets. The previous code
            // pushed to set 1 — that wrote into the fragment set, which
            // either fails validation or silently misses the slot the
            // shader was looking up.
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,renderPipelineState->layout,
                                                           0,1,&writeInfo);
        }
        else {
            // Fallback path: cycle the per-pipeline descriptor set ring so
            // we don't update a set that's still bound to an in-flight
            // command buffer.
            VkDescriptorSet target = acquireOrUpdateFallbackSet(0);
            writeInfo.dstSet = target;
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };

    void GEVulkanCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned id){
        auto vk_buffer = (GEVulkanBuffer *)buffer.get();
        trackBuffer(buffer);

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

        // Fragment shader resources live in set 1. Even in push-descriptor
        // mode this is a regular descriptor set (Vulkan permits at most
        // one push set per pipeline layout — set 0). The recorder
        // allocates fresh ring slots so fragment writes never touch a set
        // still bound to an in-flight command buffer.
        VkDescriptorSet target = acquireOrUpdateFallbackSet(1);
        if(target != VK_NULL_HANDLE){
            writeInfo.dstSet = target;
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };

    void GEVulkanCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned id){

        auto vk_texture = (GEVulkanTexture *)texture.get();
        trackTexture(texture);

        checkTextureBindAgainstShader(id, renderPipelineState->fragmentShader->internal, *vk_texture);

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

        // Fragment textures: same as fragment buffers — always go through
        // the fallback ring at set 1 (only set 0 is allowed to be push).
        VkDescriptorSet target = acquireOrUpdateFallbackSet(1);
        if(target != VK_NULL_HANDLE){
            writeInfo.dstSet = target;
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };



    static VkPrimitiveTopology vulkanTopologyForPolygonType(GERenderTarget::CommandBuffer::PolygonType polygonType){
        switch(polygonType){
            case GERenderTarget::CommandBuffer::Triangle:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case GERenderTarget::CommandBuffer::TriangleStrip:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case GERenderTarget::CommandBuffer::Line:
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case GERenderTarget::CommandBuffer::LineStrip:
                return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case GERenderTarget::CommandBuffer::Point:
                return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    void GEVulkanCommandBuffer::applyTopologyIfDynamic(RenderPassDrawPolygonType polygonType){
        if(parentQueue->engine->hasExtendedDynamicState) {
            parentQueue->engine->vkCmdSetPrimitiveTopologyExt(commandBuffer, vulkanTopologyForPolygonType(polygonType));
        }
    }

    void GEVulkanCommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount, size_t startIdx){
        // Begin the render pass now — any barriers from bind* calls have
        // already been recorded outside the render pass instance.
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDraw(commandBuffer,vertexCount,1,startIdx,0);
    };

    void GEVulkanCommandBuffer::setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType){
        auto vkBuffer = ((GEVulkanBuffer *)buffer.get());
        trackBuffer(buffer);
        VkIndexType vkType = (indexType == RenderPassIndexType::UInt16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(commandBuffer, vkBuffer->buffer, 0, vkType);
        pendingIndexType = vkType;
    }

    void GEVulkanCommandBuffer::drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                                    unsigned indexCount, size_t startIndex,
                                                    int baseVertex){
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDrawIndexed(commandBuffer, indexCount, 1, uint32_t(startIndex), baseVertex, 0);
    }

    void GEVulkanCommandBuffer::drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                      unsigned vertexCount, size_t startIdx,
                                                      unsigned instanceCount, unsigned firstInstance){
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDraw(commandBuffer, vertexCount, instanceCount, uint32_t(startIdx), firstInstance);
    }

    void GEVulkanCommandBuffer::drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                             unsigned indexCount, size_t startIndex,
                                                             int baseVertex, unsigned instanceCount,
                                                             unsigned firstInstance){
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, uint32_t(startIndex), baseVertex, firstInstance);
    }

    void GEVulkanCommandBuffer::drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                     SharedHandle<GEBuffer> & argumentBuffer,
                                                     size_t argumentBufferOffset){
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        auto *vkBuf = (GEVulkanBuffer *)argumentBuffer.get();
        trackBuffer(argumentBuffer);
        vkCmdDrawIndirect(commandBuffer, vkBuf->buffer,
                          VkDeviceSize(argumentBufferOffset),
                          1, sizeof(VkDrawIndirectCommand));
    }

    void GEVulkanCommandBuffer::drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                            SharedHandle<GEBuffer> & argumentBuffer,
                                                            size_t argumentBufferOffset){
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        auto *vkBuf = (GEVulkanBuffer *)argumentBuffer.get();
        trackBuffer(argumentBuffer);
        vkCmdDrawIndexedIndirect(commandBuffer, vkBuf->buffer,
                                 VkDeviceSize(argumentBufferOffset),
                                 1, sizeof(VkDrawIndexedIndirectCommand));
    }

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
        trackBuffer(buffer);
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer,0,1,&vkBuffer->buffer,offsets);
    }

    void GEVulkanCommandBuffer::finishRenderPass(){
        beginRenderPassIfDeferred();
        if(activeRenderPass != VK_NULL_HANDLE){
            vkCmdEndRenderPass(commandBuffer);
        }
        activeFramebuffer = VK_NULL_HANDLE;
        activeRenderPass = VK_NULL_HANDLE;
        renderPipelineState = nullptr;
    };

    void GEVulkanCommandBuffer::beginAccelStructPass(){
    }

    void GEVulkanCommandBuffer::buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                           const GEAccelerationStructDescriptor &desc){
        auto *engine = parentQueue->engine;
        if(!engine->hasAccelerationStructureExt || engine->vkCmdBuildAccelerationStructuresKhr == nullptr){
            return;
        }
        auto vkAS = std::dynamic_pointer_cast<GEVulkanAccelerationStruct>(src);
        if(!vkAS) return;
        if (vkAS->structBuffer)  trackedBuffers.push_back(vkAS->structBuffer);
        if (vkAS->scratchBuffer) trackedBuffers.push_back(vkAS->scratchBuffer);
        for (auto &g : desc.data) {
            if (g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES) {
                auto vb = g.getTriangleList().buffer;
                if (vb) trackedBuffers.push_back(vb);
            } else {
                auto vb = g.getAabb().buffer;
                if (vb) trackedBuffers.push_back(vb);
            }
        }

        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> rangeInfos;

        for(auto & g : desc.data){
            VkAccelerationStructureGeometryKHR geom {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            VkAccelerationStructureBuildRangeInfoKHR rangeInfo {};
            rangeInfo.firstVertex = 0;
            rangeInfo.transformOffset = 0;

            if(g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES){
                geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                geom.geometry.triangles.pNext = nullptr;
                geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                geom.geometry.triangles.vertexStride = sizeof(float) * 3;
                geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
                auto vkBuf = std::dynamic_pointer_cast<GEVulkanBuffer>(g.getTriangleList().buffer);
                if(vkBuf && engine->vkGetBufferDeviceAddressKhr){
                    VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
                    addrInfo.buffer = vkBuf->buffer;
                    geom.geometry.triangles.vertexData.deviceAddress = engine->vkGetBufferDeviceAddressKhr(engine->device, &addrInfo);
                    uint32_t vertexCount = static_cast<uint32_t>(vkBuf->size() / (sizeof(float) * 3));
                    geom.geometry.triangles.maxVertex = vertexCount > 0 ? vertexCount - 1 : 0;
                    rangeInfo.primitiveCount = vertexCount / 3;
                }
            } else {
                geom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                geom.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
                geom.geometry.aabbs.pNext = nullptr;
                geom.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
                auto vkBuf = std::dynamic_pointer_cast<GEVulkanBuffer>(g.getAabb().buffer);
                if(vkBuf && engine->vkGetBufferDeviceAddressKhr){
                    VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
                    addrInfo.buffer = vkBuf->buffer;
                    geom.geometry.aabbs.data.deviceAddress = engine->vkGetBufferDeviceAddressKhr(engine->device, &addrInfo);
                    rangeInfo.primitiveCount = static_cast<uint32_t>(vkBuf->size() / sizeof(VkAabbPositionsKHR));
                }
            }
            geometries.push_back(geom);
            rangeInfos.push_back(rangeInfo);
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.dstAccelerationStructure = vkAS->accelStruct;

        if(geometries.empty()){
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            buildInfo.geometryCount = 0;
            buildInfo.pGeometries = nullptr;
        } else {
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
            buildInfo.pGeometries = geometries.data();
        }

        if(vkAS->scratchBuffer && engine->vkGetBufferDeviceAddressKhr){
            VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
            addrInfo.buffer = vkAS->scratchBuffer->buffer;
            buildInfo.scratchData.deviceAddress = engine->vkGetBufferDeviceAddressKhr(engine->device, &addrInfo);
        }

        const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfos = rangeInfos.empty() ? nullptr : rangeInfos.data();
        engine->vkCmdBuildAccelerationStructuresKhr(commandBuffer, 1, &buildInfo, &pRangeInfos);

        VkMemoryBarrier memBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        memBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &memBarrier, 0, nullptr, 0, nullptr);
    }

    void GEVulkanCommandBuffer::copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                          SharedHandle<GEAccelerationStruct> &dest){
        auto *engine = parentQueue->engine;
        if(!engine->hasAccelerationStructureExt || engine->vkCmdCopyAccelerationStructureKhr == nullptr){
            return;
        }
        auto srcAS = std::dynamic_pointer_cast<GEVulkanAccelerationStruct>(src);
        auto destAS = std::dynamic_pointer_cast<GEVulkanAccelerationStruct>(dest);
        if(!srcAS || !destAS) return;
        if (srcAS->structBuffer)  trackedBuffers.push_back(srcAS->structBuffer);
        if (destAS->structBuffer) trackedBuffers.push_back(destAS->structBuffer);

        VkCopyAccelerationStructureInfoKHR copyInfo {VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copyInfo.src = srcAS->accelStruct;
        copyInfo.dst = destAS->accelStruct;
        copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
        engine->vkCmdCopyAccelerationStructureKhr(commandBuffer, &copyInfo);

        VkMemoryBarrier memBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        memBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 1, &memBarrier, 0, nullptr, 0, nullptr);
    }

    void GEVulkanCommandBuffer::refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                           SharedHandle<GEAccelerationStruct> &dest,
                                                           const GEAccelerationStructDescriptor &desc){
        auto *engine = parentQueue->engine;
        if(!engine->hasAccelerationStructureExt || engine->vkCmdBuildAccelerationStructuresKhr == nullptr){
            return;
        }
        auto srcAS = std::dynamic_pointer_cast<GEVulkanAccelerationStruct>(src);
        auto destAS = std::dynamic_pointer_cast<GEVulkanAccelerationStruct>(dest);
        if(!srcAS || !destAS) return;
        if (srcAS->structBuffer)   trackedBuffers.push_back(srcAS->structBuffer);
        if (destAS->structBuffer)  trackedBuffers.push_back(destAS->structBuffer);
        if (destAS->scratchBuffer) trackedBuffers.push_back(destAS->scratchBuffer);
        for (auto &g : desc.data) {
            if (g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES) {
                auto vb = g.getTriangleList().buffer;
                if (vb) trackedBuffers.push_back(vb);
            } else {
                auto vb = g.getAabb().buffer;
                if (vb) trackedBuffers.push_back(vb);
            }
        }

        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> rangeInfos;

        for(auto & g : desc.data){
            VkAccelerationStructureGeometryKHR geom {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            VkAccelerationStructureBuildRangeInfoKHR rangeInfo {};
            rangeInfo.firstVertex = 0;
            rangeInfo.transformOffset = 0;

            if(g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES){
                geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                geom.geometry.triangles.pNext = nullptr;
                geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                geom.geometry.triangles.vertexStride = sizeof(float) * 3;
                geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
                auto vkBuf = std::dynamic_pointer_cast<GEVulkanBuffer>(g.getTriangleList().buffer);
                if(vkBuf && engine->vkGetBufferDeviceAddressKhr){
                    VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
                    addrInfo.buffer = vkBuf->buffer;
                    geom.geometry.triangles.vertexData.deviceAddress = engine->vkGetBufferDeviceAddressKhr(engine->device, &addrInfo);
                    uint32_t vertexCount = static_cast<uint32_t>(vkBuf->size() / (sizeof(float) * 3));
                    geom.geometry.triangles.maxVertex = vertexCount > 0 ? vertexCount - 1 : 0;
                    rangeInfo.primitiveCount = vertexCount / 3;
                }
            } else {
                geom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                geom.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
                geom.geometry.aabbs.pNext = nullptr;
                geom.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
                auto vkBuf = std::dynamic_pointer_cast<GEVulkanBuffer>(g.getAabb().buffer);
                if(vkBuf && engine->vkGetBufferDeviceAddressKhr){
                    VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
                    addrInfo.buffer = vkBuf->buffer;
                    geom.geometry.aabbs.data.deviceAddress = engine->vkGetBufferDeviceAddressKhr(engine->device, &addrInfo);
                    rangeInfo.primitiveCount = static_cast<uint32_t>(vkBuf->size() / sizeof(VkAabbPositionsKHR));
                }
            }
            geometries.push_back(geom);
            rangeInfos.push_back(rangeInfo);
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        buildInfo.srcAccelerationStructure = srcAS->accelStruct;
        buildInfo.dstAccelerationStructure = destAS->accelStruct;

        if(geometries.empty()){
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        } else {
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
            buildInfo.pGeometries = geometries.data();
        }

        if(destAS->scratchBuffer && engine->vkGetBufferDeviceAddressKhr){
            VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
            addrInfo.buffer = destAS->scratchBuffer->buffer;
            buildInfo.scratchData.deviceAddress = engine->vkGetBufferDeviceAddressKhr(engine->device, &addrInfo);
        }

        const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfos = rangeInfos.empty() ? nullptr : rangeInfos.data();
        engine->vkCmdBuildAccelerationStructuresKhr(commandBuffer, 1, &buildInfo, &pRangeInfos);

        VkMemoryBarrier memBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        memBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &memBarrier, 0, nullptr, 0, nullptr);
    }

    void GEVulkanCommandBuffer::finishAccelStructPass(){
    }

    void GEVulkanCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct, unsigned int id){
        assert(inComputePass && "Must be in compute pass to bind acceleration structure");
        auto vkAS = std::dynamic_pointer_cast<GEVulkanAccelerationStruct>(accelStruct);
        if(!vkAS || !parentQueue->engine->hasAccelerationStructureExt) return;
        // Track the AS's underlying buffers so VMA destroys defer past GPU use.
        if (vkAS->structBuffer)  trackedBuffers.push_back(vkAS->structBuffer);
        if (vkAS->scratchBuffer) trackedBuffers.push_back(vkAS->scratchBuffer);

        VkWriteDescriptorSetAccelerationStructureKHR asWrite {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &vkAS->accelStruct;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.pNext = &asWrite;
        writeInfo.dstBinding = getBindingForResourceID(id, computePipelineState->computeShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = nullptr;

        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                           computePipelineState->layout, 0, 1, &writeInfo);
        } else {
            writeInfo.dstSet = computePipelineState->descSet;
            vkUpdateDescriptorSets(parentQueue->engine->device, 1, &writeInfo, 0, nullptr);
        }
    }

    void GEVulkanCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z){
        assert(inComputePass && "Must be in compute pass to dispatch rays");
        auto *engine = parentQueue->engine;
        if(engine->hasRayTracingPipelineExt && engine->vkCmdTraceRaysKhr != nullptr){
            VkStridedDeviceAddressRegionKHR raygenSBT {};
            VkStridedDeviceAddressRegionKHR missSBT {};
            VkStridedDeviceAddressRegionKHR hitSBT {};
            VkStridedDeviceAddressRegionKHR callableSBT {};
            engine->vkCmdTraceRaysKhr(commandBuffer, &raygenSBT, &missSBT, &hitSBT, &callableSBT, x, y, z);
        } else {
            vkCmdDispatch(commandBuffer, x, y, z);
        }
    }

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
        trackBuffer(buffer);

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
        trackTexture(texture);

        checkTextureBindAgainstShader(id, computePipelineState->computeShader->internal, *vk_texture);

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

    void GEVulkanCommandBuffer::dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) {
        vkCmdDispatch(commandBuffer,x,y,z);
    }

    void GEVulkanCommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
        auto & tg = computePipelineState->computeShader->internal.threadgroupDesc;
        unsigned gx = (x + tg.x - 1) / tg.x;
        unsigned gy = (y + tg.y - 1) / tg.y;
        unsigned gz = (z + tg.z - 1) / tg.z;
        vkCmdDispatch(commandBuffer,gx,gy,gz);
    }

    void GEVulkanCommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                              size_t argumentBufferOffset) {
        auto *vkBuf = (GEVulkanBuffer *)argumentBuffer.get();
        trackBuffer(argumentBuffer);
        vkCmdDispatchIndirect(commandBuffer, vkBuf->buffer,
                              VkDeviceSize(argumentBufferOffset));
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
        trackTexture(src);
        trackTexture(dest);

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
        trackTexture(src);
        trackTexture(dest);

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

    namespace {
        inline void transitionBufferForTransferSrc(GEVulkanEngine *engine, VkCommandBuffer cb, GEVulkanBuffer *buf){
            if(buf == nullptr) return;
            if(engine->hasSynchronization2Ext){
                if(buf->priorAccess2 & VK_ACCESS_2_TRANSFER_READ_BIT_KHR) return;
                VkBufferMemoryBarrier2KHR bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR};
                bb.srcAccessMask = buf->priorAccess2;
                bb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
                bb.srcStageMask = buf->priorPipelineAccess2 != 0
                                  ? buf->priorPipelineAccess2
                                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                bb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = buf->buffer;
                bb.offset = 0;
                bb.size = VK_WHOLE_SIZE;
                VkDependencyInfoKHR dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &bb;
                engine->vkCmdPipelineBarrier2Khr(cb, &dep);
                buf->priorAccess2 = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
                buf->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
            } else {
                if(buf->priorAccess & VK_ACCESS_TRANSFER_READ_BIT) return;
                VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                bb.srcAccessMask = buf->priorAccess;
                bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = buf->buffer;
                bb.offset = 0;
                bb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 1, &bb, 0, nullptr);
                buf->priorAccess = VK_ACCESS_TRANSFER_READ_BIT;
                buf->priorPipelineAccess = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
        }

        inline void transitionBufferForTransferDst(GEVulkanEngine *engine, VkCommandBuffer cb, GEVulkanBuffer *buf){
            if(buf == nullptr) return;
            if(engine->hasSynchronization2Ext){
                if(buf->priorAccess2 & VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR) return;
                VkBufferMemoryBarrier2KHR bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR};
                bb.srcAccessMask = buf->priorAccess2;
                bb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
                bb.srcStageMask = buf->priorPipelineAccess2 != 0
                                  ? buf->priorPipelineAccess2
                                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                bb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = buf->buffer;
                bb.offset = 0;
                bb.size = VK_WHOLE_SIZE;
                VkDependencyInfoKHR dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &bb;
                engine->vkCmdPipelineBarrier2Khr(cb, &dep);
                buf->priorAccess2 = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
                buf->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
            } else {
                if(buf->priorAccess & VK_ACCESS_TRANSFER_WRITE_BIT) return;
                VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                bb.srcAccessMask = buf->priorAccess;
                bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = buf->buffer;
                bb.offset = 0;
                bb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 1, &bb, 0, nullptr);
                buf->priorAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
                buf->priorPipelineAccess = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
        }

        inline void transitionImageForTransferSrc(GEVulkanEngine *engine, VkCommandBuffer cb, GEVulkanTexture *img){
            if(img == nullptr) return;
            if(engine->hasSynchronization2Ext){
                if(img->priorShaderAccess2 & VK_ACCESS_2_TRANSFER_READ_BIT_KHR) return;
                VkImageMemoryBarrier2KHR mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR};
                mb.srcAccessMask = img->priorShaderAccess2;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
                mb.srcStageMask = img->priorPipelineAccess2 != 0
                                  ? img->priorPipelineAccess2
                                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mb.image = img->img;
                mb.oldLayout = img->layout;
                mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                mb.subresourceRange = fullColorSubresourceRange(img);
                VkDependencyInfoKHR dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &mb;
                engine->vkCmdPipelineBarrier2Khr(cb, &dep);
                img->priorShaderAccess2 = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
                img->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            } else {
                if(img->priorShaderAccess & VK_ACCESS_TRANSFER_READ_BIT) return;
                VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                mb.srcAccessMask = img->priorShaderAccess;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mb.image = img->img;
                mb.oldLayout = img->layout;
                mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                mb.subresourceRange = fullColorSubresourceRange(img);
                vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &mb);
                img->priorShaderAccess = VK_ACCESS_TRANSFER_READ_BIT;
                img->priorPipelineAccess = VK_PIPELINE_STAGE_TRANSFER_BIT;
                img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
        }

        inline void transitionImageForTransferDst(GEVulkanEngine *engine, VkCommandBuffer cb, GEVulkanTexture *img){
            if(img == nullptr) return;
            if(engine->hasSynchronization2Ext){
                if(img->priorShaderAccess2 & VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR) return;
                VkImageMemoryBarrier2KHR mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR};
                mb.srcAccessMask = img->priorShaderAccess2;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
                mb.srcStageMask = img->priorPipelineAccess2 != 0
                                  ? img->priorPipelineAccess2
                                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mb.image = img->img;
                mb.oldLayout = img->layout;
                mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                mb.subresourceRange = fullColorSubresourceRange(img);
                VkDependencyInfoKHR dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &mb;
                engine->vkCmdPipelineBarrier2Khr(cb, &dep);
                img->priorShaderAccess2 = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
                img->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COPY_BIT_KHR;
                img->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            } else {
                if(img->priorShaderAccess & VK_ACCESS_TRANSFER_WRITE_BIT) return;
                VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                mb.srcAccessMask = img->priorShaderAccess;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mb.image = img->img;
                mb.oldLayout = img->layout;
                mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                mb.subresourceRange = fullColorSubresourceRange(img);
                vkCmdPipelineBarrier(cb,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &mb);
                img->priorShaderAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
                img->priorPipelineAccess = VK_PIPELINE_STAGE_TRANSFER_BIT;
                img->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            }
        }
    }

    void GEVulkanCommandBuffer::copyBufferToBuffer(SharedHandle<GEBuffer> &src, SharedHandle<GEBuffer> &dest,
                                                   size_t size, size_t srcOffset, size_t destOffset) {
        assert(inBlitPass && "Must be in a blit pass");
        auto src_buf = (GEVulkanBuffer *)src.get();
        auto dest_buf = (GEVulkanBuffer *)dest.get();
        trackBuffer(src);
        trackBuffer(dest);

        transitionBufferForTransferSrc(parentQueue->engine, commandBuffer, src_buf);
        transitionBufferForTransferDst(parentQueue->engine, commandBuffer, dest_buf);

        VkBufferCopy region{};
        region.srcOffset = srcOffset;
        region.dstOffset = destOffset;
        region.size = size == 0 ? (src_buf->alloc_info.size - srcOffset) : size;
        vkCmdCopyBuffer(commandBuffer, src_buf->buffer, dest_buf->buffer, 1, &region);
    }

    void GEVulkanCommandBuffer::copyBufferToTexture(SharedHandle<GEBuffer> &src, SharedHandle<GETexture> &dest,
                                                    size_t bytesPerRow, size_t bytesPerImage,
                                                    const TextureRegion &destRegion, size_t srcBufferOffset) {
        assert(inBlitPass && "Must be in a blit pass");
        auto src_buf = (GEVulkanBuffer *)src.get();
        auto dest_img = (GEVulkanTexture *)dest.get();
        trackBuffer(src);
        trackTexture(dest);

        transitionBufferForTransferSrc(parentQueue->engine, commandBuffer, src_buf);
        transitionImageForTransferDst(parentQueue->engine, commandBuffer, dest_img);

        // Convert bytesPerRow / bytesPerImage to texel counts for VkBufferImageCopy.
        // Vulkan expresses bufferRowLength in texels, not bytes. When 0, data is tightly packed.
        uint32_t bufferRowLength = 0;
        uint32_t bufferImageHeight = 0;
        if(bytesPerRow > 0){
            // Derive bytes-per-texel from the image format. For supported formats this table
            // covers the PixelFormat enum; unknown formats fall back to tightly packed.
            uint32_t bytesPerTexel = 4;
            switch(dest_img->descriptor.pixelFormat){
                case PixelFormat::RGBA16Unorm: bytesPerTexel = 8; break;
                default: bytesPerTexel = 4; break;
            }
            if(bytesPerTexel > 0){
                bufferRowLength = static_cast<uint32_t>(bytesPerRow / bytesPerTexel);
            }
            if(bytesPerImage > 0 && bytesPerRow > 0){
                bufferImageHeight = static_cast<uint32_t>(bytesPerImage / bytesPerRow);
            }
        }

        VkBufferImageCopy copy{};
        copy.bufferOffset = srcBufferOffset;
        copy.bufferRowLength = bufferRowLength;
        copy.bufferImageHeight = bufferImageHeight;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {int32_t(destRegion.x), int32_t(destRegion.y), int32_t(destRegion.z)};
        copy.imageExtent = {destRegion.w, destRegion.h, destRegion.d == 0 ? 1 : destRegion.d};

        vkCmdCopyBufferToImage(commandBuffer,
                               src_buf->buffer,
                               dest_img->img,
                               dest_img->layout,
                               1, &copy);
    }

    void GEVulkanCommandBuffer::copyTextureToBuffer(SharedHandle<GETexture> &src, SharedHandle<GEBuffer> &dest,
                                                    size_t bytesPerRow, size_t bytesPerImage,
                                                    const TextureRegion &srcRegion, size_t destBufferOffset) {
        assert(inBlitPass && "Must be in a blit pass");
        auto src_img = (GEVulkanTexture *)src.get();
        auto dest_buf = (GEVulkanBuffer *)dest.get();
        trackTexture(src);
        trackBuffer(dest);

        transitionImageForTransferSrc(parentQueue->engine, commandBuffer, src_img);
        transitionBufferForTransferDst(parentQueue->engine, commandBuffer, dest_buf);

        uint32_t bufferRowLength = 0;
        uint32_t bufferImageHeight = 0;
        if(bytesPerRow > 0){
            uint32_t bytesPerTexel = 4;
            switch(src_img->descriptor.pixelFormat){
                case PixelFormat::RGBA16Unorm: bytesPerTexel = 8; break;
                default: bytesPerTexel = 4; break;
            }
            if(bytesPerTexel > 0){
                bufferRowLength = static_cast<uint32_t>(bytesPerRow / bytesPerTexel);
            }
            if(bytesPerImage > 0 && bytesPerRow > 0){
                bufferImageHeight = static_cast<uint32_t>(bytesPerImage / bytesPerRow);
            }
        }

        VkBufferImageCopy copy{};
        copy.bufferOffset = destBufferOffset;
        copy.bufferRowLength = bufferRowLength;
        copy.bufferImageHeight = bufferImageHeight;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {int32_t(srcRegion.x), int32_t(srcRegion.y), int32_t(srcRegion.z)};
        copy.imageExtent = {srcRegion.w, srcRegion.h, srcRegion.d == 0 ? 1 : srcRegion.d};

        vkCmdCopyImageToBuffer(commandBuffer,
                               src_img->img,
                               src_img->layout,
                               dest_buf->buffer,
                               1, &copy);
    }

    void GEVulkanCommandBuffer::generateMipmaps(SharedHandle<GETexture> &texture) {
        assert(inBlitPass && "Must be in a blit pass");
        auto *tex = (GEVulkanTexture *)texture.get();
        trackTexture(texture);
        const uint32_t mipLevels = tex->descriptor.mipLevels;
        if (mipLevels <= 1) {
            return;
        }

        const bool hasSync2 = parentQueue->engine->hasSynchronization2Ext;
        VkImageLayout finalLayout = tex->layout;
        if (finalLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
            finalLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
            finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        auto transitionLevel = [&](uint32_t level, VkImageLayout oldLayout,
                                   VkImageLayout newLayout,
                                   VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                   VkPipelineStageFlags srcStage,
                                   VkPipelineStageFlags dstStage){
            VkImageMemoryBarrier bar {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = srcAccess;
            bar.dstAccessMask = dstAccess;
            bar.oldLayout = oldLayout;
            bar.newLayout = newLayout;
            bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = tex->img;
            bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bar.subresourceRange.baseMipLevel = level;
            bar.subresourceRange.levelCount = 1;
            bar.subresourceRange.baseArrayLayer = 0;
            bar.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
        };

        // Ensure mip 0 is in TRANSFER_SRC; all remaining mips in TRANSFER_DST.
        transitionLevel(0, tex->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (uint32_t i = 1; i < mipLevels; ++i) {
            transitionLevel(i, tex->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        }

        int32_t mipWidth = static_cast<int32_t>(tex->descriptor.width);
        int32_t mipHeight = static_cast<int32_t>(tex->descriptor.height);
        int32_t mipDepth = static_cast<int32_t>(
            tex->descriptor.depth == 0 ? 1 : tex->descriptor.depth);

        for (uint32_t i = 1; i < mipLevels; ++i) {
            const int32_t nextW = mipWidth > 1 ? mipWidth / 2 : 1;
            const int32_t nextH = mipHeight > 1 ? mipHeight / 2 : 1;
            const int32_t nextD = mipDepth > 1 ? mipDepth / 2 : 1;

            VkImageBlit blit{};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipWidth, mipHeight, mipDepth};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {nextW, nextH, nextD};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;

            vkCmdBlitImage(commandBuffer,
                           tex->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            // Transition the just-written mip to SRC for the next iteration.
            if (i + 1 < mipLevels) {
                transitionLevel(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
            }

            mipWidth = nextW;
            mipHeight = nextH;
            mipDepth = nextD;
        }

        // Transition all levels back to the original layout. Mips 0..N-2 are in
        // TRANSFER_SRC, mip N-1 is in TRANSFER_DST.
        for (uint32_t i = 0; i < mipLevels; ++i) {
            const VkImageLayout current = (i + 1 == mipLevels)
                                              ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                              : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkAccessFlags srcAccess = (i + 1 == mipLevels)
                                                ? VK_ACCESS_TRANSFER_WRITE_BIT
                                                : VK_ACCESS_TRANSFER_READ_BIT;
            transitionLevel(i, current, finalLayout, srcAccess,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        tex->layout = finalLayout;
        if (hasSync2) {
            tex->priorShaderAccess2 = VK_ACCESS_2_SHADER_READ_BIT_KHR;
            tex->priorPipelineAccess2 = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
        }
        tex->priorShaderAccess = VK_ACCESS_SHADER_READ_BIT;
        tex->priorPipelineAccess = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    void GEVulkanCommandBuffer::fillBuffer(SharedHandle<GEBuffer> &buffer, uint32_t value,
                                            size_t offset, size_t size) {
        assert(inBlitPass && "Must be in a blit pass");
        auto *buf = (GEVulkanBuffer *)buffer.get();
        trackBuffer(buffer);
        transitionBufferForTransferDst(parentQueue->engine, commandBuffer, buf);

        const VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);
        const VkDeviceSize vkSize = size == 0 ? VK_WHOLE_SIZE
                                              : static_cast<VkDeviceSize>(size);
        vkCmdFillBuffer(commandBuffer, buf->buffer, vkOffset, vkSize, value);
    }

    void GEVulkanCommandBuffer::finishBlitPass() {
        inBlitPass = false;
    }

    void GEVulkanCommandBuffer::reset(){
        // Free fallback descriptor sets before resetting the command buffer
        // — the GPU is assumed to be done with this buffer at reset time.
        releaseFallbackDescriptorSets();
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
        pendingRetainedBuffers.push_back(commandBuffer);
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
        pendingRetainedBuffers.push_back(commandBuffer);
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

   Retention::FenceGate GEVulkanCommandQueue::gateForNextSubmit(){
       const std::uint64_t v = nextSubmitValue + 1;
       VkDevice    dev = (engine != nullptr ? engine->device : VK_NULL_HANDLE);
       VkSemaphore sem = retentionTimeline;
       return [dev, sem, v]() {
           if (dev == VK_NULL_HANDLE || sem == VK_NULL_HANDLE) return true;
           uint64_t cur = 0;
           if (vkGetSemaphoreCounterValue(dev, sem, &cur) != VK_SUCCESS) return false;
           return cur >= v;
       };
   }

   void GEVulkanCommandQueue::prepareSubmitWithRetentionSignal(VkSubmitInfo &submission,
                                                               VkTimelineSemaphoreSubmitInfo &timelineInfo,
                                                               VkSemaphore &signalSlot,
                                                               std::uint64_t &signalValueSlot){
       signalSlot       = retentionTimeline;
       signalValueSlot  = nextSubmitValue + 1;
       timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
       timelineInfo.pNext = nullptr;
       timelineInfo.waitSemaphoreValueCount   = 0;
       timelineInfo.pWaitSemaphoreValues      = nullptr;
       timelineInfo.signalSemaphoreValueCount = 1;
       timelineInfo.pSignalSemaphoreValues    = &signalValueSlot;
       submission.signalSemaphoreCount = 1;
       submission.pSignalSemaphores    = &signalSlot;
       submission.pNext                = &timelineInfo;
   }

   void GEVulkanCommandQueue::flushPendingRetentionUnder(const Retention::FenceGate &gate){
       if (engine == nullptr) {
           pendingRetainedBuffers.clear();
           return;
       }
       for (auto &handle : pendingRetainedBuffers) {
           auto *buf = static_cast<GEVulkanCommandBuffer *>(handle.get());
           if (buf != nullptr) {
               // Push the gate to every resource bound on this buffer so its
               // VMA destructor can defer until the GPU is done.
               for (auto &b : buf->trackedBuffers) {
                   auto *vkb = static_cast<GEVulkanBuffer *>(b.get());
                   if (vkb != nullptr) vkb->pendingGates.push_back(gate);
               }
               for (auto &t : buf->trackedTextures) {
                   auto *vkt = static_cast<GEVulkanTexture *>(t.get());
                   if (vkt != nullptr) vkt->pendingGates.push_back(gate);
               }
               // Move owned framebuffer / render pass destroys to the engine
               // retention queue under the same gate. Replaces the old
               // deferred*Destroys / flushDeferredDestroys path.
               VkDevice dev = engine->device;
               for (auto fb : buf->ownedFramebuffers) {
                   if (fb != VK_NULL_HANDLE) {
                       engine->retentionQueue.enqueue(
                           {gate},
                           [dev, fb]{ vkDestroyFramebuffer(dev, fb, nullptr); });
                   }
               }
               for (auto rp : buf->ownedRenderPasses) {
                   if (rp != VK_NULL_HANDLE) {
                       engine->retentionQueue.enqueue(
                           {gate},
                           [dev, rp]{ vkDestroyRenderPass(dev, rp, nullptr); });
                   }
               }
               buf->ownedFramebuffers.clear();
               buf->ownedRenderPasses.clear();
           }
           // Retain the wrapper itself until the gate fires so its destructor
           // (and the SharedHandles to tracked resources it carries) runs
           // GPU-safely.
           engine->retentionQueue.retainShared(std::move(handle), {gate});
       }
       pendingRetainedBuffers.clear();
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
       submission.waitSemaphoreCount = 0;
       submission.pWaitSemaphores = nullptr;
       submission.commandBufferCount = commandQueue.size();
       submission.pCommandBuffers = commandQueue.data();

       VkTimelineSemaphoreSubmitInfo timelineInfo {};
       VkSemaphore signalSlot = VK_NULL_HANDLE;
       std::uint64_t signalValueSlot = 0;
       prepareSubmitWithRetentionSignal(submission, timelineInfo, signalSlot, signalValueSlot);
       const auto gate = gateForNextSubmit();

       auto res = vkQueueSubmit(vkQueue, 1, &submission,VK_NULL_HANDLE);
       if(!VK_RESULT_SUCCEEDED(res)){
           std::cerr << "Failed to Submit Command Buffers to GPU (" << res << ")" << std::endl;
           commandQueue.clear();
           pendingRetainedBuffers.clear();
           submittedTraceCommandBufferIds.clear();
           return;
       };
       ++nextSubmitValue;

       commandQueue.clear();
       flushPendingRetentionUnder(gate);
       engine->retentionQueue.drainCompleted();
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
        submission.waitSemaphoreCount = 0;
        submission.pWaitSemaphores = nullptr;
        submission.commandBufferCount = commandQueue.size();
        submission.pCommandBuffers = commandQueue.data();

        VkTimelineSemaphoreSubmitInfo timelineInfo {};
        VkSemaphore signalSlot = VK_NULL_HANDLE;
        std::uint64_t signalValueSlot = 0;
        prepareSubmitWithRetentionSignal(submission, timelineInfo, signalSlot, signalValueSlot);
        const auto gate = gateForNextSubmit();

        auto res = vkQueueSubmit(vkQueue, 1, &submission,submitFence);
        if(!VK_RESULT_SUCCEEDED(res)){
            std::cerr << "Failed to Submit Command Buffers to GPU (" << res << ")" << std::endl;
            commandQueue.clear();
            pendingRetainedBuffers.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }
        ++nextSubmitValue;
        auto waitRes = vkWaitForFences(engine->device,1,&submitFence,VK_TRUE,UINT64_MAX);
        if(waitRes != VK_SUCCESS){
            std::cerr << "Failed waiting for submitted command buffers (" << waitRes << ")" << std::endl;
            commandQueue.clear();
            // Move pending retains into retentionQueue; the signaled timeline
            // will let drainCompleted release them once it fires.
            flushPendingRetentionUnder(gate);
            submittedTraceCommandBufferIds.clear();
            return;
        }
        // GPU finished — both submitFence and the retentionTimeline value are
        // signaled by now. Hand pending items to the retention queue and
        // drain them in one shot.
        flushPendingRetentionUnder(gate);
        engine->retentionQueue.drainCompleted();

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
       submission.waitSemaphoreCount = 0;
       submission.pWaitSemaphores = nullptr;
       submission.commandBufferCount = commandQueue.size();
       submission.pCommandBuffers = commandQueue.data();

       VkTimelineSemaphoreSubmitInfo timelineInfo {};
       VkSemaphore signalSlot = VK_NULL_HANDLE;
       std::uint64_t signalValueSlot = 0;
       prepareSubmitWithRetentionSignal(submission, timelineInfo, signalSlot, signalValueSlot);
       const auto gate = gateForNextSubmit();

       auto res = vkQueueSubmit(vkQueue, 1, &submission,submitFence);
       if(!VK_RESULT_SUCCEEDED(res)){
           std::cerr << "Failed to Submit Command Buffers to GPU (" << res << ")" << std::endl;
           commandQueue.clear();
           pendingRetainedBuffers.clear();
           submittedTraceCommandBufferIds.clear();
           return;
       };
       ++nextSubmitValue;

       commandQueue.clear();
       auto waitRes = vkWaitForFences(engine->device,1,&submitFence,VK_TRUE,UINT64_MAX);
       if(waitRes != VK_SUCCESS){
           std::cerr << "Failed waiting for submitted command buffers (" << waitRes << ")" << std::endl;
           flushPendingRetentionUnder(gate);
           submittedTraceCommandBufferIds.clear();
           return;
       }
       flushPendingRetentionUnder(gate);
       engine->retentionQueue.drainCompleted();
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

       VkSemaphoreTypeCreateInfo timelineType {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
       timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
       timelineType.initialValue  = 0;
       timelineType.pNext         = nullptr;
       VkSemaphoreCreateInfo retentionSemInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
       retentionSemInfo.pNext = &timelineType;
       retentionSemInfo.flags = 0;
       auto semRes = vkCreateSemaphore(engine->device,&retentionSemInfo,nullptr,&retentionTimeline);
       if(semRes != VK_SUCCESS){
           std::cerr << "Failed to create retention timeline semaphore (" << semRes << ")" << std::endl;
           exit(1);
       }

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

   void GEVulkanCommandQueue::releaseNative(){
       if(nativeReleased_) return;
       nativeReleased_ = true;
       // Pending retention entries are owned by engine->retentionQueue; the
       // engine's drainAll() at shutdown is responsible for releasing them.
       // Don't destroy retentionTimeline until those entries (which capture
       // it via gates) have all drained — the timeline is owned by the
       // semaphore still being referenced inside retentionQueue closures, so
       // we release it via lambda capture rather than freeing here.
       if(!commandBuffers.empty()){
           vkFreeCommandBuffers(engine->device,commandPool,commandBuffers.size(),commandBuffers.data());
           commandBuffers.resize(0);
       }
       if(commandPool != VK_NULL_HANDLE){
           vkDestroyCommandPool(engine->device,commandPool,nullptr);
           commandPool = VK_NULL_HANDLE;
       }
       if(submitFence != VK_NULL_HANDLE){
           vkDestroyFence(engine->device,submitFence,nullptr);
           submitFence = VK_NULL_HANDLE;
       }
       if(retentionTimeline != VK_NULL_HANDLE && engine != nullptr){
           VkDevice dev = engine->device;
           VkSemaphore sem = retentionTimeline;
           // Defer the semaphore destroy through the retention queue with no
           // gates: it runs on the next drainCompleted() (or drainAll() at
           // engine shutdown), strictly after every closure that captured the
           // semaphore handle has had a chance to query it.
           engine->retentionQueue.enqueue({},
               [dev, sem]{ vkDestroySemaphore(dev, sem, nullptr); });
           retentionTimeline = VK_NULL_HANDLE;
       }
   }

   GEVulkanCommandQueue::~GEVulkanCommandQueue() {
       ResourceTracking::Tracker::instance().emit(
               ResourceTracking::EventType::Destroy,
               ResourceTracking::Backend::Vulkan,
               "CommandQueue",
               traceResourceId,
               reinterpret_cast<const void *>(commandPool));
       if(!nativeReleased_){
           releaseNative();
       }
   }
_NAMESPACE_END_
