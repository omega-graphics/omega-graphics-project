#import "GEMetalCommandQueue.h"
#include <memory>
#import "GEMetalRenderTarget.h"
#import "GEMetalPipeline.h"
#import "GEMetal.h"

#include <cstdlib>

#import <QuartzCore/QuartzCore.h>

/// @note In Metal, we use id<MTLFence> as a Resource Barrier between different shader stages.

_NAMESPACE_BEGIN_

    GEMetalCommandBuffer::GEMetalCommandBuffer(GEMetalCommandQueue *parentQueue):parentQueue(parentQueue),
    buffer({NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,parentQueue->commandQueue.handle()) commandBuffer]}){
       
    };

    unsigned GEMetalCommandBuffer::getResourceLocalIndexFromGlobalIndex(unsigned _id,omegasl_shader & shader){
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> descArr {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto l : descArr){
            if(l.location == _id){
                return l.gpu_relative_loc;
            }
        }
        return -1;
    };

    bool GEMetalCommandBuffer::shaderHasWriteAccessForResource(unsigned int &_id, omegasl_shader &shader) {
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> descArr {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto l : descArr){
            if(l.location == _id){
                return l.io_mode != OMEGASL_SHADER_DESC_IO_IN;
            }
        }
        return false;
    }

    void GEMetalCommandBuffer::startBlitPass(){
        buffer.assertExists();
        bp = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) blitCommandEncoder];
    };

    void GEMetalCommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) {
        assert(bp && "Must be in BLIT PASS");
        auto srcTexture = (GEMetalTexture *)src.get(),destTexture = (GEMetalTexture *)dest.get();

        /// Use MTLFences as ResourceBarrier.


        if(srcTexture->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,srcTexture->resourceBarrier.handle())];
        }


        [bp copyFromTexture:
                NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,((GEMetalTexture *)src.get())->texture.handle())
                  toTexture:
        NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,((GEMetalTexture *)dest.get())->texture.handle())];
        destTexture->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,destTexture->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest,
                                                    const TextureRegion &region, const GPoint3D &destCoord) {
        assert(bp && "Must be in BLIT PASS");
        auto mtl_src_texture = (GEMetalTexture *)src.get();
        auto mtl_dest_texture = (GEMetalTexture *)dest.get();

        if(mtl_src_texture->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_src_texture->resourceBarrier.handle())];
        }

        [bp copyFromTexture: NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_src_texture->texture.handle())
                sourceSlice:0 sourceLevel:0
                  toTexture: NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_dest_texture->texture.handle())
           destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:
                        NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_src_texture->texture.handle()).mipmapLevelCount];

        mtl_dest_texture->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_dest_texture->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::finishBlitPass(){
        [bp endEncoding];
        bp = nil;
    };


    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

     void GEMetalCommandBuffer::beginAccelStructPass(){
        ap = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) accelerationStructureCommandEncoder];
    };

    void GEMetalCommandBuffer::buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &structure,const GEAccelerationStructDescriptor &desc){
        MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        auto metal_structure = std::dynamic_pointer_cast<GEMetalAccelerationStruct>(structure);
        [ap buildAccelerationStructure:NSOBJECT_OBJC_BRIDGE(id<MTLAccelerationStructure>,metal_structure->accelStruct.handle()) 
            descriptor:d 
            scratchBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metal_structure->scratchBuffer->metalBuffer.handle()) 
            scratchBufferOffset:0];

    }

    void GEMetalCommandBuffer::copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                         SharedHandle<GEAccelerationStruct> &dest) {
        auto src_struct = std::dynamic_pointer_cast<GEMetalAccelerationStruct>(src), dest_struct = std::dynamic_pointer_cast<GEMetalAccelerationStruct>(dest);
//        [ap copyAccelerationStructure:]
    }

    void GEMetalCommandBuffer::refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc){
         MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
         auto metal_structure = std::dynamic_pointer_cast<GEMetalAccelerationStruct>(src);
         [ap refitAccelerationStructure:NSOBJECT_OBJC_BRIDGE(id<MTLAccelerationStructure>,metal_structure->accelStruct.handle()) 
            descriptor:d 
            destination:NSOBJECT_OBJC_BRIDGE(id<MTLAccelerationStructure>,metal_structure->accelStruct.handle()) scratchBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metal_structure->scratchBuffer->metalBuffer.handle())  scratchBufferOffset:0];
    };

    void GEMetalCommandBuffer::finishAccelStructPass(){
        [ap endEncoding];
        ap = nil;
    };

    #endif

    void GEMetalCommandBuffer::setVertexBuffer(SharedHandle<GEBuffer> &buffer) {

    }

    void GEMetalCommandBuffer::startRenderPass(const GERenderPassDescriptor & desc){
        buffer.assertExists();
        MTLRenderPassDescriptor *renderPassDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        renderPassDesc.renderTargetArrayLength = 1;

        id<MTLTexture> multiSampleTextureTarget = nil;

        bool needsBarrier = false;
        NSSmartPtr barrier = NSObjectHandle{nullptr};

        if(desc.nRenderTarget != nullptr){
            NSLog(@"Prepare Render Pass For NativeTarget");
            auto *n_rt = (GEMetalNativeRenderTarget *)desc.nRenderTarget;
            auto metalDrawable = n_rt->getDrawable();
            metalDrawable.assertExists();
            renderPassDesc.renderTargetWidth = (NSUInteger)n_rt->drawableSize.width;
            renderPassDesc.renderTargetHeight = (NSUInteger)n_rt->drawableSize.height;
            id<MTLTexture> renderTarget;
            if(desc.multisampleResolve){
                renderTarget = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,((GEMetalTexture *)desc.resolveDesc.multiSampleTextureSrc.get())->texture.handle());
                multiSampleTextureTarget = NSOBJECT_OBJC_BRIDGE(id<CAMetalDrawable>,metalDrawable.handle()).texture;
            }
            else {
                renderTarget =  NSOBJECT_OBJC_BRIDGE(id<CAMetalDrawable>,metalDrawable.handle()).texture;
            }
            renderPassDesc.colorAttachments[0].texture =renderTarget;
           
            if(!desc.depthStencilAttachment.disabled){
                renderPassDesc.depthAttachment.texture = renderPassDesc.stencilAttachment.texture = renderTarget;
            }
        }
        else if(desc.tRenderTarget != nullptr){
            NSLog(@"Prepare Render Pass For TextureTarget");
            auto *t_rt = (GEMetalTextureRenderTarget *)desc.tRenderTarget;
            if(t_rt->texturePtr->needsBarrier){
                needsBarrier = true;
                barrier = t_rt->texturePtr->resourceBarrier;
            }

            id<MTLTexture> renderTarget;
            if(desc.multisampleResolve){
                renderTarget = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,((GEMetalTexture *)desc.resolveDesc.multiSampleTextureSrc.get())->texture.handle());
                multiSampleTextureTarget =  NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,t_rt->texturePtr->texture.handle());
            }
            else {
                renderTarget =  NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,t_rt->texturePtr->texture.handle());
            }
            renderPassDesc.renderTargetWidth = renderTarget.width;
            renderPassDesc.renderTargetHeight = renderTarget.height;
            renderPassDesc.colorAttachments[0].texture = renderTarget;
            if(!desc.depthStencilAttachment.disabled){
                renderPassDesc.depthAttachment.texture = renderPassDesc.stencilAttachment.texture = renderTarget;
            }
        }
        else {
            DEBUG_STREAM("Failed to Create GERenderPass");
            exit(1);
        };

        if(desc.multisampleResolve){
            renderPassDesc.colorAttachments[0].resolveTexture = multiSampleTextureTarget;
            renderPassDesc.colorAttachments[0].resolveSlice = desc.resolveDesc.slice;
            renderPassDesc.colorAttachments[0].resolveDepthPlane = desc.resolveDesc.depth;
            renderPassDesc.colorAttachments[0].resolveLevel = desc.resolveDesc.level;
        }
        
        switch (desc.colorAttachment->loadAction) {
            case GERenderPassDescriptor::ColorAttachment::Load : {
                renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionLoad;
                renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::LoadPreserve : {
                renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionLoad;
                renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Discard : {
                renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Clear : {
                renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
                renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
                auto & clearColor = desc.colorAttachment->clearColor;
                renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(clearColor.r,clearColor.g,clearColor.b,clearColor.a);
                break;
            }
        }

        if(!desc.depthStencilAttachment.disabled){
            renderPassDesc.depthAttachment.clearDepth = (double)desc.depthStencilAttachment.clearDepth;
            renderPassDesc.stencilAttachment.clearStencil = desc.depthStencilAttachment.clearStencil;
            switch (desc.depthStencilAttachment.depthloadAction) {
                case GERenderPassDescriptor::DepthStencilAttachment::Load : {
                    renderPassDesc.depthAttachment.loadAction = MTLLoadActionLoad;
                    renderPassDesc.depthAttachment.storeAction = MTLStoreActionDontCare;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::LoadPreserve : {
                    renderPassDesc.depthAttachment.loadAction = MTLLoadActionLoad;
                    renderPassDesc.depthAttachment.storeAction = MTLStoreActionStore;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::Clear : {
                    renderPassDesc.depthAttachment.loadAction = MTLLoadActionClear;
                    renderPassDesc.depthAttachment.storeAction = MTLStoreActionStore;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::Discard : {
                    renderPassDesc.depthAttachment.loadAction = MTLLoadActionDontCare;
                    renderPassDesc.depthAttachment.storeAction = MTLStoreActionDontCare;
                     break;
                }
            }
            switch (desc.depthStencilAttachment.stencilLoadAction) {
                case GERenderPassDescriptor::DepthStencilAttachment::Load : {
                    renderPassDesc.stencilAttachment.loadAction = MTLLoadActionLoad;
                    renderPassDesc.stencilAttachment.storeAction = MTLStoreActionDontCare;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::LoadPreserve : {
                    renderPassDesc.stencilAttachment.loadAction = MTLLoadActionLoad;
                    renderPassDesc.stencilAttachment.storeAction = MTLStoreActionStore;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::Clear : {
                    renderPassDesc.stencilAttachment.loadAction = MTLLoadActionClear;
                    renderPassDesc.stencilAttachment.storeAction = MTLStoreActionStore;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::Discard : {
                    renderPassDesc.stencilAttachment.loadAction = MTLLoadActionDontCare;
                    renderPassDesc.stencilAttachment.storeAction = MTLStoreActionDontCare;
                    break;
                }
            }
        }

        rp = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) renderCommandEncoderWithDescriptor:renderPassDesc];
        NSLog(@"Starting Render Pass: %@",rp);
        if(needsBarrier){
            /// Ensure texture is ready to render to when fragment stage begins.
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,barrier.handle()) beforeStages:MTLRenderStageFragment];
        }
    };

    void GEMetalCommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState){
        auto *ps = (GEMetalRenderPipelineState *)pipelineState.get();
        ps->renderPipelineState.assertExists();
        NSLog(@"Render Pipeline Set: %@",(id<MTLRenderPipelineState>)ps->renderPipelineState.handle());
        [rp setRenderPipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLRenderPipelineState>,ps->renderPipelineState.handle())];
        
        [rp setFrontFacingWinding:ps->rasterizerState.winding];
        [rp setCullMode:ps->rasterizerState.cullMode];
        [rp setTriangleFillMode:ps->rasterizerState.fillMode];
        if(ps->hasDepthStencilState){
            [rp setDepthStencilState:NSOBJECT_OBJC_BRIDGE(id<MTLDepthStencilState>,ps->depthStencilState.handle())];
            [rp setDepthBias:ps->rasterizerState.depthBias slopeScale:ps->rasterizerState.slopeScale clamp:ps->rasterizerState.depthClamp];
        }
        renderPipelineState = ps;
    };

    void GEMetalCommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Vertex Func when not in render pass");
        auto *metalBuffer = (GEMetalBuffer *)buffer.get();
        metalBuffer->metalBuffer.assertExists();


        if(metalBuffer->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) beforeStages:MTLRenderStageVertex];
            metalBuffer->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->vertexShader->internal);
         NSLog(@"Binding GEBuffer at Vertex Shader: %@ At Index: %i",NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()),index);
        [rp setVertexBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()) offset:0 atIndex:index];

        if(shaderHasWriteAccessForResource(_id,renderPipelineState->vertexShader->internal)){
            metalBuffer->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) afterStages:MTLRenderStageVertex];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Vertex Func when not in render pass");
        auto *metalTexture = (GEMetalTexture *)texture.get();

        if(metalTexture->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) beforeStages:MTLRenderStageVertex];
            metalTexture->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->vertexShader->internal);
        NSLog(@"Binding GETexture at Vertex Shader: %@ At Index: %i",NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,metalTexture->texture.handle()),index);
        [rp setVertexTexture:NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,metalTexture->texture.handle()) atIndex:index];
        if(shaderHasWriteAccessForResource(_id,renderPipelineState->vertexShader->internal)){
            metalTexture->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) afterStages:MTLRenderStageVertex];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Fragment Func when not in render pass");
        auto *metalBuffer = (GEMetalBuffer *)buffer.get();
        metalBuffer->metalBuffer.assertExists();


        if(metalBuffer->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) beforeStages:MTLRenderStageFragment];
            metalBuffer->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);

        NSLog(@"Binding GEBuffer at Fragment Shader: %@ At Index: %i",NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()),index);
        [rp setFragmentBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()) offset:0 atIndex:index];
        if(shaderHasWriteAccessForResource(_id,renderPipelineState->fragmentShader->internal)){
            metalBuffer->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) afterStages:MTLRenderStageFragment];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Fragment Func when not in render pass");
        auto *metalTexture = (GEMetalTexture *)texture.get();


        if(metalTexture->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) beforeStages:MTLRenderStageFragment];
            metalTexture->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);
        NSLog(@"Binding GETexture at Fragment Shader: %@ At Index: %i",NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,metalTexture->texture.handle()),index);

        [rp setFragmentTexture:NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,metalTexture->texture.handle()) atIndex:index];
        // if(shaderHasWriteAccessForResource(_id,renderPipelineState->fragmentShader->internal)){
        //     metalTexture->needsBarrier = true;
        //     [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) afterStages:MTLRenderStageFragment];
        // }
    };

    void GEMetalCommandBuffer::setViewports(std::vector<GEViewport> viewports){
        std::vector<MTLViewport> metalViewports;
        auto viewports_it = viewports.begin();
        while(viewports_it != viewports.end()){
            GEViewport & viewport = *viewports_it;
            MTLViewport metalViewport;
            metalViewport.originX = viewport.x;
            metalViewport.originY = viewport.y;
            metalViewport.width = viewport.width;
            metalViewport.height = viewport.height;
            metalViewport.znear = viewport.nearDepth;
            metalViewport.zfar = viewport.farDepth;
            metalViewports.push_back(metalViewport);
            ++viewports_it;
        };
        auto s = metalViewports.size();
        [rp setViewports:metalViewports.data() count:s];
    };

    void GEMetalCommandBuffer::setScissorRects(std::vector<GEScissorRect> scissorRects){
        std::vector<MTLScissorRect> metalRects;
        auto rects_it = scissorRects.begin();
        while(rects_it != scissorRects.end()){
            GEScissorRect & rect = *rects_it;
            MTLScissorRect metalRect;
            metalRect.x = (NSUInteger)rect.x;
            metalRect.y = (NSUInteger)rect.y;
            metalRect.width = (NSUInteger)rect.width;
            metalRect.height = (NSUInteger)rect.height;
            metalRects.push_back(metalRect);
            ++rects_it;
        };
        auto s = metalRects.size();
        [rp setScissorRects:metalRects.data() count:s];
    };

    void GEMetalCommandBuffer::setStencilRef(unsigned int ref) {
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        [rp setStencilReferenceValue:ref];
    }
    
    void GEMetalCommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType,unsigned vertexCount,size_t startIdx){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        MTLPrimitiveType primativeType;
        if(polygonType == GECommandBuffer::RenderPassDrawPolygonType::Triangle){
            primativeType = MTLPrimitiveTypeTriangle;
        }
        else if(polygonType == GECommandBuffer::RenderPassDrawPolygonType::TriangleStrip){
            primativeType = MTLPrimitiveTypeTriangleStrip;
        }
        else {
            primativeType = MTLPrimitiveTypeTriangle;
        };

//        NSLog(@"CALLING DRAW PRIMITIVES");
        [rp drawPrimitives:primativeType vertexStart:startIdx vertexCount:vertexCount];
    };

    void GEMetalCommandBuffer::finishRenderPass(){
        renderPipelineState = nullptr;
        [rp endEncoding];
        rp = nil;
    };

    void GEMetalCommandBuffer::startComputePass(const GEComputePassDescriptor & desc){
        buffer.assertExists();
        cp = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) computeCommandEncoder];
    };

    void GEMetalCommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> & pipelineState){
        assert(cp != nil && "");
        auto * ps = (GEMetalComputePipelineState *)pipelineState.get();
        ps->computePipelineState.assertExists();
        computePipelineState = ps;
        [cp setComputePipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLComputePipelineState>,ps->computePipelineState.handle())];
    };

    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int _id) {
        assert(cp != nil && "");
        auto mtl_buffer = (GEMetalBuffer *)buffer.get();

        if(mtl_buffer->needsBarrier){
            [cp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_buffer->resourceBarrier.handle())];
            mtl_buffer->needsBarrier = false;
        }

        [cp setBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,mtl_buffer->metalBuffer.handle()) offset:0 atIndex:getResourceLocalIndexFromGlobalIndex(_id,computePipelineState->computeShader->internal)];
        if(shaderHasWriteAccessForResource(_id,computePipelineState->computeShader->internal)){
            mtl_buffer->needsBarrier = true;
            [cp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_buffer->resourceBarrier.handle())];
        }
    }

    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int _id) {
        assert(cp != nil && "");
        auto mtl_texture = (GEMetalTexture *)texture.get();

        if(mtl_texture->needsBarrier){
            [cp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_texture->resourceBarrier.handle())];
            mtl_texture->needsBarrier = false;
        }

        [cp setTexture:NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_texture->texture.handle()) atIndex:getResourceLocalIndexFromGlobalIndex(_id,computePipelineState->computeShader->internal)];
        if(shaderHasWriteAccessForResource(_id,computePipelineState->computeShader->internal)){
            mtl_texture->needsBarrier = true;
            [cp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_texture->resourceBarrier.handle())];
        }
    }

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct,unsigned int idx){
         assert(cp != nil && "");
        auto mtl_accel_struct = (GEMetalAccelerationStruct *)accelStruct.get();
        [cp setAccelerationStructure:NSOBJECT_OBJC_BRIDGE(id<MTLAccelerationStructure>,mtl_accel_struct->accelStruct.handle()) atBufferIndex:getResourceLocalIndexFromGlobalIndex(idx,computePipelineState->computeShader->internal)];
    }

    void GEMetalCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z){
        dispatchThreads(x,y,z);
    }

    #endif

    void GEMetalCommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
        assert(cp != nil && "");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        [cp dispatchThreadgroups:MTLSizeMake(x,y,z) threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
    }

    void GEMetalCommandBuffer::finishComputePass(){
        [cp endEncoding];
        computePipelineState = nullptr;
        cp = nil;
    };

    void GEMetalCommandBuffer::_present_drawable(NSSmartPtr & drawable){
         buffer.assertExists();
         drawable.assertExists();
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) presentDrawable:
        NSOBJECT_OBJC_BRIDGE(id<CAMetalDrawable>,drawable.handle())];
        NSLog(@"Present Drawable");
    };
    
    void GEMetalCommandBuffer::_commit(){
         buffer.assertExists();
         [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer){
            if(commandBuffer.status == MTLCommandBufferStatusError){
                NSLog(@"Command Buffer Failed to Execute. Error: %@",commandBuffer.error);
            }
            else if(commandBuffer.status == MTLCommandBufferStatusCompleted){
                NSLog(@"Successfully completed Command Buffer! (Logs: %@) (Warning: %@) Duration:%f",commandBuffer.logs,commandBuffer.error,1000.f * (commandBuffer.GPUEndTime - commandBuffer.GPUStartTime));
            }
         }];
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) commit];
        // NSLog(@"Commit to GPU!");
    };

