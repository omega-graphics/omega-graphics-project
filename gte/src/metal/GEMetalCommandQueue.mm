#import "GEMetalCommandQueue.h"
#include <memory>
#import "GEMetalRenderTarget.h"
#import "GEMetalPipeline.h"
#import "GEMetal.h"
#include "../common/GEResourceTracker.h"

#include <cstdlib>
#include <utility>

#import <QuartzCore/QuartzCore.h>

/// @note In Metal, we use id<MTLFence> as a Resource Barrier between different shader stages.

_NAMESPACE_BEGIN_

// §6.3 — bind-time validation helper. Same shape as the D3D12 / Vulkan
// counterparts: walk the shader's layout-desc array, find the descriptor
// owning the bound location, and consult validateTextureBindKind().
static bool checkTextureBindAgainstShader(unsigned int location,
                                          const omegasl_shader &shader,
                                          GETexture &tex) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            return validateTextureBindKind((int)l.type, tex.getKind(),
                                           tex.getSampleCount(), shader.name, location);
        }
    }
    return true;
}

// A GEBuffer bound to a `uniform<T>` slot must have been created with
// BufferDescriptor::Uniform, and a `buffer<T>` slot with Storage. Metal binds
// both kinds the same way (setBuffer:offset:atIndex:), so a mismatch is
// harmless here — but it *would* fail on Vulkan (wrong descriptor type /
// missing usage bit) and D3D12 (CBV alignment). Assert early so the bug
// surfaces on Metal during development rather than only on another backend.
static void checkBufferRoleAgainstShader(unsigned location,
                                         const omegasl_shader &shader,
                                         const GEMetalBuffer &buf) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            bool slotIsUniform = (l.type == OMEGASL_SHADER_UNIFORM_DESC);
            bool bufIsUniform = (buf.role == BufferDescriptor::Uniform);
            if (slotIsUniform != bufIsUniform) {
                NSLog(@"OmegaGTE: buffer role mismatch binding to shader `%s` at slot %u: "
                       "shader expects %s but the GEBuffer was created as %s.",
                      shader.name, location,
                      slotIsUniform ? "uniform<T>" : "buffer<T>",
                      bufIsUniform ? "Uniform" : "Storage");
                assert(false && "GEBuffer role does not match shader resource kind (uniform<T> vs buffer<T>)");
            }
            return;
        }
    }
}

// §2.2/§10.2 push constants. A pipeline may bind at most one push-constant
// block, so the bind-time API carries no slot id — instead we scan the
// shader's layout for the single OMEGASL_SHADER_PUSH_CONSTANT_DESC entry and
// return its Metal buffer index (gpu_relative_loc). Returns true and writes
// `outIndex` if the shader declares one; false if it does not.
static bool findPushConstantBufferIndex(const omegasl_shader &shader, unsigned &outIndex) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.type == OMEGASL_SHADER_PUSH_CONSTANT_DESC) {
            outIndex = l.gpu_relative_loc;
            return true;
        }
    }
    return false;
}

// Extension 8 §8.5 — sampler-bind validation. Walk the shader's layout-desc
// array, find the descriptor owning the bound location, and consult
// validateSamplerBindKind() (rejects static-sampler and non-sampler slots).
static bool checkSamplerBindAgainstShader(unsigned int location,
                                          const omegasl_shader &shader) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            return validateSamplerBindKind((int)l.type, shader.name, location);
        }
    }
    return true;
}

