#include "GEVulkanTexture.h"

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

};

void GEVulkanTexture::copyBytes(void *bytes, size_t len){
    void *ptr;
    vmaMapMemory(engine->memAllocator,alloc,&ptr);
    memcpy(ptr,bytes,len);
    vmaUnmapMemory(engine->memAllocator,alloc);
}

GEVulkanTexture::~GEVulkanTexture(){
    vmaDestroyImage(engine->memAllocator,img,alloc);
    vkDestroyImageView(engine->device,img_view,nullptr);
}

_NAMESPACE_END_