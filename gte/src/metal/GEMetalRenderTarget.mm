#import "GEMetalRenderTarget.h"
#include "GEMetal.h"
#import "GEMetalCommandQueue.h"
#include "GEMetalTexture.h"
#include "../common/GEResourceTracker.h"

_NAMESPACE_BEGIN_

static inline void runOnMainThreadSync(dispatch_block_t block){
    if([NSThread isMainThread]){
        block();
    }
    else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

GEMetalNativeRenderTarget::GEMetalNativeRenderTarget(SharedHandle<GECommandQueue> commandQueue,CAMetalLayer *metalLayer):metalLayer(metalLayer),
commandQueue(commandQueue),drawableSize([metalLayer drawableSize]),currentDrawable({nullptr}){
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Metal,
            "NativeRenderTarget",
            traceResourceId,
            metalLayer,
            drawableSize.width,
            drawableSize.height,
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

SharedHandle<GERenderTarget::CommandBuffer> GEMetalNativeRenderTarget::commandBuffer(){
    // Read CAMetalLayer properties directly — thread-safe.
    drawableSize = metalLayer.drawableSize;
    reset();
    return std::shared_ptr<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,GERenderTarget::CommandBuffer::Native,commandQueue->getAvailableBuffer()));
};


void GEMetalNativeRenderTarget::commitAndPresent(){
    auto mtlqueue = (GEMetalCommandQueue *)commandQueue.get();
    if(currentDrawable.handle() != nullptr){
        id<CAMetalDrawable> drawable = NSOBJECT_OBJC_BRIDGE(id<CAMetalDrawable>,currentDrawable.handle());

        auto & lastCB = mtlqueue->commandBuffers.back();
        auto *metalCB = (GEMetalCommandBuffer *)lastCB.get();
        id<MTLCommandBuffer> mtlCB = NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,metalCB->buffer.handle());
        [mtlCB presentDrawable:drawable];
        mtlqueue->commitToGPU();
    }
    else {
        mtlqueue->commitToGPU();
    }
};

void GEMetalNativeRenderTarget::reset(){
    if(currentDrawable.handle() != nullptr){
        [NSOBJECT_OBJC_BRIDGE(id,currentDrawable.handle()) release];
        currentDrawable = NSObjectHandle{nullptr};
    }
    id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
    if(drawable != nil){
        currentDrawable = NSObjectHandle{NSOBJECT_CPP_BRIDGE [drawable retain]};
    }
};

void GEMetalNativeRenderTarget::notifyCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> &commandBuffer,
                                                    SharedHandle<GEFence> &fence) {
    commandQueue->notifyCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEMetalNativeRenderTarget::submitCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> &commandBuffer,
                                                    SharedHandle<GEFence> &fence) {
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEMetalNativeRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &commandBuffer){
    if(commandBuffer->commandBuffer)
        commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
};




GEMetalTextureRenderTarget::GEMetalTextureRenderTarget(SharedHandle<GETexture> & texture,SharedHandle<GECommandQueue> & commandQueue):commandQueue(commandQueue),texturePtr(std::dynamic_pointer_cast<GEMetalTexture>(texture)){
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


SharedHandle<GERenderTarget::CommandBuffer> GEMetalTextureRenderTarget::commandBuffer(){
    return std::shared_ptr<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,GERenderTarget::CommandBuffer::Texture,commandQueue->getAvailableBuffer()));
};

void GEMetalTextureRenderTarget::submitCommandBuffer(SharedHandle<CommandBuffer> &commandBuffer){
    if(commandBuffer->commandBuffer)
        commandQueue->submitCommandBuffer(commandBuffer->commandBuffer);
};

void GEMetalTextureRenderTarget::notifyCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> &commandBuffer,
                                                     SharedHandle<GEFence> &fence) {
    commandQueue->notifyCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEMetalTextureRenderTarget::submitCommandBuffer(SharedHandle<GERenderTarget::CommandBuffer> &commandBuffer,
                                                     SharedHandle<GEFence> &fence) {
    commandQueue->submitCommandBuffer(commandBuffer->commandBuffer,fence);
}

void GEMetalTextureRenderTarget::commit(){
    texturePtr->needsBarrier = false;
    commandQueue->commitToGPU();
};

SharedHandle<GETexture> GEMetalTextureRenderTarget::underlyingTexture() {
    return std::static_pointer_cast<GETexture>(texturePtr);
}




_NAMESPACE_END_