GEMetalCommandBuffer::GEMetalCommandBuffer(GEMetalCommandQueue *parentQueue):parentQueue(parentQueue),
buffer({NSOBJECT_CPP_BRIDGE [[NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,parentQueue->commandQueue.handle()) commandBuffer] retain]}){
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::Metal,
            "CommandBuffer",
            traceResourceId,
            buffer.handle());
    };

    unsigned GEMetalCommandBuffer::getResourceLocalIndexFromGlobalIndex(unsigned _id,omegasl_shader & shader){
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> descArr {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto l : descArr){
            if(l.location == _id){
                return l.gpu_relative_loc;
            }
        }
        return _id;
    };

    TextureSwizzle GEMetalCommandBuffer::resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader & shader){
        if(!runtime.isIdentity()) return runtime;
        // Layout-desc encoding: 0=Identity, 1=R, 2=G, 3=B, 4=A, 5=Zero, 6=One.
        auto decode = [](unsigned char b) -> TextureSwizzleChannel {
            switch(b){
                case 1: return TextureSwizzleChannel::Red;
                case 2: return TextureSwizzleChannel::Green;
                case 3: return TextureSwizzleChannel::Blue;
                case 4: return TextureSwizzleChannel::Alpha;
                case 5: return TextureSwizzleChannel::Zero;
                case 6: return TextureSwizzleChannel::One;
                default: return TextureSwizzleChannel::Identity;
            }
        };
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> descArr {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto l : descArr){
            if(l.location == id){
                if(l.swizzle_desc.r == 0 && l.swizzle_desc.g == 0
                   && l.swizzle_desc.b == 0 && l.swizzle_desc.a == 0){
                    return TextureSwizzle::identity();
                }
                return TextureSwizzle{
                    decode(l.swizzle_desc.r),
                    decode(l.swizzle_desc.g),
                    decode(l.swizzle_desc.b),
                    decode(l.swizzle_desc.a)
                };
            }
        }
        return TextureSwizzle::identity();
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

        id<MTLTexture> src_mtl = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_src_texture->texture.handle());
        id<MTLTexture> dest_mtl = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_dest_texture->texture.handle());

        MTLOrigin srcOrigin = MTLOriginMake(region.x, region.y, region.z);
        MTLSize   srcSize   = MTLSizeMake(region.w, region.h, region.d == 0 ? 1 : region.d);
        MTLOrigin destOrigin = MTLOriginMake((NSUInteger)destCoord.x,
                                             (NSUInteger)destCoord.y,
                                             (NSUInteger)destCoord.z);

        [bp copyFromTexture: src_mtl
                sourceSlice: 0
                sourceLevel: 0
               sourceOrigin: srcOrigin
                 sourceSize: srcSize
                  toTexture: dest_mtl
           destinationSlice: 0
           destinationLevel: 0
          destinationOrigin: destOrigin];

        mtl_dest_texture->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_dest_texture->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::copyBufferToBuffer(SharedHandle<GEBuffer> &src, SharedHandle<GEBuffer> &dest,
                                                  size_t size, size_t srcOffset, size_t destOffset) {
        assert(bp && "Must be in BLIT PASS");
        auto mtl_src = (GEMetalBuffer *)src.get();
        auto mtl_dest = (GEMetalBuffer *)dest.get();

        if(mtl_src->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_src->resourceBarrier.handle())];
        }

        id<MTLBuffer> src_buf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,mtl_src->metalBuffer.handle());
        id<MTLBuffer> dest_buf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,mtl_dest->metalBuffer.handle());

        NSUInteger bytes = size == 0 ? ([src_buf length] - srcOffset) : (NSUInteger)size;

        [bp copyFromBuffer: src_buf
              sourceOffset: (NSUInteger)srcOffset
                  toBuffer: dest_buf
         destinationOffset: (NSUInteger)destOffset
                      size: bytes];

        mtl_dest->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_dest->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::copyBufferToTexture(SharedHandle<GEBuffer> &src, SharedHandle<GETexture> &dest,
                                                   size_t bytesPerRow, size_t bytesPerImage,
                                                   const TextureRegion &destRegion, size_t srcBufferOffset) {
        assert(bp && "Must be in BLIT PASS");
        auto mtl_src = (GEMetalBuffer *)src.get();
        auto mtl_dest = (GEMetalTexture *)dest.get();

        if(mtl_src->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_src->resourceBarrier.handle())];
        }

        id<MTLBuffer> src_buf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,mtl_src->metalBuffer.handle());
        id<MTLTexture> dest_tex = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_dest->texture.handle());

        MTLSize   sourceSize  = MTLSizeMake(destRegion.w, destRegion.h,
                                            destRegion.d == 0 ? 1 : destRegion.d);
        MTLOrigin destOrigin  = MTLOriginMake(destRegion.x, destRegion.y, destRegion.z);

        [bp copyFromBuffer: src_buf
             sourceOffset: (NSUInteger)srcBufferOffset
        sourceBytesPerRow: (NSUInteger)bytesPerRow
      sourceBytesPerImage: (NSUInteger)bytesPerImage
               sourceSize: sourceSize
                toTexture: dest_tex
         destinationSlice: (NSUInteger)destRegion.arrayLayer   // §7.1
         destinationLevel: (NSUInteger)destRegion.mipLevel     // §7.1
        destinationOrigin: destOrigin];

        mtl_dest->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_dest->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::copyTextureToBuffer(SharedHandle<GETexture> &src, SharedHandle<GEBuffer> &dest,
                                                   size_t bytesPerRow, size_t bytesPerImage,
                                                   const TextureRegion &srcRegion, size_t destBufferOffset) {
        assert(bp && "Must be in BLIT PASS");
        auto mtl_src = (GEMetalTexture *)src.get();
        auto mtl_dest = (GEMetalBuffer *)dest.get();

        if(mtl_src->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_src->resourceBarrier.handle())];
        }

        id<MTLTexture> src_tex = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_src->texture.handle());
        id<MTLBuffer> dest_buf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,mtl_dest->metalBuffer.handle());

        MTLOrigin srcOrigin = MTLOriginMake(srcRegion.x, srcRegion.y, srcRegion.z);
        MTLSize   srcSize   = MTLSizeMake(srcRegion.w, srcRegion.h,
                                          srcRegion.d == 0 ? 1 : srcRegion.d);

        [bp copyFromTexture: src_tex
                sourceSlice: (NSUInteger)srcRegion.arrayLayer   // §7.1
                sourceLevel: (NSUInteger)srcRegion.mipLevel     // §7.1
               sourceOrigin: srcOrigin
                 sourceSize: srcSize
                   toBuffer: dest_buf
          destinationOffset: (NSUInteger)destBufferOffset
     destinationBytesPerRow: (NSUInteger)bytesPerRow
   destinationBytesPerImage: (NSUInteger)bytesPerImage];

        mtl_dest->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_dest->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::generateMipmaps(SharedHandle<GETexture> &texture){
        assert(bp && "Must be in BLIT PASS");
        auto mtl_tex = (GEMetalTexture *)texture.get();

        if(mtl_tex->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_tex->resourceBarrier.handle())];
        }

        id<MTLTexture> tex = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_tex->texture.handle());
        [bp generateMipmapsForTexture:tex];

        mtl_tex->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_tex->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                                SharedHandle<GETexture> &src,
                                                SharedHandle<GETexture> &dest){
        auto *mtl_dst = (GEMetalTexture *)dest.get();
        id<MTLTexture> tex = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,mtl_dst->texture.handle());
        TextureRegion srcRegion{0,0,0,(unsigned)tex.width,(unsigned)tex.height,1};
        TextureRegion destRegion{0,0,0,(unsigned)tex.width,(unsigned)tex.height,1};
        blitWithPipeline(pipelineState, src, dest, srcRegion, destRegion);
    }

    void GEMetalCommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                                SharedHandle<GETexture> &src,
                                                SharedHandle<GETexture> &dest,
                                                const TextureRegion &srcRegion,
                                                const TextureRegion &destRegion){
        (void)srcRegion;
        assert(rp == nil && cp == nil && bp == nil && ap == nil &&
               "blitWithPipeline must not be called inside an existing pass scope");
        if(!pipelineState){
            DEBUG_STREAM("blitWithPipeline: pipelineState is null");
            return;
        }
        auto *blitPipe = (GEMetalBlitPipelineState *)pipelineState.get();
        if(!blitPipe->renderPipeline){
            DEBUG_STREAM("blitWithPipeline: underlying render pipeline is null");
            return;
        }

        // One-shot texture render target wrapping `dest`. Built directly
        // (no engine handle reachable from the command buffer) — the
        // GEMetalTextureRenderTarget ctor only needs the SharedHandle<GETexture>.
        SharedHandle<GETextureRenderTarget> trtSh(new GEMetalTextureRenderTarget(dest));

        GERenderPassDescriptor rpDesc{};
        rpDesc.tRenderTarget = trtSh.get();
        rpDesc.colorAttachments.emplace_back(
            GERenderPassDescriptor::ColorAttachment::ClearColor(0.f, 0.f, 0.f, 0.f),
            GERenderPassDescriptor::ColorAttachment::Discard);
        rpDesc.depthStencilAttachment.disabled = true;

        startRenderPass(rpDesc);
        setRenderPipelineState(blitPipe->renderPipeline);
        bindResourceAtFragmentShader(src, 0, TextureSwizzle::identity());
        GEViewport vp{(float)destRegion.x, (float)destRegion.y,
                      (float)destRegion.w, (float)destRegion.h,
                      0.f, 1.f};
        setViewports({vp});
        GEScissorRect sr{(float)destRegion.x, (float)destRegion.y,
                         (float)destRegion.w, (float)destRegion.h};
        setScissorRects({sr});
        drawPolygons(GECommandBuffer::Triangle, 3, 0);
        finishRenderPass();
    }

    void GEMetalCommandBuffer::fillBuffer(SharedHandle<GEBuffer> &buffer, uint32_t value,
                                          size_t offset, size_t size){
        assert(bp && "Must be in BLIT PASS");
        auto mtl_buf = (GEMetalBuffer *)buffer.get();

        if(mtl_buf->needsBarrier){
            [bp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_buf->resourceBarrier.handle())];
        }

        id<MTLBuffer> buf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,mtl_buf->metalBuffer.handle());
        const NSUInteger bufLen = [buf length];
        const NSUInteger fillSize = (size == 0) ? (bufLen - offset) : (NSUInteger)size;

        const std::uint8_t b0 = static_cast<std::uint8_t>(value & 0xFF);
        const std::uint8_t b1 = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        const std::uint8_t b2 = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        const std::uint8_t b3 = static_cast<std::uint8_t>((value >> 24) & 0xFF);
        const bool uniform = (b0 == b1) && (b1 == b2) && (b2 == b3);

        if(!uniform){
            GTE_NSLOG(@"[GEMetalCommandBuffer::fillBuffer] WARNING: Metal blit fill only supports 8-bit patterns. "
                       "Requested 32-bit value 0x%08X is not byte-uniform; falling back to low byte 0x%02X. "
                       "Use a compute shader for non-uniform patterns.",
                       (unsigned)value, (unsigned)b0);
        }

        [bp fillBuffer:buf
                  range:NSMakeRange((NSUInteger)offset, fillSize)
                  value:b0];

        mtl_buf->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_buf->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::finishBlitPass(){
        [bp endEncoding];
        bp = nil;
    };


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
            auto *n_rt = (GEMetalNativeRenderTarget *)desc.nRenderTarget;
            // First render pass of the frame: pull a fresh CAMetalDrawable.
            // Subsequent in-frame restarts (texture-fence handling) keep the
            // same drawable — only acquire when none is currently held.
            if(n_rt->getDrawable().handle() == nullptr){
                n_rt->acquireDrawable();
            }
            auto metalDrawable = n_rt->getDrawable();
            metalDrawable.assertExists();
            id<CAMetalDrawable> drawable = NSOBJECT_OBJC_BRIDGE(id<CAMetalDrawable>,metalDrawable.handle());
            id<MTLTexture> drawableTexture = drawable.texture;
            CAMetalLayer *drawableLayer = (CAMetalLayer *)drawable.layer;
            GTE_NSLOG(@"Prepare Render Pass For NativeTarget: drawable=%p texture=%p %lux%lu format=%lu layer=%p layer.superlayer=%@",
                      drawable, drawableTexture,
                      (unsigned long)drawableTexture.width, (unsigned long)drawableTexture.height,
                      (unsigned long)drawableTexture.pixelFormat,
                      drawableLayer, drawableLayer.superlayer);
            renderPassDesc.renderTargetWidth = (NSUInteger)n_rt->drawableSize.width;
            renderPassDesc.renderTargetHeight = (NSUInteger)n_rt->drawableSize.height;
            id<MTLTexture> renderTarget;
            if(desc.multisampleResolve){
                renderTarget = NSOBJECT_OBJC_BRIDGE(id<MTLTexture>,((GEMetalTexture *)desc.resolveDesc.multiSampleTextureSrc.get())->texture.handle());
                multiSampleTextureTarget = drawableTexture;
            }
            else {
                renderTarget = drawableTexture;
            }
            renderPassDesc.colorAttachments[0].texture =renderTarget;
           
            if(!desc.depthStencilAttachment.disabled){
                renderPassDesc.depthAttachment.texture = renderPassDesc.stencilAttachment.texture = renderTarget;
            }
        }
        else if(desc.tRenderTarget != nullptr){
            GTE_NSLOG(@"Prepare Render Pass For TextureTarget");
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

        const unsigned colorAttachmentCount = desc.colorAttachments.empty()
                                                  ? 1u
                                                  : (unsigned)desc.colorAttachments.size();

        for(unsigned i = 0; i < colorAttachmentCount; ++i){
            const GERenderPassDescriptor::ColorAttachment *attachment =
                desc.colorAttachments.empty() ? nullptr : &desc.colorAttachments[i];

            if(i > 0){
                assert(attachment != nullptr && attachment->texture != nullptr &&
                       "Color attachments beyond index 0 must supply an explicit texture.");
                auto *attachTex = (GEMetalTexture *)attachment->texture.get();
                renderPassDesc.colorAttachments[i].texture =
                    NSOBJECT_OBJC_BRIDGE(id<MTLTexture>, attachTex->texture.handle());
            }
            else if(attachment != nullptr && attachment->texture != nullptr){
                auto *attachTex = (GEMetalTexture *)attachment->texture.get();
                renderPassDesc.colorAttachments[0].texture =
                    NSOBJECT_OBJC_BRIDGE(id<MTLTexture>, attachTex->texture.handle());
            }

            const auto loadAction = (attachment != nullptr)
                                        ? attachment->loadAction
                                        : GERenderPassDescriptor::ColorAttachment::Discard;

            switch (loadAction) {
                case GERenderPassDescriptor::ColorAttachment::Load : {
                    renderPassDesc.colorAttachments[i].loadAction = MTLLoadActionLoad;
                    renderPassDesc.colorAttachments[i].storeAction = MTLStoreActionDontCare;
                    break;
                }
                case GERenderPassDescriptor::ColorAttachment::LoadPreserve : {
                    renderPassDesc.colorAttachments[i].loadAction = MTLLoadActionLoad;
                    renderPassDesc.colorAttachments[i].storeAction = MTLStoreActionStore;
                    break;
                }
                case GERenderPassDescriptor::ColorAttachment::Discard : {
                    renderPassDesc.colorAttachments[i].loadAction = MTLLoadActionDontCare;
                    renderPassDesc.colorAttachments[i].storeAction = MTLStoreActionDontCare;
                    break;
                }
                case GERenderPassDescriptor::ColorAttachment::Clear : {
                    renderPassDesc.colorAttachments[i].loadAction = MTLLoadActionClear;
                    renderPassDesc.colorAttachments[i].storeAction = MTLStoreActionStore;
                    if(attachment != nullptr){
                        const auto & clearColor = attachment->clearColor;
                        renderPassDesc.colorAttachments[i].clearColor =
                            MTLClearColorMake(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
                    }
                    else {
                        renderPassDesc.colorAttachments[i].clearColor = MTLClearColorMake(0,0,0,0);
                    }
                    break;
                }
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
        GTE_NSLOG(@"Starting Render Pass: %@",rp);
        if(needsBarrier){
            /// Ensure texture is ready to render to when fragment stage begins.
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,barrier.handle()) beforeStages:MTLRenderStageFragment];
        }
    };

    void GEMetalCommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState){
        auto *ps = (GEMetalRenderPipelineState *)pipelineState.get();
        ps->renderPipelineState.assertExists();
        GTE_NSLOG(@"Render Pipeline Set: %@",(id<MTLRenderPipelineState>)ps->renderPipelineState.handle());
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

        checkBufferRoleAgainstShader(_id, renderPipelineState->vertexShader->internal, *metalBuffer);

        /// Mesh-Shader-Plan Phase 4c.5.1 — when the bound PSO is the
        /// mesh variant (Phase 4c.1 slot-doubling: mesh shader lives in
        /// `vertexShader`), route buffer binds to `setMeshBuffer:` and
        /// the fence sync to `MTLRenderStageMesh`. The shader-info
        /// reads (`vertexShader->internal`) stay symmetric because the
        /// omegasl_shader handle in that slot belongs to whichever
        /// stage was bound — no second resource-table lookup needed.
        const bool isMeshVariant = renderPipelineState->isMesh;
        const MTLRenderStages stages = isMeshVariant ? MTLRenderStageMesh : MTLRenderStageVertex;

        if(metalBuffer->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) beforeStages:stages];
            metalBuffer->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->vertexShader->internal);
        GTE_NSLOG(@"Binding GEBuffer at %s Shader: %@ At Index: %i",
                  isMeshVariant ? "Mesh" : "Vertex",
                  NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()), index);
        if(isMeshVariant){
            [rp setMeshBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()) offset:0 atIndex:index];
        } else {
            [rp setVertexBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()) offset:0 atIndex:index];
        }

        if(shaderHasWriteAccessForResource(_id,renderPipelineState->vertexShader->internal)){
            metalBuffer->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) afterStages:stages];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned _id,
                                                            const TextureSwizzle & swizzle){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Vertex Func when not in render pass");
        auto *metalTexture = (GEMetalTexture *)texture.get();

        checkTextureBindAgainstShader(_id, renderPipelineState->vertexShader->internal, *metalTexture);

        /// Mesh-Shader-Plan Phase 4c.5.2 — mesh-variant routing.
        /// Swizzle-view resolution is stage-independent (the
        /// swizzle metadata rides the omegasl_shader, not the bind
        /// API), so the existing `getOrCreateSwizzledView` call survives
        /// unchanged.
        const bool isMeshVariant = renderPipelineState->isMesh;
        const MTLRenderStages stages = isMeshVariant ? MTLRenderStageMesh : MTLRenderStageVertex;

        if(metalTexture->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) beforeStages:stages];
            metalTexture->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->vertexShader->internal);
        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, _id, renderPipelineState->vertexShader->internal);
        id<MTLTexture> view = metalTexture->getOrCreateSwizzledView(effective);
        GTE_NSLOG(@"Binding GETexture at %s Shader: %@ At Index: %i",
                  isMeshVariant ? "Mesh" : "Vertex", view, index);
        if(isMeshVariant){
            [rp setMeshTexture:view atIndex:index];
        } else {
            [rp setVertexTexture:view atIndex:index];
        }
        if(shaderHasWriteAccessForResource(_id,renderPipelineState->vertexShader->internal)){
            metalTexture->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) afterStages:stages];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtVertexShader(SharedHandle<GESamplerState> & sampler,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot bind sampler at a Vertex Func when not in render pass");
        auto *metalSampler = (GEMetalSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(_id, renderPipelineState->vertexShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;
        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->vertexShader->internal);
        /// Mesh-Shader-Plan Phase 4c.5.3 — mesh-variant routing.
        /// Samplers don't ride a fence (the GPU treats them as
        /// stateless), so the routing is purely the selector choice.
        if(renderPipelineState->isMesh){
            [rp setMeshSamplerState:NSOBJECT_OBJC_BRIDGE(id<MTLSamplerState>,metalSampler->samplerState.handle()) atIndex:index];
        } else {
            [rp setVertexSamplerState:NSOBJECT_OBJC_BRIDGE(id<MTLSamplerState>,metalSampler->samplerState.handle()) atIndex:index];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Fragment Func when not in render pass");
        auto *metalBuffer = (GEMetalBuffer *)buffer.get();
        metalBuffer->metalBuffer.assertExists();

        checkBufferRoleAgainstShader(_id, renderPipelineState->fragmentShader->internal, *metalBuffer);

        if(metalBuffer->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) beforeStages:MTLRenderStageFragment];
            metalBuffer->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);

        GTE_NSLOG(@"Binding GEBuffer at Fragment Shader: %@ At Index: %i",NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()),index);
        [rp setFragmentBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()) offset:0 atIndex:index];
        if(shaderHasWriteAccessForResource(_id,renderPipelineState->fragmentShader->internal)){
            metalBuffer->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) afterStages:MTLRenderStageFragment];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned _id,
                                                              const TextureSwizzle & swizzle){
        assert((rp && (cp == nil)) && "Cannot Resource Const on a Fragment Func when not in render pass");
        auto *metalTexture = (GEMetalTexture *)texture.get();

        checkTextureBindAgainstShader(_id, renderPipelineState->fragmentShader->internal, *metalTexture);

        if(metalTexture->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) beforeStages:MTLRenderStageFragment];
            metalTexture->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);
        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, _id, renderPipelineState->fragmentShader->internal);
        id<MTLTexture> view = metalTexture->getOrCreateSwizzledView(effective);
        GTE_NSLOG(@"Binding GETexture at Fragment Shader: %@ At Index: %i",view,index);

        [rp setFragmentTexture:view atIndex:index];
        // if(shaderHasWriteAccessForResource(_id,renderPipelineState->fragmentShader->internal)){
        //     metalTexture->needsBarrier = true;
        //     [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) afterStages:MTLRenderStageFragment];
        // }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GESamplerState> & sampler,unsigned _id){
        assert((rp && (cp == nil)) && "Cannot bind sampler at a Fragment Func when not in render pass");
        auto *metalSampler = (GEMetalSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(_id, renderPipelineState->fragmentShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;
        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);
        [rp setFragmentSamplerState:NSOBJECT_OBJC_BRIDGE(id<MTLSamplerState>,metalSampler->samplerState.handle()) atIndex:index];
    };

    void GEMetalCommandBuffer::setRenderConstants(const void *data, unsigned size, unsigned offset){
        assert((rp && (cp == nil)) && "setRenderConstants must be called inside a render pass");
        assert(renderPipelineState && "setRenderConstants requires a bound render pipeline");
        // Metal's setBytes replaces the whole binding from the encoder's
        // perspective; it has no destination-offset-into-range concept, so a
        // partial update (offset != 0) is not expressible. Honor it on
        // D3D12/Vulkan; require a full-block set here. (Documented follow-up.)
        assert(offset == 0 && "Metal setRenderConstants supports only offset == 0 (full-block set)");
        (void)offset;
        // Apply to every stage that declared the push constant (`[in pc]`).
        // setVertexBytes / setFragmentBytes / setMeshBytes write the inline
        // data into the stage's buffer-index slot the `constant T&` was
        // emitted at. Mesh-Shader-Plan Phase 4c.5.4 — when the bound PSO is
        // the mesh variant (4c.1 slot-doubling) the "vertex slot" actually
        // holds the mesh shader, so the vertex branch dispatches to
        // setMeshBytes. Fragment is fragment regardless of upstream stage.
        unsigned idx = 0;
        bool any = false;
        if(findPushConstantBufferIndex(renderPipelineState->vertexShader->internal, idx)){
            if(renderPipelineState->isMesh){
                [rp setMeshBytes:data length:size atIndex:idx];
            } else {
                [rp setVertexBytes:data length:size atIndex:idx];
            }
            any = true;
        }
        if(findPushConstantBufferIndex(renderPipelineState->fragmentShader->internal, idx)){
            [rp setFragmentBytes:data length:size atIndex:idx];
            any = true;
        }
        assert(any && "setRenderConstants: bound pipeline declares no `constant<T>` push constant");
        (void)any;
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
    
    static MTLPrimitiveType metalPrimitiveTypeForPolygonType(GECommandBuffer::PolygonType polygonType){
        switch(polygonType){
            case GECommandBuffer::Triangle:
                return MTLPrimitiveTypeTriangle;
            case GECommandBuffer::TriangleStrip:
                return MTLPrimitiveTypeTriangleStrip;
            case GECommandBuffer::Line:
                return MTLPrimitiveTypeLine;
            case GECommandBuffer::LineStrip:
                return MTLPrimitiveTypeLineStrip;
            case GECommandBuffer::Point:
                return MTLPrimitiveTypePoint;
        }
        return MTLPrimitiveTypeTriangle;
    }

    void GEMetalCommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType,unsigned vertexCount,size_t startIdx){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        [rp drawPrimitives:metalPrimitiveTypeForPolygonType(polygonType) vertexStart:startIdx vertexCount:vertexCount];
    };

    void GEMetalCommandBuffer::setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType){
        auto *metalBuffer = (GEMetalBuffer *)buffer.get();
        pendingIndexBuffer = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalBuffer->metalBuffer.handle());
        pendingIndexType = (indexType == RenderPassIndexType::UInt16) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    };

    void GEMetalCommandBuffer::drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                                   unsigned indexCount, size_t startIndex,
                                                   int baseVertex){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        assert(pendingIndexBuffer != nil && "Index buffer must be set before drawIndexedPolygons");
        NSUInteger indexByteSize = (pendingIndexType == MTLIndexTypeUInt16) ? 2u : 4u;
        [rp drawIndexedPrimitives:metalPrimitiveTypeForPolygonType(polygonType)
                       indexCount:indexCount
                        indexType:pendingIndexType
                      indexBuffer:pendingIndexBuffer
                indexBufferOffset:startIndex * indexByteSize
                    instanceCount:1
                       baseVertex:baseVertex
                     baseInstance:0];
    };

    void GEMetalCommandBuffer::drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                     unsigned vertexCount, size_t startIdx,
                                                     unsigned instanceCount, unsigned firstInstance){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        [rp drawPrimitives:metalPrimitiveTypeForPolygonType(polygonType)
               vertexStart:startIdx
               vertexCount:vertexCount
             instanceCount:instanceCount
              baseInstance:firstInstance];
    };

    void GEMetalCommandBuffer::drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                            unsigned indexCount, size_t startIndex,
                                                            int baseVertex, unsigned instanceCount,
                                                            unsigned firstInstance){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        assert(pendingIndexBuffer != nil && "Index buffer must be set before drawIndexedPolygonsInstanced");
        NSUInteger indexByteSize = (pendingIndexType == MTLIndexTypeUInt16) ? 2u : 4u;
        [rp drawIndexedPrimitives:metalPrimitiveTypeForPolygonType(polygonType)
                       indexCount:indexCount
                        indexType:pendingIndexType
                      indexBuffer:pendingIndexBuffer
                indexBufferOffset:startIndex * indexByteSize
                    instanceCount:instanceCount
                       baseVertex:baseVertex
                     baseInstance:firstInstance];
    };

    void GEMetalCommandBuffer::drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                    SharedHandle<GEBuffer> & argumentBuffer,
                                                    size_t argumentBufferOffset){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        auto *metalArgBuffer = (GEMetalBuffer *)argumentBuffer.get();
        id<MTLBuffer> argBuf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalArgBuffer->metalBuffer.handle());
        [rp drawPrimitives:metalPrimitiveTypeForPolygonType(polygonType)
            indirectBuffer:argBuf
      indirectBufferOffset:argumentBufferOffset];
    };

    void GEMetalCommandBuffer::drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                           SharedHandle<GEBuffer> & argumentBuffer,
                                                           size_t argumentBufferOffset){
        assert((rp && (cp == nil)) && "Cannot Draw Polygons when not in render pass");
        assert(pendingIndexBuffer != nil && "Index buffer must be set before drawIndexedPolygonsIndirect");
        auto *metalArgBuffer = (GEMetalBuffer *)argumentBuffer.get();
        id<MTLBuffer> argBuf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalArgBuffer->metalBuffer.handle());
        [rp drawIndexedPrimitives:metalPrimitiveTypeForPolygonType(polygonType)
                        indexType:pendingIndexType
                      indexBuffer:pendingIndexBuffer
                indexBufferOffset:0
                   indirectBuffer:argBuf
             indirectBufferOffset:argumentBufferOffset];
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

        checkBufferRoleAgainstShader(_id, computePipelineState->computeShader->internal, *mtl_buffer);

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

    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int _id,
                                                            const TextureSwizzle & swizzle) {
        assert(cp != nil && "");
        auto mtl_texture = (GEMetalTexture *)texture.get();

        checkTextureBindAgainstShader(_id, computePipelineState->computeShader->internal, *mtl_texture);

        if(mtl_texture->needsBarrier){
            [cp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_texture->resourceBarrier.handle())];
            mtl_texture->needsBarrier = false;
        }

        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, _id, computePipelineState->computeShader->internal);
        id<MTLTexture> view = mtl_texture->getOrCreateSwizzledView(effective);
        [cp setTexture:view atIndex:getResourceLocalIndexFromGlobalIndex(_id,computePipelineState->computeShader->internal)];
        if(shaderHasWriteAccessForResource(_id,computePipelineState->computeShader->internal)){
            mtl_texture->needsBarrier = true;
            [cp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_texture->resourceBarrier.handle())];
        }
    }

    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GESamplerState> &sampler, unsigned int _id) {
        assert(cp != nil && "Cannot bind sampler at a Compute Func when not in compute pass");
        auto *metalSampler = (GEMetalSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(_id, computePipelineState->computeShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;
        [cp setSamplerState:NSOBJECT_OBJC_BRIDGE(id<MTLSamplerState>,metalSampler->samplerState.handle())
                    atIndex:getResourceLocalIndexFromGlobalIndex(_id,computePipelineState->computeShader->internal)];
    }


    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct,unsigned int idx){
         assert(cp != nil && "");
        auto mtl_accel_struct = (GEMetalAccelerationStruct *)accelStruct.get();
        [cp setAccelerationStructure:NSOBJECT_OBJC_BRIDGE(id<MTLAccelerationStructure>,mtl_accel_struct->accelStruct.handle()) atBufferIndex:getResourceLocalIndexFromGlobalIndex(idx,computePipelineState->computeShader->internal)];
    }

    void GEMetalCommandBuffer::setComputeConstants(const void *data, unsigned size, unsigned offset){
        assert(cp != nil && "setComputeConstants must be called inside a compute pass");
        assert(computePipelineState && "setComputeConstants requires a bound compute pipeline");
        assert(offset == 0 && "Metal setComputeConstants supports only offset == 0 (full-block set)");
        (void)offset;
        unsigned idx = 0;
        bool found = findPushConstantBufferIndex(computePipelineState->computeShader->internal, idx);
        assert(found && "setComputeConstants: bound pipeline declares no `constant<T>` push constant");
        if(!found) return;
        [cp setBytes:data length:size atIndex:idx];
    }

    void GEMetalCommandBuffer::drawMeshTasks(uint32_t groupCountX,
                                             uint32_t groupCountY,
                                             uint32_t groupCountZ) {
        /// Mesh-Shader-Plan Phase 4c.3 — live `drawMeshThreadgroups:`.
        /// The Metal command buffer still has no reachable engine
        /// handle, so the feature gate cannot fire from here today; the
        /// gate at `makeMeshPipelineState` is the real contract (an
        /// unsupported device returns `nullptr` for the PSO, so the
        /// `renderPipelineState->isMesh` assertion below fails before
        /// reaching `drawMeshThreadgroups:`). The PSO is bound via the
        /// existing `setRenderPipelineState` because mesh PSOs surface
        /// as `GERenderPipelineState`; the `isMesh` flag stamped at
        /// construction (Phase 4c.1) is what tells us the bound
        /// pipeline can dispatch mesh tasks rather than polygon draws.
        assert(rp != nil && "drawMeshTasks: must be called inside a render pass");
        assert(renderPipelineState != nullptr
               && "drawMeshTasks: no pipeline bound (call setRenderPipelineState first)");
        assert(renderPipelineState->isMesh
               && "drawMeshTasks: bound pipeline is a graphics pipeline, not a mesh pipeline. "
                  "Use makeMeshPipelineState to build a mesh-variant PSO.");

        /// Per-meshlet threadgroup dims come from the mesh shader's
        /// `[[max_total_threads_per_threadgroup(N)]]` / declared
        /// dimensions baked into `omegasl_shader::threadgroupDesc` by
        /// the MSL codegen in Phase 2c — the same field compute uses
        /// for `dispatchThreadgroups`. The mesh shader handle sits in
        /// the `vertexShader` base slot by the Phase 4c.1 doubling
        /// trick, so the read is symmetric with the compute path.
        auto & meshShader = renderPipelineState->vertexShader;
        auto & tg = meshShader->internal.threadgroupDesc;

        /// `threadsPerObjectThreadgroup` is ignored when no object
        /// (amplification) shader is bound — Metal SDK explicitly
        /// documents this; the Phase 4c hard-stop at
        /// `makeMeshPipelineState` keeps that always true today. When
        /// Phase 5 lands amplification, this becomes the object-stage's
        /// `[numthreads(...)]` from a sibling field on the PSO.
        [rp drawMeshThreadgroups:MTLSizeMake(groupCountX,groupCountY,groupCountZ)
        threadsPerObjectThreadgroup:MTLSizeMake(1,1,1)
          threadsPerMeshThreadgroup:MTLSizeMake(tg.x,tg.y,tg.z)];
    }

    void GEMetalCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z){
        dispatchThreads(x,y,z);
    }

    void GEMetalCommandBuffer::dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) {
        assert(cp != nil && "");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        [cp dispatchThreadgroups:MTLSizeMake(x,y,z) threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
    }

    void GEMetalCommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
        assert(cp != nil && "");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        [cp dispatchThreads:MTLSizeMake(x,y,z) threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
    }

    void GEMetalCommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                            size_t argumentBufferOffset) {
        assert(cp != nil && "Must be in a compute pass to dispatch threadgroups");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        auto *metalArgBuffer = (GEMetalBuffer *)argumentBuffer.get();
        id<MTLBuffer> argBuf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalArgBuffer->metalBuffer.handle());
        [cp dispatchThreadgroupsWithIndirectBuffer:argBuf
                              indirectBufferOffset:argumentBufferOffset
                             threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
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
        ResourceTracking::Event presentEvent {};
        presentEvent.backend = ResourceTracking::Backend::Metal;
        presentEvent.eventType = ResourceTracking::EventType::Present;
        presentEvent.resourceType = "CommandBuffer";
        presentEvent.resourceId = traceResourceId;
        presentEvent.queueId = parentQueue != nullptr ? parentQueue->traceResourceId : 0;
        presentEvent.commandBufferId = traceResourceId;
        presentEvent.nativeHandle = reinterpret_cast<std::uint64_t>(buffer.handle());
        ResourceTracking::Tracker::instance().emit(presentEvent);
        GTE_NSLOG(@"Present Drawable");
    };
    
    void GEMetalCommandBuffer::_commit(){
         GTE_NSLOG(@"[_commit] MTLCommandBuffer=%p status=%lu",
                   NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()),
                   (unsigned long)NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()).status);
         buffer.assertExists();
         auto completion = completionHandler;
         completionHandler = nullptr;
         // Capture member values by copy so the block remains valid even
         // if this GEMetalCommandBuffer is destroyed before the GPU finishes.
         const auto capturedTraceId = traceResourceId;
         const auto capturedQueueTraceId = parentQueue != nullptr ? parentQueue->traceResourceId : static_cast<std::uint64_t>(0);
         [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer){
            if(commandBuffer.status == MTLCommandBufferStatusError){
                NSLog(@"Command Buffer Failed to Execute. Error: %@",commandBuffer.error);
            }
            else if(commandBuffer.status == MTLCommandBufferStatusCompleted){
                GTE_NSLOG(@"Successfully completed Command Buffer! (Logs: %@) (Warning: %@) Duration:%f",commandBuffer.logs,commandBuffer.error,1000.f * (commandBuffer.GPUEndTime - commandBuffer.GPUStartTime));
            }
            if(completion){
                GECommandBufferCompletionInfo info {};
                if(commandBuffer.status == MTLCommandBufferStatusError){
                    info.status = GECommandBufferCompletionInfo::CompletionStatus::Error;
                }
                else {
                    info.status = GECommandBufferCompletionInfo::CompletionStatus::Completed;
                }
                info.gpuStartTimeSec = commandBuffer.GPUStartTime;
                info.gpuEndTimeSec = commandBuffer.GPUEndTime;
                completion(info);
            }
            if(commandBuffer.status == MTLCommandBufferStatusCompleted){
                ResourceTracking::Event completeEvent {};
                completeEvent.backend = ResourceTracking::Backend::Metal;
                completeEvent.eventType = ResourceTracking::EventType::Complete;
                completeEvent.resourceType = "CommandBuffer";
                completeEvent.resourceId = capturedTraceId;
                completeEvent.queueId = capturedQueueTraceId;
                completeEvent.commandBufferId = capturedTraceId;
                completeEvent.nativeHandle = reinterpret_cast<std::uint64_t>(commandBuffer);
                ResourceTracking::Tracker::instance().emit(completeEvent);
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
        if(buffer.handle() != nullptr){
            [NSOBJECT_OBJC_BRIDGE(id,buffer.handle()) release];
        }
        id<MTLCommandBuffer> newBuffer = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,parentQueue->commandQueue.handle()) commandBuffer];
        buffer = NSObjectHandle{NSOBJECT_CPP_BRIDGE [newBuffer retain]};
    };

    void GEMetalCommandBuffer::setCompletionHandler(const GECommandBufferCompletionHandler & handler){
        completionHandler = std::move(handler);
    }

