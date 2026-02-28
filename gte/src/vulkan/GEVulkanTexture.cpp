#include "GEVulkanTexture.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

GEVulkanTexture::GEVulkanTexture(
    const GETexture::GETextureType & type,
    const GETexture::GETextureUsage & usage,
    const TexturePixelFormat & format,
    GEVulkanEngine *engine,
    VkImage & img,
    VkImageView & img_view,
    VkImageLayout & layout,
    VmaAllocationInfo alloc_info,
    VmaAllocation alloc,const TextureDescriptor & descriptor,VmaMemoryUsage memoryUsage):

GETexture(type,usage,format),
engine(engine),
img(std::move(img)),
img_view(std::move(img_view)),
layout(layout),
descriptor(descriptor),
alloc_info(alloc_info),alloc(alloc),memoryUsage(memoryUsage)
{
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Vulkan,
            "Texture",
            traceResourceId,
            reinterpret_cast<const void *>(img),
            static_cast<float>(descriptor.width),
            static_cast<float>(descriptor.height));
};

size_t GEVulkanTexture::getBytes(void *bytes, size_t bytesPerRow){
    auto len = bytesPerRow * descriptor.height;
    void *ptr;
    vmaMapMemory(engine->memAllocator,alloc,&ptr);
    memcpy(bytes,ptr,len);
    vmaUnmapMemory(engine->memAllocator,alloc);
    return len;
}

void GEVulkanTexture::copyBytes(void *bytes, size_t bytesPerRow){
    auto len = bytesPerRow * descriptor.height;
    void *ptr;
    vmaMapMemory(engine->memAllocator,alloc,&ptr);
    memcpy(ptr,bytes,len);
    vmaUnmapMemory(engine->memAllocator,alloc);
}

GEVulkanTexture::~GEVulkanTexture(){
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::Vulkan,
            "Texture",
            traceResourceId,
            reinterpret_cast<const void *>(img),
            static_cast<float>(descriptor.width),
            static_cast<float>(descriptor.height));
    vmaDestroyImage(engine->memAllocator,img,alloc);
    vkDestroyImageView(engine->device,img_view,nullptr);
}

_NAMESPACE_END_
