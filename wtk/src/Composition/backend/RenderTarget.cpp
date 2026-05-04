

#include "RenderTarget.h"
#include "VisualTree.h"
#include "TexturePool.h"
#include "BufferPool.h"
#include "FencePool.h"
#include "MainThreadDispatch.h"
#include "Pipeline.h"
#include "ResourceFactory.h"
#include "omegaWTK/Composition/Canvas.h"
#include "GeometryConvert.h"
#include "ResourceTrace.h"

#include "omegaWTK/Media/ImgCodec.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>

namespace OmegaWTK::Composition {
    #ifdef TARGET_MACOS
    void stopMTLCapture();
    #endif

    namespace {
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
        inline BufferPool * bufferPool(){
            return BackendResourceFactory::instance().bufferPool();
        }
        inline FencePool * fencePool(){
            return BackendResourceFactory::instance().fencePool();
        }

        // Sizing math, collapsed in from BackingTextureSet in Phase 4.2.
        // The backing texture is always the swap chain drawable on the
        // always-direct path; these helpers only sanitize / scale the
        // logical rect and clamp to engine limits.
        constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
        constexpr float kRenderScaleFloor = 2.f;
#else
        constexpr float kRenderScaleFloor = 1.f;
#endif

        inline float sanitizeRenderScale(float scale){
            if(!std::isfinite(scale) || scale <= 0.f){
                return kRenderScaleFloor;
            }
            return std::clamp(scale, kRenderScaleFloor, kMaxTextureDimension);
        }

        inline float sanitizeCoordinate(float value, float fallback){
            if(!std::isfinite(value)){
                return fallback;
            }
            return value;
        }

        inline Composition::Rect sanitizeRenderRect(const Composition::Rect & candidate,
                                                    const Composition::Rect & fallback,
                                                    float renderScale){
            Composition::Rect sanitizedFallback = fallback;
            sanitizedFallback.pos.x = sanitizeCoordinate(sanitizedFallback.pos.x, 0.f);
            sanitizedFallback.pos.y = sanitizeCoordinate(sanitizedFallback.pos.y, 0.f);
            if(!std::isfinite(sanitizedFallback.w) || sanitizedFallback.w <= 0.f){
                sanitizedFallback.w = 1.f;
            }
            if(!std::isfinite(sanitizedFallback.h) || sanitizedFallback.h <= 0.f){
                sanitizedFallback.h = 1.f;
            }

            const float scale = sanitizeRenderScale(renderScale);
            const float maxLogicalDimension = std::max(1.f, kMaxTextureDimension / scale);
            sanitizedFallback.w = std::clamp(sanitizedFallback.w, 1.f, maxLogicalDimension);
            sanitizedFallback.h = std::clamp(sanitizedFallback.h, 1.f, maxLogicalDimension);

            Composition::Rect sanitized = candidate;
            sanitized.pos.x = sanitizeCoordinate(sanitized.pos.x, sanitizedFallback.pos.x);
            sanitized.pos.y = sanitizeCoordinate(sanitized.pos.y, sanitizedFallback.pos.y);

            if(!std::isfinite(sanitized.w) || sanitized.w <= 0.f){
                sanitized.w = sanitizedFallback.w;
            }
            if(!std::isfinite(sanitized.h) || sanitized.h <= 0.f){
                sanitized.h = sanitizedFallback.h;
            }

            sanitized.w = std::clamp(sanitized.w, 1.f, maxLogicalDimension);
            sanitized.h = std::clamp(sanitized.h, 1.f, maxLogicalDimension);
            return sanitized;
        }

        inline unsigned toBackingDimension(float logicalDimension, float renderScale){
            const float saneScale = sanitizeRenderScale(renderScale);
            float saneLogical = logicalDimension;
            if(!std::isfinite(saneLogical) || saneLogical <= 0.f){
                saneLogical = 1.f;
            }
            const auto scaled = static_cast<long>(std::lround(saneLogical * saneScale));
            const auto clamped = std::clamp<long>(scaled, 1L, static_cast<long>(kMaxTextureDimension));
            return static_cast<unsigned>(clamped);
        }
    }

    void InitializeEngine(){
        BackendResourceFactory::instance().pipelines().initialize();
        BackendResourceFactory::instance().initializePools();
    }

    void CleanupEngine(){
        BackendResourceFactory::instance().shutdownPools();
        BackendResourceFactory::instance().pipelines().shutdown();
    }

BackendRenderTargetContext::BackendRenderTargetContext(Composition::Rect & rect,
        SharedHandle<OmegaGTE::GENativeRenderTarget> &renderTargetIn,
        float renderScaleValue):
        fence(fencePool() != nullptr ? fencePool()->acquire() : gte.graphicsEngine->makeFence()),
        renderTarget(renderTargetIn),
        renderTargetSize_(rect),
        renderScale_(sanitizeRenderScale(renderScaleValue)),
        frameRenderPass_(*this)
        {
    renderTargetSize_ = sanitizeRenderRect(rect,
                                           Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f},
                                           renderScale_);
    backingWidth_  = toBackingDimension(renderTargetSize_.w, renderScale_);
    backingHeight_ = toBackingDimension(renderTargetSize_.h, renderScale_);

