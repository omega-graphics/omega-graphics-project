#include "GEVulkanCommandQueue.h"
#include "omegaGTE/GTEDevice.h"
#include "GEVulkanRenderTarget.h"
#include "GEVulkanPipeline.h"
#include "GEVulkan.h"
#include "GEVulkanTexture.h"
#include "VulkanQueueFamilies.h"
#include "vulkan/vulkan_core.h"
#include "../common/GEResourceTracker.h"

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <utility>

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

        // Extension 8 §8.5 — sampler-bind validation. Rejects static-sampler
        // and non-sampler slots via validateSamplerBindKind().
        inline bool checkSamplerBindAgainstShader(unsigned int location,
                                                  const omegasl_shader &shader){
            OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{
                shader.pLayout, shader.pLayout + shader.nLayout};
            for (auto &l : layoutArr) {
                if (l.location == location) {
                    return validateSamplerBindKind((int)l.type, shader.name, location);
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

    VkDescriptorType
    GEVulkanCommandBuffer::getBufferDescriptorTypeForResourceID(unsigned int &id, omegasl_shader &shader) {
        ArrayRef<omegasl_shader_layout_desc> layoutDesc {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto & l : layoutDesc){
            if(l.location == id){
                return l.type == OMEGASL_SHADER_UNIFORM_DESC
                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
        }
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }

    TextureSwizzle
    GEVulkanCommandBuffer::resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader & shader) {
        if(!runtime.isIdentity()) return runtime;
        // Layout-desc encoding: 0=Identity, 1=R, 2=G, 3=B, 4=A, 5=Zero, 6=One.
        auto decode = [](unsigned char b) -> TextureSwizzleChannel {
            switch(b){
                case 1: return TextureSwizzleChannel::Red;
                case 2: return TextureSwizzleChannel::Green;
                case 3: return TextureSwizzleChannel::Blue;
                case 4: return TextureSwizzleChannel::Alpha;
                case 5: return TextureSwizzleChannel::Zero;
                case 6: return TextureSwizzleChannel::One;
                default: return TextureSwizzleChannel::Identity;
            }
        };
        ArrayRef<omegasl_shader_layout_desc> layoutDesc {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto & l : layoutDesc){
            if(l.location == id){
                if(l.swizzle_desc.r == 0 && l.swizzle_desc.g == 0
                   && l.swizzle_desc.b == 0 && l.swizzle_desc.a == 0){
                    return TextureSwizzle::identity();
                }
                return TextureSwizzle{
                    decode(l.swizzle_desc.r),
                    decode(l.swizzle_desc.g),
                    decode(l.swizzle_desc.b),
                    decode(l.swizzle_desc.a)
                };
            }
        }
        return TextureSwizzle::identity();
    }

    void GEVulkanCommandBuffer::insertResourceBarrierIfNeeded(GEVulkanBuffer *buffer, unsigned int &resource_id,
                                                              omegasl_shader &shader) {
        // The graphics (vertex/fragment) buffer-barrier path can trigger
        // VK_ERROR_DEVICE_LOST on some drivers and is still being reworked, so
        // keep skipping it there to keep BasicAppTest stable. Compute is
        // re-enabled: multi-pass GPU kernels (AQUA physics) need the
        // write->read hazard barrier between successive dispatches, or the
        // passes race and produce non-deterministic, oracle-mismatching output.
        // A COMPUTE->COMPUTE buffer barrier on a compute queue (no render pass
        // in flight) is not affected by the graphics-path issue.
        if(shader.type != OMEGASL_SHADER_COMPUTE){
            (void)buffer;
            (void)resource_id;
            return;
        }

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

    void GEVulkanCommandBuffer::reportBarrierInsideRenderPass(GEVulkanTexture *texture,
                                                              VkImageLayout oldLayout,
                                                              VkImageLayout newLayout,
                                                              const omegasl_shader &shader) const {
        // Always-on: this is a frontend contract violation, not a tunable. A
        // texture bound for sampling/storage inside a live render pass needed a
        // layout transition (oldLayout -> newLayout) that cannot be recorded
        // there. The fix belongs in the caller: split the render pass (finish +
        // restart with LoadPreserve) so the transition lands outside it, exactly
        // as the compositor already does around freshly-written scratch targets.
        std::cerr << "[GEVulkanCommandBuffer] resource barrier requested inside an active "
                     "render pass instance — skipping (would violate "
                     "VUID-vkCmdPipelineBarrier2-pDependencies-02285). image="
                  << texture->img << " oldLayout=" << oldLayout << " newLayout=" << newLayout
                  << " shaderType=" << shader.type
                  << ". The frontend must split the pass before this transition." << std::endl;
        assert(false && "Texture layout transition requested inside an active render pass; "
                        "split the render pass before binding this resource.");
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
            /// Emit a barrier only for a real hazard: a layout transition, or a
            /// write on either side of the access (RAW / WAR / WAW). A read-after-read
            /// in the same layout — e.g. the same glyph atlas sampled by several draws
            /// in one render pass — needs no synchronization, and emitting one here
            /// would also be illegal: bind-time barriers for draws after the first land
            /// inside the active render pass instance, which forbids them
            /// (VUID-vkCmdPipelineBarrier2-pDependencies-02285).
            /// The layout still differs without prior shader access when the texture was
            /// a render-target attachment (GENERAL/COLOR_ATTACHMENT) and is now sampled.
            const bool layoutChanges  = texture->layout != layout;
            const bool priorWasWrite  = (texture->priorShaderAccess2 & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT_KHR) != 0;
            const bool currentIsWrite = ioMode != OMEGASL_SHADER_DESC_IO_IN;
            if (layoutChanges || ((priorWasWrite || currentIsWrite) && hasPipelineAccess)) {
                if (isInsideRenderPassInstance()) {
                    // Contract violation: a real transition cannot be recorded
                    // inside a live render pass. Scream and skip — leave the
                    // tracked state truthful (the transition did not happen).
                    reportBarrierInsideRenderPass(texture, texture->layout, layout, shader);
                    return;
                }
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
            /// Read-after-read in the same layout needs no barrier (see the
            /// Synchronization2 branch above); only a layout transition or a write on
            /// either side is a real hazard.
            const bool layoutChanges  = texture->layout != layout;
            const bool priorWasWrite  = (texture->priorShaderAccess & VK_ACCESS_SHADER_WRITE_BIT) != 0;
            const bool currentIsWrite = ioMode != OMEGASL_SHADER_DESC_IO_IN;
            if (layoutChanges || ((priorWasWrite || currentIsWrite) && hasPipelineAccess)) {
                if (isInsideRenderPassInstance()) {
                    reportBarrierInsideRenderPass(texture, texture->layout, layout, shader);
                    return;
                }
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
            DEBUG_STREAM("[GEVulkan_RP] beginRenderPassIfDeferred: flushed vkCmdBeginRenderPass"
                         << " activeRP=" << (activeRenderPass != VK_NULL_HANDLE ? "yes" : "NO"));
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
                case OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    break;
                }
                case OmegaGTE::GERenderPassDescriptor::ColorAttachment::Load: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    break;
                }
                case OmegaGTE::GERenderPassDescriptor::ColorAttachment::LoadPreserve: {
                    ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    break;
                }
                case OmegaGTE::GERenderPassDescriptor::ColorAttachment::Discard: {
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

            // One acquire per frame. The image is acquired on the frame's first
            // swapchain pass; a split/restarted pass (resumeFrameAfterScratch's
            // LoadPreserve composite) REUSES it. A second acquire while the
            // image is still held is the swapchain-01802 violation that stalls
            // the compositor. present() clears `imageAcquired` once the image
            // returns to the swapchain.
            const bool justAcquired = !nativeTarget->imageAcquired;
            if(justAcquired){
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
                nativeTarget->imageAcquired = true;
            }

            // Attachment 0 falls back to native swapchain image when attachment 0's texture is null.
            if(desc.colorAttachments[0].texture == nullptr){
                attachmentDescription.format = nativeTarget->format;
                // First pass on a freshly-acquired image: discard prior contents
                // (UNDEFINED → forces CLEAR below). A reused image (frame split
                // for a scratch composite) is already mid-frame and lives in
                // PRESENT_SRC from the prior subpass — keep that layout so a
                // LoadPreserve attachment actually preserves the in-progress
                // frame instead of clearing the base render.
                attachmentDescription.initialLayout = justAcquired
                        ? VK_IMAGE_LAYOUT_UNDEFINED
                        : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
        DEBUG_STREAM("[GEVulkan_RP] startRenderPass: attachments=" << attachmentCount
                     << " renderArea=" << beginInfo.renderArea.extent.width
                     << "x" << beginInfo.renderArea.extent.height
                     << " target=" << (desc.nRenderTarget != nullptr ? "swapchain" : "texture")
                     << " deferred=1");
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
            // Pair each retired set with its origin pool (the current
            // fallbackDescriptorPool — the live ring is always single-pool).
            // The release path needs this to call vkFreeDescriptorSets
            // against the pool the set was allocated from, not whatever
            // pool the LATEST pipeline happened to use.
            for(auto s : fallbackDescriptorSets){
                if(s != VK_NULL_HANDLE){
                    retiredFallbackSets.push_back({fallbackDescriptorPool, s});
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
        // Same pool-pairing as the bind-time retirement: the live ring is
        // single-pool (== fallbackDescriptorPool), and the upcoming pipeline
        // will overwrite fallbackDescriptorPool to its own pool. Pair now so
        // the freed-against pool survives the switch.
        for(auto s : fallbackDescriptorSets){
            if(s != VK_NULL_HANDLE){
                retiredFallbackSets.push_back({fallbackDescriptorPool, s});
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
        // Bucket every set by its origin pool. Retired sets carry their
        // origin pool explicitly; the live ring is single-pool (its pool ==
        // fallbackDescriptorPool by invariant — acquireOrUpdateFallbackSet
        // only allocates against the current pipeline's pool and clears the
        // ring on pipeline switch). vkFreeDescriptorSets requires every set
        // in the batch to belong to the pool being passed
        // (VUID-vkFreeDescriptorSets-pDescriptorSets-parent), so we issue
        // one free call per pool.
        std::unordered_map<VkDescriptorPool, OmegaCommon::Vector<VkDescriptorSet>> buckets;
        for(auto & ps : retiredFallbackSets){
            if(ps.first != VK_NULL_HANDLE && ps.second != VK_NULL_HANDLE){
                buckets[ps.first].push_back(ps.second);
            }
        }
        if(fallbackDescriptorPool != VK_NULL_HANDLE){
            for(auto s : fallbackDescriptorSets){
                if(s != VK_NULL_HANDLE){
                    buckets[fallbackDescriptorPool].push_back(s);
                }
            }
        }
        retiredFallbackSets.clear();
        fallbackDescriptorSets.clear();
        fallbackDescriptorPool = VK_NULL_HANDLE;
        if(buckets.empty()){ return; }
        // GPU-safety: ~CB runs only after `flushPendingRetentionUnder`'s
        // `retainShared` lambda fires (the wrapper's submission gate has
        // signaled, GPU is done with this CB), or for an unsubmitted CB
        // there was never any GPU work. So the sets are physically safe
        // to free synchronously here.
        //
        // VVL-safety: at this moment the underlying VkCommandBuffer has
        // transitioned pending → executable but still carries VVL's
        // tracked binding dependencies on every descriptor set this
        // recording bound. Freeing those sets first (and only later
        // resetting the CB in `getAvailableBuffer` for its next reuse)
        // is what causes VUID-vkCmd*-commandBuffer-recording errors to
        // fire on the NEXT recording into this CB even though the next
        // recording begins with its own vkResetCommandBuffer — VVL's
        // invalidation cascade outlives the eager free. The fix is to
        // reset the CB FIRST (executable → initial, all binding
        // dependencies dropped), then free. `getAvailableBuffer` will
        // reset again on the next reuse — idempotent. Bypassing the
        // retention queue here is deliberate: the empty-gate enqueue it
        // previously used fired the free at "next drainCompleted" which
        // was during a still-recording later frame, exactly the timing
        // VVL flagged.
        vkResetCommandBuffer(commandBuffer, 0);
        for(auto & [pool, sets] : buckets){
            if(sets.empty()){ continue; }
            vkFreeDescriptorSets(dev, pool,
                                 static_cast<std::uint32_t>(sets.size()),
                                 sets.data());
        }
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
        writeInfo.descriptorType = getBufferDescriptorTypeForResourceID(id,renderPipelineState->vertexShader->internal);
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

    void GEVulkanCommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned id,
                                                            const TextureSwizzle & swizzle){
        auto vk_texture = (GEVulkanTexture *)texture.get();
        trackTexture(texture);
        /// TODO!

        checkTextureBindAgainstShader(id, renderPipelineState->vertexShader->internal, *vk_texture);

        auto ioMode = getResourceIOModeForResourceID(id,renderPipelineState->vertexShader->internal);

        insertResourceBarrierIfNeeded(vk_texture,id,renderPipelineState->vertexShader->internal);

        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, id, renderPipelineState->vertexShader->internal);

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->vertexShader->internal);
        writeInfo.descriptorCount = 1;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = vk_texture->getOrCreateSwizzledView(effective);
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

    void GEVulkanCommandBuffer::bindResourceAtVertexShader(SharedHandle<GESamplerState> &sampler, unsigned id){
        auto *vk_sampler = (GEVulkanSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(id, renderPipelineState->vertexShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = vk_sampler->sampler;
        imgInfo.imageView = VK_NULL_HANDLE;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->vertexShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = &imgInfo;
        writeInfo.pTexelBufferView = nullptr;

        // Vertex-stage resources live on set 0 (the push set when available).
        if(parentQueue->engine->hasPushDescriptorExt){
            parentQueue->engine->vkCmdPushDescriptorSetKhr(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,renderPipelineState->layout,
                                                           0,1,&writeInfo);
        }
        else {
            VkDescriptorSet target = acquireOrUpdateFallbackSet(0);
            if(target != VK_NULL_HANDLE){
                writeInfo.dstSet = target;
                vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
            }
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
        writeInfo.descriptorType = getBufferDescriptorTypeForResourceID(id,renderPipelineState->fragmentShader->internal);
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

    void GEVulkanCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned id,
                                                              const TextureSwizzle & swizzle){

        auto vk_texture = (GEVulkanTexture *)texture.get();
        trackTexture(texture);

        checkTextureBindAgainstShader(id, renderPipelineState->fragmentShader->internal, *vk_texture);

        auto ioMode = getResourceIOModeForResourceID(id,renderPipelineState->fragmentShader->internal);

        insertResourceBarrierIfNeeded(vk_texture,id,renderPipelineState->fragmentShader->internal);

        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, id, renderPipelineState->fragmentShader->internal);

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->fragmentShader->internal);
        writeInfo.descriptorCount = 1;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = vk_texture->getOrCreateSwizzledView(effective);
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

    void GEVulkanCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GESamplerState> &sampler, unsigned id){
        auto *vk_sampler = (GEVulkanSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(id, renderPipelineState->fragmentShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = vk_sampler->sampler;
        imgInfo.imageView = VK_NULL_HANDLE;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,renderPipelineState->fragmentShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = &imgInfo;
        writeInfo.pTexelBufferView = nullptr;

        // Fragment resources live in set 1 — always the fallback ring (only
        // set 0 may be a push set).
        VkDescriptorSet target = acquireOrUpdateFallbackSet(1);
        if(target != VK_NULL_HANDLE){
            writeInfo.dstSet = target;
            vkUpdateDescriptorSets(parentQueue->engine->device,1,&writeInfo,0,nullptr);
        }
    };



    static VkPrimitiveTopology vulkanTopologyForPolygonType(GECommandBuffer::PolygonType polygonType){
        switch(polygonType){
            case GECommandBuffer::Triangle:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case GECommandBuffer::TriangleStrip:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case GECommandBuffer::Line:
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case GECommandBuffer::LineStrip:
                return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case GECommandBuffer::Point:
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
        // Defensive: when startRenderPass silently early-returned (most
        // commonly `vkAcquireNextImageKHR` returning OUT_OF_DATE on a stale
        // swapchain mid-resize), `activeRenderPass` is VK_NULL_HANDLE and
        // the deferred-begin would never have been set up. Recording
        // vkCmdDraw outside a render pass triggers VVL VUID-vkCmdDraw-
        // renderpass and dereferences a NULL render-pass tracker inside
        // the layer (the SIGSEGV at offset 0x90). The frame is unusable
        // either way — bail before the GPU/validation crash.
        if(activeRenderPass == VK_NULL_HANDLE){
            return;
        }
        // Begin the render pass now — any barriers from bind* calls have
        // already been recorded outside the render pass instance.
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDraw(commandBuffer,vertexCount,1,startIdx,0);
        DEBUG_STREAM("[GEVulkan_RP] drawPolygons: topology=" << (int)polygonType
                     << " vertexCount=" << vertexCount << " startIdx=" << startIdx);
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
        if(activeRenderPass == VK_NULL_HANDLE){ return; }  // see drawPolygons rationale
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDrawIndexed(commandBuffer, indexCount, 1, uint32_t(startIndex), baseVertex, 0);
    }

    void GEVulkanCommandBuffer::drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                      unsigned vertexCount, size_t startIdx,
                                                      unsigned instanceCount, unsigned firstInstance){
        if(activeRenderPass == VK_NULL_HANDLE){ return; }
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDraw(commandBuffer, vertexCount, instanceCount, uint32_t(startIdx), firstInstance);
    }

    void GEVulkanCommandBuffer::drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                             unsigned indexCount, size_t startIndex,
                                                             int baseVertex, unsigned instanceCount,
                                                             unsigned firstInstance){
        if(activeRenderPass == VK_NULL_HANDLE){ return; }
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        applyTopologyIfDynamic(polygonType);
        vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, uint32_t(startIndex), baseVertex, firstInstance);
    }

    void GEVulkanCommandBuffer::drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                     SharedHandle<GEBuffer> & argumentBuffer,
                                                     size_t argumentBufferOffset){
        if(activeRenderPass == VK_NULL_HANDLE){ return; }
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
        if(activeRenderPass == VK_NULL_HANDLE){ return; }
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

    // §2.2 push constant — a pipeline binds at most one block, so test whether
    // a shader declares it (vs scanning for a slot id). Push constants carry
    // no descriptor binding, so this only gates the vkCmdPushConstants stage
    // flags / the no-op when the bound pipeline has none.
    static bool shaderDeclaresPushConstant(const omegasl_shader &shader){
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
        for(auto & l : layoutArr){
            if(l.type == OMEGASL_SHADER_PUSH_CONSTANT_DESC){ return true; }
        }
        return false;
    }

    void GEVulkanCommandBuffer::setRenderConstants(const void *data, unsigned size, unsigned offset){
        assert(renderPipelineState && "setRenderConstants requires a bound render pipeline");
        // Push to exactly the stages that declared `[in pc]`; this must equal
        // the VkPushConstantRange's stageFlags built at pipeline-layout time
        // (the union of using stages), satisfying the per-byte stage rule.
        VkShaderStageFlags stages = 0;
        if(shaderDeclaresPushConstant(renderPipelineState->vertexShader->internal)){
            stages |= VK_SHADER_STAGE_VERTEX_BIT;
        }
        if(shaderDeclaresPushConstant(renderPipelineState->fragmentShader->internal)){
            stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        assert(stages != 0 && "setRenderConstants: bound pipeline declares no `constant<T>` push constant");
        if(stages == 0){ return; }
        vkCmdPushConstants(commandBuffer, renderPipelineState->layout, stages, offset, size, data);
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
        // Negative-height viewport (Vulkan 1.1+ core) flips NDC Y so the
        // rasterizer maps NDC y=+1 to framebuffer top, matching Metal/D3D12.
        // Under the Phase-7 top-left GEViewport convention, the platform-
        // agnostic vertex math `1 - (2y/h)` (used in
        // Composition::emitSdfPrimitive, the bitmap/text inline emitters,
        // and the TE rect tessellator's translateCoords) emits Y-up NDC
        // from top-left input — this viewport flip carries that NDC through
        // to a top-left framebuffer mapping on Vulkan without per-path
        // software flips. All WTK pipelines use cullMode=None, so the
        // implicit winding reversal from the flipped viewport is harmless.
        std::vector<VkViewport> vk_viewports;
        for(auto & v : viewports){
            VkViewport viewport {};
            viewport.x = v.x;
            viewport.y = v.y + v.height;
            viewport.width = v.width;
            viewport.height = -v.height;
            viewport.minDepth = v.nearDepth;
            viewport.maxDepth = v.farDepth;
            DEBUG_STREAM("[GEVulkan_RP] setViewports: in=("
                         << v.x << "," << v.y << "," << v.width << "x" << v.height
                         << ") vk=(" << viewport.x << "," << viewport.y
                         << "," << viewport.width << "x" << viewport.height << ")");
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
        const bool hadActiveRP = (activeRenderPass != VK_NULL_HANDLE);
        const bool wasDeferred = renderPassBeginDeferred;
        beginRenderPassIfDeferred();
        if(activeRenderPass != VK_NULL_HANDLE){
            vkCmdEndRenderPass(commandBuffer);
        }
        activeFramebuffer = VK_NULL_HANDLE;
        activeRenderPass = VK_NULL_HANDLE;
        renderPipelineState = nullptr;
        DEBUG_STREAM("[GEVulkan_RP] finishRenderPass: hadActiveRP=" << (hadActiveRP ? 1 : 0)
                     << " enteredDeferred=" << (wasDeferred ? 1 : 0));
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

    void GEVulkanCommandBuffer::setComputeConstants(const void *data, unsigned size, unsigned offset){
        assert(computePipelineState && "setComputeConstants requires a bound compute pipeline");
        assert(shaderDeclaresPushConstant(computePipelineState->computeShader->internal)
               && "setComputeConstants: bound pipeline declares no `constant<T>` push constant");
        if(!shaderDeclaresPushConstant(computePipelineState->computeShader->internal)){ return; }
        vkCmdPushConstants(commandBuffer, computePipelineState->layout, VK_SHADER_STAGE_COMPUTE_BIT, offset, size, data);
    }

    void GEVulkanCommandBuffer::drawMeshTasks(uint32_t groupCountX,
                                              uint32_t groupCountY,
                                              uint32_t groupCountZ) {
        /// Mesh-Shader-Plan Phase 4a — live `vkCmdDrawMeshTasksEXT`.
        /// The feature gate stays as a defensive front line (the gate
        /// at `makeMeshPipelineState` is the real contract — an
        /// unsupported device returns nullptr there, so the bound PSO
        /// would be null and `isMesh` would assert below first). The
        /// extension function pointer was loaded at device init when
        /// `hasMeshShaderExt` was true; if either is missing the
        /// dispatch is unrecoverable and we bail rather than crash on
        /// a null function-pointer call.
        auto *engine = parentQueue->engine;
        if(!engine->gteDevice->features.hasFeature(GTEDEVICE_FEATURE_MESH_SHADER)){
            DEBUG_STREAM("drawMeshTasks: device does not advertise "
                         "GTEDEVICE_FEATURE_MESH_SHADER");
            return;
        }
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if(!engine->hasMeshShaderExt || engine->vkCmdDrawMeshTasksExt == nullptr){
            DEBUG_STREAM("drawMeshTasks: VK_EXT_mesh_shader is not enabled / vkCmdDrawMeshTasksEXT not loaded");
            return;
        }
        assert(renderPipelineState != nullptr
               && "drawMeshTasks: no pipeline bound (call setRenderPipelineState first)");
        assert(renderPipelineState->isMesh
               && "drawMeshTasks: bound pipeline is a graphics pipeline, not a mesh pipeline. "
                  "Use makeMeshPipelineState to build a mesh-variant PSO.");
        // Mirror the classic draw entry points: startRenderPass defers
        // vkCmdBeginRenderPass so bind-time barriers can land outside the
        // pass, and pending mesh-stage descriptor writes from bindResource*
        // need to flush before dispatch. Without these the validator fires
        // VUID-vkCmdDrawMeshTasksEXT-renderpass.
        beginRenderPassIfDeferred();
        bindDescriptorSetsIfPending();
        engine->vkCmdDrawMeshTasksExt(commandBuffer, groupCountX, groupCountY, groupCountZ);
    #else
        (void)groupCountX; (void)groupCountY; (void)groupCountZ;
        DEBUG_STREAM("drawMeshTasks: Vulkan headers built without VK_EXT_mesh_shader");
    #endif
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

        // Only bind a seed descriptor set in fallback (non-push) mode. In
        // push-descriptor mode set 0 is a push set: bindResourceAtComputeShader
        // records its resources inline via vkCmdPushDescriptorSetKhr, and the
        // pipeline allocates no VkDescriptorSet, so descSet == VK_NULL_HANDLE.
        // Binding a null set here trips VUID-vkCmdBindDescriptorSets-pDescriptorSets-06563
        // (harmless in practice — the push writes supply the real descriptors —
        // but a spec violation). The null guard also covers a shader with no
        // bound resources. Mirrors the push-aware setRenderPipelineState path.
        if(vkPipelineState->descSet != VK_NULL_HANDLE){
            vkCmdBindDescriptorSets(commandBuffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    vkPipelineState->layout,
                                    0,
                                    1,
                                    &vkPipelineState->descSet,
                                    0,
                                    nullptr);
        }
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
        writeInfo.descriptorType = getBufferDescriptorTypeForResourceID(id,computePipelineState->computeShader->internal);
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

    void GEVulkanCommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id,
                                                             const TextureSwizzle & swizzle) {

        auto vk_texture = (GEVulkanTexture *)texture.get();
        trackTexture(texture);

        checkTextureBindAgainstShader(id, computePipelineState->computeShader->internal, *vk_texture);

        insertResourceBarrierIfNeeded(vk_texture,id,computePipelineState->computeShader->internal);

        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, id, computePipelineState->computeShader->internal);

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,computePipelineState->computeShader->internal);
        writeInfo.descriptorCount = 1;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = VK_NULL_HANDLE;
        imgInfo.imageView = vk_texture->getOrCreateSwizzledView(effective);
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

    void GEVulkanCommandBuffer::bindResourceAtComputeShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
        auto *vk_sampler = (GEVulkanSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(id, computePipelineState->computeShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;

        VkDescriptorImageInfo imgInfo {};
        imgInfo.sampler = vk_sampler->sampler;
        imgInfo.imageView = VK_NULL_HANDLE;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkWriteDescriptorSet writeInfo {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writeInfo.dstBinding = getBindingForResourceID(id,computePipelineState->computeShader->internal);
        writeInfo.descriptorCount = 1;
        writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writeInfo.pNext = nullptr;
        writeInfo.dstArrayElement = 0;
        writeInfo.pBufferInfo = nullptr;
        writeInfo.pImageInfo = &imgInfo;
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
        copy.imageSubresource.mipLevel = destRegion.mipLevel;       // §7.1
        copy.imageSubresource.baseArrayLayer = destRegion.arrayLayer; // §7.1
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
        copy.imageSubresource.mipLevel = srcRegion.mipLevel;       // §7.1
        copy.imageSubresource.baseArrayLayer = srcRegion.arrayLayer; // §7.1
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

    void GEVulkanCommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                                 SharedHandle<GETexture> &src,
                                                 SharedHandle<GETexture> &dest) {
        auto *vk_dst = (GEVulkanTexture *)dest.get();
        TextureRegion srcRegion{0,0,0,vk_dst->descriptor.width,vk_dst->descriptor.height,1};
        TextureRegion destRegion = srcRegion;
        blitWithPipeline(pipelineState, src, dest, srcRegion, destRegion);
    }

    void GEVulkanCommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                                 SharedHandle<GETexture> &src,
                                                 SharedHandle<GETexture> &dest,
                                                 const TextureRegion &srcRegion,
                                                 const TextureRegion &destRegion) {
        (void)srcRegion;
        assert(activeRenderPass == VK_NULL_HANDLE && !inBlitPass && !inComputePass &&
               "blitWithPipeline must not be called inside an existing pass scope");
        if(!pipelineState){
            DEBUG_STREAM("blitWithPipeline: pipelineState is null");
            return;
        }
        auto *blitPipe = (GEVulkanBlitPipelineState *)pipelineState.get();
        if(!blitPipe->renderPipeline){
            DEBUG_STREAM("blitWithPipeline: underlying render pipeline is null");
            return;
        }

        TextureRenderTargetDescriptor trtDesc{};
        trtDesc.renderToExistingTexture = true;
        trtDesc.texture = dest;
        auto trtSh = parentQueue->engine->makeTextureRenderTarget(trtDesc);
        if(!trtSh){
            DEBUG_STREAM("blitWithPipeline: makeTextureRenderTarget failed");
            return;
        }

        GERenderPassDescriptor rpDesc{};
        rpDesc.tRenderTarget = trtSh.get();
        rpDesc.colorAttachments.emplace_back(
            GERenderPassDescriptor::ColorAttachment::ClearColor(0.f, 0.f, 0.f, 0.f),
            GERenderPassDescriptor::ColorAttachment::Discard);
        rpDesc.depthStencilAttachment.disabled = true;

        startRenderPass(rpDesc);
        setRenderPipelineState(blitPipe->renderPipeline);
        bindResourceAtFragmentShader(src, 0, TextureSwizzle::identity());
        GEViewport vp{(float)destRegion.x, (float)destRegion.y,
                      (float)destRegion.w, (float)destRegion.h,
                      0.f, 1.f};
        setViewports({vp});
        GEScissorRect sr{(float)destRegion.x, (float)destRegion.y,
                         (float)destRegion.w, (float)destRegion.h};
        setScissorRects({sr});
        drawPolygons(GECommandBuffer::Triangle, 3, 0);
        finishRenderPass();
    }

    void GEVulkanCommandBuffer::reset(){
        // Free fallback descriptor sets before resetting the command buffer
        // — the GPU is assumed to be done with this buffer at reset time.
        releaseFallbackDescriptorSets();
        vkResetCommandBuffer(commandBuffer,VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    };

    void GEVulkanCommandBuffer::setCompletionHandler(const GECommandBufferCompletionHandler & handler){
        completionHandler = handler;
    }

    void GEVulkanCommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence){
        auto buffer = (GEVulkanCommandBuffer *)commandBuffer.get();
        auto fence = (GEVulkanFence *)waitFence.get();
        if(buffer == nullptr || fence == nullptr || fence->event == VK_NULL_HANDLE){
            return;
        }
        // Ordering guard (mirrors D3D12 `lastSignaledValue > 0` / Metal
        // `waitValue > 0`): only wait if the producer actually recorded a
        // vkCmdSetEvent for this event. When the producing render was skipped
        // — e.g. a content-cache hit reuses an already-rendered texture, so no
        // submitCommandBuffer(cb, fence) ran this cycle — the event was never
        // set, and recording vkCmdWaitEvents on a never-set VkEvent is the spec
        // violation (VUID-vkCmdWaitEvents-srcStageMask-parameter) that crashed
        // Vulkan once the content cache was enabled. Nothing to wait for here.
        if(!fence->eventSignalRecorded){
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
        // The set has now been consumed by this wait; the next cycle must
        // observe a fresh producer signal before another wait is recorded.
        fence->eventSignalRecorded = false;
        // Keep the fence (and its VkEvent) alive until this command buffer's
        // submit retires on the GPU — the wait command above binds the event
        // to `buffer`, so the event must outlive the submission. The wrapper is
        // retained under the submit's gate in flushPendingRetentionUnder.
        buffer->trackedFences.push_back(waitFence);
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
        // G.5.1 Vulkan follow-up — stage this buffer's completion handler (if
        // WTK registered one). Its VkCommandBuffer is now queued in
        // `commandQueue`; the handler is gated to a retentionTimeline value
        // when that batch is flushed via vkQueueSubmit (commitToGPU /
        // commitToGPUPresent / commitToGPUAndWait). No-op for the common
        // non-frame CB with no handler.
        stageCompletionHandlerFrom(buffer);
        // CommandQueue-Typed-Pool Phase 3 — mark the pool slot busy
        // immediately at submit time with a sentinel (UINT64_MAX) so the
        // recycler in getAvailableBuffer() doesn't hand the same buffer
        // back before commitToGPU fires. commitToGPU re-stamps with the
        // actual signal value via stampPendingSlots so the GPU's
        // completed counter eventually overtakes the slot. UINT32_MAX
        // means the buffer wasn't from getAvailableBuffer; skip it.
        if (buffer->poolSlot != UINT32_MAX
            && buffer->poolSlot < commandBufferSubmissionIndex.size()) {
            commandBufferSubmissionIndex[buffer->poolSlot] = UINT64_MAX;
        }
        pendingSlots.push_back(buffer->poolSlot);
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
            // Mark the CPU-side signal so a later notifyCommandBuffer knows the
            // event will actually be set and may safely record its wait.
            fence->eventSignalRecorded = true;
            // Keep the fence (and its VkEvent) alive until this producer
            // submit retires — the set command binds the event to `buffer`.
            buffer->trackedFences.push_back(signalFence);
        }
        commandQueue.push_back(buffer->commandBuffer);
        // G.5.1 Vulkan follow-up — stage this buffer's completion handler, same
        // as the unsignaled overload above. WTK only registers handlers on the
        // frame CB (submitted via the single-arg path), so this is normally a
        // no-op here, but keep the contract general.
        stageCompletionHandlerFrom(buffer);
        // CommandQueue-Typed-Pool Phase 3 — same slot busy-stamp as the
        // unsignaled submit overload above. See that comment for why.
        if (buffer->poolSlot != UINT32_MAX
            && buffer->poolSlot < commandBufferSubmissionIndex.size()) {
            commandBufferSubmissionIndex[buffer->poolSlot] = UINT64_MAX;
        }
        pendingSlots.push_back(buffer->poolSlot);
        pendingRetainedBuffers.push_back(commandBuffer);
    }

   SharedHandle<GECommandBuffer> GEVulkanCommandQueue::getAvailableBuffer(){
       // CommandQueue-Typed-Pool Phase 3 — growable pool with in-flight
       // tracking via the retention timeline semaphore. The flow:
       //   1. Read the most recent submission counter value the GPU has
       //      already finished. Slots whose last submission is <= that
       //      value are safe to recycle without any further wait.
       //   2. Walk the pool starting from currentBufferIndex for locality;
       //      return the first free slot.
       //   3. If every slot is still in flight, allocate a new
       //      VkCommandBuffer from the pool's command pool (one at a time)
       //      up to a hard ceiling of 256. Soft-warn exactly once when the
       //      pool first grows past 4× the initial hint so a user who
       //      under-sized the hint sees the signal but not the spam.
       //   4. At the ceiling, return nullptr — the caller has 256
       //      simultaneously in-flight buffers, which is almost certainly
       //      a leak somewhere in the user's submit/commit pairing.
       // `kPoolCeiling` is the queue-wide member the timestamp query pool is also
       // sized against, so both stay in lockstep at 256.

       // G.5.1 Vulkan follow-up — fire any frame-completion handlers the GPU
       // has already retired before this frame acquires a buffer, so the
       // compositor's drainCompletedBufferReleases (which runs just after, at
       // beginFrame) sees the freshly-flipped `done` flags and recycles the
       // pooled scratch buffers. Cheap + safe: single GetSemaphoreCounterValue
       // on the same compositor thread, early-out when nothing is gated.
       pollCompletions();

       if(commandBuffers.empty()){
           return nullptr;
       }

       std::uint64_t completed = 0;
       if (engine != nullptr && engine->device != VK_NULL_HANDLE &&
           retentionTimeline != VK_NULL_HANDLE) {
           // Failure leaves completed = 0 — every "never-submitted" slot
           // still qualifies as free, so the first cycle through a fresh
           // queue still works without a semaphore query.
           vkGetSemaphoreCounterValue(engine->device, retentionTimeline, &completed);
       }

       const std::uint32_t poolSize = static_cast<std::uint32_t>(commandBuffers.size());
       if (currentBufferIndex >= poolSize) {
           currentBufferIndex = 0;
       }

       std::uint32_t chosenSlot = UINT32_MAX;
       for (std::uint32_t step = 0; step < poolSize; ++step) {
           const std::uint32_t slot = (currentBufferIndex + step) % poolSize;
           if (commandBufferSubmissionIndex[slot] <= completed) {
               chosenSlot = slot;
               break;
           }
       }

       if (chosenSlot == UINT32_MAX) {
           // No free slot — grow if we can.
           if (poolSize >= kPoolCeiling) {
               std::cerr << "GEVulkanCommandQueue: pool at ceiling (" << kPoolCeiling
                         << ") and every slot is still in flight — refusing to grow; "
                            "check for missed commitToGPU / over-submission." << std::endl;
               return nullptr;
           }
           VkCommandBufferAllocateInfo allocInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
           allocInfo.pNext = nullptr;
           allocInfo.commandPool = commandPool;
           allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
           allocInfo.commandBufferCount = 1;
           VkCommandBuffer fresh = VK_NULL_HANDLE;
           const auto allocRes = vkAllocateCommandBuffers(engine->device, &allocInfo, &fresh);
           if (allocRes != VK_SUCCESS || fresh == VK_NULL_HANDLE) {
               std::cerr << "GEVulkanCommandQueue: vkAllocateCommandBuffers (grow) failed ("
                         << allocRes << ")" << std::endl;
               return nullptr;
           }
           commandBuffers.push_back(fresh);
           commandBufferSubmissionIndex.push_back(0);
           chosenSlot = poolSize;  // old size == new slot index
           if (!poolGrowthWarned && initialBufferHint > 0
               && commandBuffers.size() > 4ull * initialBufferHint) {
               std::cerr << "GEVulkanCommandQueue: pool grew to " << commandBuffers.size()
                         << " (initial hint=" << initialBufferHint
                         << "); consider raising desc.maxBufferCount on this queue."
                         << std::endl;
               poolGrowthWarned = true;
           }
       }

       auto &commandBuffer = commandBuffers[chosenSlot];
       const auto resetRes = vkResetCommandBuffer(commandBuffer, 0);
       if (resetRes != VK_SUCCESS) {
           std::cerr << "Vulkan reset command buffer failed (" << resetRes << ")" << std::endl;
       }
       auto res = std::make_shared<GEVulkanCommandBuffer>(commandBuffer, this);
       res->poolSlot = chosenSlot;
       // GPU Commit-Timing P1 — the ctor above already ran vkBeginCommandBuffer,
       // so the buffer is recording and the slot is known: arm its start
       // timestamp as the first command (resets + writes this slot's start
       // query). No-op when timing is disabled.
       writeStartTimestamp(commandBuffer, chosenSlot);
       currentBufferIndex = (chosenSlot + 1) % static_cast<std::uint32_t>(commandBuffers.size());
       return res;
   };

    VkCommandBuffer &GEVulkanCommandQueue::getLastCommandBufferInQueue() {
        return commandQueue.back();
    }

   void GEVulkanCommandQueue::stampPendingSlots(std::uint64_t signalValue){
       for (auto slot : pendingSlots) {
           if (slot != UINT32_MAX && slot < commandBufferSubmissionIndex.size()) {
               commandBufferSubmissionIndex[slot] = signalValue;
           }
       }
       pendingSlots.clear();
   }

   void GEVulkanCommandQueue::writeStartTimestamp(VkCommandBuffer cmd, std::uint32_t slot){
       // GPU Commit-Timing P1 — start of the buffer's GPU span. Recorded as the
       // first command on a freshly-begun command buffer (outside any render
       // pass), so it brackets the whole execution. The pair must be reset before
       // reuse; vkCmdResetQueryPool here is legal because nothing has been
       // recorded yet.
       if (!timestampsEnabled || cmd == VK_NULL_HANDLE
           || slot == UINT32_MAX || slot >= kPoolCeiling) {
           return;
       }
       vkCmdResetQueryPool(cmd, timestampPool, slot * 2, 2);
       vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool, slot * 2);
   }

   void GEVulkanCommandQueue::writeEndTimestampAndResolve(VkCommandBuffer cmd, std::uint32_t slot){
       // GPU Commit-Timing P1 — end of the buffer's GPU span. Recorded just
       // before vkEndCommandBuffer (after every user pass has finished, so we are
       // outside any render-pass instance). BOTTOM_OF_PIPE so the timestamp lands
       // once all prior work in this buffer has completed. Unlike D3D12 there is
       // no resolve command — pollCompletions reads the result host-side after
       // the buffer's retentionTimeline value retires.
       if (!timestampsEnabled || cmd == VK_NULL_HANDLE
           || slot == UINT32_MAX || slot >= kPoolCeiling) {
           return;
       }
       vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool, slot * 2 + 1);
   }

   bool GEVulkanCommandQueue::resolveSlotTiming(std::uint32_t slot,
                                                double &startSec, double &endSec) const {
       if (!timestampsEnabled || timestampPool == VK_NULL_HANDLE || timestampPeriodNs <= 0.0
           || engine == nullptr || engine->device == VK_NULL_HANDLE
           || slot == UINT32_MAX || slot >= kPoolCeiling) {
           return false;
       }
       std::uint64_t ticks[2] = {0, 0};
       // No WAIT bit: the caller only reads a slot whose submission has already
       // retired on the GPU, so the results are available; treat any non-success
       // (incl. VK_NOT_READY) as "no usable timing" and leave the fields at 0.0.
       const VkResult r = vkGetQueryPoolResults(engine->device, timestampPool, slot * 2, 2,
                                                sizeof(ticks), ticks, sizeof(std::uint64_t),
                                                VK_QUERY_RESULT_64_BIT);
       if (r != VK_SUCCESS) {
           return false;
       }
       const std::uint64_t startTicks = ticks[0] & timestampMask;
       const std::uint64_t endTicks   = ticks[1] & timestampMask;
       // A slot whose buffer recorded no timestamps, or a pair not written this
       // cycle, leaves stale / zero ticks. Reject any non-increasing pair so it
       // can't poison the aggregator's min/max fold.
       if (endTicks <= startTicks) {
           return false;
       }
       const double secPerTick = timestampPeriodNs * 1e-9;
       startSec = static_cast<double>(startTicks) * secPerTick;
       endSec   = static_cast<double>(endTicks) * secPerTick;
       return true;
   }

   void GEVulkanCommandQueue::stageCompletionHandlerFrom(GEVulkanCommandBuffer *cb){
       // G.5.1 Vulkan follow-up. Move the handler out of the command buffer so
       // the wrapper can be recycled/destroyed without taking the callback with
       // it; the queue now owns firing it. No-op for the common (non-frame) CB
       // that never had a handler registered.
       if (cb != nullptr && cb->completionHandler) {
           // GPU Commit-Timing P1 — carry the buffer's pool slot so
           // pollCompletions can read its resolved timestamps when this handler
           // fires.
           PendingCompletion pending;
           pending.handler  = std::move(cb->completionHandler);
           pending.poolSlot = cb->poolSlot;
           {
               std::lock_guard<std::mutex> lock(completionMutex_);
               stagedCompletionHandlers_.push_back(std::move(pending));
           }
           cb->completionHandler = nullptr;
       }
   }

   void GEVulkanCommandQueue::gateStagedCompletions(std::uint64_t signalValue){
       // Bind every staged handler to the retentionTimeline value just signaled
       // for the vkQueueSubmit that ran its command buffer. Once
       // vkGetSemaphoreCounterValue reaches `signalValue` the GPU is done with
       // that submission and pollCompletions can fire the handler.
       std::lock_guard<std::mutex> lock(completionMutex_);
       for (auto & pending : stagedCompletionHandlers_) {
           gatedCompletionHandlers_.emplace_back(signalValue, std::move(pending));
       }
       stagedCompletionHandlers_.clear();
   }

   void GEVulkanCommandQueue::pollCompletions(){
       // Collect the ready handlers and compact the survivors in one stable pass
       // *under the lock*, then fire outside it. Holding the lock only for the
       // container mutation keeps the frame path cheap and lets a fired handler
       // call back into the queue without self-deadlocking on the non-recursive
       // mutex (the waiter thread and the frame thread can both reach here once an
       // async commit has armed the waiter).
       std::vector<PendingCompletion> ready;
       {
           std::lock_guard<std::mutex> lock(completionMutex_);
           if (gatedCompletionHandlers_.empty()) {
               return;
           }
           std::uint64_t completed = 0;
           if (engine != nullptr && engine->device != VK_NULL_HANDLE &&
               retentionTimeline != VK_NULL_HANDLE) {
               // Failure leaves completed = 0 — no handler fires this pass, the
               // same defensive read getAvailableBuffer's recycler already
               // relies on.
               vkGetSemaphoreCounterValue(engine->device, retentionTimeline, &completed);
           }
           std::size_t writeIdx = 0;
           for (std::size_t readIdx = 0; readIdx < gatedCompletionHandlers_.size(); ++readIdx) {
               auto & entry = gatedCompletionHandlers_[readIdx];
               if (entry.first <= completed) {
                   if (entry.second.handler) {
                       ready.push_back(std::move(entry.second));
                   }
               } else {
                   if (writeIdx != readIdx) {
                       gatedCompletionHandlers_[writeIdx] = std::move(gatedCompletionHandlers_[readIdx]);
                   }
                   ++writeIdx;
               }
           }
           gatedCompletionHandlers_.resize(writeIdx);
           if (ready.empty()) {
               return;
           }
       }
       // Vulkan has no per-command-buffer success/fail status on this path; a
       // failed GPU execution surfaces as device loss on a later call. Report
       // Completed and fold in each buffer's own resolved GPU span.
       // resolveSlotTiming leaves the fields at 0.0 (the degraded contract) when
       // timing is disabled, the slot was untracked, or the buffer recorded no
       // usable timestamp pair. Matches the Metal / D3D12 fire-once contract.
       for (auto & entry : ready) {
           GECommandBufferCompletionInfo info {};
           info.status = GECommandBufferCompletionInfo::CompletionStatus::Completed;
           resolveSlotTiming(entry.poolSlot, info.gpuStartTimeSec, info.gpuEndTimeSec);
           entry.handler(info);
       }
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
            // CommandQueue-Typed-Pool Phase 3 — drop any slot tags that
            // were enqueued but never made it to a real submit (e.g. caller
            // submitted then committed with no work). Without this the
            // slots would stay flagged at UINT32_MAX-ish stale values.
            pendingSlots.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }
        for(std::size_t i = 0; i < commandQueue.size(); ++i){
            auto cb = commandQueue[i];
            // GPU Commit-Timing P1 — end timestamp before Close; `pendingSlots`
            // is parallel to `commandQueue`.
            const std::uint32_t slot = i < pendingSlots.size() ? pendingSlots[i] : UINT32_MAX;
            writeEndTimestampAndResolve(cb, slot);
            auto endRes = vkEndCommandBuffer(cb);
            if(endRes != VK_SUCCESS){
                std::cerr << "Vulkan end command buffer failed (" << endRes << ")" << std::endl;
                commandQueue.clear();
                pendingSlots.clear();
                submittedTraceCommandBufferIds.clear();
                return;
            }
        }
        // CommandQueue-Typed-Pool Phase 2 — submit through this queue's own
        // (familyIndex, VkQueue) resolved at construction, NOT through
        // `engine->deviceQueuefamilies.front().front()`. The latter would
        // collapse every typed queue back onto family 0 and defeat the
        // whole point of opening multiple families. `nativeQueue` is
        // VK_NULL_HANDLE only when construction couldn't resolve a family
        // (e.g. requireDedicated with no dedicated family available), in
        // which case OmegaGraphicsEngine::makeCommandQueue should have
        // returned nullptr to the caller — but defend against use anyway.
        if(engine == nullptr || nativeQueue == VK_NULL_HANDLE){
            commandQueue.clear();
            pendingSlots.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto vkQueue = nativeQueue;

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
           // G.5.1 Vulkan follow-up — abandon this batch's staged handlers
           // alongside its retained buffers (the GPU never ran the work, so
           // WTK's pooled buffers stay in pendingReleaseBatches_ and return to
           // the pool at context teardown — the no-recycling fallback, no leak).
           {
               std::lock_guard<std::mutex> lock(completionMutex_);
               stagedCompletionHandlers_.clear();
           }
           pendingSlots.clear();
           submittedTraceCommandBufferIds.clear();
           return;
       };
       ++nextSubmitValue;
       // CommandQueue-Typed-Pool Phase 3 — stamp the new submission counter
       // onto every pool slot that contributed a buffer to this submit.
       // After this point getAvailableBuffer() can recycle those slots as
       // soon as vkGetSemaphoreCounterValue(retentionTimeline) reaches
       // nextSubmitValue.
       stampPendingSlots(nextSubmitValue);
       // G.5.1 Vulkan follow-up — bind every staged completion handler to this
       // submit's signal value; pollCompletions fires them once the GPU reaches
       // it. A staged handler always implies a non-empty commandQueue (both are
       // set in submitCommandBuffer), so this gates exactly the buffers just
       // flushed.
       gateStagedCompletions(nextSubmitValue);

       commandQueue.clear();
       flushPendingRetentionUnder(gate);
       engine->retentionQueue.drainCompleted();
       // G.5.1 Vulkan follow-up — fire any handlers the GPU has already retired.
       pollCompletions();
   };

   void GEVulkanCommandQueue::commitToGPUPresent(VkPresentInfoKHR *info){
        if(info == nullptr){
            submittedTraceCommandBufferIds.clear();
            return;
        }
        // CommandQueue-Typed-Pool Phase 2 — submit through this queue's own
        // (familyIndex, VkQueue) resolved at construction, NOT through
        // `engine->deviceQueuefamilies.front().front()`. The latter would
        // collapse every typed queue back onto family 0 and defeat the
        // whole point of opening multiple families. `nativeQueue` is
        // VK_NULL_HANDLE only when construction couldn't resolve a family
        // (e.g. requireDedicated with no dedicated family available), in
        // which case OmegaGraphicsEngine::makeCommandQueue should have
        // returned nullptr to the caller — but defend against use anyway.
        if(engine == nullptr || nativeQueue == VK_NULL_HANDLE){
            commandQueue.clear();
            pendingSlots.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto vkQueue = nativeQueue;

        // Two flows reach here after the queue-decoupling refactor:
        //   (a) Caller submitted CBs but has not yet committed — flush them
        //       through submitFence so they finish before present.
        //   (b) Caller already called commitToGPU() and the internal queue
        //       is empty — just sync the present queue so prior submissions
        //       are drained, then present.
        if(!commandQueue.empty()){
            for(std::size_t i = 0; i < commandQueue.size(); ++i){
                auto cb = commandQueue[i];
                // GPU Commit-Timing P1 — end timestamp before Close;
                // `pendingSlots` is parallel to `commandQueue`.
                const std::uint32_t slot = i < pendingSlots.size() ? pendingSlots[i] : UINT32_MAX;
                writeEndTimestampAndResolve(cb, slot);
                auto endRes = vkEndCommandBuffer(cb);
                if(endRes != VK_SUCCESS){
                    std::cerr << "Vulkan end command buffer failed (" << endRes << ")" << std::endl;
                    commandQueue.clear();
                    submittedTraceCommandBufferIds.clear();
                    return;
                }
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
                // G.5.1 Vulkan follow-up — abandon staged handlers with the
                // retained buffers (see commitToGPU for the rationale).
                {
                    std::lock_guard<std::mutex> lock(completionMutex_);
                    stagedCompletionHandlers_.clear();
                }
                pendingSlots.clear();
                submittedTraceCommandBufferIds.clear();
                return;
            }
            ++nextSubmitValue;
            // CommandQueue-Typed-Pool Phase 3 — see commitToGPU above for
            // why this lands here. commitToGPUPresent waits on submitFence
            // below, so by the time control returns to the caller these
            // slots are also reachable via the fence wait, but the
            // submission-index path is the canonical signal.
            stampPendingSlots(nextSubmitValue);
            // G.5.1 Vulkan follow-up — gate this batch's staged completion
            // handlers to the submit value; the vkWaitForFences below makes the
            // GPU finish, so the pollCompletions at the end of this function
            // fires them this same call (the next frame's drain then recycles).
            gateStagedCompletions(nextSubmitValue);
            auto waitRes = vkWaitForFences(engine->device,1,&submitFence,VK_TRUE,UINT64_MAX);
            if(waitRes != VK_SUCCESS){
                std::cerr << "Failed waiting for submitted command buffers (" << waitRes << ")" << std::endl;
                commandQueue.clear();
                flushPendingRetentionUnder(gate);
                submittedTraceCommandBufferIds.clear();
                return;
            }
            flushPendingRetentionUnder(gate);
            engine->retentionQueue.drainCompleted();
        }
        else {
            // Decoupled flow: commitToGPU() already submitted the draw work.
            // We need the swapchain image's RENDER_TARGET → PRESENT_SRC_KHR
            // transition (done as the render-pass finalLayout) to have
            // completed before vkQueuePresentKHR. waitSemaphoreCount on
            // the present info is 0, so synchronize on the queue itself.
            auto idleRes = vkQueueWaitIdle(vkQueue);
            if(idleRes != VK_SUCCESS){
                std::cerr << "vkQueueWaitIdle before present failed (" << idleRes << ")" << std::endl;
            }
            engine->retentionQueue.drainCompleted();
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
        // G.5.1 Vulkan follow-up — fire any frame-completion handlers the GPU
        // has retired (the non-empty branch's vkWaitForFences / the decoupled
        // branch's vkQueueWaitIdle have already drained this frame's work), so
        // the next beginFrame's drainCompletedBufferReleases recycles its pooled
        // scratch buffers. pending stays bounded at the in-flight frame count.
        pollCompletions();

   }

   void GEVulkanCommandQueue::commitToGPUAndWait(){
        if(commandQueue.empty()){
            pendingSlots.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }
        for(std::size_t i = 0; i < commandQueue.size(); ++i){
            auto cb = commandQueue[i];
            // GPU Commit-Timing P1 — end timestamp before Close; `pendingSlots`
            // is parallel to `commandQueue`.
            const std::uint32_t slot = i < pendingSlots.size() ? pendingSlots[i] : UINT32_MAX;
            writeEndTimestampAndResolve(cb, slot);
            auto endRes = vkEndCommandBuffer(cb);
            if(endRes != VK_SUCCESS){
                std::cerr << "Vulkan end command buffer failed (" << endRes << ")" << std::endl;
                commandQueue.clear();
                pendingSlots.clear();
                submittedTraceCommandBufferIds.clear();
                return;
            }
        }
        // CommandQueue-Typed-Pool Phase 2 — submit through this queue's own
        // (familyIndex, VkQueue) resolved at construction, NOT through
        // `engine->deviceQueuefamilies.front().front()`. The latter would
        // collapse every typed queue back onto family 0 and defeat the
        // whole point of opening multiple families. `nativeQueue` is
        // VK_NULL_HANDLE only when construction couldn't resolve a family
        // (e.g. requireDedicated with no dedicated family available), in
        // which case OmegaGraphicsEngine::makeCommandQueue should have
        // returned nullptr to the caller — but defend against use anyway.
        if(engine == nullptr || nativeQueue == VK_NULL_HANDLE){
            commandQueue.clear();
            pendingSlots.clear();
            submittedTraceCommandBufferIds.clear();
            return;
        }

        auto vkQueue = nativeQueue;

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
           // G.5.1 Vulkan follow-up — abandon staged handlers with the
           // retained buffers (see commitToGPU for the rationale).
           {
               std::lock_guard<std::mutex> lock(completionMutex_);
               stagedCompletionHandlers_.clear();
           }
           pendingSlots.clear();
           submittedTraceCommandBufferIds.clear();
           return;
       };
       ++nextSubmitValue;
       // CommandQueue-Typed-Pool Phase 3 — stamp slots before the
       // synchronous wait below. After vkWaitForFences returns success
       // every slot stamped here is already recyclable.
       stampPendingSlots(nextSubmitValue);
       // G.5.1 Vulkan follow-up — gate staged handlers to this submit value;
       // the vkWaitForFences below guarantees the GPU has reached it by the
       // time the pollCompletions at the end of this function runs.
       gateStagedCompletions(nextSubmitValue);

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
       // G.5.1 Vulkan follow-up — the vkWaitForFences above has retired this
       // batch, so fire its gated completion handlers now (used by blit /
       // upload paths such as BitmapTextureCache; the frame path goes through
       // commitToGPU + commitToGPUPresent).
       pollCompletions();


   }

   void GEVulkanCommandQueue::stageCommitAggregator(const GECommitCompletionHandler & onComplete){
       // GPU Commit-Timing P1 — the P2 aggregator (installCommitAggregator) sets
       // a fold handler on each buffer in the pending batch; we then stage those
       // buffers so each freshly-set handler is staged with the buffer's pool
       // slot and gated to the following commit's submit. pollCompletions later
       // fires each with its resolved per-buffer GPU span, and the aggregator
       // fires `onComplete` once with the whole-batch span.
       //
       // The handlers are staged here, at *commit* time, deliberately separate
       // from the WTK recycler handler that submitCommandBuffer already staged at
       // submit time — both gate to the same submit and fire independently, so
       // this composes with the recycler rather than clobbering it.
       if(!onComplete){
           return;
       }
       installCommitAggregator(pendingRetainedBuffers, onComplete);
       for(auto & handle : pendingRetainedBuffers){
           stageCompletionHandlerFrom(static_cast<GEVulkanCommandBuffer *>(handle.get()));
       }
   }

   void GEVulkanCommandQueue::commitToGPU(const GECommitCompletionHandler & onComplete){
       // GPU Commit-Timing P1 — async commit timing. Install + stage the
       // aggregator onto the pending batch, commit it, then (structural fix #3)
       // arm the waiter thread so the gated handler fires once the GPU retires
       // this commit — there is no frame loop to re-pump the poller for a
       // standalone async commit. installCommitAggregator handles the empty-batch
       // case by firing `onComplete` synchronously; armCompletionWaiter then
       // no-ops because nothing is gated.
       stageCommitAggregator(onComplete);
       commitToGPU();
       if(onComplete){
           armCompletionWaiter();
       }
   }

   GECommitCompletionInfo GEVulkanCommandQueue::commitToGPUAndWaitTimed(){
       // GPU Commit-Timing P1 — sync counterpart. Unlike D3D12 this does NOT
       // route through commitToGPU(): Vulkan's commitToGPUAndWait early-returns
       // on an empty commandQueue, so committing first would leave it a no-op
       // that never drains or polls. Instead install + stage the aggregator and
       // drive commitToGPUAndWait directly — it submits the batch, blocks on
       // submitFence (GPU idle), gates the staged handlers, and its terminal
       // pollCompletions fires the aggregator on this thread before returning.
       // No waiter thread for the synchronous one-shot path.
       GECommitCompletionInfo result {};
       stageCommitAggregator([&result](const GECommitCompletionInfo & info) { result = info; });
       commitToGPUAndWait();
       return result;
   }

   void GEVulkanCommandQueue::ensureCompletionWaiter(){
       std::lock_guard<std::mutex> lock(completionMutex_);
       if(waiterStarted_){
           return;
       }
       waiterStarted_ = true;
       completionWaiter_ = std::thread(&GEVulkanCommandQueue::completionWaiterLoop, this);
   }

   void GEVulkanCommandQueue::armCompletionWaiter(){
       {
           // Nothing gated means the handler already fired synchronously — an
           // empty batch (installCommitAggregator's count==0 fast path) or a GPU
           // that finished before commitToGPU()'s own pollCompletions ran. Don't
           // start a thread we'd never need.
           std::lock_guard<std::mutex> lock(completionMutex_);
           if(gatedCompletionHandlers_.empty()){
               return;
           }
       }
       ensureCompletionWaiter();
       {
           std::lock_guard<std::mutex> lock(completionMutex_);
           if(nextSubmitValue > waiterTargetValue_){
               waiterTargetValue_ = nextSubmitValue;
           }
           waiterArmed_ = true;
       }
       waiterCv_.notify_one();
   }

   void GEVulkanCommandQueue::completionWaiterLoop(){
       for(;;){
           std::uint64_t target;
           {
               std::unique_lock<std::mutex> lock(completionMutex_);
               waiterCv_.wait(lock, [this]{ return waiterStop_ || waiterArmed_; });
               if(waiterStop_){
                   break;
               }
               waiterArmed_ = false;
               target = waiterTargetValue_;
           }
           // Host-wait (lock released) for the GPU to retire `target`. The
           // timeline counter is monotonic and can't be signalled backward, so we
           // can't unblock an infinite wait on teardown — instead wait on a
           // bounded (50 ms) timeout and re-check the stop flag between waits.
           // vkWaitSemaphores / vkGetSemaphoreCounterValue are free-threaded
           // w.r.t. the frame thread's vkQueueSubmit signaling.
           if(engine != nullptr && engine->device != VK_NULL_HANDLE
              && retentionTimeline != VK_NULL_HANDLE){
               for(;;){
                   std::uint64_t cur = 0;
                   vkGetSemaphoreCounterValue(engine->device, retentionTimeline, &cur);
                   if(cur >= target){
                       break;
                   }
                   {
                       std::lock_guard<std::mutex> lock(completionMutex_);
                       if(waiterStop_){
                           break;
                       }
                   }
                   VkSemaphoreWaitInfo waitInfo {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                   waitInfo.pNext = nullptr;
                   waitInfo.flags = 0;
                   waitInfo.semaphoreCount = 1;
                   waitInfo.pSemaphores = &retentionTimeline;
                   waitInfo.pValues = &target;
                   vkWaitSemaphores(engine->device, &waitInfo, 50ull * 1000ull * 1000ull);
               }
           }
           {
               std::lock_guard<std::mutex> lock(completionMutex_);
               if(waiterStop_){
                   break;
               }
           }
           // pollCompletions takes completionMutex_ itself, so it must be called
           // without holding it here.
           pollCompletions();
       }
   }

   GEVulkanCommandQueue::GEVulkanCommandQueue(GEVulkanEngine *engine, const GECommandQueueDesc & desc):
       // Provisionally tell the base ctor that we achieved exactly what
       // the user asked for; if Pick downgrades below, the body patches
       // `desc_.type` to the actual achieved type. That keeps the family
       // picker a single call instead of resolving twice in the
       // initializer list.
       GECommandQueue(desc, /*achievedType=*/desc.type){
       this->engine = engine;
       VkResult res;

       auto picked = VulkanQueueFamilies::Pick(
           engine->queueFamilyProps.data(),
           static_cast<std::uint32_t>(engine->queueFamilyProps.size()),
           desc.type,
           desc.requireDedicated);
       if (!picked) {
           // No family satisfies the request. Leave nativeQueue at
           // VK_NULL_HANDLE; the engine factory checks for that and
           // returns nullptr to the caller. We still need to construct
           // the trace ID so the destructor's emit path is consistent.
           DEBUG_STREAM("GEVulkanCommandQueue: no queue family satisfies type=" << static_cast<int>(desc.type)
                       << " (requireDedicated=" << desc.requireDedicated << ")");
           traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
           return;
       }

       // Patch the achieved type onto the base if Pick downgraded — the
       // initializer list always passes `desc.type` as the optimistic
       // achievement, but the picker's fallback ladder may have routed us
       // to a less-specific family (e.g. Transfer → Compute when no
       // dedicated DMA family exists). isDedicated() reads `desc_.type`
       // against `requestedType_`, so writing the achieved value here is
       // what makes the downgrade observable to callers.
       desc_.type = picked->achievedType;

       boundFamilyIndex = picked->familyIndex;
       // CommandQueue-Typed-Pool follow-up — route through the engine's
       // priority-aware lookup. When VK_KHR_global_priority is enabled,
       // this picks the VkQueue created with the matching global priority
       // (with HIGH/MEDIUM/LOW fallback); when the extension is disabled,
       // it falls through to the single default-priority queue. REALTIME
       // requests resolve to HIGH (or MEDIUM) because we don't open
       // REALTIME VkQueues at device-create — the entitlement gate makes
       // that an opt-in concern that lives outside this path.
       const VkQueueGlobalPriorityKHR wantedKhrPrio = [&]() {
           switch (desc.priority) {
               case GECommandQueueDesc::Priority::Low:      return VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
               case GECommandQueueDesc::Priority::High:     return VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
               case GECommandQueueDesc::Priority::Realtime: return VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR;
               case GECommandQueueDesc::Priority::Normal:
               default:                                     return VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
           }
       }();
       nativeQueue = engine->lookupQueueOnFamily(
           boundFamilyIndex, static_cast<std::int32_t>(wantedKhrPrio));
       if (nativeQueue == VK_NULL_HANDLE) {
           DEBUG_STREAM("GEVulkanCommandQueue: family " << boundFamilyIndex
                       << " was not opened at device-create time; no VkQueue handle");
           traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
           return;
       }

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

       VkCommandPoolCreateInfo poolCreateInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
       poolCreateInfo.queueFamilyIndex = boundFamilyIndex;
       poolCreateInfo.pNext = nullptr;
       poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

       res = vkCreateCommandPool(engine->device,&poolCreateInfo,nullptr,&commandPool);

       if(!VK_RESULT_SUCCEEDED(res)){
           exit(1);
       };

       VkCommandBufferAllocateInfo commandBufferCreateInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
       commandBufferCreateInfo.commandBufferCount = desc.maxBufferCount;
       commandBufferCreateInfo.commandPool = commandPool;
       commandBufferCreateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
       commandBufferCreateInfo.pNext = nullptr;
       commandBuffers.resize(desc.maxBufferCount);

       res = vkAllocateCommandBuffers(engine->device,&commandBufferCreateInfo,commandBuffers.data());

       if(!VK_RESULT_SUCCEEDED(res)){
           exit(1);
       };

       // CommandQueue-Typed-Pool Phase 3 — initial state for the growable
       // pool: every slot's "last submitted at" counter is 0 ("never
       // submitted"), so the first `getAvailableBuffer()` returns slot 0
       // immediately without polling the GPU. `initialBufferHint` is
       // sticky for the 4×-hint soft-warn threshold.
       commandBufferSubmissionIndex.resize(desc.maxBufferCount, 0);
       initialBufferHint = desc.maxBufferCount;

       // GPU Commit-Timing P1 — per-buffer GPU timestamp infrastructure. Timing
       // is available iff the bound queue family reports a non-zero
       // `timestampValidBits` and the device a positive period; on Vulkan that
       // covers graphics / compute / transfer families alike (no COPY carve-out
       // like D3D12). Sized to `kPoolCeiling` (2 queries per slot) once so the
       // pool never needs recreating as the command-buffer pool grows. Disable
       // silently — keeping the documented zero-timing fallback — on any failure.
       {
           VkPhysicalDeviceProperties props {};
           vkGetPhysicalDeviceProperties(engine->physicalDevice, &props);
           const std::uint32_t validBits =
               boundFamilyIndex < engine->queueFamilyProps.size()
                   ? engine->queueFamilyProps[boundFamilyIndex].timestampValidBits
                   : 0u;
           if(validBits > 0 && props.limits.timestampPeriod > 0.0f){
               VkQueryPoolCreateInfo qpInfo {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
               qpInfo.pNext = nullptr;
               qpInfo.flags = 0;
               qpInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
               qpInfo.queryCount = kPoolCeiling * 2;
               qpInfo.pipelineStatistics = 0;
               VkQueryPool pool = VK_NULL_HANDLE;
               if(vkCreateQueryPool(engine->device, &qpInfo, nullptr, &pool) == VK_SUCCESS){
                   timestampPool     = pool;
                   timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
                   timestampMask     = (validBits >= 64)
                                           ? ~std::uint64_t(0)
                                           : ((std::uint64_t(1) << validBits) - 1);
                   timestampsEnabled = true;
               }
           }
       }

       // Apply desc.label via VK_EXT_debug_utils when supplied. The
       // setName() override below uses the same call shape for user-driven
       // post-construction renames — kept in lockstep so the debug-name
       // semantics don't drift between create-time and rename-time.
       if (!desc.label.empty()) {
           VkDebugUtilsObjectNameInfoEXT nameInfo {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
           nameInfo.pNext = nullptr;
           nameInfo.objectType = VK_OBJECT_TYPE_COMMAND_POOL;
           nameInfo.objectHandle = reinterpret_cast<std::uint64_t>(commandPool);
           nameInfo.pObjectName = desc.label.c_str();
           vkSetDebugUtilsObjectNameEXT(engine->device, &nameInfo);
       }

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
       // GPU Commit-Timing P1 structural fix #3 — stop the async-completion
       // waiter before tearing down the timeline / query pool / containers it
       // reads. The CV wake + stop flag unblock a waiter idling between arms; the
       // bounded vkWaitSemaphores timeout bounds an in-flight wait; join
       // guarantees no pollCompletions runs after this.
       if(waiterStarted_){
           {
               std::lock_guard<std::mutex> lock(completionMutex_);
               waiterStop_ = true;
           }
           waiterCv_.notify_all();
           if(completionWaiter_.joinable()){
               completionWaiter_.join();
           }
       }
       // G.5.1 Vulkan follow-up — drop any staged / gated completion handlers
       // without firing them: firing pre-GPU-completion would race, and WTK
       // doesn't depend on teardown firing (its BackendRenderTargetContext
       // destructor returns every pending PendingReleaseBatch straight to the
       // longer-lived BufferPool). Matches the D3D12 queue's teardown contract.
       {
           std::lock_guard<std::mutex> lock(completionMutex_);
           stagedCompletionHandlers_.clear();
           gatedCompletionHandlers_.clear();
       }
       // GPU Commit-Timing P1 — the waiter is joined and no frame work remains,
       // so nothing references the timestamp query pool any more.
       if(timestampPool != VK_NULL_HANDLE && engine != nullptr){
           vkDestroyQueryPool(engine->device, timestampPool, nullptr);
           timestampPool = VK_NULL_HANDLE;
           timestampsEnabled = false;
       }
       // Pending retention entries are owned by engine->retentionQueue; the
       // engine's drainAll() at shutdown is responsible for releasing them.
       // Don't destroy retentionTimeline until those entries (which capture
       // it via gates) have all drained — the timeline is owned by the
       // semaphore still being referenced inside retentionQueue closures, so
       // we release it via lambda capture rather than freeing here.
       if(!commandBuffers.empty()){
           vkFreeCommandBuffers(engine->device,commandPool,commandBuffers.size(),commandBuffers.data());
           commandBuffers.resize(0);
           // CommandQueue-Typed-Pool Phase 3 — keep the growable-pool
           // parallel state aligned with `commandBuffers`. Without these
           // resets the engine's destructor or a subsequent re-init would
           // see a stale submission-index table.
           commandBufferSubmissionIndex.resize(0);
           pendingSlots.clear();
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
