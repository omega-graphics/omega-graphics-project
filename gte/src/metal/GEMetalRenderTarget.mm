#import "GEMetalRenderTarget.h"
#include "GEMetal.h"
#import "GEMetalCommandQueue.h"
#include "GEMetalTexture.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

GEMetalNativeRenderTarget::GEMetalNativeRenderTarget(SharedHandle<GECommandQueue> presentQueue,CAMetalLayer *metalLayer):metalLayer(metalLayer),
presentQueue_(presentQueue),drawableSize([metalLayer drawableSize]),currentDrawable({nullptr}){
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Metal,
            "NativeRenderTarget",
            traceResourceId,
            metalLayer,
            float(drawableSize.width),
            float(drawableSize.height),
            static_cast<float>(metalLayer.contentsScale));
};

GEMetalNativeRenderTarget::~GEMetalNativeRenderTarget(){
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::Metal,
            "NativeRenderTarget",
            traceResourceId,
            metalLayer,
            drawableSize.width,
            drawableSize.height,
            static_cast<float>(metalLayer.contentsScale));
    if(currentDrawable.handle() != nullptr){
        [NSOBJECT_OBJC_BRIDGE(id,currentDrawable.handle()) release];
    }
}

NSSmartPtr & GEMetalNativeRenderTarget::getDrawable(){
    return currentDrawable;
};

void GEMetalNativeRenderTarget::acquireDrawable(){
    drawableSize = metalLayer.drawableSize;
    if(currentDrawable.handle() != nullptr){
        [NSOBJECT_OBJC_BRIDGE(id,currentDrawable.handle()) release];
        currentDrawable = NSObjectHandle{nullptr};
    }
    id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
    if(drawable != nil){
        currentDrawable = NSObjectHandle{NSOBJECT_CPP_BRIDGE [drawable retain]};
    }
}

void GEMetalNativeRenderTarget::present(){
    auto mtlqueue = (GEMetalCommandQueue *)presentQueue_.get();
    if(currentDrawable.handle() != nullptr){
        id<CAMetalDrawable> drawable = NSOBJECT_OBJC_BRIDGE(id<CAMetalDrawable>,currentDrawable.handle());

        // Prefer riding on the last pending command buffer. If WTK already
        // drained the queue (commit() pattern: commitToGPU → present), the
        // pending list is empty — allocate a present-only CB. Metal tracks
        // the drawable hazard, so the present waits for the render CBs that
        // wrote to the drawable's texture to complete first.
        if(mtlqueue->commandBuffers.empty()){
            auto presentCB = mtlqueue->getAvailableBuffer();
            auto *metalCB = (GEMetalCommandBuffer *)presentCB.get();
            id<MTLCommandBuffer> mtlCB = NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,metalCB->buffer.handle());
            [mtlCB presentDrawable:drawable];
            mtlqueue->submitCommandBuffer(presentCB);
        }
        else {
            auto & lastCB = mtlqueue->commandBuffers.back();
            auto *metalCB = (GEMetalCommandBuffer *)lastCB.get();
            id<MTLCommandBuffer> mtlCB = NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,metalCB->buffer.handle());
            [mtlCB presentDrawable:drawable];
        }
    }
    mtlqueue->commitToGPU();
    // Drop our retain — the command buffer keeps the drawable alive until
    // it has presented. Clearing currentDrawable lets the next frame's
    // startRenderPass acquire a fresh one via [layer nextDrawable].
    if(currentDrawable.handle() != nullptr){
        [NSOBJECT_OBJC_BRIDGE(id,currentDrawable.handle()) release];
        currentDrawable = NSObjectHandle{nullptr};
    }
}

GEMetalTextureRenderTarget::GEMetalTextureRenderTarget(SharedHandle<GETexture> & texture):texturePtr(std::dynamic_pointer_cast<GEMetalTexture>(texture)){
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    auto nativeTexture = texturePtr != nullptr ? texturePtr->native() : nullptr;
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Metal,
            "TextureRenderTarget",
            traceResourceId,
            nativeTexture);
};

GEMetalTextureRenderTarget::~GEMetalTextureRenderTarget(){
    auto nativeTexture = texturePtr != nullptr ? texturePtr->native() : nullptr;
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::Metal,
            "TextureRenderTarget",
            traceResourceId,
            nativeTexture);
}

SharedHandle<GETexture> GEMetalTextureRenderTarget::underlyingTexture() {
    return std::static_pointer_cast<GETexture>(texturePtr);
}




_NAMESPACE_END_
