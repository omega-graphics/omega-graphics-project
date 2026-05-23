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

// Vulkan-Texture-Memory-Plan Phase 3 — copyBytes / getBytes now go
// through the HOST_VISIBLE staging buffer allocated by makeTexture
// alongside the device-local OPTIMAL image, then submit a one-shot
// transfer via the engine's upload pool. This mirrors D3D12's
// cpuSideresource → resource path.

// Bytes per texel for the small set of formats `pixelFormatToVkFormat`
// currently emits. Must stay in sync with the switch in GEVulkan.cpp's
// makeTexture staging branch — if a new format lands without an entry
// here, copyBytes writes the wrong stride and the texture samples as
// garbage.
static std::uint32_t bytesPerTexelFor(PixelFormat fmt){
    switch(fmt){
        case PixelFormat::RGBA8Unorm:
        case PixelFormat::RGBA8Unorm_SRGB:
        case PixelFormat::BGRA8Unorm:
        case PixelFormat::BGRA8Unorm_SRGB:
            return 4;
        case PixelFormat::RGBA16Unorm:
            return 8;
    }
    return 4;
}

size_t GEVulkanTexture::getBytes(void *bytes, size_t bytesPerRow){
    if(bytes == nullptr){
        return 0;
    }
    const std::size_t mip0Width  = descriptor.width > 0 ? descriptor.width : 1;
    const std::size_t mip0Height = descriptor.height > 0 ? descriptor.height : 1;
    const std::uint32_t bpt = bytesPerTexelFor(descriptor.pixelFormat);

    if(stagingBuffer == VK_NULL_HANDLE){
        // No staging buffer — usage isn't ToGPU/FromGPU. Older callers
        // hit this when reading back through a non-FromGPU image; the
        // legacy direct-map only worked for LINEAR-tiled images and
        // would have failed at vmaMapMemory anyway with the OPTIMAL
        // tiling those usages always had. Return 0 to surface the
        // mistake rather than silently returning garbage.
        return 0;
    }

    if(!engine->submitImmediateReadbackToStaging(*this)){
        return 0;
    }

    void *ptr = nullptr;
    if(vmaMapMemory(engine->memAllocator, stagingAlloc, &ptr) != VK_SUCCESS || ptr == nullptr){
        return 0;
    }
    // Staging layout is tightly-packed rows of width × bytesPerTexel.
    // If the caller-supplied bytesPerRow matches, one memcpy; else
    // copy row-by-row honouring the caller's pitch.
    const std::size_t srcRow = mip0Width * bpt;
    const auto *src = static_cast<const std::uint8_t *>(ptr);
    auto *dst       = static_cast<std::uint8_t *>(bytes);
    std::size_t totalRead = 0;
    if(bytesPerRow == 0 || bytesPerRow == srcRow){
        const std::size_t len = srcRow * mip0Height;
        std::memcpy(dst, src, len);
        totalRead = len;
    } else {
        for(std::size_t y = 0; y < mip0Height; ++y){
            std::memcpy(dst + y * bytesPerRow, src + y * srcRow, srcRow);
        }
        totalRead = bytesPerRow * mip0Height;
    }
    vmaUnmapMemory(engine->memAllocator, stagingAlloc);
    return totalRead;
}

