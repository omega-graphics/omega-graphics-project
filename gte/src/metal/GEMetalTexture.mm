#import "GEMetalTexture.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

GEMetalTexture::GEMetalTexture(const TextureKind & kind,
                               const GETexture::GETextureUsage & usage,
                               const TexturePixelFormat & pixelFormat,NSSmartPtr texture): GETexture(kind,usage,pixelFormat),texture(texture){
    id<MTLDevice> device = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).device;
    resourceBarrier = NSObjectHandle {NSOBJECT_CPP_BRIDGE [device newFence]};
    auto mtlTexture = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle());
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Metal,
            "Texture",
            traceResourceId,
            texture.handle(),
            static_cast<float>(mtlTexture.width),
            static_cast<float>(mtlTexture.height));
    DEBUG_INFO(DEBUG_DOMAIN_RESOURCE, "Texture created: id=" << traceResourceId << " " << mtlTexture.width << "x" << mtlTexture.height);
};

GEMetalTexture::~GEMetalTexture(){
    auto mtlTexture = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle());
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::Metal,
            "Texture",
            traceResourceId,
            texture.handle(),
            static_cast<float>(mtlTexture.width),
            static_cast<float>(mtlTexture.height));
    DEBUG_INFO(DEBUG_DOMAIN_RESOURCE, "Texture destroyed: id=" << traceResourceId);
}

void GEMetalTexture::copyBytes(void *bytes, size_t bytesPerRow){
    auto width = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).width;
    auto height = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).height;
    const NSUInteger mipmapLevel = 0;
    if(bytes != nullptr && width > 0 && height > 0 && bytesPerRow > 0){
        [NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()) replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mipmapLevel withBytes:bytes bytesPerRow:bytesPerRow];
    }
};

void GEMetalTexture::copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion &destRegion){
    if(bytes == nullptr || bytesPerRow == 0 || destRegion.w == 0 || destRegion.h == 0){
        DEBUG_CRITICAL(DEBUG_DOMAIN_RESOURCE, "GETexture::copyBytes: invalid arguments (null bytes, zero bytesPerRow, or zero region extent)");
        return;
    }
    const NSUInteger depth = destRegion.d == 0 ? 1u : destRegion.d;
    MTLRegion region = MTLRegionMake3D(destRegion.x, destRegion.y, destRegion.z,
                                       destRegion.w, destRegion.h, depth);
    // §7.1: address (mipLevel, arrayLayer) via the slice-aware overload.
    // bytesPerImage = 0 is correct for 2D / single-slice uploads; a 3D or
    // array slice stride lands with §7.8's bytesPerImage overload.
    [NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()) replaceRegion:region
                                                             mipmapLevel:(NSUInteger)destRegion.mipLevel
                                                                   slice:(NSUInteger)destRegion.arrayLayer
                                                               withBytes:bytes
                                                             bytesPerRow:bytesPerRow
                                                           bytesPerImage:0];
}

size_t GEMetalTexture::getBytes(void *bytes, size_t bytesPerRow) {
    auto width = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).width;
    auto height = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).height;
    const NSUInteger mipmapLevel = 0;
    if(bytes != nullptr && width > 0 && height > 0 && bytesPerRow > 0){
        [NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()) getBytes:bytes bytesPerRow:bytesPerRow fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mipmapLevel];
    }
    return (size_t)height* bytesPerRow;
}

static MTLTextureSwizzle metalSwizzleFor(TextureSwizzleChannel ch, MTLTextureSwizzle positionalIdentity){
    switch(ch){
        case TextureSwizzleChannel::Identity: return positionalIdentity;
        case TextureSwizzleChannel::Red:      return MTLTextureSwizzleRed;
        case TextureSwizzleChannel::Green:    return MTLTextureSwizzleGreen;
        case TextureSwizzleChannel::Blue:     return MTLTextureSwizzleBlue;
        case TextureSwizzleChannel::Alpha:    return MTLTextureSwizzleAlpha;
        case TextureSwizzleChannel::Zero:     return MTLTextureSwizzleZero;
        case TextureSwizzleChannel::One:      return MTLTextureSwizzleOne;
    }
    return positionalIdentity;
}

id<MTLTexture> GEMetalTexture::getOrCreateSwizzledView(const TextureSwizzle & swizzle){
    id<MTLTexture> base = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>, texture.handle());
    if(swizzle.isIdentity()) return base;
    for(auto & entry : swizzledViewCache){
        if(entry.first == swizzle){
            return NSOBJECT_OBJC_BRIDGE(id<MTLTexture>, entry.second.handle());
        }
    }

    if(@available(macOS 10.15, iOS 13.0, *)){
        MTLTextureSwizzleChannels mtlSwizzle = MTLTextureSwizzleChannelsMake(
            metalSwizzleFor(swizzle.r, MTLTextureSwizzleRed),
            metalSwizzleFor(swizzle.g, MTLTextureSwizzleGreen),
            metalSwizzleFor(swizzle.b, MTLTextureSwizzleBlue),
            metalSwizzleFor(swizzle.a, MTLTextureSwizzleAlpha)
        );
        id<MTLTexture> view = [base newTextureViewWithPixelFormat:base.pixelFormat
                                                       textureType:base.textureType
                                                            levels:NSMakeRange(0, base.mipmapLevelCount)
                                                            slices:NSMakeRange(0, base.arrayLength)
                                                           swizzle:mtlSwizzle];
        NSSmartPtr held = NSObjectHandle{NSOBJECT_CPP_BRIDGE view};
        swizzledViewCache.push_back({swizzle, held});
        return view;
    }
    // No-op fallback on platforms without MTLTextureSwizzleChannels.
    return base;
}


_NAMESPACE_END_
