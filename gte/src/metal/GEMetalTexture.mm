#import "GEMetalTexture.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

GEMetalTexture::GEMetalTexture(const GETexture::GETextureType &type,
                               const GETexture::GETextureUsage & usage,
                               const TexturePixelFormat & pixelFormat,NSSmartPtr texture): GETexture(type,usage,pixelFormat),texture(texture){
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
}

void GEMetalTexture::copyBytes(void *bytes, size_t bytesPerRow){
    auto width = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).width;
    auto height = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).height;
    const NSUInteger mipmapLevel = 0;
    if(bytes != nullptr && width > 0 && height > 0 && bytesPerRow > 0){
        [NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()) replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mipmapLevel withBytes:bytes bytesPerRow:bytesPerRow];
    }
};

size_t GEMetalTexture::getBytes(void *bytes, size_t bytesPerRow) {
    auto width = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).width;
    auto height = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).height;
    const NSUInteger mipmapLevel = 0;
    if(bytes != nullptr && width > 0 && height > 0 && bytesPerRow > 0){
        [NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()) getBytes:bytes bytesPerRow:bytesPerRow fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mipmapLevel];
    }
    return (size_t)height* bytesPerRow;
}


_NAMESPACE_END_