void GEVulkanTexture::copyBytes(void *bytes, size_t bytesPerRow){
    if(bytes == nullptr){
        return;
    }
    const std::size_t mip0Width  = descriptor.width > 0 ? descriptor.width : 1;
    const std::size_t mip0Height = descriptor.height > 0 ? descriptor.height : 1;
    const std::uint32_t bpt = bytesPerTexelFor(descriptor.pixelFormat);

    if(stagingBuffer == VK_NULL_HANDLE){
        // Not a ToGPU/FromGPU texture — no upload path. Same rationale
        // as getBytes above: surface the mistake instead of silently
        // dropping the upload.
        return;
    }

    void *ptr = nullptr;
    if(vmaMapMemory(engine->memAllocator, stagingAlloc, &ptr) != VK_SUCCESS || ptr == nullptr){
        return;
    }
    // Write mip 0 only — higher mips are populated by
    // `generateMipmaps` (the compute-shader blit pass). The staging
    // buffer's region for mip 0 starts at offset 0 by construction
    // (see makeTexture's region-build loop in GEVulkan.cpp).
    const std::size_t dstRow = mip0Width * bpt;
    auto *dst       = static_cast<std::uint8_t *>(ptr);
    const auto *src = static_cast<const std::uint8_t *>(bytes);
    if(bytesPerRow == 0 || bytesPerRow == dstRow){
        std::memcpy(dst, src, dstRow * mip0Height);
    } else {
        for(std::size_t y = 0; y < mip0Height; ++y){
            std::memcpy(dst + y * dstRow, src + y * bytesPerRow, dstRow);
        }
    }
    vmaUnmapMemory(engine->memAllocator, stagingAlloc);

    if(!engine->submitImmediateUploadFromStaging(*this)){
        std::cerr << "GEVulkanTexture::copyBytes: staging upload submit failed; "
                  << "texture left in indeterminate state." << std::endl;
    }
}

