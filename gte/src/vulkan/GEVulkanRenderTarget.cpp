#include "GEVulkanRenderTarget.h"
#include "vulkan/vulkan_core.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

GEVulkanNativeRenderTarget::GEVulkanNativeRenderTarget(GEVulkanEngine *parentEngine,
                                                       VkSurfaceKHR &surface,
                                                       VkSwapchainKHR &swapchainKHR,
                                                       VkFormat & surfaceFormat,
                                                       unsigned & mipLevel,
                                                       VkExtent2D & surfaceExtent):
                                                       parentEngine(parentEngine),
                                                       surface(surface),
                                                       framebuffer(VK_NULL_HANDLE),
                                                       swapchainKHR(swapchainKHR),
                                                       currentFrameIndex(0),
                                                       format(surfaceFormat),
                                                       extent(surfaceExtent),
                                                       semaphore(VK_NULL_HANDLE),
                                                       frameIsReadyFence(VK_NULL_HANDLE) {
    if(this->parentEngine == nullptr || this->swapchainKHR == VK_NULL_HANDLE){
        return;
    }

    uint32_t count = 0;
    auto swapchainImagesRes = vkGetSwapchainImagesKHR(this->parentEngine->device,this->swapchainKHR,&count,nullptr);
    if(swapchainImagesRes != VK_SUCCESS || count == 0){
        return;
    }

    frames.resize(count);

    swapchainImagesRes = vkGetSwapchainImagesKHR(this->parentEngine->device,this->swapchainKHR,&count,frames.data());
    if(swapchainImagesRes != VK_SUCCESS){
        frames.clear();
        return;
    }

    VkImageViewCreateInfo imgView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageView view;

    for(auto & f : frames){
        imgView.image = f;
        imgView.pNext = nullptr;
        imgView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imgView.format = surfaceFormat;
        imgView.subresourceRange.baseArrayLayer = 0;
        imgView.subresourceRange.layerCount = 1;
        imgView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgView.subresourceRange.baseMipLevel = 0;
        imgView.subresourceRange.levelCount = mipLevel > 0 ? mipLevel : 1;
        auto viewRes = vkCreateImageView(this->parentEngine->device,&imgView,nullptr,&view);
        if(viewRes != VK_SUCCESS){
            continue;
        }
        frameViews.push_back(view);
    }

    commandQueue = std::dynamic_pointer_cast<GEVulkanCommandQueue>(this->parentEngine->makeCommandQueue(100));

    VkSemaphoreCreateInfo semaphoreCreateInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreCreateInfo.pNext = nullptr;
    semaphoreCreateInfo.flags = 0;
    auto semaphoreRes = vkCreateSemaphore(this->parentEngine->device,&semaphoreCreateInfo,nullptr,&semaphore);
    if(semaphoreRes != VK_SUCCESS){
        semaphore = VK_NULL_HANDLE;
    }

    VkFenceCreateInfo fenceCreateInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceCreateInfo.pNext = nullptr;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    auto fenceRes = vkCreateFence(this->parentEngine->device,&fenceCreateInfo,nullptr,&frameIsReadyFence);
    if(fenceRes != VK_SUCCESS){
        frameIsReadyFence = VK_NULL_HANDLE;
    }

    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Vulkan,
            "NativeRenderTarget",
            traceResourceId,
            reinterpret_cast<const void *>(this->swapchainKHR),
            static_cast<float>(extent.width),
            static_cast<float>(extent.height));

}

PixelFormat GEVulkanNativeRenderTarget::pixelFormat() {
    switch(format){
        case VK_FORMAT_B8G8R8A8_UNORM: return PixelFormat::BGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:  return PixelFormat::BGRA8Unorm_SRGB;
        case VK_FORMAT_R8G8B8A8_SRGB:  return PixelFormat::RGBA8Unorm_SRGB;
        case VK_FORMAT_R16G16B16A16_UNORM: return PixelFormat::RGBA16Unorm;
        case VK_FORMAT_R8G8B8A8_UNORM:
        default: return PixelFormat::RGBA8Unorm;
    }
}

SharedHandle<GERenderTarget::CommandBuffer> GEVulkanNativeRenderTarget::commandBuffer() {
    return SharedHandle<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,
                                                                                         GERenderTarget::CommandBuffer::GERTType::Native,
                                                                                         commandQueue->getAvailableBuffer()));
}

void GEVulkanNativeRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &commandBuffer) {
    if(commandBuffer->commandBuffer)
        commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
}

void GEVulkanNativeRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence){
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEVulkanNativeRenderTarget::notifyCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence){
    commandQueue->notifyCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEVulkanNativeRenderTarget::commitAndPresent() {
    if(parentEngine == nullptr || parentEngine->device == VK_NULL_HANDLE || commandQueue == nullptr || swapchainKHR == VK_NULL_HANDLE){
        return;
    }

    const auto presentedCommandBufferId = commandQueue->lastSubmittedCommandBufferTraceId();

    VkPresentInfoKHR presentInfoKhr {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfoKhr.pNext = nullptr;
    presentInfoKhr.pImageIndices = &currentFrameIndex;
    presentInfoKhr.swapchainCount = 1;
    presentInfoKhr.pSwapchains = &swapchainKHR;
    presentInfoKhr.waitSemaphoreCount = 0;
    presentInfoKhr.pWaitSemaphores = nullptr;
   

    commandQueue->commitToGPUPresent(&presentInfoKhr);
    ResourceTracking::Event presentEvent {};
    presentEvent.backend = ResourceTracking::Backend::Vulkan;
    presentEvent.eventType = ResourceTracking::EventType::Present;
    presentEvent.resourceType = "NativeRenderTarget";
    presentEvent.resourceId = traceResourceId;
    presentEvent.queueId = commandQueue->traceId();
    presentEvent.commandBufferId = presentedCommandBufferId;
    presentEvent.nativeHandle = reinterpret_cast<std::uint64_t>(swapchainKHR);
    ResourceTracking::Tracker::instance().emit(presentEvent);
    commandQueue->clearSubmittedTraceCommandBufferIds();
}

void GEVulkanNativeRenderTarget::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;

    if(parentEngine == nullptr || parentEngine->device == VK_NULL_HANDLE){
        return;
    }

    commandQueue.reset();

    if(framebuffer != VK_NULL_HANDLE){
        vkDestroyFramebuffer(parentEngine->device,framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    for(auto view : frameViews){
        vkDestroyImageView(parentEngine->device,view,nullptr);
    }
    frameViews.clear();
    if(frameIsReadyFence != VK_NULL_HANDLE){
        vkDestroyFence(parentEngine->device,frameIsReadyFence,nullptr);
        frameIsReadyFence = VK_NULL_HANDLE;
    }
    if(semaphore != VK_NULL_HANDLE){
        vkDestroySemaphore(parentEngine->device,semaphore,nullptr);
        semaphore = VK_NULL_HANDLE;
    }
    if(swapchainKHR != VK_NULL_HANDLE){
        vkDestroySwapchainKHR(parentEngine->device,swapchainKHR,nullptr);
        swapchainKHR = VK_NULL_HANDLE;
    }
    if(surface != VK_NULL_HANDLE){
        vkDestroySurfaceKHR(GEVulkanEngine::instance,surface,nullptr);
        surface = VK_NULL_HANDLE;
    }
}

GEVulkanNativeRenderTarget::~GEVulkanNativeRenderTarget() {
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::Vulkan,
            "NativeRenderTarget",
            traceResourceId,
            reinterpret_cast<const void *>(swapchainKHR),
            static_cast<float>(extent.width),
            static_cast<float>(extent.height));

    if(!nativeReleased_){
        if(parentEngine == nullptr || parentEngine->device == VK_NULL_HANDLE){
            return;
        }

        if(framebuffer != VK_NULL_HANDLE){
            vkDestroyFramebuffer(parentEngine->device,framebuffer, nullptr);
        }
        for(auto view : frameViews){
            vkDestroyImageView(parentEngine->device,view,nullptr);
        }
        if(frameIsReadyFence != VK_NULL_HANDLE){
            vkDestroyFence(parentEngine->device,frameIsReadyFence,nullptr);
        }
        if(semaphore != VK_NULL_HANDLE){
            vkDestroySemaphore(parentEngine->device,semaphore,nullptr);
        }
        if(swapchainKHR != VK_NULL_HANDLE){
            vkDestroySwapchainKHR(parentEngine->device,swapchainKHR,nullptr);
        }
        if(surface != VK_NULL_HANDLE){
            vkDestroySurfaceKHR(GEVulkanEngine::instance,surface,nullptr);
        }
    }
}

GEVulkanTextureRenderTarget::GEVulkanTextureRenderTarget(GEVulkanEngine * engine,
                                SharedHandle<GEVulkanTexture> & texture,
                                VkFramebuffer & framebuffer):parentEngine(engine),
                                                             texture(texture),
                                                             frameBuffer(framebuffer){
    commandQueue = std::dynamic_pointer_cast<GEVulkanCommandQueue>(parentEngine->makeCommandQueue(100));
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Vulkan,
            "TextureRenderTarget",
            traceResourceId,
            texture != nullptr ? reinterpret_cast<const void *>(texture->img) : nullptr,
            texture != nullptr ? static_cast<float>(texture->descriptor.width) : -1.f,
            texture != nullptr ? static_cast<float>(texture->descriptor.height) : -1.f);
}

SharedHandle<GERenderTarget::CommandBuffer> GEVulkanTextureRenderTarget::commandBuffer(){
    return SharedHandle<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,
        GERenderTarget::CommandBuffer::GERTType::Texture,
        commandQueue->getAvailableBuffer()));
}

SharedHandle<GETexture> GEVulkanTextureRenderTarget::underlyingTexture(){
    return texture;
}

void GEVulkanTextureRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &commandBuffer){
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
}

void GEVulkanTextureRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence){
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEVulkanTextureRenderTarget::notifyCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence){
    commandQueue->notifyCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEVulkanTextureRenderTarget::commit(){
    commandQueue->commitToGPU();
    commandQueue->clearSubmittedTraceCommandBufferIds();
}

void GEVulkanTextureRenderTarget::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;
    commandQueue.reset();
    texture.reset();
    if(frameBuffer != VK_NULL_HANDLE){
        vkDestroyFramebuffer(parentEngine->device,frameBuffer,nullptr);
        frameBuffer = VK_NULL_HANDLE;
    }
}

GEVulkanTextureRenderTarget::~GEVulkanTextureRenderTarget(){
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::Vulkan,
            "TextureRenderTarget",
            traceResourceId,
            texture != nullptr ? reinterpret_cast<const void *>(texture->img) : nullptr,
            texture != nullptr ? static_cast<float>(texture->descriptor.width) : -1.f,
            texture != nullptr ? static_cast<float>(texture->descriptor.height) : -1.f);
    if(!nativeReleased_){
        if(frameBuffer != VK_NULL_HANDLE){
            vkDestroyFramebuffer(parentEngine->device,frameBuffer,nullptr);
        }
    }
}

_NAMESPACE_END_
