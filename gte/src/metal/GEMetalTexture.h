#include "GEMetal.h"

#include "omegaGTE/GTEBase.h"
#include "omegaGTE/GETexture.h"
#include <cstdint>
#include <utility>

#import <Metal/Metal.h>

#ifndef OMEGAGTE_METAL_GEMETALTEXTURE_H
#define OMEGAGTE_METAL_GEMETALTEXTURE_H

_NAMESPACE_BEGIN_

class GEMetalTexture : public GETexture {
    NSSmartPtr texture;
    NSSmartPtr resourceBarrier;

    bool needsBarrier = false;
    std::uint64_t traceResourceId = 0;

    /// Lazily-created `id<MTLTexture>` views for non-identity swizzles
    /// requested at bind time. Linear scan; views released alongside the
    /// parent `texture` via NSSmartPtr/ARC.
    OmegaCommon::Vector<std::pair<TextureSwizzle, NSSmartPtr>> swizzledViewCache;

    friend class GEMetalCommandBuffer;
    friend class GEMetalTextureRenderTarget;
public:
    void setName(OmegaCommon::StrRef name) override {
        NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,texture.handle()).label = [[NSString alloc] initWithUTF8String:name.data()];
    }
    void *native() override {
        return const_cast<void *>(texture.handle());
    }
    void copyBytes(void *bytes,size_t bytesPerRow) override;
    size_t getBytes(void *bytes, size_t bytesPerRow) override;

    /// Resolve (or lazily create) a Metal texture view for the given
    /// swizzle. Identity returns the primary `texture`; any other value
    /// hits the per-texture cache or builds a new view with
    /// `newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:`.
    id<MTLTexture> getOrCreateSwizzledView(const TextureSwizzle & swizzle);

    ~GEMetalTexture() override;
    explicit GEMetalTexture(const GETexture::GETextureType &type,
                   const GETexture::GETextureUsage & usage,
                   const TexturePixelFormat & pixelFormat,
                   NSSmartPtr texture);
};

_NAMESPACE_END_

#endif
