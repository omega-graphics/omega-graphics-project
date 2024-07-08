#include "GEVulkanRenderTarget.h"
#include "vulkan/vulkan_core.h"

_NAMESPACE_BEGIN_

GEVulkanNativeRenderTarget::GEVulkanNativeRenderTarget(GEVulkanEngine *parentEngine,
                                                       VkSurfaceKHR &surface,
                                                       VkSwapchainKHR &swapchainKHR,
                                                       VkFormat & surfaceFormat,
                                                       unsigned & mipLevel,
                                                       VkExtent2D & surfaceExtent):
                                                       format(surfaceFormat), extent(surfaceExtent) {
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(parentEngine->device,swapchainKHR,&count,nullptr);

    frames.resize(count);

    vkGetSwapchainImagesKHR(parentEngine->device,swapchainKHR,&count,frames.data());

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
        imgView.subresourceRange.levelCount = mipLevel;
        vkCreateImageView(parentEngine->device,&imgView,nullptr,&view);
        frameViews.push_back(view);
    }

    VkFramebufferCreateInfo framebufferCreateInfo {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferCreateInfo.pNext = nullptr;
    framebufferCreateInfo.renderPass = VK_NULL_HANDLE;
    framebufferCreateInfo.width = surfaceExtent.width;
    framebufferCreateInfo.height = surfaceExtent.height;
    framebufferCreateInfo.attachmentCount = frameViews.size();
    framebufferCreateInfo.pAttachments = frameViews.data();
    framebufferCreateInfo.layers = 1;
    framebufferCreateInfo.flags = 0;

    vkCreateFramebuffer(parentEngine->device,&framebufferCreateInfo,nullptr,&framebuffer);

    commandQueue = std::dynamic_pointer_cast<GEVulkanCommandQueue>(parentEngine->makeCommandQueue(100));

    VkSemaphoreCreateInfo semaphoreCreateInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreCreateInfo.pNext = nullptr;
    semaphoreCreateInfo.flags = 0;
    vkCreateSemaphore(parentEngine->device,&semaphoreCreateInfo,nullptr,&semaphore);

    VkFenceCreateInfo fenceCreateInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceCreateInfo.pNext = nullptr;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    vkCreateFence(parentEngine->device,&fenceCreateInfo,nullptr,&frameIsReadyFence);

    currentFrameIndex = 0;

}

SharedHandle<GERenderTarget::CommandBuffer> GEVulkanNativeRenderTarget::commandBuffer() {
    return SharedHandle<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,
                                                                                         GERenderTarget::CommandBuffer::GERTType::Native,
                                                                                         commandQueue->getAvailableBuffer()));
}

void GEVulkanNativeRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &commandBuffer) {
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
}

void GEVulkanNativeRenderTarget::commitAndPresent() {


    uint64_t val;
    vkGetSemaphoreCounterValue(parentEngine->device,semaphore,&val);

    VkPresentInfoKHR presentInfoKhr {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfoKhr.pNext = nullptr;
    presentInfoKhr.pImageIndices = &currentFrameIndex;
    presentInfoKhr.swapchainCount = 1;
    presentInfoKhr.pSwapchains = &swapchainKHR;
    presentInfoKhr.waitSemaphoreCount = 0;
    presentInfoKhr.pWaitSemaphores = nullptr;
   

    commandQueue->commitToGPUPresent(&presentInfoKhr);

    /// Wait for value to increment!!
    // val += 1;

    // VkSemaphoreWaitInfo waitInfo {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    // waitInfo.pSemaphores = &semaphore;
    // waitInfo.semaphoreCount = 1;
    // waitInfo.pNext = nullptr;
    // waitInfo.pValues = &val;
    // waitInfo.flags = VK_SEMAPHORE_WAIT_ANY_BIT;

    // Wait forever until frame is finished

    // vkWaitSemaphores(parentEngine->device,&waitInfo,UINT64_MAX);

    vkAcquireNextImageKHR(parentEngine->device,swapchainKHR,UINT64_MAX,VK_NULL_HANDLE,frameIsReadyFence,&currentFrameIndex);
}

GEVulkanNativeRenderTarget::~GEVulkanNativeRenderTarget() {

    vkDestroyFramebuffer(parentEngine->device,framebuffer, nullptr);
    for(auto view : frameViews){
        vkDestroyImageView(parentEngine->device,view,nullptr);
    }
    vkDestroySwapchainKHR(parentEngine->device,swapchainKHR,nullptr);
    vkDestroySurfaceKHR(GEVulkanEngine::instance,surface,nullptr);

}

GEVulkanTextureRenderTarget::GEVulkanTextureRenderTarget(GEVulkanEngine * engine,
                                SharedHandle<GEVulkanTexture> & texture,
                                VkFramebuffer & framebuffer):parentEngine(engine),
                                                             texture(texture),
                                                             frameBuffer(framebuffer){
    
}

SharedHandle<GERenderTarget::CommandBuffer> GEVulkanTextureRenderTarget::commandBuffer(){
    return SharedHandle<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,
        GERenderTarget::CommandBuffer::GERTType::Texture,
        commandQueue->getAvailableBuffer()));
}

void GEVulkanTextureRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &commandBuffer){
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
}

void GEVulkanTextureRenderTarget::commit(){
    commandQueue->commitToGPU();
}

GEVulkanTextureRenderTarget::~GEVulkanTextureRenderTarget(){
    vkDestroyFramebuffer(parentEngine->device,frameBuffer,nullptr);
}

_NAMESPACE_END_