    traceResourceId = ResourceTrace::nextResourceId();
    ResourceTrace::emit("Create",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        renderTargetSize_.w,
                        renderTargetSize_.h,
                        renderScale_);
    rebuildBackingTarget();
    imageProcessor = BackendResourceFactory::instance().effectProcessor();
}

void BackendRenderTargetContext::recomputeBackingDimensions(){
    renderTargetSize_ = sanitizeRenderRect(renderTargetSize_,
                                           Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f},
                                           renderScale_);
    backingWidth_  = toBackingDimension(renderTargetSize_.w, renderScale_);
    backingHeight_ = toBackingDimension(renderTargetSize_.h, renderScale_);
}

void BackendRenderTargetContext::rebuildBackingTarget(){
    recomputeBackingDimensions();
    ResourceTrace::emit("ResizeRebuild",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        static_cast<float>(backingWidth_),
                        static_cast<float>(backingHeight_),
                        renderScale_);
    // Always-direct path: the only resource bound to the backing target
    // dimensions is the tessellation context. Textures live on the swap
    // chain (managed externally) or per-blurred-layer scratches (allocated
    // on demand by `LayerBlurScratch`).
    if(renderTarget == nullptr){
        tessellationContext_.reset();
        return;
    }
    tessellationContext_ = gte.triangulationEngine->createTEContextFromNativeRenderTarget(renderTarget);
    if(tessellationContext_ == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "Failed to create tessellation context for native render target." << std::endl;
#endif
    }
}

BackendRenderTargetContext::~BackendRenderTargetContext(){
    ResourceTrace::emit("Destroy",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        renderTargetSize_.w,
                        renderTargetSize_.h,
                        renderScale_);
    tessellationContext_.reset();
    imageProcessor.reset();
    for(auto & entry : deferredBufferReleases){
        if(bufferPool() != nullptr && entry.first){
            bufferPool()->release(std::move(entry.first), entry.second);
        }
    }
    deferredBufferReleases.clear();
    if(fencePool() != nullptr && fence){
        fencePool()->release(std::move(fence));
    }
}

    void BackendRenderTargetContext::setRenderTargetSize(Composition::Rect &rect) {
        const unsigned oldW = backingWidth_;
        const unsigned oldH = backingHeight_;
        renderTargetSize_ = sanitizeRenderRect(rect, renderTargetSize_, renderScale_);
        const unsigned newW = toBackingDimension(renderTargetSize_.w, renderScale_);
        const unsigned newH = toBackingDimension(renderTargetSize_.h, renderScale_);
        if(oldW != newW || oldH != newH){
            rebuildBackingTarget();
        }
    }

    void BackendRenderTargetContext::setViewportOverride(float offsetX,float offsetY,float width,float height){
        frameRenderPass_.setViewportOverride(offsetX, offsetY, width, height);
        // Update logical size for tessellation; grow backing surface if the
        // requested viewport extent exceeds the current backing. Never
        // shrinks.
        renderTargetSize_.pos.x = 0.f;
        renderTargetSize_.pos.y = 0.f;
        renderTargetSize_.w = width;
        renderTargetSize_.h = height;
        const unsigned neededW = toBackingDimension(offsetX + width,  renderScale_);
        const unsigned neededH = toBackingDimension(offsetY + height, renderScale_);
        if(neededW > backingWidth_ || neededH > backingHeight_){
            backingWidth_  = std::max(backingWidth_,  neededW);
            backingHeight_ = std::max(backingHeight_, neededH);
            rebuildBackingTarget();
        }
    }

    void BackendRenderTargetContext::clearViewportOverride(){
        frameRenderPass_.clearViewportOverride();
    }

#ifdef _WIN32
void BackendRenderTargetContext::resizeSwapChain(unsigned int newBackingWidth, unsigned int newBackingHeight) {
    if (renderTarget != nullptr)
        renderTarget->resizeSwapChain(newBackingWidth, newBackingHeight);
}
void BackendRenderTargetContext::waitForGPU() {
    if (renderTarget != nullptr)
        renderTarget->waitForGPU();
}
#endif

void BackendRenderTargetContext::beginFrame(float clearR, float clearG, float clearB, float clearA) {
    frameRenderPass_.begin(clearR, clearG, clearB, clearA);
}

void BackendRenderTargetContext::endFrame() {
    frameRenderPass_.end();
}