void GEVulkanTexture::copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion &destRegion){
    if(bytes == nullptr || bytesPerRow == 0 || destRegion.w == 0 || destRegion.h == 0){
        return;
    }
    if(stagingBuffer == VK_NULL_HANDLE){
        return;
    }

    // Pipeline-Completion-Extension-Plan §4.5 + §7.1, re-routed onto the
    // staging buffer instead of vmaMapMemory'ing the image directly.
    // `stagingRegions` was pre-computed at create time for the whole
    // mip × layer chain (ordered layer-major: index = arrayLayer*mips +
    // mipLevel), each entry carrying its subresource's tightly-packed
    // bufferOffset and full-mip imageExtent. We pick the entry for
    // (mipLevel, arrayLayer), stamp the sub-rect into that subresource's
    // slot at the mip's own row pitch, and upload only that one region so
    // we don't clobber sibling subresources still sitting in staging.
    const std::uint32_t mipLevels  = descriptor.mipLevels  > 0 ? descriptor.mipLevels  : 1;
    const std::uint32_t arrayLayers = descriptor.arrayLayers > 0 ? descriptor.arrayLayers : 1;
    if(destRegion.mipLevel >= mipLevels || destRegion.arrayLayer >= arrayLayers){
        std::cerr << "GEVulkanTexture::copyBytes(region): subresource out of range (mip "
                  << destRegion.mipLevel << "/" << mipLevels << ", layer "
                  << destRegion.arrayLayer << "/" << arrayLayers << ")." << std::endl;
        return;
    }
    const std::size_t regionIdx = static_cast<std::size_t>(destRegion.arrayLayer) * mipLevels
                                + destRegion.mipLevel;
    if(regionIdx >= stagingRegions.size()){
        std::cerr << "GEVulkanTexture::copyBytes(region): no staging region for subresource."
                  << std::endl;
        return;
    }
    const VkBufferImageCopy &sub = stagingRegions[regionIdx];
    const std::uint32_t mipW = sub.imageExtent.width;
    const std::uint32_t mipH = sub.imageExtent.height;
    const std::uint32_t mipD = sub.imageExtent.depth;
    const std::uint32_t depth = destRegion.d == 0 ? 1u : destRegion.d;

    // Fail loud rather than silently overrun into the next subresource's
    // staging slot if the caller hands us a region larger than the mip.
    if(destRegion.x + destRegion.w > mipW ||
       destRegion.y + destRegion.h > mipH ||
       destRegion.z + depth        > mipD){
        std::cerr << "GEVulkanTexture::copyBytes(region): region exceeds mip "
                  << destRegion.mipLevel << " extent (" << mipW << "x" << mipH << "x" << mipD
                  << ")." << std::endl;
        return;
    }

    const std::uint32_t bpt = bytesPerTexelFor(descriptor.pixelFormat);
    const std::size_t dstRow   = static_cast<std::size_t>(mipW) * bpt;          // mip's own pitch
    const std::size_t dstSlice = dstRow * mipH;                                 // one depth slice

    void *ptr = nullptr;
    if(vmaMapMemory(engine->memAllocator, stagingAlloc, &ptr) != VK_SUCCESS || ptr == nullptr){
        return;
    }
    auto *base = static_cast<std::uint8_t *>(ptr) + sub.bufferOffset;
    const auto *src = static_cast<const std::uint8_t *>(bytes);
    const std::size_t rowBytes = static_cast<std::size_t>(destRegion.w) * bpt;

    for(std::uint32_t z = 0; z < depth; ++z){
        for(std::uint32_t y = 0; y < destRegion.h; ++y){
            std::uint8_t *dst = base
                + static_cast<std::size_t>(destRegion.z + z) * dstSlice
                + static_cast<std::size_t>(destRegion.y + y) * dstRow
                + static_cast<std::size_t>(destRegion.x) * bpt;
            const std::uint8_t *srow = src
                + static_cast<std::size_t>(z) * bytesPerRow * destRegion.h
                + static_cast<std::size_t>(y) * bytesPerRow;
            std::memcpy(dst, srow, rowBytes);
        }
    }

    vmaUnmapMemory(engine->memAllocator, stagingAlloc);

    if(!engine->submitImmediateUploadFromStaging(*this, &sub, 1)){
        std::cerr << "GEVulkanTexture::copyBytes(region): staging upload submit failed." << std::endl;
    }
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
    if(stagingBuffer != VK_NULL_HANDLE){
        vmaDestroyBuffer(engine->memAllocator, stagingBuffer, stagingAlloc);
        stagingBuffer = VK_NULL_HANDLE;
        stagingAlloc  = nullptr;
    }
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
        VkBuffer       sb = stagingBuffer;
        VmaAllocation  sa = stagingAlloc;
        VmaAllocator alc = engine->memAllocator;
        VkDevice     dev = engine->device;
        std::vector<VkImageView> swizzledViews;
        swizzledViews.reserve(swizzledViewCache.size());
        for(auto & entry : swizzledViewCache){
            if(entry.second != VK_NULL_HANDLE) swizzledViews.push_back(entry.second);
        }
        swizzledViewCache.clear();
        std::vector<Retention::FenceGate> gates(pendingGates.begin(), pendingGates.end());
        // Staging buffer is gated on the same fences as the image:
        // the only thing that touches `stagingBuffer` is the upload
        // path, which always completes synchronously *before*
        // copyBytes returns. So by the time the texture is dropped,
        // the staging buffer is provably idle from the engine's
        // perspective. Bundling its destroy into the same retention
        // callback keeps both VMA frees adjacent and means we don't
        // accumulate a parallel buffer-only retention queue.
        engine->retentionQueue.enqueue(std::move(gates),
            [alc, dev, i, iv, a, sb, sa, swizzledViews = std::move(swizzledViews)]() {
                vmaDestroyImage(alc, i, a);
                if (iv != VK_NULL_HANDLE) vkDestroyImageView(dev, iv, nullptr);
                if (sb != VK_NULL_HANDLE) vmaDestroyBuffer(alc, sb, sa);
                for(VkImageView sv : swizzledViews){
                    vkDestroyImageView(dev, sv, nullptr);
                }
            });
        img = VK_NULL_HANDLE;
        img_view = VK_NULL_HANDLE;
        alloc = nullptr;
        stagingBuffer = VK_NULL_HANDLE;
        stagingAlloc  = nullptr;
    }
}

_NAMESPACE_END_
