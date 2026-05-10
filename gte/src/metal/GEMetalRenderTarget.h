#include "GEMetal.h"
#include "omegaGTE/GERenderTarget.h"
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "GEMetalTexture.h"
#include "GEMetalCommandQueue.h"
#include <cstdint>


#ifndef OMEGAGTE_METAL_GEMETALRENDERTARGET_H
#define OMEGAGTE_METAL_GEMETALRENDERTARGET_H

_NAMESPACE_BEGIN_


class GEMetalNativeRenderTarget : public GENativeRenderTarget {
    SharedHandle<GECommandQueue> presentQueue_;
    CAMetalLayer *metalLayer;
    NSSmartPtr currentDrawable;
    std::uint64_t traceResourceId = 0;
public:
    GEMetalNativeRenderTarget(SharedHandle<GECommandQueue> presentQueue,CAMetalLayer *metalLayer);
    ~GEMetalNativeRenderTarget();
    CGSize drawableSize;
    NSSmartPtr & getDrawable();
    /// Acquire (or refresh) the current `CAMetalDrawable` and the current
    /// drawable size. Callers should invoke this once per frame before
    /// recording a render pass that targets the drawable.
    void acquireDrawable();
    PixelFormat pixelFormat() override {
        return PixelFormat::BGRA8Unorm;
    };
    SharedHandle<GECommandQueue> presentQueue() const override { return presentQueue_; }
    void present() override;
};

class GEMetalTextureRenderTarget : public GETextureRenderTarget {
    std::uint64_t traceResourceId = 0;
public:
    explicit GEMetalTextureRenderTarget(SharedHandle<GETexture> & texture);
    ~GEMetalTextureRenderTarget();
    SharedHandle<GEMetalTexture> texturePtr;
    SharedHandle<GETexture> underlyingTexture() override;
};

_NAMESPACE_END_

#endif