void BackendRenderTargetContext::resetElementState() {
    currentTransform = OmegaGTE::FMatrix<4,4>::Identity();
    currentOpacity   = 1.f;
}

    namespace {
        OmegaCommon::Vector<CanvasEffect> layerBlursToCanvasEffects(
                const OmegaCommon::Vector<LayerBlur> & blurs){
            OmegaCommon::Vector<CanvasEffect> out;
            out.reserve(blurs.size());
            for(const auto & blur : blurs){
                if(!std::isfinite(blur.radius) || blur.radius <= 0.f){
                    continue;
                }
                CanvasEffect ce {};
                if(blur.type == LayerBlur::Type::Directional){
                    ce.type = CanvasEffect::Type::DirectionalBlur;
                    ce.directionalBlur.radius = blur.radius;
                    ce.directionalBlur.angle  = blur.angle;
                }
                else {
                    ce.type = CanvasEffect::Type::GaussianBlur;
                    ce.gaussianBlur.radius = blur.radius;
                }
                out.push_back(ce);
            }
            return out;
        }

        unsigned scaledExtent(float logical, float scale){
            const float v = std::max(1.f, logical) * std::max(1.f, scale);
            const long  c = std::clamp<long>(static_cast<long>(std::lround(v)),
                                             1L, 16384L);
            return static_cast<unsigned>(c);
        }
    }

    namespace {
        /// Author a 6-vertex unit NDC quad into a fresh upload buffer using
        /// the texture pipeline's vertex layout `(float4 pos, float2 uv,
        /// float2 tintPad)`. The buffer is positioned by GPU viewport
        /// remap, never by recalculating vertex coordinates. Used by the
        /// per-layer blur composite path; allocated from the buffer pool
        /// when one is available so the bytes return after the frame
        /// completes.
        SharedHandle<OmegaGTE::GEBuffer>
        authorCompositeQuadBuffer(BufferPool *pool, std::size_t & outBytes){
            const std::size_t structSize = OmegaGTE::omegaSLStructStride(
                    {OMEGASL_FLOAT4, OMEGASL_FLOAT2, OMEGASL_FLOAT2});
            const std::size_t totalBytes = structSize * 6;
            outBytes = totalBytes;

            SharedHandle<OmegaGTE::GEBuffer> buffer;
            if(pool != nullptr){
                buffer = pool->acquire(totalBytes, structSize);
            }
            else {
                OmegaGTE::BufferDescriptor desc {
                        OmegaGTE::BufferDescriptor::Upload,
                        totalBytes,
                        structSize};
                buffer = gte.graphicsEngine->makeBuffer(desc);
            }
            if(buffer == nullptr){
                return nullptr;
            }

            auto bufferWriter = pipelineRegistry().bufferWriter();
            if(bufferWriter == nullptr){
                return nullptr;
            }
            bufferWriter->setOutputBuffer(buffer);

            auto pos      = OmegaGTE::FVec<4>::Create();
            auto texCoord = OmegaGTE::FVec<2>::Create();
            auto pad      = OmegaGTE::FVec<2>::Create();
            // tintPad.x = 1 (full opacity) carries through textureFragment's
            // alpha multiply unchanged; tintPad.y is reserved.
            pad[0][0] = 1.f; pad[1][0] = 0.f;

            auto emit = [&](float x, float y, float u, float v){
                pos[0][0] = x; pos[1][0] = y; pos[2][0] = 0.f; pos[3][0] = 1.f;
                texCoord[0][0] = u; texCoord[1][0] = v;
                bufferWriter->structBegin();
                bufferWriter->writeFloat4(pos);
                bufferWriter->writeFloat2(texCoord);
                bufferWriter->writeFloat2(pad);
                bufferWriter->structEnd();
                bufferWriter->sendToBuffer();
            };

            // Triangle 1: (-1,1) (-1,-1) (1,-1)
            emit(-1.f,  1.f, 0.f, 0.f);
            emit(-1.f, -1.f, 0.f, 1.f);
            emit( 1.f, -1.f, 1.f, 1.f);
            // Triangle 2: (-1,1) (1,1) (1,-1)
            emit(-1.f,  1.f, 0.f, 0.f);
            emit( 1.f,  1.f, 1.f, 0.f);
            emit( 1.f, -1.f, 1.f, 1.f);

            bufferWriter->flush();
            return buffer;
        }
    }

    void BackendRenderTargetContext::compositeScratchOntoFrame(
            LayerBlurScratch & scratch,
            const Composition::Rect & destBounds,
            const Composition::Point2D & windowOffset){
        auto & pipelines = pipelineRegistry();
        auto texturePipeline = pipelines.texture();
        if(texturePipeline == nullptr){
            return;
        }

        std::size_t quadBytes = 0;
        auto quadBuffer = authorCompositeQuadBuffer(bufferPool(), quadBytes);
        if(quadBuffer == nullptr){
            return;
        }

        // The composite waits on the scratch's fence so the compute writes
        // have finished before sampling. beginDraw handles the
        // outside-render-pass notify dance.
        auto scope = frameRenderPass_.beginDraw(scratch.fence());
        if(scope.cb == nullptr){
            if(bufferPool() != nullptr){
                bufferPool()->release(std::move(quadBuffer), quadBytes);
            }
            return;
        }
        auto & cb = scope.cb;

        // Position the composite quad at the slice's window position. The
        // unit NDC quad spans [-1..1] x [-1..1]; the GPU viewport remaps it
        // onto (windowOffset.{x,y}) -> (windowOffset + bounds).
        const float scale = renderScale_;
        OmegaGTE::GEViewport vp {};
        vp.x = std::max(0.f, windowOffset.x) * scale;
        vp.y = std::max(0.f, windowOffset.y) * scale;
        vp.width  = std::max(1.f, destBounds.w) * scale;
        vp.height = std::max(1.f, destBounds.h) * scale;
        vp.nearDepth = 0.f;
        vp.farDepth  = 1.f;
        OmegaGTE::GEScissorRect sr {vp.x, vp.y, vp.width, vp.height};
        cb->setViewports({vp});
        cb->setScissorRects({sr});

        frameRenderPass_.bindTexturePipeline(scope);
        cb->bindResourceAtVertexShader(quadBuffer, 1);
        auto sourceTex = scratch.source();
        cb->bindResourceAtFragmentShader(sourceTex, 2);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);

        frameRenderPass_.endDraw(scope);

        if(bufferPool() != nullptr && quadBuffer){
            deferredBufferReleases.push_back({std::move(quadBuffer), quadBytes});
        }
    }

    void BackendRenderTargetContext::renderBlurredSlice(
            const CompositeFrame::WidgetSlice & slice){
        if(slice.targetLayer == nullptr || !slice.targetLayer->hasBlur()){
            for(auto & cmd : slice.commands){
                renderToTarget(cmd.type, (void *)&cmd.params);
            }
            return;
        }

        const float w = (std::isfinite(slice.bounds.w) && slice.bounds.w > 0.f) ? slice.bounds.w : 1.f;
        const float h = (std::isfinite(slice.bounds.h) && slice.bounds.h > 0.f) ? slice.bounds.h : 1.f;
        const float scale = renderScale_;
        const unsigned sw = scaledExtent(w, scale);
        const unsigned sh = scaledExtent(h, scale);

        // Acquire / resize the per-layer scratch.
        auto & scratchSlot = layerScratches[slice.targetLayer];
        if(scratchSlot == nullptr){
            scratchSlot = std::make_unique<LayerBlurScratch>();
        }
        // Match the texture/color pipelines' BGRA8Unorm color format. Once
        // pipeline format negotiation lands (Phase 6 / risk #1) this should
        // pick the native target's pixel format.
        scratchSlot->resize(sw, sh, OmegaGTE::PixelFormat::BGRA8Unorm);
        if(!scratchSlot->valid()){
            // Allocation failed; fall back to the direct path so the layer
            // still draws (without blur) instead of disappearing.
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] LayerBlurScratch allocation failed; rendering slice unblurred." << std::endl;
#endif
            for(auto & cmd : slice.commands){
                renderToTarget(cmd.type, (void *)&cmd.params);
            }
            return;
        }
        auto & scratch = *scratchSlot;

        // Suspend the frame pass and start a fresh pass on the scratch
        // target. While the scratch pass is open, beginDraw records onto
        // the scratch CB; the renderToTarget vertex math uses
        // renderTargetSize_ (= slice extent thanks to
        // setViewportOverride) to compute NDC, and the scratch's GPU
        // viewport is (0,0,sw,sh) so the layer fills its scratch.
        auto scratchTarget = scratch.sourceTarget();
        frameRenderPass_.beginScratchPass(scratchTarget, sw, sh);
        if(!frameRenderPass_.scratchActive()){
            // Couldn't start scratch pass (no active frame). Fall back to
            // direct render so the layer still appears.
            for(auto & cmd : slice.commands){
                renderToTarget(cmd.type, (void *)&cmd.params);
            }
            return;
        }

        // Reset transient transform/opacity inside the scratch so prior
        // slices' SetTransform/SetOpacity don't bleed in.
        const auto savedTransform = currentTransform;
        const float savedOpacity  = currentOpacity;
        currentTransform = OmegaGTE::FMatrix<4,4>::Identity();
        currentOpacity   = 1.f;

        for(auto & cmd : slice.commands){
            renderToTarget(cmd.type, (void *)&cmd.params);
        }

        currentTransform = savedTransform;
        currentOpacity   = savedOpacity;

        frameRenderPass_.endScratchPass();

        // Apply the layer's blur effects. The processor signals the
        // scratch's own fence; the composite below waits on it.
        bool blurApplied = false;
        if(imageProcessor != nullptr){
            auto blurEffects = layerBlursToCanvasEffects(slice.targetLayer->blurEffects());
            if(!blurEffects.empty()){
                imageProcessor->applyEffects(scratch.pingPong(),
                                             scratchTarget,
                                             blurEffects,
                                             scratch.width(),
                                             scratch.height(),
                                             scratch.fence());
                blurApplied = true;
            }
        }

        // Resume the frame's render pass and composite the blurred scratch
        // onto the swap chain at the slice's window position.
        SharedHandle<OmegaGTE::GEFence> waitFence = blurApplied
                ? scratch.fence()
                : SharedHandle<OmegaGTE::GEFence>{};
        frameRenderPass_.resumeFrameAfterScratch(waitFence);
        compositeScratchOntoFrame(scratch, slice.bounds, slice.windowOffset);
    }

    void BackendRenderTargetContext::purgeDeadLayerScratches(
            const OmegaCommon::Vector<Layer *> & liveLayers){
        if(layerScratches.empty()){
            return;
        }
        auto it = layerScratches.begin();
        while(it != layerScratches.end()){
            bool live = false;
            for(auto *layer : liveLayers){
                if(layer == it->first){
                    live = true;
                    break;
                }
            }
            if(!live){
                it = layerScratches.erase(it);
            }
            else {
                ++it;
            }
        }
    }


    void BackendRenderTargetContext::commit(){
        commit(0,0,std::chrono::steady_clock::now(),{});
    }

    void BackendRenderTargetContext::commit(std::uint64_t syncLaneId,
                                            std::uint64_t syncPacketId,
                                            std::chrono::steady_clock::time_point submitTimeCpu,
                                            BackendSubmissionCompletionHandler completionHandler){
        // Always-direct path: rendering already went to the native drawable in
        // beginFrame/endFrame. Just present.
        renderTarget->commitAndPresent();
        if(completionHandler){
            BackendSubmissionTelemetry telemetry {};
            telemetry.syncLaneId = syncLaneId;
            telemetry.syncPacketId = syncPacketId;
            telemetry.submitTimeCpu = submitTimeCpu;
            telemetry.completeTimeCpu = std::chrono::steady_clock::now();
            telemetry.presentTimeCpu = telemetry.completeTimeCpu;
            telemetry.status = BackendSubmissionStatus::Completed;
            completionHandler(telemetry);
        }
    }

    void BackendRenderTargetContext::releaseDeferredBuffers(){
        if(bufferPool()){
            for(auto & entry : deferredBufferReleases){
                if(entry.first)
                    bufferPool()->release(std::move(entry.first), entry.second);
            }
            deferredBufferReleases.clear();
        }
    }

    typedef decltype(VisualCommand::params) VisualCommandParams;

    void BackendRenderTargetContext::renderToTarget(VisualCommand::Type type, void *params) {
        auto & pipelines = pipelineRegistry();
        auto bufferWriter = pipelines.bufferWriter();
        auto renderPipelineState = pipelines.color();
        auto textureRenderPipelineState = pipelines.texture();
        if(bufferWriter == nullptr || renderTarget == nullptr || tessellationContext_ == nullptr){
            return;
        }
        OmegaGTE::TETriangulationResult result;

        OmegaGTE::GEViewport viewPort {};
        viewPort.x = viewPort.y = viewPort.nearDepth = 0.f;
        viewPort.farDepth = 1.f;
        viewPort.width = renderTargetSize_.w;
        viewPort.height = renderTargetSize_.h;

#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "W:" << renderTargetSize_.w
                  << " H:" << renderTargetSize_.h
                  << " BW:" << backingWidth_
                  << " BH:" << backingHeight_
                  << " S:" << renderScale_
                  << std::endl;
#endif

        if (params == nullptr) {
            return;
        }

        size_t struct_size;
        bool useTextureRenderPipeline = false;
        float textureCoordDenomW = 1.f;
        float textureCoordDenomH = 1.f;

        SharedHandle<OmegaGTE::GETexture> texturePaint;

        SharedHandle<OmegaGTE::GEFence> textureFence;

        switch (type) {
            case VisualCommand::Rect : {
                auto & _params = ((VisualCommandParams*)params)->rectParams;
                if (_params.brush == nullptr) return;
                auto gteRect = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::Rect(gteRect);

                switch (_params.brush->type) {
                    case Brush::Type::Color:    useTextureRenderPipeline = false; break;
                    case Brush::Type::Gradient: useTextureRenderPipeline = true;  break;
                    case Brush::Type::None:     return;
                }
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(color));
                }

                result = tessellationContext_->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::Bitmap : {
                auto & _params = ((VisualCommandParams*)params)->bitmapParams;
                auto gteBmpRect = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::Rect(gteBmpRect);

                useTextureRenderPipeline = true;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
                if(_params.texture){
                    texturePaint = _params.texture;
                    textureFence = _params.textureFence;
                }
                else {
                    OmegaGTE::TextureDescriptor texDesc {OmegaGTE::GETexture::Texture2D};
                    texDesc.usage = OmegaGTE::GETexture::ToGPU;
                    texDesc.width = _params.img->header.width;
                    texDesc.height = _params.img->header.height;
#ifdef OMEGAWTK_TRACE_RENDER
                    std::cout << "TEX W:" << texDesc.width << "TEX H:" << texDesc.height << std::endl;
#endif
                    texturePaint = gte.graphicsEngine->makeTexture(texDesc);
                    texturePaint->copyBytes((void *)_params.img->data,_params.img->header.stride);
                }

                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeTexture2D(uint32_t(_params.rect.w),uint32_t(_params.rect.h)));

                result = tessellationContext_->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::RoundedRect : {
                auto & _params = ((VisualCommandParams*)params)->roundedRectParams;
                if (_params.brush == nullptr) return;
                auto gteRR = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(gteRR);

                switch (_params.brush->type) {
                    case Brush::Type::Color:    useTextureRenderPipeline = false; break;
                    case Brush::Type::Gradient: useTextureRenderPipeline = true;  break;
                    case Brush::Type::None:     return;
                }
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);

                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(color));
                }
                result = tessellationContext_->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::Ellipse : {
                auto & _params = ((VisualCommandParams*)params)->ellipseParams;
                const float cx = _params.ellipse.x;
                const float cy = _params.ellipse.y;
                const float rx = std::max(0.0f,_params.ellipse.rad_x);
                const float ry = std::max(0.0f,_params.ellipse.rad_y);
                if(rx <= 0.0f || ry <= 0.0f){
                    return;
                }

                auto color = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                if(_params.brush != nullptr && _params.brush->type == Brush::Type::Color){
                    color = OmegaGTE::makeColor(_params.brush->color.r,
                                                _params.brush->color.g,
                                                _params.brush->color.b,
                                                _params.brush->color.a);
                }
                if(color[0][0] == 0.f && color[1][0] == 0.f && color[2][0] == 0.f && color[3][0] == 0.f)
                    color = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);

                auto toNdcPoint = [&](float px,float py){
                    return OmegaGTE::GPoint3D{
                            ((2.0f * px) / viewPort.width) - 1.0f,
                            ((2.0f * py) / viewPort.height) - 1.0f,
                            0.0f};
                };

                OmegaGTE::TETriangulationResult::TEMesh mesh {OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangle};
                const auto center = toNdcPoint(cx,cy);

                const float twoPi = static_cast<float>(2.0 * OmegaGTE::PI);
                const unsigned segmentCount = std::min(4096u, std::max(
                        96u,
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * renderScale_))));
                auto prev = toNdcPoint(cx + rx,cy);

                for(unsigned i = 1; i <= segmentCount; i++){
                    const float angle = (twoPi * static_cast<float>(i)) / static_cast<float>(segmentCount);
                    const float px = cx + (std::cos(angle) * rx);
                    const float py = cy + (std::sin(angle) * ry);
                    auto next = toNdcPoint(px,py);

                    OmegaGTE::TETriangulationResult::TEMesh::Polygon tri {};
                    tri.a.pt = center;
                    tri.b.pt = prev;
                    tri.c.pt = next;
                    tri.a.attachment = tri.b.attachment = tri.c.attachment =
                            std::make_optional<OmegaGTE::TETriangulationResult::AttachmentData>(
                                    OmegaGTE::TETriangulationResult::AttachmentData{
                                            color,
                                            OmegaGTE::FVec<2>::Create(),
                                            OmegaGTE::FVec<3>::Create()});

                    mesh.vertexPolygons.push_back(tri);
                    prev = next;
                }

                result.meshes.push_back(mesh);

                break;
            }
            case VisualCommand::VectorPath : {
                auto & _params = ((VisualCommandParams*)params)->pathParams;
                if(_params.path == nullptr || _params.path->size() < 2){
                    return;
                }
                auto te_params = OmegaGTE::TETriangulationParams::GraphicsPath2D(*_params.path,
                                                                                 _params.strokeWidth,
                                                                                 _params.contour,
                                                                                 _params.fill);
                // First attachment: stroke color.
                auto strokeColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                if(_params.brush != nullptr && _params.brush->type == Brush::Type::Color){
                    strokeColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                      _params.brush->color.g,
                                                      _params.brush->color.b,
                                                      _params.brush->color.a);
                }
                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(strokeColor));

                // Second attachment: fill color.
                if(_params.fill && _params.fillBrush != nullptr && _params.fillBrush->type == Brush::Type::Color){
                    auto fillColor = OmegaGTE::makeColor(_params.fillBrush->color.r,
                                                         _params.fillBrush->color.g,
                                                         _params.fillBrush->color.b,
                                                         _params.fillBrush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(fillColor));
                }

                result = tessellationContext_->triangulateSync(te_params,
                                                                  OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,
                                                                  &viewPort);
                break;
            }
            case VisualCommand::Shadow: {
                auto & _params = ((VisualCommandParams*)params)->shadowParams;
                const auto & shadow = _params.shadow;

                // Offset and expand the shape rect by blurAmount.
                float expand = std::max(0.f,shadow.blurAmount);
                Composition::Rect shadowRect {
                    Composition::Point2D{
                        _params.shapeRect.pos.x + shadow.x_offset - expand,
                        _params.shapeRect.pos.y + shadow.y_offset - expand
                    },
                    std::max(1.f,_params.shapeRect.w + expand * 2.f),
                    std::max(1.f,_params.shapeRect.h + expand * 2.f)
                };

                auto shadowColor = OmegaGTE::makeColor(shadow.color.r,
                                                         shadow.color.g,
                                                         shadow.color.b,
                                                         shadow.color.a * shadow.opacity);

                if(_params.isEllipse){
                    // Tessellate as ellipse.
                    float cx = shadowRect.pos.x + shadowRect.w * 0.5f;
                    float cy = shadowRect.pos.y + shadowRect.h * 0.5f;
                    float rx = shadowRect.w * 0.5f;
                    float ry = shadowRect.h * 0.5f;

                    auto toNdcPoint = [&](float px,float py){
                        return OmegaGTE::GPoint3D{
                                ((2.0f * px) / viewPort.width) - 1.0f,
                                ((2.0f * py) / viewPort.height) - 1.0f,
                                0.0f};
                    };

                    OmegaGTE::TETriangulationResult::TEMesh mesh {OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangle};
                    const auto center = toNdcPoint(cx,cy);
                    const unsigned segCount = std::min(4096u,std::max(96u,
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * renderScale_))));
                    auto prev = toNdcPoint(cx + rx,cy);
                    const float twoPi = static_cast<float>(2.0 * OmegaGTE::PI);

                    for(unsigned i = 1; i <= segCount; i++){
                        const float angle = (twoPi * static_cast<float>(i)) / static_cast<float>(segCount);
                        auto next = toNdcPoint(cx + std::cos(angle) * rx,cy + std::sin(angle) * ry);

                        OmegaGTE::TETriangulationResult::TEMesh::Polygon tri {};
                        tri.a.pt = center; tri.b.pt = prev; tri.c.pt = next;
                        tri.a.attachment = tri.b.attachment = tri.c.attachment =
                            std::make_optional<OmegaGTE::TETriangulationResult::AttachmentData>(
                                OmegaGTE::TETriangulationResult::AttachmentData{
                                    shadowColor,OmegaGTE::FVec<2>::Create(),OmegaGTE::FVec<3>::Create()});
                        mesh.vertexPolygons.push_back(tri);
                        prev = next;
                    }
                    result.meshes.push_back(mesh);
                }
                else if(_params.cornerRadius > 0.f){
                    Composition::RoundedRect rr {};
                    rr.pos = shadowRect.pos;
                    rr.w = shadowRect.w;
                    rr.h = shadowRect.h;
                    rr.rad_x = std::min(_params.cornerRadius + expand,shadowRect.w * 0.5f);
                    rr.rad_y = rr.rad_x;
                    auto gteRR_s = toGTE(rr);
                    auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(gteRR_s);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(shadowColor));
                    result = tessellationContext_->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                }
                else {
                    auto gteShadowRect = toGTE(shadowRect);
                    auto te_params = OmegaGTE::TETriangulationParams::Rect(gteShadowRect);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(shadowColor));
                    result = tessellationContext_->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                }
                break;
            }
            case VisualCommand::SetTransform: {
                auto & _params = ((VisualCommandParams*)params)->transformMatrix;
                currentTransform = toGTEMatrix(_params);
                return;
            }
            case VisualCommand::SetOpacity: {
                currentOpacity = ((VisualCommandParams*)params)->opacityValue;
                return;
            }
            case VisualCommand::Text:
            default:
                return;
        }

        if(result.totalVertexCount() == 0){
            return;
        }
        if(useTextureRenderPipeline){
            if(textureRenderPipelineState == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
                std::cout << "Texture render pipeline unavailable. Skipping textured draw command." << std::endl;
#endif
                return;
            }
            struct_size = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4,OMEGASL_FLOAT2,OMEGASL_FLOAT2});
        }
        else {
            if(renderPipelineState == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
                std::cout << "Color render pipeline unavailable. Skipping draw command." << std::endl;
#endif
                return;
            }
            struct_size = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4,OMEGASL_FLOAT4});
        }

        std::size_t requiredBytes = result.totalVertexCount() * struct_size;
        SharedHandle<OmegaGTE::GEBuffer> buffer;
        if(bufferPool()){
            buffer = bufferPool()->acquire(requiredBytes, struct_size);
        }
        else {
            OmegaGTE::BufferDescriptor bufferDesc {OmegaGTE::BufferDescriptor::Upload,requiredBytes,struct_size};
            buffer = gte.graphicsEngine->makeBuffer(bufferDesc);
        }

        bufferWriter->setOutputBuffer(buffer);

        // Acquire the command buffer for this draw. The scope wraps the
        // frame's CB (or the per-layer scratch's CB when a scratch pass is
        // open). Texture-fence handling (mid-pass end+notify+restart) is
        // bundled into beginDraw(). On the always-direct path
        // renderToTarget is only invoked inside an active frame, so a null
        // scope CB indicates a contract violation; short-circuit to avoid
        // recording onto an undefined target.
        auto scope = frameRenderPass_.beginDraw(textureFence);
        if(scope.cb == nullptr){
            if(bufferPool() != nullptr && buffer){
                bufferPool()->release(std::move(buffer), requiredBytes);
            }
            return;
        }
        auto & cb = scope.cb;

        unsigned startVertexIndex = 0;

        const bool hasTransform = !(currentTransform == OmegaGTE::FMatrix<4,4>::Identity());
        const float opacityMul = currentOpacity;

        auto applyTransform = [&](OmegaGTE::FVec<4> & pos){
            if(hasTransform){
                pos = currentTransform * pos;
            }
        };

        auto writeColorVertexToBuffer = [&](OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<4> color){
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = pt.x;
            pos[1][0] = pt.y;
            pos[2][0] = pt.z;
            pos[3][0] = 1.f;
            applyTransform(pos);
            if(opacityMul < 1.f){
                color[3][0] *= opacityMul;
            }
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat4(color);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        };

         auto writeTexVertexToBuffer = [&](OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<2> coord){
            auto normalizedCoord = OmegaGTE::FVec<2>::Create();
            float u = coord[0][0];
            float v = coord[1][0];
            if((u < 0.f || u > 1.f || v < 0.f || v > 1.f) &&
               textureCoordDenomW > 0.f &&
               textureCoordDenomH > 0.f){
                u /= textureCoordDenomW;
                v /= textureCoordDenomH;
            }
            u = std::clamp(u,0.f,1.f);
            v = std::clamp(v,0.f,1.f);
            normalizedCoord[0][0] = u;
            normalizedCoord[1][0] = v;
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = pt.x;
            pos[1][0] = pt.y;
            pos[2][0] = pt.z;
            pos[3][0] = 1.f;
            applyTransform(pos);
            // Trailing float2 is the high half of the shader-side
            // `texCoordTint` float4 (Phase 3): [0] carries the per-element
            // alpha tint (currentOpacity); [1] is reserved for future RGB
            // tinting and held at zero today.
            auto tintPair = OmegaGTE::FVec<2>::Create();
            tintPair[0][0] = std::clamp(opacityMul, 0.f, 1.f);
            tintPair[1][0] = 0.f;
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat2(normalizedCoord);
            bufferWriter->writeFloat2(tintPair);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        };


        const auto fallbackColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
        auto fallbackTexCoord = OmegaGTE::FVec<2>::Create();
        fallbackTexCoord[0][0] = 0.f;
        fallbackTexCoord[1][0] = 0.f;

        for(auto & m : result.meshes) {
            for(auto & v : m.vertexPolygons){
                if(useTextureRenderPipeline){
                    auto & aCoord = v.a.attachment ? v.a.attachment->texture2Dcoord : fallbackTexCoord;
                    auto & bCoord = v.b.attachment ? v.b.attachment->texture2Dcoord : fallbackTexCoord;
                    auto & cCoord = v.c.attachment ? v.c.attachment->texture2Dcoord : fallbackTexCoord;
                    writeTexVertexToBuffer(v.a.pt,aCoord);
                    writeTexVertexToBuffer(v.b.pt,bCoord);
                    writeTexVertexToBuffer(v.c.pt,cCoord);
                }
                else {
                    auto useColor = [&fallbackColor](const std::optional<OmegaGTE::TETriangulationResult::AttachmentData> &att) -> OmegaGTE::FVec<4> {
                        if (!att) return fallbackColor;
                        const auto &c = att->color;
                        if (c[0][0] == 0.f && c[1][0] == 0.f && c[2][0] == 0.f && c[3][0] == 0.f)
                            return fallbackColor;
                        return c;
                    };
                    OmegaGTE::FVec<4> aColor = useColor(v.a.attachment);
                    OmegaGTE::FVec<4> bColor = useColor(v.b.attachment);
                    OmegaGTE::FVec<4> cColor = useColor(v.c.attachment);
                    writeColorVertexToBuffer(v.a.pt, aColor);
                    writeColorVertexToBuffer(v.b.pt, bColor);
                    writeColorVertexToBuffer(v.c.pt, cColor);
                }
            }
        }

        // Flush vertex data before draw calls
        bufferWriter->flush();

        // Bind pipeline and resources. The frame render pass suppresses
        // redundant pipeline binds within an open frame and forces a rebind
        // for standalone scopes.
        if(useTextureRenderPipeline){
            frameRenderPass_.bindTexturePipeline(scope);
            cb->bindResourceAtVertexShader(buffer,1);
            cb->bindResourceAtFragmentShader(texturePaint,2);
        }
        else {
            frameRenderPass_.bindColorPipeline(scope);
            cb->bindResourceAtVertexShader(buffer,0);
        }

        for(auto & m : result.meshes){
            OmegaGTE::GERenderTarget::CommandBuffer::PolygonType topology;
            if(m.topology == OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangleStrip){
                topology = OmegaGTE::GERenderTarget::CommandBuffer::TriangleStrip;
            }
            else {
                topology = OmegaGTE::GERenderTarget::CommandBuffer::Triangle;
            }
            cb->drawPolygons(topology, m.vertexCount(), startVertexIndex);
            startVertexIndex += m.vertexCount();
        }

        frameRenderPass_.endDraw(scope);

        if(bufferPool() && buffer){
            deferredBufferReleases.push_back({std::move(buffer), requiredBytes});
        }
    }

    void RenderTargetStore::cleanTargets(LayerTree *tree){
        if(tree == nullptr)
            return;
        OmegaCommon::Vector<Layer *> liveLayers {};
        tree->collectAllLayers(liveLayers);

        for(auto & storeEntry : store){
            auto & compTarget = storeEntry.second;
            auto surfIt = compTarget.surfaceTargets.begin();
            while(surfIt != compTarget.surfaceTargets.end()){
                bool isLive = false;
                for(auto *liveLayer : liveLayers){
                    if(liveLayer == surfIt->first){
                        isLive = true;
                        break;
                    }
                }
                if(!isLive){
                    surfIt = compTarget.surfaceTargets.erase(surfIt);
                }
                else {
                    ++surfIt;
                }
            }
            // Drop per-layer blur scratches whose layers were removed.
            if(compTarget.visualTree != nullptr && compTarget.visualTree->root != nullptr){
                auto *rootCtx = compTarget.visualTree->root->renderTarget.get();
                if(rootCtx != nullptr){
                    rootCtx->purgeDeadLayerScratches(liveLayers);
                }
            }
        }
    }

    void RenderTargetStore::cleanTreeTargets(LayerTree *tree){
        if(tree == nullptr)
            return;
        cleanTargets(tree);
    }

    void RenderTargetStore::removeRenderTarget(const SharedHandle<CompositionRenderTarget> & target){
        auto it = store.find(target);
        if(it != store.end()){
            store.erase(it);
        }
    }

}
