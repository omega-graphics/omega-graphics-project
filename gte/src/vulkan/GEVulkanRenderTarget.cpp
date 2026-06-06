#include "GEVulkanRenderTarget.h"
#include "vulkan/vulkan_core.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

GEVulkanNativeRenderTarget::GEVulkanNativeRenderTarget(GEVulkanEngine *parentEngine,
                                                       SharedHandle<GECommandQueue> presentQueue,
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

    commandQueue = std::dynamic_pointer_cast<GEVulkanCommandQueue>(presentQueue);

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

SharedHandle<GECommandQueue> GEVulkanNativeRenderTarget::presentQueue() const {
    return std::static_pointer_cast<GECommandQueue>(commandQueue);
}

void GEVulkanNativeRenderTarget::present() {
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

void GEVulkanNativeRenderTarget::resizeSwapChain(unsigned int width, unsigned int height){
    if(parentEngine == nullptr || parentEngine->device == VK_NULL_HANDLE ||
       surface == VK_NULL_HANDLE){
        return;
    }
    if(width == 0 || height == 0){
        return;
    }

    // Idle the device before tearing down the old swapchain + image views.
    // The render-pass framebuffers built from these views are command-buffer-
    // scope (recreated per pass in startRenderPass) so retention here only
    // needs to cover in-flight present/acquire work.
    vkDeviceWaitIdle(parentEngine->device);

    // Resolve the new extent against surface capabilities. The platform may
    // clamp our request (e.g. Wayland reports UINT32_MAX as "you decide"; X11
    // reports the actual server-side window extent).
    VkSurfaceCapabilitiesKHR caps{};
    auto capsRes = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(parentEngine->physicalDevice,
                                                             surface, &caps);
    if(capsRes != VK_SUCCESS){
        std::cerr << "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed during resize ("
                  << capsRes << ")" << std::endl;
        return;
    }

    VkExtent2D newExtent{ width, height };
    if(caps.currentExtent.width != UINT32_MAX){
        newExtent = caps.currentExtent;
    }
    if(newExtent.width  < caps.minImageExtent.width)  newExtent.width  = caps.minImageExtent.width;
    if(newExtent.height < caps.minImageExtent.height) newExtent.height = caps.minImageExtent.height;
    if(caps.maxImageExtent.width  != 0 && newExtent.width  > caps.maxImageExtent.width)  newExtent.width  = caps.maxImageExtent.width;
    if(caps.maxImageExtent.height != 0 && newExtent.height > caps.maxImageExtent.height) newExtent.height = caps.maxImageExtent.height;
    if(newExtent.width == 0)  newExtent.width  = 1;
    if(newExtent.height == 0) newExtent.height = 1;

    // Re-pick the present mode — the surface may have lost support for our
    // prior choice on a different output (rare; cheap to re-query).
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    {
        std::uint32_t count = 0;
        if(vkGetPhysicalDeviceSurfacePresentModesKHR(parentEngine->physicalDevice, surface, &count, nullptr) == VK_SUCCESS && count > 0){
            OmegaCommon::Vector<VkPresentModeKHR> modes(count);
            if(vkGetPhysicalDeviceSurfacePresentModesKHR(parentEngine->physicalDevice, surface, &count, modes.data()) == VK_SUCCESS){
                for(auto mode : modes){
                    if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR){ presentMode = mode; break; }
                    if(mode == VK_PRESENT_MODE_FIFO_KHR){ presentMode = mode; }
                }
            }
        }
    }

    // Resolve the color space that pairs with our stable `format`. The format
    // we keep on the target is the VkFormat the swapchain was built with;
    // pair it with whatever colorSpace the surface still advertises for it.
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    {
        std::uint32_t count = 0;
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(parentEngine->physicalDevice, surface, &count, nullptr) == VK_SUCCESS && count > 0){
            OmegaCommon::Vector<VkSurfaceFormatKHR> formats(count);
            if(vkGetPhysicalDeviceSurfaceFormatsKHR(parentEngine->physicalDevice, surface, &count, formats.data()) == VK_SUCCESS){
                for(auto & f : formats){
                    if(f.format == format){
                        colorSpace = f.colorSpace;
                        break;
                    }
                }
            }
        }
    }

    std::uint32_t imageCount = caps.minImageCount > 2 ? caps.minImageCount : 2;
    if(caps.maxImageCount > 0 && imageCount > caps.maxImageCount){
        imageCount = caps.maxImageCount;
    }

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = format;
    info.imageColorSpace = colorSpace;
    info.imageExtent = newExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = parentEngine->queueFamilyIndices.size() > 1
                            ? VK_SHARING_MODE_CONCURRENT
                            : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = static_cast<std::uint32_t>(parentEngine->queueFamilyIndices.size());
    info.pQueueFamilyIndices = parentEngine->queueFamilyIndices.data();
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_FALSE;
    info.oldSwapchain = swapchainKHR;

    auto createRes = vkCreateSwapchainKHR(parentEngine->device, &info, nullptr, &newSwapchain);
    if(createRes != VK_SUCCESS || newSwapchain == VK_NULL_HANDLE){
        std::cerr << "vkCreateSwapchainKHR failed during resize (" << createRes << ")" << std::endl;
        return;
    }

    // Tear down the old views + swapchain. The acquire fence stays (it's
    // signaled per-acquire, not per-swapchain), the semaphore stays.
    for(auto view : frameViews){
        if(view != VK_NULL_HANDLE){
            vkDestroyImageView(parentEngine->device, view, nullptr);
        }
    }
    frameViews.clear();
    frames.clear();
    if(swapchainKHR != VK_NULL_HANDLE){
        vkDestroySwapchainKHR(parentEngine->device, swapchainKHR, nullptr);
    }
    swapchainKHR = newSwapchain;
    extent = newExtent;
    currentFrameIndex = 0;

    // Repopulate the per-image views.
    std::uint32_t imgCount = 0;
    if(vkGetSwapchainImagesKHR(parentEngine->device, swapchainKHR, &imgCount, nullptr) != VK_SUCCESS || imgCount == 0){
        std::cerr << "vkGetSwapchainImagesKHR(count) failed after resize" << std::endl;
        return;
    }
    frames.resize(imgCount);
    if(vkGetSwapchainImagesKHR(parentEngine->device, swapchainKHR, &imgCount, frames.data()) != VK_SUCCESS){
        std::cerr << "vkGetSwapchainImagesKHR(list) failed after resize" << std::endl;
        frames.clear();
        return;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    frameViews.reserve(frames.size());
    for(auto & img : frames){
        viewInfo.image = img;
        VkImageView view = VK_NULL_HANDLE;
        if(vkCreateImageView(parentEngine->device, &viewInfo, nullptr, &view) != VK_SUCCESS){
            continue;
        }
        frameViews.push_back(view);
    }
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

SharedHandle<GETexture> GEVulkanTextureRenderTarget::underlyingTexture(){
    return texture;
}

void GEVulkanTextureRenderTarget::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;
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
