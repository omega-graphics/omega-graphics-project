#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "GEMetal.h"
#include "omegaGTE/GECommandQueue.h"

#ifndef OMEGAGTE_METAL_GEMETALCOMMANDQUEUE_H
#define OMEGAGTE_METAL_GEMETALCOMMANDQUEUE_H

_NAMESPACE_BEGIN_

    class GEMetalCommandQueue;
    class GEMetalRenderPipelineState;
    class GEMetalComputePipelineState;

    class GEMetalCommandBuffer final : public GECommandBuffer {
        id<MTLRenderCommandEncoder> rp = nil;
        id<MTLComputeCommandEncoder> cp = nil;
        id<MTLBlitCommandEncoder> bp = nil;

         #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
        id<MTLAccelerationStructureCommandEncoder> ap = nil;
        #endif

        GEMetalCommandQueue *parentQueue = nullptr;

        GEMetalRenderPipelineState *renderPipelineState = nullptr;
        GEMetalComputePipelineState *computePipelineState = nullptr;

        friend class GEMetalCommandQueue;
        unsigned getResourceLocalIndexFromGlobalIndex(unsigned _id,omegasl_shader & shader);
        bool shaderHasWriteAccessForResource(unsigned & _id,omegasl_shader & shader);
        void _present_drawable(NSSmartPtr & drawable);
        void _commit();
    public:
        NSSmartPtr buffer;

        void setName(OmegaCommon::StrRef name) override{
            NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()).label = [[NSString alloc] initWithUTF8String:name.data()];
        }

        void *native() override {
            return const_cast<void *>(buffer.handle());
        };

        void startBlitPass() override;
        void copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) override;
        void copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest, const TextureRegion &region, const GPoint3D &destCoord) override;
        void finishBlitPass() override;

        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

        void beginAccelStructPass() override;
        void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &structure,const GEAccelerationStructDescriptor &desc) override;
        void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc) override;
        void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest) override;
        void finishAccelStructPass() override;

        void dispatchRays(unsigned int x, unsigned int y, unsigned int z) override;

        #endif
        
        void startRenderPass(const GERenderPassDescriptor &desc) override;
        void setVertexBuffer(SharedHandle<GEBuffer> &buffer) override;
        void setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) override;
        void bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned id) override;
        void bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned id) override;
        void bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned id) override;
        void bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned id) override;
        void setViewports(std::vector<GEViewport> viewports) override;
        void setScissorRects(std::vector<GEScissorRect> scissorRects) override;
        void setStencilRef(unsigned ref) override;
        void drawPolygons(RenderPassDrawPolygonType polygonType, unsigned vertexCount, size_t startIdx) override;
        void finishRenderPass() override;
        
        void startComputePass(const GEComputePassDescriptor &desc) override;
        void setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) override;
        void bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned id) override;
        void bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned id) override;
        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
        void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct, unsigned int id) override;
        #endif
        void dispatchThreads(unsigned int x, unsigned int y, unsigned int z) override;
        void finishComputePass() override;

        GEMetalCommandBuffer(GEMetalCommandQueue *parentQueue);
        ~GEMetalCommandBuffer();
        void reset() override;
    };

    class GEMetalCommandQueue : public GECommandQueue {
        NSSmartPtr commandQueue;

        std::vector<SharedHandle<GECommandBuffer>> commandBuffers;

        dispatch_semaphore_t semaphore;

        
        friend class GEMetalCommandBuffer;
        friend class GEMetalNativeRenderTarget;
        friend class GEMetalTextureRenderTarget;
        void commitToGPUAndPresent(NSSmartPtr & drawable);
    public:
        void setName(OmegaCommon::StrRef name) override{
            NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,commandQueue.handle()).label = [[NSString alloc] initWithUTF8String:name.data()];
        }
        void *native() override {
            return const_cast<void *>(commandQueue.handle());
        }

        SharedHandle<GECommandBuffer> getAvailableBuffer() override;
        GEMetalCommandQueue(NSSmartPtr & commandQueue,unsigned size);
        ~GEMetalCommandQueue() override;
        void notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &waitFence) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer) override;
        void submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer, SharedHandle<GEFence> &signalFence) override;
        void commitToGPU() override;
        void commitToGPUAndWait() override;
    };
_NAMESPACE_END_

#endif
