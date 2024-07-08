#include "GEVulkan.h"
#include "omegaGTE/GERenderTarget.h"

#include "GEVulkanTexture.h"
#include "GEVulkanCommandQueue.h"

#ifndef OMEGAGTE_VULKAN_GEVULKANRENDERTARGET_H
#define OMEGAGTE_VULKAN_GEVULKANRENDERTARGET_H

_NAMESPACE_BEGIN_

class GEVulkanNativeRenderTarget : public GENativeRenderTarget {
    GEVulkanEngine *parentEngine;
public:

    SharedHandle<GEVulkanCommandQueue> commandQueue;

    VkSurfaceKHR surface;
    VkFramebuffer framebuffer;
    VkSwapchainKHR swapchainKHR;

    unsigned currentFrameIndex;

    VkFormat format;

    VkExtent2D extent;
    /// This semaphore is used for both frame completion.
    VkSemaphore semaphore;
    /// The fence is used for frame acquiring.
    VkFence frameIsReadyFence;

    OmegaCommon::Vector<VkImage> frames;
    OmegaCommon::Vector<VkImageView> frameViews;

    GEVulkanNativeRenderTarget(GEVulkanEngine *parentEngine,
                               VkSurfaceKHR & surface,
                               VkSwapchainKHR & swapchainKHR,
                               VkFormat & surfaceFormat,
                               unsigned & mipLevel,
                               VkExtent2D & surfaceExtent);

    SharedHandle<CommandBuffer> commandBuffer() override;
    void submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer) override;
    void commitAndPresent() override;

    ~GEVulkanNativeRenderTarget();
};

class GEVulkanTextureRenderTarget : public GETextureRenderTarget {
    GEVulkanEngine *parentEngine;
public:

    SharedHandle<GEVulkanCommandQueue> commandQueue;


    SharedHandle<GEVulkanTexture> texture;
    VkFramebuffer frameBuffer;
    VkFence fence;

    GEVulkanTextureRenderTarget(GEVulkanEngine * engine,
                                SharedHandle<GEVulkanTexture> & texture,
                                VkFramebuffer & framebuffer);

    SharedHandle<CommandBuffer> commandBuffer() override;
    void submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer) override;
    void commit() override;

    ~GEVulkanTextureRenderTarget();
};

_NAMESPACE_END_

#endif