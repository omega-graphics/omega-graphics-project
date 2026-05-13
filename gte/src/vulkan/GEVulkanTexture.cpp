#include "GEVulkanTexture.h"
#include "../common/GEResourceTracker.h"
#include <iostream>

_NAMESPACE_BEGIN_

GEVulkanTexture::GEVulkanTexture(
    const TextureKind & kind,
    const GETexture::GETextureUsage & usage,
    const TexturePixelFormat & format,
    GEVulkanEngine *engine,
    VkImage & img,
    VkImageView & img_view,
    VkImageLayout & layout,
    VmaAllocationInfo alloc_info,
    VmaAllocation alloc,const TextureDescriptor & descriptor,VmaMemoryUsage memoryUsage):

GETexture(kind,usage,format),
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

void GEVulkanTexture::copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion &destRegion){
    if(bytes == nullptr || bytesPerRow == 0 || destRegion.w == 0 || destRegion.h == 0){
        return;
    }

    // Pipeline-Completion-Extension-Plan §4.5. Mirrors the single-arg path:
    // map the host-visible image directly and memcpy row-by-row into the
    // destination sub-region. The image's actual row pitch comes from
    // `vkGetImageSubresourceLayout` so packing/alignment is honoured even
    // when the linear-tiled image has a wider stride than the requested
    // region width × bytesPerTexel.
    std::uint32_t bytesPerTexel = 4;
    switch(descriptor.pixelFormat){
        case PixelFormat::RGBA16Unorm: bytesPerTexel = 8; break;
        default: bytesPerTexel = 4; break;
    }

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.arrayLayer = 0;
    VkSubresourceLayout subresourceLayout{};
    vkGetImageSubresourceLayout(engine->device, img, &subresource, &subresourceLayout);

    void *ptr = nullptr;
    if(vmaMapMemory(engine->memAllocator, alloc, &ptr) != VK_SUCCESS || ptr == nullptr){
        return;
    }

    auto *base = static_cast<std::uint8_t *>(ptr) + subresourceLayout.offset;
    const auto *src = static_cast<const std::uint8_t *>(bytes);
    const std::size_t rowBytes = static_cast<std::size_t>(destRegion.w) * bytesPerTexel;
    const std::uint32_t depth = destRegion.d == 0 ? 1u : destRegion.d;

    for(std::uint32_t z = 0; z < depth; ++z){
        for(std::uint32_t y = 0; y < destRegion.h; ++y){
            std::uint8_t *dst = base
                + static_cast<std::size_t>(destRegion.z + z) * subresourceLayout.depthPitch
                + static_cast<std::size_t>(destRegion.y + y) * subresourceLayout.rowPitch
                + static_cast<std::size_t>(destRegion.x) * bytesPerTexel;
            const std::uint8_t *srow = src
                + static_cast<std::size_t>(z) * bytesPerRow * destRegion.h
                + static_cast<std::size_t>(y) * bytesPerRow;
            std::memcpy(dst, srow, rowBytes);
        }
    }

    vmaUnmapMemory(engine->memAllocator, alloc);
}

static VkComponentSwizzle vulkanComponentSwizzleFor(TextureSwizzleChannel ch,
                                                    VkComponentSwizzle positionalIdentity){
    switch(ch){
        case TextureSwizzleChannel::Identity: return positionalIdentity;
        case TextureSwizzleChannel::Red:      return VK_COMPONENT_SWIZZLE_R;
        case TextureSwizzleChannel::Green:    return VK_COMPONENT_SWIZZLE_G;
        case TextureSwizzleChannel::Blue:     return VK_COMPONENT_SWIZZLE_B;
        case TextureSwizzleChannel::Alpha:    return VK_COMPONENT_SWIZZLE_A;
        case TextureSwizzleChannel::Zero:     return VK_COMPONENT_SWIZZLE_ZERO;
        case TextureSwizzleChannel::One:      return VK_COMPONENT_SWIZZLE_ONE;
    }
    return positionalIdentity;
}

