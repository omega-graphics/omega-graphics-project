#include "GEVulkan.h"
#include "omegaGTE/GETexture.h"
#include "vulkan/vulkan_core.h"
#include <cstdint>
#include <utility>


#ifndef OMEGAGTE_VULKAN_GEVULKANTEXTURE_H
#define OMEGAGTE_VULKAN_GEVULKANTEXTURE_H

_NAMESPACE_BEGIN_

class GEVulkanTexture : public GETexture {
    GEVulkanEngine *engine;
    std::uint64_t traceResourceId = 0;
    bool nativeReleased_ = false;
public:
    VkImage img;
    VkImageView img_view;
    VkImageLayout layout;

    TextureDescriptor descriptor;

    VkFormat format;

    /// Lazily-created `VkImageView` instances for non-identity swizzles
    /// requested at bind time. Keyed by the requested `TextureSwizzle`.
    /// Linear scan is intentional — typical textures see 0–2 distinct
    /// swizzles in their lifetime, and a vector avoids hashing
    /// `TextureSwizzle`. All entries are destroyed alongside `img_view`
    /// in `releaseNative()`.
    OmegaCommon::Vector<std::pair<TextureSwizzle, VkImageView>> swizzledViewCache;

    /// Resolve (or lazily create) a `VkImageView` for the given swizzle.
    /// Identity returns `img_view` directly; any other value either hits
    /// the per-texture cache or builds a new view with `VkComponentMapping`.
    VkImageView getOrCreateSwizzledView(const TextureSwizzle & swizzle);

    VmaAllocationInfo alloc_info;
    VmaAllocation alloc;

    VmaMemoryUsage memoryUsage;

    // Gates accumulated by encoders that bound this texture. Texture must
    // outlive every gate before vmaDestroyImage can run.
    OmegaCommon::Vector<Retention::FenceGate> pendingGates;

    /// Sync 2.0 Extension

    VkAccessFlags2KHR priorShaderAccess2 = 0;
    VkPipelineStageFlags2KHR priorPipelineAccess2 = 0;

    /// Standard Sync
    VkAccessFlags priorShaderAccess = 0;
    VkPipelineStageFlags priorPipelineAccess = 0;

    void setName(OmegaCommon::StrRef name) override{
        VkDebugUtilsObjectNameInfoEXT ext;
        ext.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        ext.pNext = nullptr;
        ext.pObjectName = name.data();
        ext.objectHandle = (uint64_t)img;
        ext.objectType = VK_OBJECT_TYPE_IMAGE;
        vkSetDebugUtilsObjectNameEXT(this->engine->device,&ext);
    };

    void *native() override{
        return (void *)img;
    }

    size_t getBytes(void *bytes, size_t bytesPerRow) override;

    void copyBytes(void *bytes, size_t bytesPerRow) override;
    void copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion &destRegion) override;


    explicit GEVulkanTexture(
        const TextureKind & kind,
        const GETexture::GETextureUsage & usage,
        const TexturePixelFormat & format,
        GEVulkanEngine *engine,
        VkImage & img,
        VkImageView & img_view,
        VkImageLayout & layout,
        VmaAllocationInfo alloc_info,
        VmaAllocation alloc,
        const TextureDescriptor & descriptor,
        VmaMemoryUsage memoryUsage);
    void releaseNative();
    ~GEVulkanTexture();
};

_NAMESPACE_END_

#endif
