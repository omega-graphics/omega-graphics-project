#include "GEVulkan.h"
#include "omegaGTE/GERenderTarget.h"

#include "GEVulkanTexture.h"
#include "GEVulkanCommandQueue.h"

#ifndef OMEGAGTE_VULKAN_GEVULKANRENDERTARGET_H
#define OMEGAGTE_VULKAN_GEVULKANRENDERTARGET_H

_NAMESPACE_BEGIN_

class GEVulkanNativeRenderTarget : public GENativeRenderTarget {
    GEVulkanEngine *parentEngine;
    std::uint64_t traceResourceId = 0;
    bool nativeReleased_ = false;
public:

    SharedHandle<GEVulkanCommandQueue> commandQueue;

    VkSurfaceKHR surface;
    VkFramebuffer framebuffer;
    VkSwapchainKHR swapchainKHR;

    unsigned currentFrameIndex;

    /// True once this frame has acquired a swapchain image and before it has
    /// been presented. The image is acquired lazily on the frame's FIRST
    /// swapchain `startRenderPass`; a frame that splits its render pass — e.g.
    /// the texture-fence `resumeFrameAfterScratch` path restarts the swapchain
    /// pass with LoadPreserve to composite a layer scratch — must REUSE that
    /// image, not acquire a second one. With a 2-image swapchain
    /// (minImageCount 2) only one image may be held at a time, so a second
    /// acquire mid-frame is the `VUID-vkAcquireNextImageKHR-swapchain-01802`
    /// "already acquired 1 image" violation that stalls the compositor. This
    /// flag enforces one acquire (one logical swapchain render pass) per frame;
    /// `present()` clears it after the image returns to the swapchain.
    bool imageAcquired = false;

    VkFormat format;

    VkExtent2D extent;
    /// This semaphore is used for both frame completion.
    VkSemaphore semaphore;
    /// The fence is used for frame acquiring.
    VkFence frameIsReadyFence;

    OmegaCommon::Vector<VkImage> frames;
    OmegaCommon::Vector<VkImageView> frameViews;

    GEVulkanNativeRenderTarget(GEVulkanEngine *parentEngine,
                               SharedHandle<GECommandQueue> presentQueue,
                               VkSurfaceKHR & surface,
                               VkSwapchainKHR & swapchainKHR,
                               VkFormat & surfaceFormat,
                               unsigned & mipLevel,
                               VkExtent2D & surfaceExtent);

    PixelFormat pixelFormat() override;

    SharedHandle<GECommandQueue> presentQueue() const override;
    void present() override;

    /// Recreate the swapchain at the requested extent. The surface is reused;
    /// the old VkSwapchainKHR / image views / per-frame fence are destroyed
    /// after the device is idle. `currentFrameIndex` is reset to 0 so the
    /// next acquire targets the fresh image set.
    void resizeSwapChain(unsigned int width, unsigned int height) override;

    void releaseNative();
    ~GEVulkanNativeRenderTarget();
};

class GEVulkanTextureRenderTarget : public GETextureRenderTarget {
    GEVulkanEngine *parentEngine;
    std::uint64_t traceResourceId = 0;
    bool nativeReleased_ = false;
public:

    SharedHandle<GEVulkanTexture> texture;
    VkFramebuffer frameBuffer;
    VkFence fence;

    GEVulkanTextureRenderTarget(GEVulkanEngine * engine,
                                SharedHandle<GEVulkanTexture> & texture,
                                VkFramebuffer & framebuffer);

    SharedHandle<GETexture> underlyingTexture() override;

    void releaseNative();
    ~GEVulkanTextureRenderTarget();
};

_NAMESPACE_END_

#endif