GEMetalCommandBuffer::~GEMetalCommandBuffer(){
        // NSLog(@"Metal Command Buffer Destroy");
        buffer.assertExists();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::Metal,
                "CommandBuffer",
                traceResourceId,
                buffer.handle());
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) release];
    }

    /// Pick the auto-generated label suffix for a queue type when the caller
    /// did not supply one. Matches the suffixes named in the
    /// CommandQueue-Typed-Pool plan's backend mapping table ("gfx", "compute",
    /// "xfer", "any"). Metal has no native split, so this is purely a hint
    /// in the Xcode GPU frame capture / Instruments traces.
    static const char * metal_autoLabelFor(GECommandQueueDesc::Type t){
        switch(t){
            case GECommandQueueDesc::Type::Graphics:  return "GECommandQueue[gfx]";
            case GECommandQueueDesc::Type::Compute:   return "GECommandQueue[compute]";
            case GECommandQueueDesc::Type::Transfer:  return "GECommandQueue[xfer]";
            case GECommandQueueDesc::Type::Universal:
            default:                                  return "GECommandQueue[any]";
        }
    }

    GEMetalCommandQueue::GEMetalCommandQueue(NSSmartPtr & queue, const GECommandQueueDesc & desc):
    GECommandQueue(desc, /*achievedType=*/desc.type),
    commandQueue(queue),commandBuffers(),semaphore(dispatch_semaphore_create(0)){
        // Apply the descriptor label to the MTLCommandQueue so it shows up
        // in Xcode's GPU capture / Instruments. Fall back to a type-derived
        // auto label when no user label was supplied — non-empty labels are
        // more informative than the default `<MTLCommandQueue: 0x…>`.
        const char * labelCStr = desc.label.empty()
                                     ? metal_autoLabelFor(desc.type)
                                     : desc.label.c_str();
        NSString *label = [[NSString alloc] initWithUTF8String:labelCStr];
        if(label != nil){
            NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>, commandQueue.handle()).label = label;
            [label release];
        }
        traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Create,
                ResourceTracking::Backend::Metal,
                "CommandQueue",
                traceResourceId,
                commandQueue.handle());
    };

    void GEMetalCommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                                  SharedHandle<GEFence> &waitFence) {
        auto _commandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
        auto _fence = (GEMetalFence *)waitFence.get();
        auto waitValue = _fence->currentEventValue();
        if(waitValue > 0){
            [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle())
                    encodeWaitForEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,_fence->metalEvent.handle()) value:waitValue];
        }
    }

    void GEMetalCommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer){
        auto _commandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle()) enqueue];
        GTE_NSLOG(@"[submitCB] queue=%p enqueue CB=%p bufferCount=%lu->%lu",
                  this, _commandBuffer->buffer.handle(),
                  (unsigned long)commandBuffers.size(),
                  (unsigned long)(commandBuffers.size()+1));
        ResourceTracking::Event submitEvent {};
        submitEvent.backend = ResourceTracking::Backend::Metal;
        submitEvent.eventType = ResourceTracking::EventType::Submit;
        submitEvent.resourceType = "CommandBuffer";
        submitEvent.resourceId = _commandBuffer->traceResourceId;
        submitEvent.queueId = traceResourceId;
        submitEvent.commandBufferId = _commandBuffer->traceResourceId;
        submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(_commandBuffer->buffer.handle());
        ResourceTracking::Tracker::instance().emit(submitEvent);
        commandBuffers.push_back(commandBuffer);
    };

    void GEMetalCommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                                  SharedHandle<GEFence> &signalFence) {
        submitCommandBuffer(commandBuffer);
        auto _commandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
        auto _fence = (GEMetalFence *)signalFence.get();
        auto signalValue = _fence->reserveNextEventValue();
        [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,_commandBuffer->buffer.handle())
                encodeSignalEvent:NSOBJECT_OBJC_BRIDGE(id<MTLEvent>,_fence->metalEvent.handle()) value:signalValue];
    }

    void GEMetalCommandQueue::commitToGPU(){
        for(auto & commandBuffer : commandBuffers){
            auto mtlCommandBuffer = (GEMetalCommandBuffer *)commandBuffer.get();
            mtlCommandBuffer->_commit();
        };
       commandBuffers.clear();
    };

    void GEMetalCommandQueue::commitToGPUAndPresent(NSSmartPtr & drawable){
        GTE_NSLOG(@"[commitToGPUAndPresent] commandBuffers.size=%lu", (unsigned long)commandBuffers.size());
        if(commandBuffers.empty()){
            NSLog(@"[commitToGPUAndPresent] ERROR: no command buffers to present!");
            return;
        }
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
        ResourceTracking::Tracker::instance().emit(
                ResourceTracking::EventType::Destroy,
                ResourceTracking::Backend::Metal,
                "CommandQueue",
                traceResourceId,
                commandQueue.handle());
        GTE_NSLOG(@"Metal Command Queue Destroy");
        dispatch_release(semaphore);
    //    [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,commandQueue.handle()) autorelease];
    };

    SharedHandle<GECommandBuffer> GEMetalCommandQueue::getAvailableBuffer(){
        ++currentlyOccupied;
        auto s = this;
        return SharedHandle<GECommandBuffer>(new GEMetalCommandBuffer(s));
    };
_NAMESPACE_END_
