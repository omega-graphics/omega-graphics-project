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
    SharedHandle<GECommandQueue> commandQueue;
    CAMetalLayer *metalLayer;
    NSSmartPtr currentDrawable;
    std::uint64_t traceResourceId = 0;
public:
    GEMetalNativeRenderTarget(SharedHandle<GECommandQueue> commandQueue,CAMetalLayer *metalLayer);
    ~GEMetalNativeRenderTarget();
    CGSize drawableSize;
    SharedHandle<CommandBuffer> commandBuffer() override;
    NSSmartPtr & getDrawable();
    void *nativeCommandQueue() override {
        return commandQueue->native();
    };
    void commitAndPresent() override;
    void reset();
    void notifyCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence) override;
    void submitCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> & commandBuffer) override;
    void submitCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence) override;
};

class GEMetalTextureRenderTarget : public GETextureRenderTarget {
    SharedHandle<GECommandQueue> commandQueue;
    std::uint64_t traceResourceId = 0;
public:
    GEMetalTextureRenderTarget(SharedHandle<GETexture> & texture,SharedHandle<GECommandQueue> & commandQueue);
    ~GEMetalTextureRenderTarget();
    SharedHandle<GEMetalTexture> texturePtr;
    SharedHandle<CommandBuffer> commandBuffer() override;
    void *nativeCommandQueue() override {
        return commandQueue->native();
    };
    SharedHandle<GETexture> underlyingTexture() override;
    void commit() override;
    void notifyCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence) override;
    void submitCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> & commandBuffer) override;
    void submitCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence) override;
};

_NAMESPACE_END_

#endif
