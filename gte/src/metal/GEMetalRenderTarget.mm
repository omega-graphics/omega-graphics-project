#import "GEMetalRenderTarget.h"
#include "GEMetal.h"
#import "GEMetalCommandQueue.h"
#include "GEMetalTexture.h"

_NAMESPACE_BEGIN_

GEMetalNativeRenderTarget::GEMetalNativeRenderTarget(SharedHandle<GECommandQueue> commandQueue,CAMetalLayer *metalLayer):metalLayer(metalLayer),
commandQueue(commandQueue),drawableSize([metalLayer drawableSize]),currentDrawable({NSOBJECT_CPP_BRIDGE [metalLayer nextDrawable]}){
    
};

NSSmartPtr & GEMetalNativeRenderTarget::getDrawable(){
    return currentDrawable;
};

SharedHandle<GERenderTarget::CommandBuffer> GEMetalNativeRenderTarget::commandBuffer(){
    return std::shared_ptr<GERenderTarget::CommandBuffer>(new GERenderTarget::CommandBuffer(this,GERenderTarget::CommandBuffer::Native,commandQueue->getAvailableBuffer()));
};


void GEMetalNativeRenderTarget::commitAndPresent(){
    auto mtlqueue = (GEMetalCommandQueue *)commandQueue.get();
    mtlqueue->commitToGPUAndPresent(currentDrawable);
    [metalLayer setNeedsDisplay];
};

void GEMetalNativeRenderTarget::reset(){
    currentDrawable = NSObjectHandle{ NSOBJECT_CPP_BRIDGE [metalLayer nextDrawable]};
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
    
};


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
    texturePtr->needsBarrier = true;
    auto lastCommandBuffer = std::dynamic_pointer_cast<GEMetalCommandBuffer>(((GEMetalCommandQueue *)commandQueue.get())->commandBuffers.back());
    id<MTLResourceStateCommandEncoder> resStateEncoder = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,lastCommandBuffer->buffer.handle()) resourceStateCommandEncoder];
    [resStateEncoder updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,texturePtr->resourceBarrier.handle())];
    [resStateEncoder endEncoding];
    commandQueue->commitToGPU();
};

SharedHandle<GETexture> GEMetalTextureRenderTarget::underlyingTexture() {
    return std::static_pointer_cast<GETexture>(texturePtr);
}




_NAMESPACE_END_
