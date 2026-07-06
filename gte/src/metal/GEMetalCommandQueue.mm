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
                // Debug-Layer-Plan §4.3 — caller-contract violation: report
                // through DEBUG_CRITICAL (always-on, even with the layer off)
                // and keep the assert for the debug-build abort.
                DEBUG_CRITICAL(DEBUG_DOMAIN_RESOURCE,
                    "Bind role mismatch: shader=" << shader.name << " slot=" << location
                    << " shader expects " << (slotIsUniform ? "uniform<T>" : "buffer<T>")
                    << " but the GEBuffer was created as "
                    << (bufIsUniform ? "Uniform" : "Storage"));
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

// Debug-Layer-Plan §4.3 — caller-contract guard for the encoding API. `ok`
// is the precondition the caller must satisfy (e.g. "an active render pass
// exists"). When it is false this reports a DEBUG_CRITICAL line in `domain` —
// surfacing the misuse even in a release build with the debug layer off
// (§4.1.6) — trips the debug-build assert, and then **returns from the calling
// method**. The early return is the hardening: most encoder methods
// dereference a pipeline-state / encoder immediately after the guard, so
// without it a release build (asserts compiled out) would fall straight
// through a contract violation into a null dereference instead of a clean
// no-op. Every guarded method is a `void` encoder call, so skipping the
// operation on misuse is the graceful, documented behaviour (§4 intro). The
// `OrReturn` suffix keeps that control flow legible at the call site.
//
// `ok` must be a side-effect-free predicate (it is evaluated twice: once for
// the branch, once inside the assert so the failed condition prints in debug).
// A macro rather than a function because only a macro can return from the
// caller; `do/while(0)` lets it be used as an ordinary `stmt;`.
#define metalRequireOrReturn(ok, domain, what)                                 \
    do {                                                                       \
        if(!(ok)){                                                             \
            DEBUG_CRITICAL((domain), (what));                                  \
            assert((ok) && "GTE caller-contract violation; see the CRITICAL log line above"); \
            return;                                                            \
        }                                                                      \
    } while(0)

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
        DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "BlitPass begin");
    };

    void GEMetalCommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) {
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
        metalRequireOrReturn(rp == nil && cp == nil && bp == nil && ap == nil, DEBUG_DOMAIN_RENDERTGT,
                     "blitWithPipeline: must not be called inside an existing pass scope");
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
        metalRequireOrReturn(bp != nil, DEBUG_DOMAIN_RESOURCE,
                     "copy/fill blit op: called outside an active blit pass (call startBlitPass first)");
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
            // Engine fallback (not caller misuse), so INFO not CRITICAL.
            DEBUG_INFO(DEBUG_DOMAIN_RESOURCE,
                "fillBuffer: Metal blit fill only supports 8-bit patterns; requested 32-bit value 0x"
                << std::hex << (unsigned)value << " is not byte-uniform, falling back to low byte 0x"
                << (unsigned)b0 << std::dec
                << " (use a compute shader for non-uniform patterns)");
        }

        [bp fillBuffer:buf
                  range:NSMakeRange((NSUInteger)offset, fillSize)
                  value:b0];

        mtl_buf->needsBarrier = true;
        [bp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,mtl_buf->resourceBarrier.handle())];
    }

    void GEMetalCommandBuffer::finishBlitPass(){
        DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "BlitPass end");
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

    void GEMetalCommandBuffer::startTessRenderPass(const GERenderPassDescriptor & desc){
        buffer.assertExists();
        /// §16 Phase E — deferred tessellation pass. Record the descriptor but
        /// open NO encoder yet: the hull/factor compute dispatch has to be
        /// encoded before the render encoder exists (Metal cannot run compute
        /// inside a render encoder), and both need the pipeline + control points
        /// that arrive after this call. `drawPatches` does the dispatch, then
        /// opens the render encoder via the stored descriptor. The ordering is
        /// enforced by Metal's hazard tracking on the intermediate buffers, so
        /// the render work waits for the dispatch on the GPU without a CPU stall.
        tessRenderPassDesc = desc;
        inTessRenderPass = true;
        DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "TessRenderPass begin (deferred)");
    };

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
            DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT,
                "RenderPass begin: native rt "
                << (unsigned long)drawableTexture.width << "x"
                << (unsigned long)drawableTexture.height);
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
            DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "RenderPass begin: texture rt");
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
            DEBUG_CRITICAL(DEBUG_DOMAIN_RENDERTGT,
                "startRenderPass: descriptor has no render target (neither nRenderTarget nor tRenderTarget set)");
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
                metalRequireOrReturn(attachment != nullptr && attachment->texture != nullptr, DEBUG_DOMAIN_RENDERTGT,
                             "startRenderPass: color attachments beyond index 0 must supply an explicit texture");
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
        // (Debug-Layer-Plan §4.3: the "Starting Render Pass" line is dropped —
        // the "RenderPass begin" trace above already announces the pass.)
        if(needsBarrier){
            /// Ensure texture is ready to render to when fragment stage begins.
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,barrier.handle()) beforeStages:MTLRenderStageFragment];
        }
    };

    void GEMetalCommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState){
        auto *ps = (GEMetalRenderPipelineState *)pipelineState.get();
        ps->renderPipelineState.assertExists();
        DEBUG_TRACE(DEBUG_DOMAIN_PIPELINE, "PSO set");
        /// §16 Phase E — in a deferred tessellation pass the render encoder does
        /// not exist yet (it opens in `drawPatches`, after the hull dispatch), so
        /// just record the pipeline; `drawPatches` applies it to the encoder it
        /// opens. Binding a tessellation pipeline in a plain `startRenderPass`
        /// (rp already open) falls through to the normal apply below, which is
        /// wrong for tessellation — reject it so the caller uses
        /// `startTessRenderPass`.
        if(inTessRenderPass && rp == nil){
            renderPipelineState = ps;
            return;
        }
        if(ps->isTessellation){
            DEBUG_CRITICAL(DEBUG_DOMAIN_PIPELINE,
                "setRenderPipelineState: a tessellation pipeline must be used inside a "
                "startTessRenderPass scope, not a plain startRenderPass");
            assert(false && "tessellation pipeline bound outside startTessRenderPass");
            return;
        }
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtVertexShader(GEBuffer): called outside an active render pass");
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
        // (Debug-Layer-Plan §4.1.5 hot-path rule: no per-bind log on the
        // success path; compile-time opt-in under OMEGAGTE_DEBUG_TRACE_HOT.)
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtVertexShader(GETexture): called outside an active render pass");
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
        // (§4.1.5 hot-path rule: no per-bind log on the success path.)
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtVertexShader(GESamplerState): called outside an active render pass");
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtFragmentShader(GEBuffer): called outside an active render pass");
        auto *metalBuffer = (GEMetalBuffer *)buffer.get();
        metalBuffer->metalBuffer.assertExists();

        checkBufferRoleAgainstShader(_id, renderPipelineState->fragmentShader->internal, *metalBuffer);

        if(metalBuffer->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) beforeStages:MTLRenderStageFragment];
            metalBuffer->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);
        // (§4.1.5 hot-path rule: no per-bind log on the success path.)
        [rp setFragmentBuffer:NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,metalBuffer->metalBuffer.handle()) offset:0 atIndex:index];
        if(shaderHasWriteAccessForResource(_id,renderPipelineState->fragmentShader->internal)){
            metalBuffer->needsBarrier = true;
            [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalBuffer->resourceBarrier.handle()) afterStages:MTLRenderStageFragment];
        }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned _id,
                                                              const TextureSwizzle & swizzle){
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtFragmentShader(GETexture): called outside an active render pass");
        auto *metalTexture = (GEMetalTexture *)texture.get();

        checkTextureBindAgainstShader(_id, renderPipelineState->fragmentShader->internal, *metalTexture);

        if(metalTexture->needsBarrier){
            [rp waitForFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) beforeStages:MTLRenderStageFragment];
            metalTexture->needsBarrier = false;
        }

        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);
        TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, _id, renderPipelineState->fragmentShader->internal);
        id<MTLTexture> view = metalTexture->getOrCreateSwizzledView(effective);
        // (§4.1.5 hot-path rule: no per-bind log on the success path.)
        [rp setFragmentTexture:view atIndex:index];
        // if(shaderHasWriteAccessForResource(_id,renderPipelineState->fragmentShader->internal)){
        //     metalTexture->needsBarrier = true;
        //     [rp updateFence:NSOBJECT_OBJC_BRIDGE(id<MTLFence>,metalTexture->resourceBarrier.handle()) afterStages:MTLRenderStageFragment];
        // }
    };

    void GEMetalCommandBuffer::bindResourceAtFragmentShader(SharedHandle<GESamplerState> & sampler,unsigned _id){
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtFragmentShader(GESamplerState): called outside an active render pass");
        auto *metalSampler = (GEMetalSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(_id, renderPipelineState->fragmentShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;
        unsigned index = getResourceLocalIndexFromGlobalIndex(_id,renderPipelineState->fragmentShader->internal);
        [rp setFragmentSamplerState:NSOBJECT_OBJC_BRIDGE(id<MTLSamplerState>,metalSampler->samplerState.handle()) atIndex:index];
    };

    void GEMetalCommandBuffer::setRenderConstants(const void *data, unsigned size, unsigned offset){
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_PIPELINE,
                     "setRenderConstants: called outside an active render pass");
        metalRequireOrReturn(renderPipelineState != nullptr, DEBUG_DOMAIN_PIPELINE,
                     "setRenderConstants: no render pipeline bound");
        // Metal's setBytes replaces the whole binding from the encoder's
        // perspective; it has no destination-offset-into-range concept, so a
        // partial update (offset != 0) is not expressible. Honor it on
        // D3D12/Vulkan; require a full-block set here. (Documented follow-up.)
        metalRequireOrReturn(offset == 0, DEBUG_DOMAIN_PIPELINE,
                     "setRenderConstants: Metal supports only offset == 0 (full-block set)");
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
        metalRequireOrReturn(any, DEBUG_DOMAIN_PIPELINE,
                     "setRenderConstants: bound pipeline declares no constant<T> push constant");
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_RENDERTGT,
                     "setStencilRef: called outside an active render pass");
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "draw call: called outside an active render pass");
        [rp drawPrimitives:metalPrimitiveTypeForPolygonType(polygonType) vertexStart:startIdx vertexCount:vertexCount];
    };

    void GEMetalCommandBuffer::drawPatches(unsigned patchCount, SharedHandle<GEBuffer> & controlPointBuffer, size_t startPatch){
        metalRequireOrReturn(inTessRenderPass, DEBUG_DOMAIN_QUEUE,
                     "drawPatches: called outside a startTessRenderPass scope");
        metalRequireOrReturn(rp == nil && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "drawPatches: an encoder is already open in this tessellation pass");
        metalRequireOrReturn(renderPipelineState != nullptr && renderPipelineState->isTessellation, DEBUG_DOMAIN_QUEUE,
                     "drawPatches: no tessellation pipeline bound (set a hull+domain pipeline first)");
        if(patchCount == 0) return;

        auto *ps = renderPipelineState;
        id<MTLCommandBuffer> mtlCmd = NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle());
        id<MTLDevice> dev = mtlCmd.device;

        const unsigned N = ps->tessControlPointCount;
        const NSUInteger hullOutLen = (NSUInteger)ps->controlPointStride * (NSUInteger)N * (NSUInteger)patchCount;
        const NSUInteger factorLen  = (NSUInteger)ps->tessFactorStructSize * (NSUInteger)patchCount;

        /// Engine-owned intermediate buffers (Private / GPU-only). Created +1,
        /// bound to both encoders (Metal retains them for the command buffer's
        /// GPU lifetime), then released — freed after completion.
        id<MTLBuffer> hullOut = [dev newBufferWithLength:hullOutLen options:MTLResourceStorageModePrivate];
        id<MTLBuffer> factors = [dev newBufferWithLength:factorLen options:MTLResourceStorageModePrivate];

        auto *cpBuf = (GEMetalBuffer *)controlPointBuffer.get();
        cpBuf->metalBuffer.assertExists();
        id<MTLBuffer> inputCP = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>,cpBuf->metalBuffer.handle());

        /// 1. Hull / factor compute dispatch — one thread per patch. Writes the
        /// post-hull control points (hullOut, slot 1) and the tessellation
        /// factors (factors, the synthesized slot 2); reads the caller's input
        /// control points (slot 0).
        id<MTLComputeCommandEncoder> tessCp = [mtlCmd computeCommandEncoder];
        [tessCp setComputePipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLComputePipelineState>,ps->hullComputePipelineState.handle())];
        [tessCp setBuffer:inputCP offset:0 atIndex:0];
        [tessCp setBuffer:hullOut offset:0 atIndex:1];
        [tessCp setBuffer:factors offset:0 atIndex:2];
        NSUInteger tg = ps->hullThreadgroupSize > 0 ? ps->hullThreadgroupSize : 1;
        if(tg > patchCount) tg = patchCount;
        [tessCp dispatchThreads:MTLSizeMake(patchCount,1,1) threadsPerThreadgroup:MTLSizeMake(tg,1,1)];
        [tessCp endEncoding];

        /// 2. Render encoder — opened here (deferred). Metal serializes it behind
        /// the compute dispatch via the Private-buffer hazards, so this waits for
        /// the factors on the GPU without a CPU stall. `startRenderPass` builds
        /// the attachment/load-store setup from the stored descriptor and sets
        /// `rp`; then apply the recorded tessellation pipeline, bind the factor
        /// + post-hull control-point buffers, and drawPatches.
        startRenderPass(tessRenderPassDesc);
        [rp setRenderPipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLRenderPipelineState>,ps->renderPipelineState.handle())];
        [rp setFrontFacingWinding:ps->rasterizerState.winding];
        [rp setCullMode:ps->rasterizerState.cullMode];
        [rp setTriangleFillMode:ps->rasterizerState.fillMode];
        if(ps->hasDepthStencilState){
            [rp setDepthStencilState:NSOBJECT_OBJC_BRIDGE(id<MTLDepthStencilState>,ps->depthStencilState.handle())];
            [rp setDepthBias:ps->rasterizerState.depthBias slopeScale:ps->rasterizerState.slopeScale clamp:ps->rasterizerState.depthClamp];
        }
        [rp setTessellationFactorBuffer:factors offset:0 instanceStride:0];
        [rp setVertexBuffer:hullOut offset:0 atIndex:ps->cpStageInBufferIndex];
        [rp drawPatches:N
             patchStart:startPatch
             patchCount:patchCount
       patchIndexBuffer:nil
 patchIndexBufferOffset:0
          instanceCount:1
           baseInstance:0];

        [hullOut release];
        [factors release];
    };

    void GEMetalCommandBuffer::setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType){
        auto *metalBuffer = (GEMetalBuffer *)buffer.get();
        pendingIndexBuffer = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalBuffer->metalBuffer.handle());
        pendingIndexType = (indexType == RenderPassIndexType::UInt16) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    };

    void GEMetalCommandBuffer::drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                                   unsigned indexCount, size_t startIndex,
                                                   int baseVertex){
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "draw call: called outside an active render pass");
        metalRequireOrReturn(pendingIndexBuffer != nil, DEBUG_DOMAIN_QUEUE,
                     "drawIndexedPolygons: no index buffer bound (call setIndexBuffer first)");
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "draw call: called outside an active render pass");
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "draw call: called outside an active render pass");
        metalRequireOrReturn(pendingIndexBuffer != nil, DEBUG_DOMAIN_QUEUE,
                     "drawIndexedPolygonsInstanced: no index buffer bound (call setIndexBuffer first)");
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
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "draw call: called outside an active render pass");
        auto *metalArgBuffer = (GEMetalBuffer *)argumentBuffer.get();
        id<MTLBuffer> argBuf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalArgBuffer->metalBuffer.handle());
        [rp drawPrimitives:metalPrimitiveTypeForPolygonType(polygonType)
            indirectBuffer:argBuf
      indirectBufferOffset:argumentBufferOffset];
    };

    void GEMetalCommandBuffer::drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                           SharedHandle<GEBuffer> & argumentBuffer,
                                                           size_t argumentBufferOffset){
        metalRequireOrReturn(rp && cp == nil, DEBUG_DOMAIN_QUEUE,
                     "draw call: called outside an active render pass");
        metalRequireOrReturn(pendingIndexBuffer != nil, DEBUG_DOMAIN_QUEUE,
                     "drawIndexedPolygonsIndirect: no index buffer bound (call setIndexBuffer first)");
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
        DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "RenderPass end");
        renderPipelineState = nullptr;
        [rp endEncoding];
        rp = nil;
        /// §16 Phase E — clear any deferred-tessellation state (harmless no-op
        /// for a plain render pass). The intermediate buffers were already
        /// released in `drawPatches`; Metal holds its own references until the
        /// command buffer completes.
        inTessRenderPass = false;
    };

    void GEMetalCommandBuffer::startComputePass(const GEComputePassDescriptor & desc){
        buffer.assertExists();
        cp = [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) computeCommandEncoder];
        DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "ComputePass begin");
    };

    void GEMetalCommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> & pipelineState){
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_PIPELINE,
                     "setComputePipelineState: called outside an active compute pass");
        auto * ps = (GEMetalComputePipelineState *)pipelineState.get();
        ps->computePipelineState.assertExists();
        computePipelineState = ps;
        [cp setComputePipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLComputePipelineState>,ps->computePipelineState.handle())];
    };

    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int _id) {
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtComputeShader(GEBuffer): called outside an active compute pass");
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
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtComputeShader(GETexture): called outside an active compute pass");
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
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtComputeShader(GESamplerState): called outside an active compute pass");
        auto *metalSampler = (GEMetalSamplerState *)sampler.get();
        bool ok = checkSamplerBindAgainstShader(_id, computePipelineState->computeShader->internal);
        assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
        if(!ok) return;
        [cp setSamplerState:NSOBJECT_OBJC_BRIDGE(id<MTLSamplerState>,metalSampler->samplerState.handle())
                    atIndex:getResourceLocalIndexFromGlobalIndex(_id,computePipelineState->computeShader->internal)];
    }


    void GEMetalCommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct,unsigned int idx){
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_RESOURCE,
                     "bindResourceAtComputeShader(GEAccelerationStruct): called outside an active compute pass");
        auto mtl_accel_struct = (GEMetalAccelerationStruct *)accelStruct.get();
        [cp setAccelerationStructure:NSOBJECT_OBJC_BRIDGE(id<MTLAccelerationStructure>,mtl_accel_struct->accelStruct.handle()) atBufferIndex:getResourceLocalIndexFromGlobalIndex(idx,computePipelineState->computeShader->internal)];
    }

    void GEMetalCommandBuffer::setComputeConstants(const void *data, unsigned size, unsigned offset){
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_PIPELINE,
                     "setComputeConstants: called outside an active compute pass");
        metalRequireOrReturn(computePipelineState != nullptr, DEBUG_DOMAIN_PIPELINE,
                     "setComputeConstants: no compute pipeline bound");
        metalRequireOrReturn(offset == 0, DEBUG_DOMAIN_PIPELINE,
                     "setComputeConstants: Metal supports only offset == 0 (full-block set)");
        (void)offset;
        unsigned idx = 0;
        bool found = findPushConstantBufferIndex(computePipelineState->computeShader->internal, idx);
        metalRequireOrReturn(found, DEBUG_DOMAIN_PIPELINE,
                     "setComputeConstants: bound pipeline declares no constant<T> push constant");
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
        metalRequireOrReturn(rp != nil, DEBUG_DOMAIN_QUEUE,
                     "drawMeshTasks: called outside an active render pass");
        metalRequireOrReturn(renderPipelineState != nullptr, DEBUG_DOMAIN_QUEUE,
                     "drawMeshTasks: no pipeline bound (call setRenderPipelineState first)");
        metalRequireOrReturn(renderPipelineState->isMesh, DEBUG_DOMAIN_PIPELINE,
                     "drawMeshTasks: bound pipeline is a graphics pipeline, not a mesh pipeline; "
                     "use makeMeshPipelineState to build a mesh-variant PSO");

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
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_QUEUE,
                     "dispatchThreadgroups: called outside an active compute pass");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        [cp dispatchThreadgroups:MTLSizeMake(x,y,z) threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
    }

    void GEMetalCommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_QUEUE,
                     "dispatchThreads: called outside an active compute pass");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        [cp dispatchThreads:MTLSizeMake(x,y,z) threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
    }

    void GEMetalCommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                            size_t argumentBufferOffset) {
        metalRequireOrReturn(cp != nil, DEBUG_DOMAIN_QUEUE,
                     "dispatchThreadgroupsIndirect: called outside an active compute pass");
        auto & threadgroup_desc = computePipelineState->computeShader->internal.threadgroupDesc;
        auto *metalArgBuffer = (GEMetalBuffer *)argumentBuffer.get();
        id<MTLBuffer> argBuf = NSOBJECT_OBJC_BRIDGE(id<MTLBuffer>, metalArgBuffer->metalBuffer.handle());
        [cp dispatchThreadgroupsWithIndirectBuffer:argBuf
                              indirectBufferOffset:argumentBufferOffset
                             threadsPerThreadgroup:MTLSizeMake(threadgroup_desc.x,threadgroup_desc.y,threadgroup_desc.z)];
    }

    void GEMetalCommandBuffer::finishComputePass(){
        DEBUG_TRACE(DEBUG_DOMAIN_QUEUE, "ComputePass end");
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
        DEBUG_TRACE(DEBUG_DOMAIN_RENDERTGT, "Present: cb=" << traceResourceId);
    };
    
    void GEMetalCommandBuffer::_commit(){
         // (Debug-Layer-Plan §4.3: pre-commit status line dropped — the
         // post-completion trace below covers the command buffer's outcome.)
         buffer.assertExists();
         auto completion = completionHandler;
         completionHandler = nullptr;
         // Capture member values by copy so the block remains valid even
         // if this GEMetalCommandBuffer is destroyed before the GPU finishes.
         const auto capturedTraceId = traceResourceId;
         const auto capturedQueueTraceId = parentQueue != nullptr ? parentQueue->traceResourceId : static_cast<std::uint64_t>(0);
         [NSOBJECT_OBJC_BRIDGE(id<MTLCommandBuffer>,buffer.handle()) addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer){
            if(commandBuffer.status == MTLCommandBufferStatusError){
                // Engine-side report (the GPU told us it failed): ERROR, not
                // CRITICAL — this is not a caller-contract violation.
                const char *errStr = commandBuffer.error
                                         ? commandBuffer.error.localizedDescription.UTF8String
                                         : "<none>";
                DEBUG_ERROR(DEBUG_DOMAIN_QUEUE,
                    "CB execution error: cb=" << capturedTraceId << " error=" << errStr);
            }
            else if(commandBuffer.status == MTLCommandBufferStatusCompleted){
                DEBUG_TRACE(DEBUG_DOMAIN_QUEUE,
                    "CB complete: cb=" << capturedTraceId << " duration="
                    << (1000.f * (commandBuffer.GPUEndTime - commandBuffer.GPUStartTime)) << "ms");
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
        DEBUG_INFO(DEBUG_DOMAIN_QUEUE, "Queue created: queue=" << traceResourceId << " type=" << (int)desc.type);
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
        DEBUG_TRACE(DEBUG_DOMAIN_QUEUE,
            "CB submit: queue=" << traceResourceId << " cb=" << _commandBuffer->traceResourceId);
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

    void GEMetalCommandQueue::commitToGPU(const GECommitCompletionHandler & onComplete){
        if(!onComplete){
            commitToGPU();
            return;
        }
        // Compose an aggregating per-buffer completion handler onto the batch,
        // then commit. Metal fills GECommandBufferCompletionInfo's
        // gpu{Start,End}TimeSec from the MTLCommandBuffer's GPUStartTime /
        // GPUEndTime (see _commit), so this yields real per-commit GPU time
        // with no extra timestamp plumbing. installCommitAggregator must run
        // before commitToGPU() because _commit consumes each buffer's
        // completion handler at commit time.
        installCommitAggregator(commandBuffers, onComplete);
        commitToGPU();
    };

    void GEMetalCommandQueue::commitToGPUAndPresent(NSSmartPtr & drawable){
        if(commandBuffers.empty()){
            // Caller-contract violation (Debug-Layer-Plan §4.3): present with no
            // buffered work. Critical so it surfaces even with the layer off;
            // the existing graceful early-return stays.
            DEBUG_CRITICAL(DEBUG_DOMAIN_QUEUE, "commitToGPUAndPresent with no buffered command buffers");
            return;
        }
        auto & b = commandBuffers.back();
        ((GEMetalCommandBuffer *)b.get())->_present_drawable(drawable);
        commitToGPU();
    };

    void GEMetalCommandQueue::commitToGPUAndWait() {
        if(commandBuffers.empty()){
            // Nothing buffered on our side — a prior fire-and-forget
            // commitToGPU() already flushed and cleared the list (commitToGPU
            // unconditionally clear()s it, see above). commandBuffers.back()
            // on an empty vector is undefined behavior: it previously read
            // whatever garbage happened to occupy that memory and __bridge-
            // cast it to id<MTLCommandBuffer>, which crashed
            // ('-[__NSCFNumber addCompletedHandler:]: unrecognized selector')
            // the first time a caller's teardown path (onClose after an
            // earlier commitToGPU()) actually hit this on real hardware.
            // Insert a lightweight barrier buffer on the raw queue and wait
            // on it instead — Metal command buffers on one queue complete in
            // submission order, so once this empty buffer completes, every
            // buffer submitted before it (from the earlier commitToGPU())
            // has too.
            id<MTLCommandBuffer> barrier =
                [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,commandQueue.handle()) commandBuffer];
            [barrier commit];
            [barrier waitUntilCompleted];
            return;
        }
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
        DEBUG_INFO(DEBUG_DOMAIN_QUEUE, "Queue destroyed: queue=" << traceResourceId);
        dispatch_release(semaphore);
    //    [NSOBJECT_OBJC_BRIDGE(id<MTLCommandQueue>,commandQueue.handle()) autorelease];
    };

    SharedHandle<GECommandBuffer> GEMetalCommandQueue::getAvailableBuffer(){
        ++currentlyOccupied;
        auto s = this;
        return SharedHandle<GECommandBuffer>(new GEMetalCommandBuffer(s));
    };
_NAMESPACE_END_
