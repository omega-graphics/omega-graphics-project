#include "GEVulkan.h"
#include "omegaGTE/GETexture.h"
#include "vulkan/vulkan_core.h"
#include <cstdint>


#ifndef OMEGAGTE_VULKAN_GEVULKANTEXTURE_H
#define OMEGAGTE_VULKAN_GEVULKANTEXTURE_H

_NAMESPACE_BEGIN_

class GEVulkanTexture : public GETexture {
    GEVulkanEngine *engine;
    std::uint64_t traceResourceId = 0;
public:
    VkImage img;
    VkImageView img_view;
    VkImageLayout layout;

    TextureDescriptor descriptor;

    VkFormat format;

    VmaAllocationInfo alloc_info;
    VmaAllocation alloc;

    VmaMemoryUsage memoryUsage;

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


    explicit GEVulkanTexture(
        const GETexture::GETextureType & type,
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
    ~GEVulkanTexture();
};

_NAMESPACE_END_

#endif