//    void GEMetalCommandBuffer::waitForFence(SharedHandle<GEFence> &fence, unsigned int val) {
//        auto event = (GEMetalFence *)fence.get();
//        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle())
//                encodeWaitForEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,event->metalEvent.handle()) value:val];
//    }
//
//    void GEMetalCommandBuffer::signalFence(SharedHandle<GEFence> &fence, unsigned int val) {
//        auto event = (GEMetalFence *)fence.get();
//        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle())
//                encodeSignalEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,event->metalEvent.handle()) value:val];
//    }

    void GEMetalCommandBuffer::reset(){
        buffer = NSObjectHandle{NSOBJECT_CPP_BRIDGE [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,parentQueue->commandQueue.handle()) commandBuffer]};
    };

    GEMetalCommandBuffer::~GEMetalCommandBuffer(){
        // NSLog(@"Metal Command Buffer Destroy");
        buffer.assertExists();
        // [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) autorelease];
    }

    GEMetalCommandQueue::GEMetalCommandQueue(NSSmartPtr & queue,unsigned size):
    GECommandQueue(size),
    commandQueue(queue),commandBuffers(),semaphore(dispatch_semaphore_create(0)){
        
    };

    void GEMetalCommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                                  SharedHandle<GEFence> &waitFence) {
        auto _commandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
        auto _fence = (GEMetalFence *)waitFence.get();
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle())
                encodeWaitForEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,_fence->metalEvent.handle()) value:1];
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle())
                encodeSignalEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,_fence->metalEvent.handle()) value:0];
    }

    void GEMetalCommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer){
        auto _commandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle()) enqueue];
        commandBuffers.push_back(commandBuffer);
    };

    void GEMetalCommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                                  SharedHandle<GEFence> &signalFence) {
        submitCommandBuffer(commandBuffer);
        auto _commandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
        auto _fence = (GEMetalFence *)signalFence.get();
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle())
                encodeSignalEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,_fence->metalEvent.handle()) value:1];
    }

    void GEMetalCommandQueue::commitToGPU(){
        for(auto & commandBuffer : commandBuffers){
            auto mtlCommandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
            mtlCommandBuffer->_commit();
        };
       commandBuffers.clear();
    };

    void GEMetalCommandQueue::commitToGPUAndPresent(NSSmartPtr & drawable){
        auto & b = commandBuffers.back();
        ((GEMetalCommandBuffer *)b.get())->_present_drawable(drawable);
        commitToGPU();
    };

    void GEMetalCommandQueue::commitToGPUAndWait() {
        auto lastCommandBuffer = (GEMetalCommandBuffer *)commandBuffers.back().get();
        __block dispatch_semaphore_t sem = semaphore;
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,lastCommandBuffer->buffer.handle()) addCompletedHandler:^(id<MTLCommandBuffer> buffer){
            dispatch_semaphore_signal(sem);
        }];
        commitToGPU();
        dispatch_semaphore_wait(semaphore,DISPATCH_TIME_FOREVER);
    }

    GEMetalCommandQueue::~GEMetalCommandQueue(){
        commandQueue.assertExists();
        NSLog(@"Metal Command Queue Destroy");
        dispatch_release(semaphore);
    //    [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,commandQueue.handle()) autorelease];
    };

    SharedHandle<GECommandBuffer> GEMetalCommandQueue::getAvailableBuffer(){
        ++currentlyOccupied;
        auto s = this;
        return SharedHandle<GECommandBuffer>(new GEMetalCommandBuffer(s));
    };
_NAMESPACE_END_