static VkImageViewType vulkanViewTypeForKind(TextureKind kind){
    switch(kind){
        case TextureKind::Tex1D:         return VK_IMAGE_VIEW_TYPE_1D;
        case TextureKind::Tex1DArray:    return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case TextureKind::Tex2D:         return VK_IMAGE_VIEW_TYPE_2D;
        case TextureKind::Tex2DArray:    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureKind::TexCube:       return VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureKind::TexCubeArray:  return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case TextureKind::Tex2DMS:       return VK_IMAGE_VIEW_TYPE_2D;
        case TextureKind::Tex2DMSArray:  return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureKind::Tex3D:         return VK_IMAGE_VIEW_TYPE_3D;
        case TextureKind::Auto:          return VK_IMAGE_VIEW_TYPE_2D;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

VkImageView GEVulkanTexture::getOrCreateSwizzledView(const TextureSwizzle & swizzle){
    if(swizzle.isIdentity()) return img_view;
    for(auto & entry : swizzledViewCache){
        if(entry.first == swizzle) return entry.second;
    }

    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = img;
    // Match the viewType chosen at primary-view creation: this drives off
    // the texture's `TextureKind` so cube/array/MS textures get a viewType
    // that matches the underlying VkImage.
    info.viewType = vulkanViewTypeForKind(getKind());
    info.format = format;
    info.components.r = vulkanComponentSwizzleFor(swizzle.r, VK_COMPONENT_SWIZZLE_R);
    info.components.g = vulkanComponentSwizzleFor(swizzle.g, VK_COMPONENT_SWIZZLE_G);
    info.components.b = vulkanComponentSwizzleFor(swizzle.b, VK_COMPONENT_SWIZZLE_B);
    info.components.a = vulkanComponentSwizzleFor(swizzle.a, VK_COMPONENT_SWIZZLE_A);
    // TODO: revisit when depth/stencil formats are added to
    // `TexturePixelFormat`. Today every supported pixel format is a
    // color format, so COLOR_BIT is the only valid aspect; once depth /
    // stencil formats land, this needs to derive the aspect from `format`.
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = descriptor.mipLevels;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = descriptor.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    VkResult vr = vkCreateImageView(engine->device, &info, nullptr, &view);
    if(vr != VK_SUCCESS || view == VK_NULL_HANDLE){
        // Surface a recognizable failure to the caller. Returning the
        // identity `img_view` keeps the descriptor write valid (the bind
        // succeeds with the wrong swizzle) instead of the alternative —
        // writing VK_NULL_HANDLE into a descriptor set, which is a hard
        // validation error or a GPU hang. The bind path's diagnostics
        // already include enough context to identify the texture.
        std::cerr << "Vulkan getOrCreateSwizzledView failed: vkCreateImageView returned "
                  << vr << " (" << static_cast<int>(getKind()) << " kind, format "
                  << format << "); falling back to identity view." << std::endl;
        return img_view;
    }
    swizzledViewCache.push_back({swizzle, view});
    return view;
}

void GEVulkanTexture::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;
    vmaDestroyImage(engine->memAllocator,img,alloc);
    vkDestroyImageView(engine->device,img_view,nullptr);
    for(auto & entry : swizzledViewCache){
        if(entry.second != VK_NULL_HANDLE){
            vkDestroyImageView(engine->device, entry.second, nullptr);
        }
    }
    swizzledViewCache.clear();
    img = VK_NULL_HANDLE;
    img_view = VK_NULL_HANDLE;
    alloc = nullptr;
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
    if(nativeReleased_) return;
    if(engine != nullptr){
        VkImage      i   = img;
        VkImageView  iv  = img_view;
        VmaAllocation a  = alloc;
        VmaAllocator alc = engine->memAllocator;
        VkDevice     dev = engine->device;
        std::vector<VkImageView> swizzledViews;
        swizzledViews.reserve(swizzledViewCache.size());
        for(auto & entry : swizzledViewCache){
            if(entry.second != VK_NULL_HANDLE) swizzledViews.push_back(entry.second);
        }
        swizzledViewCache.clear();
        std::vector<Retention::FenceGate> gates(pendingGates.begin(), pendingGates.end());
        engine->retentionQueue.enqueue(std::move(gates),
            [alc, dev, i, iv, a, swizzledViews = std::move(swizzledViews)]() {
                vmaDestroyImage(alc, i, a);
                if (iv != VK_NULL_HANDLE) vkDestroyImageView(dev, iv, nullptr);
                for(VkImageView sv : swizzledViews){
                    vkDestroyImageView(dev, sv, nullptr);
                }
            });
        img = VK_NULL_HANDLE;
        img_view = VK_NULL_HANDLE;
        alloc = nullptr;
    }
}

_NAMESPACE_END_
