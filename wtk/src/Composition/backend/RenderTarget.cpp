

#include "RenderTarget.h"
#include "VisualTree.h"
#include "TexturePool.h"
#include "BufferPool.h"
#include "FencePool.h"
#include "MainThreadDispatch.h"
#include "Pipeline.h"
#include "BitmapTextureCache.h"
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
    // Phase 6.8: tessellation context is now created lazily on first
    // VectorPath / gradient-fallback draw (Phase 6.6 retired the
    // bitmap-tessellation round-trip). A frame that only touches SDF
    // primitives or bitmaps never allocates one. On every rebuild we
    // invalidate the existing context so the next consumer rebinds
    // against the new backing dimensions.
    tessellationContext_.reset();
}

bool BackendRenderTargetContext::ensureTessellationContext(){
    if(tessellationContext_ != nullptr){
        return true;
    }
    if(renderTarget == nullptr){
        return false;
    }
    tessellationContext_ = gte.triangulationEngine->createTEContextFromNativeRenderTarget(renderTarget);
    if(tessellationContext_ == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "Failed to create tessellation context for native render target." << std::endl;
#endif
        return false;
    }
    return true;
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

    void BackendRenderTargetContext::emitSdfPrimitive(
            float cx, float cy,
            float halfW, float halfH,
            float cornerRadius,
            float widthOrBlur,
            float kindCode,
            OmegaGTE::FVec<4> fillColor,
            OmegaGTE::FVec<4> strokeColor){
        auto & pipelines = pipelineRegistry();
        auto bufferWriter = pipelines.bufferWriter();
        auto sdfPipeline  = pipelines.sdf();
        if(bufferWriter == nullptr || sdfPipeline == nullptr || renderTarget == nullptr){
            return;
        }
        if(!std::isfinite(cx) || !std::isfinite(cy) ||
           !std::isfinite(halfW) || !std::isfinite(halfH) ||
           halfW <= 0.f || halfH <= 0.f){
            return;
        }

        const float viewportW = std::max(1.f, renderTargetSize_.w);
        const float viewportH = std::max(1.f, renderTargetSize_.h);

        // Quad covers the silhouette plus padding for AA + (half stroke
        // width | blur). 2 logical pixels of AA margin is enough at any
        // render scale because `fwidth(dist)` scales inversely with
        // backing resolution.
        const float pad = (kindCode >= 2.5f)
                ? std::max(2.f, std::max(0.f, widthOrBlur) + 2.f)
                : std::max(2.f, std::max(0.f, widthOrBlur) * 0.5f + 2.f);

        const float lwHalf = halfW + pad;
        const float lhHalf = halfH + pad;
        const float minX = cx - lwHalf;
        const float minY = cy - lhHalf;
        const float maxX = cx + lwHalf;
        const float maxY = cy + lhHalf;

        // Vertex buffer: 6 vertices × (float4 pos, float4 local).
        const std::size_t vertexStride = OmegaGTE::omegaSLStructStride(
                {OMEGASL_FLOAT4, OMEGASL_FLOAT4});
        const std::size_t vertexBytes  = vertexStride * 6;
        SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;
        if(bufferPool() != nullptr){
            vertexBuffer = bufferPool()->acquire(vertexBytes, vertexStride);
        }
        else {
            OmegaGTE::BufferDescriptor desc {
                    OmegaGTE::BufferDescriptor::Upload,
                    vertexBytes,
                    vertexStride};
            vertexBuffer = gte.graphicsEngine->makeBuffer(desc);
        }
        if(vertexBuffer == nullptr){
            return;
        }

        // Per-draw uniform buffer: shapeParams, fillColor, strokeColor,
        // kindOpacity (16 bytes each). One acquisition per primitive;
        // released to the pool after the frame completes via
        // `deferredBufferReleases`.
        const std::size_t paramsStride = OmegaGTE::omegaSLStructStride(
                {OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4});
        SharedHandle<OmegaGTE::GEBuffer> paramsBuffer;
        if(bufferPool() != nullptr){
            paramsBuffer = bufferPool()->acquire(paramsStride, paramsStride);
        }
        else {
            OmegaGTE::BufferDescriptor desc {
                    OmegaGTE::BufferDescriptor::Upload,
                    paramsStride,
                    paramsStride};
            paramsBuffer = gte.graphicsEngine->makeBuffer(desc);
        }
        if(paramsBuffer == nullptr){
            if(bufferPool() != nullptr && vertexBuffer){
                bufferPool()->release(std::move(vertexBuffer), vertexBytes);
            }
            return;
        }

        const bool hasTransform = !(currentTransform == OmegaGTE::FMatrix<4,4>::Identity());

        bufferWriter->setOutputBuffer(vertexBuffer);
        auto writeVertex = [&](float x, float y, float lx, float ly){
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = (2.f * x) / viewportW - 1.f;
            pos[1][0] = (2.f * y) / viewportH - 1.f;
            pos[2][0] = 0.f;
            pos[3][0] = 1.f;
            if(hasTransform){
                pos = currentTransform * pos;
            }
            auto local = OmegaGTE::FVec<4>::Create();
            local[0][0] = lx;
            local[1][0] = ly;
            local[2][0] = 0.f;
            local[3][0] = 0.f;
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat4(local);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        };

        // Triangle 1: (minX,minY), (maxX,minY), (minX,maxY)
        writeVertex(minX, minY, -lwHalf, -lhHalf);
        writeVertex(maxX, minY,  lwHalf, -lhHalf);
        writeVertex(minX, maxY, -lwHalf,  lhHalf);
        // Triangle 2: (maxX,minY), (maxX,maxY), (minX,maxY)
        writeVertex(maxX, minY,  lwHalf, -lhHalf);
        writeVertex(maxX, maxY,  lwHalf,  lhHalf);
        writeVertex(minX, maxY, -lwHalf,  lhHalf);
        bufferWriter->flush();

        bufferWriter->setOutputBuffer(paramsBuffer);
        auto shapeParams = OmegaGTE::FVec<4>::Create();
        shapeParams[0][0] = halfW;
        shapeParams[1][0] = halfH;
        shapeParams[2][0] = std::max(0.f, cornerRadius);
        shapeParams[3][0] = std::max(0.f, widthOrBlur);
        auto kindOpacity = OmegaGTE::FVec<4>::Create();
        kindOpacity[0][0] = kindCode;
        kindOpacity[1][0] = std::clamp(currentOpacity, 0.f, 1.f);
        kindOpacity[2][0] = 0.f;
        kindOpacity[3][0] = 0.f;
        bufferWriter->structBegin();
        bufferWriter->writeFloat4(shapeParams);
        bufferWriter->writeFloat4(fillColor);
        bufferWriter->writeFloat4(strokeColor);
        bufferWriter->writeFloat4(kindOpacity);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();
        bufferWriter->flush();

        SharedHandle<OmegaGTE::GEFence> noFence;
        auto scope = frameRenderPass_.beginDraw(noFence);
        if(scope.cb == nullptr){
            if(bufferPool() != nullptr){
                if(vertexBuffer){
                    bufferPool()->release(std::move(vertexBuffer), vertexBytes);
                }
                if(paramsBuffer){
                    bufferPool()->release(std::move(paramsBuffer), paramsStride);
                }
            }
            return;
        }
        auto & cb = scope.cb;

        frameRenderPass_.bindSdfPipeline(scope);
        cb->bindResourceAtVertexShader(vertexBuffer, 6);
        cb->bindResourceAtFragmentShader(paramsBuffer, 7);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);
        frameRenderPass_.endDraw(scope);

        if(bufferPool() != nullptr){
            if(vertexBuffer){
                deferredBufferReleases.push_back({std::move(vertexBuffer), vertexBytes});
            }
            if(paramsBuffer){
                deferredBufferReleases.push_back({std::move(paramsBuffer), paramsStride});
            }
        }
    }

    void BackendRenderTargetContext::emitBitmapPrimitive(
            const Composition::Rect & destRect,
            float uMin, float vMin,
            float uMax, float vMax,
            OmegaGTE::FVec<4> tint,
            SharedHandle<OmegaGTE::GETexture> texture,
            SharedHandle<OmegaGTE::GEFence> textureFence){
        auto & pipelines = pipelineRegistry();
        auto bufferWriter = pipelines.bufferWriter();
        auto bitmapPipeline = pipelines.bitmap();
        if(bufferWriter == nullptr || bitmapPipeline == nullptr || renderTarget == nullptr || texture == nullptr){
            return;
        }
        if(!std::isfinite(destRect.pos.x) || !std::isfinite(destRect.pos.y) ||
           !std::isfinite(destRect.w) || !std::isfinite(destRect.h) ||
           destRect.w <= 0.f || destRect.h <= 0.f){
            return;
        }

        const float viewportW = std::max(1.f, renderTargetSize_.w);
        const float viewportH = std::max(1.f, renderTargetSize_.h);

        const float minX = destRect.pos.x;
        const float minY = destRect.pos.y;
        const float maxX = destRect.pos.x + destRect.w;
        const float maxY = destRect.pos.y + destRect.h;

        // Vertex buffer: 6 vertices × (float4 pos, float4 uvPad).
        const std::size_t vertexStride = OmegaGTE::omegaSLStructStride(
                {OMEGASL_FLOAT4, OMEGASL_FLOAT4});
        const std::size_t vertexBytes  = vertexStride * 6;
        SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;
        if(bufferPool() != nullptr){
            vertexBuffer = bufferPool()->acquire(vertexBytes, vertexStride);
        }
        else {
            OmegaGTE::BufferDescriptor desc {
                    OmegaGTE::BufferDescriptor::Upload,
                    vertexBytes,
                    vertexStride};
            vertexBuffer = gte.graphicsEngine->makeBuffer(desc);
        }
        if(vertexBuffer == nullptr){
            return;
        }

        // Per-draw uniform buffer: tintColor (16 bytes). Released after
        // frame completes via deferredBufferReleases.
        const std::size_t paramsStride = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
        SharedHandle<OmegaGTE::GEBuffer> paramsBuffer;
        if(bufferPool() != nullptr){
            paramsBuffer = bufferPool()->acquire(paramsStride, paramsStride);
        }
        else {
            OmegaGTE::BufferDescriptor desc {
                    OmegaGTE::BufferDescriptor::Upload,
                    paramsStride,
                    paramsStride};
            paramsBuffer = gte.graphicsEngine->makeBuffer(desc);
        }
        if(paramsBuffer == nullptr){
            if(bufferPool() != nullptr && vertexBuffer){
                bufferPool()->release(std::move(vertexBuffer), vertexBytes);
            }
            return;
        }

        const bool hasTransform = !(currentTransform == OmegaGTE::FMatrix<4,4>::Identity());
        const float opacityMul = std::clamp(currentOpacity, 0.f, 1.f);

        bufferWriter->setOutputBuffer(vertexBuffer);
        auto writeVertex = [&](float x, float y, float u, float v){
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = (2.f * x) / viewportW - 1.f;
            pos[1][0] = (2.f * y) / viewportH - 1.f;
            pos[2][0] = 0.f;
            pos[3][0] = 1.f;
            if(hasTransform){
                pos = currentTransform * pos;
            }
            auto uvPad = OmegaGTE::FVec<4>::Create();
            uvPad[0][0] = u;
            uvPad[1][0] = v;
            uvPad[2][0] = 0.f;
            uvPad[3][0] = 0.f;
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat4(uvPad);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        };

        // Triangle 1: TL, TR, BL — TL=(minX,minY,uMin,vMin),
        // TR=(maxX,minY,uMax,vMin), BL=(minX,maxY,uMin,vMax).
        writeVertex(minX, minY, uMin, vMin);
        writeVertex(maxX, minY, uMax, vMin);
        writeVertex(minX, maxY, uMin, vMax);
        // Triangle 2: TR, BR, BL.
        writeVertex(maxX, minY, uMax, vMin);
        writeVertex(maxX, maxY, uMax, vMax);
        writeVertex(minX, maxY, uMin, vMax);
        bufferWriter->flush();

        bufferWriter->setOutputBuffer(paramsBuffer);
        auto tintWithOpacity = tint;
        tintWithOpacity[3][0] *= opacityMul;
        bufferWriter->structBegin();
        bufferWriter->writeFloat4(tintWithOpacity);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();
        bufferWriter->flush();

        auto scope = frameRenderPass_.beginDraw(textureFence);
        if(scope.cb == nullptr){
            if(bufferPool() != nullptr){
                if(vertexBuffer){
                    bufferPool()->release(std::move(vertexBuffer), vertexBytes);
                }
                if(paramsBuffer){
                    bufferPool()->release(std::move(paramsBuffer), paramsStride);
                }
            }
            return;
        }
        auto & cb = scope.cb;

        frameRenderPass_.bindBitmapPipeline(scope);
        cb->bindResourceAtVertexShader(vertexBuffer, 9);
        cb->bindResourceAtFragmentShader(paramsBuffer, 10);
        cb->bindResourceAtFragmentShader(texture, 11);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);
        frameRenderPass_.endDraw(scope);

        if(bufferPool() != nullptr){
            if(vertexBuffer){
                deferredBufferReleases.push_back({std::move(vertexBuffer), vertexBytes});
            }
            if(paramsBuffer){
                deferredBufferReleases.push_back({std::move(paramsBuffer), paramsStride});
            }
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
        auto pathRenderPipelineState = pipelines.path();
        // Phase 6.8: tessellationContext_ is no longer required at this
        // gate. SDF primitives (Rect/RoundedRect/Ellipse/Shadow with
        // color brush), bitmaps (Phase 6.6 — dedicated pipeline with
        // a hardcoded quad), and per-element state ops (SetTransform /
        // SetOpacity) don't touch the triangulator. Cases that do
        // (gradient fills, VectorPath) lazily acquire it via
        // `ensureTessellationContext()`.
        if(bufferWriter == nullptr || renderTarget == nullptr){
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
        bool usePathRenderPipeline = false;
        float textureCoordDenomW = 1.f;
        float textureCoordDenomH = 1.f;
        // Per-vertex attachment tagging for the path pipeline (Phase 6.4).
        // The triangulator emits stroke triangles using attachments[0]'s
        // color and fill triangles using attachments[1]'s color, so the
        // tag is recovered by exact-color match against these two values
        // in the vertex authoring lambda below.
        OmegaGTE::FVec<4> pathStrokeColor = OmegaGTE::FVec<4>::Create();
        OmegaGTE::FVec<4> pathFillColor   = OmegaGTE::FVec<4>::Create();
        bool pathHasStrokeColor = false;
        bool pathHasFillColor   = false;

        SharedHandle<OmegaGTE::GETexture> texturePaint;

        SharedHandle<OmegaGTE::GEFence> textureFence;

        switch (type) {
            case VisualCommand::Rect : {
                auto & _params = ((VisualCommandParams*)params)->rectParams;
                if (_params.brush == nullptr) return;
                if (_params.brush->type == Brush::Type::None) return;

                // Phase 6.3: color brushes go through the SDF pipeline
                // along with their (optional) color border. Gradient
                // brushes keep the existing tessellation+texture path.
                if (_params.brush->type == Brush::Type::Color) {
                    const float halfW = std::max(0.f, _params.rect.w) * 0.5f;
                    const float halfH = std::max(0.f, _params.rect.h) * 0.5f;
                    if (halfW <= 0.f || halfH <= 0.f) return;
                    const float cx = _params.rect.pos.x + halfW;
                    const float cy = _params.rect.pos.y + halfH;

                    auto fillColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                         _params.brush->color.g,
                                                         _params.brush->color.b,
                                                         _params.brush->color.a);
                    auto strokeColor = OmegaGTE::FVec<4>::Create();
                    float strokeW = 0.f;
                    if (_params.border.has_value() &&
                        _params.border->brush != nullptr &&
                        _params.border->brush->type == Brush::Type::Color) {
                        strokeColor = OmegaGTE::makeColor(_params.border->brush->color.r,
                                                          _params.border->brush->color.g,
                                                          _params.border->brush->color.b,
                                                          _params.border->brush->color.a);
                        strokeW = static_cast<float>(_params.border->width);
                    }
                    emitSdfPrimitive(cx, cy, halfW, halfH,
                                     0.f, strokeW, 0.f,
                                     fillColor, strokeColor);
                    return;
                }

                // Gradient brush — fall through to existing tessellation
                // + texture pipeline path.
                if (!ensureTessellationContext()) return;
                auto gteRect = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::Rect(gteRect);
                useTextureRenderPipeline = true;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
                result = tessellationContext_->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::Bitmap : {
                // Phase 6.6: bitmap drawing dispatches through the dedicated
                // bitmap pipeline — hardcoded 6-vertex quad authored inline,
                // no triangulator round-trip, mip-chain texture sourced from
                // the process-wide BitmapTextureCache, optional sub-rect UV
                // and RGBA tint via per-draw uniform buffer.
                auto & _params = ((VisualCommandParams*)params)->bitmapParams;

                SharedHandle<OmegaGTE::GETexture> tex;
                SharedHandle<OmegaGTE::GEFence> fence;
                unsigned texW = 1;
                unsigned texH = 1;

                if(_params.texture){
                    tex = _params.texture;
                    fence = _params.textureFence;
                    // We don't track per-pixel dimensions on a foreign
                    // GETexture; sub-rect sampling against an externally
                    // supplied texture is unsupported (sourceRect would
                    // need pixel dimensions to normalize). Callers
                    // pre-compute their own UV ranges if they want
                    // sub-rect sampling on a GETexture.
                    texW = 1;
                    texH = 1;
                }
                else if(_params.img != nullptr){
                    tex = BitmapTextureCache::instance().acquire(_params.img);
                    if(tex == nullptr){
                        return;
                    }
                    texW = std::max(1u, _params.img->header.width);
                    texH = std::max(1u, _params.img->header.height);
                }
                else {
                    return;
                }

                // Resolve UV range. When no source rect is supplied, sample
                // the full texture (UV 0..1). Otherwise normalize the
                // pixel-space sourceRect against the bitmap dimensions.
                float uMin = 0.f, vMin = 0.f, uMax = 1.f, vMax = 1.f;
                if(_params.sourceRect.has_value() && _params.img != nullptr){
                    const auto & src = *_params.sourceRect;
                    const float fW = static_cast<float>(texW);
                    const float fH = static_cast<float>(texH);
                    uMin = std::clamp(src.pos.x / fW, 0.f, 1.f);
                    vMin = std::clamp(src.pos.y / fH, 0.f, 1.f);
                    uMax = std::clamp((src.pos.x + src.w) / fW, 0.f, 1.f);
                    vMax = std::clamp((src.pos.y + src.h) / fH, 0.f, 1.f);
                }

                // Resolve tint. Identity (1,1,1,1) is the default
                // passthrough; per-element opacity is folded in during
                // emitBitmapPrimitive so Visual::SetOpacity composes.
                auto tint = OmegaGTE::makeColor(1.f, 1.f, 1.f, 1.f);
                if(_params.tintColor.has_value()){
                    const auto & tc = *_params.tintColor;
                    tint = OmegaGTE::makeColor(tc.r, tc.g, tc.b, tc.a);
                }

                emitBitmapPrimitive(_params.rect,
                                    uMin, vMin, uMax, vMax,
                                    tint, tex, fence);
                return;
            }
            case VisualCommand::RoundedRect : {
                auto & _params = ((VisualCommandParams*)params)->roundedRectParams;
                if (_params.brush == nullptr) return;
                if (_params.brush->type == Brush::Type::None) return;

                // Phase 6.3: color brushes go through SDF; gradient
                // brushes keep the existing tessellation+texture path.
                if (_params.brush->type == Brush::Type::Color) {
                    const float halfW = std::max(0.f, _params.rect.w) * 0.5f;
                    const float halfH = std::max(0.f, _params.rect.h) * 0.5f;
                    if (halfW <= 0.f || halfH <= 0.f) return;
                    const float cx = _params.rect.pos.x + halfW;
                    const float cy = _params.rect.pos.y + halfH;
                    const float cornerR = std::max(0.f, std::min(_params.rect.rad_x,
                                                                  std::min(halfW, halfH)));

                    auto fillColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                         _params.brush->color.g,
                                                         _params.brush->color.b,
                                                         _params.brush->color.a);
                    auto strokeColor = OmegaGTE::FVec<4>::Create();
                    float strokeW = 0.f;
                    if (_params.border.has_value() &&
                        _params.border->brush != nullptr &&
                        _params.border->brush->type == Brush::Type::Color) {
                        strokeColor = OmegaGTE::makeColor(_params.border->brush->color.r,
                                                          _params.border->brush->color.g,
                                                          _params.border->brush->color.b,
                                                          _params.border->brush->color.a);
                        strokeW = static_cast<float>(_params.border->width);
                    }
                    emitSdfPrimitive(cx, cy, halfW, halfH,
                                     cornerR, strokeW, 1.f,
                                     fillColor, strokeColor);
                    return;
                }

                if (!ensureTessellationContext()) return;
                auto gteRR = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(gteRR);
                useTextureRenderPipeline = true;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
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

                // Phase 6.3: ellipse always uses the SDF pipeline.
                // The previous CPU triangle-fan path (up to 4096 tris)
                // is retired for color brushes; gradient ellipses fall
                // back to the same SDF here today (gradient sampling
                // inside the SDF shader is a follow-up).
                auto fillColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                if(_params.brush != nullptr && _params.brush->type == Brush::Type::Color){
                    fillColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                    _params.brush->color.g,
                                                    _params.brush->color.b,
                                                    _params.brush->color.a);
                }
                if(fillColor[0][0] == 0.f && fillColor[1][0] == 0.f &&
                   fillColor[2][0] == 0.f && fillColor[3][0] == 0.f){
                    fillColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                }

                auto strokeColor = OmegaGTE::FVec<4>::Create();
                float strokeW = 0.f;
                if (_params.border.has_value() &&
                    _params.border->brush != nullptr &&
                    _params.border->brush->type == Brush::Type::Color) {
                    strokeColor = OmegaGTE::makeColor(_params.border->brush->color.r,
                                                      _params.border->brush->color.g,
                                                      _params.border->brush->color.b,
                                                      _params.border->brush->color.a);
                    strokeW = static_cast<float>(_params.border->width);
                }

                emitSdfPrimitive(cx, cy, rx, ry,
                                 0.f, strokeW, 2.f,
                                 fillColor, strokeColor);
                return;
            }
            case VisualCommand::VectorPath : {
                auto & _params = ((VisualCommandParams*)params)->pathParams;
                if(_params.path == nullptr || _params.path->size() < 2){
                    return;
                }
                if (!ensureTessellationContext()) return;
                auto te_params = OmegaGTE::TETriangulationParams::GraphicsPath2D(*_params.path,
                                                                                 _params.strokeWidth,
                                                                                 _params.contour,
                                                                                 _params.fill);
                // First attachment: stroke color.
                auto strokeColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                bool hasStrokeColor = false;
                if(_params.brush != nullptr && _params.brush->type == Brush::Type::Color){
                    strokeColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                      _params.brush->color.g,
                                                      _params.brush->color.b,
                                                      _params.brush->color.a);
                    hasStrokeColor = true;
                }
                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(strokeColor));

                // Second attachment: fill color.
                auto fillColor = OmegaGTE::FVec<4>::Create();
                bool hasFillColor = false;
                if(_params.fill && _params.fillBrush != nullptr && _params.fillBrush->type == Brush::Type::Color){
                    fillColor = OmegaGTE::makeColor(_params.fillBrush->color.r,
                                                    _params.fillBrush->color.g,
                                                    _params.fillBrush->color.b,
                                                    _params.fillBrush->color.a);
                    hasFillColor = true;
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(fillColor));
                }

                result = tessellationContext_->triangulateSync(te_params,
                                                                  OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,
                                                                  &viewPort);

                // Phase 6.4: route the dual-attachment mesh through the
                // path pipeline so the fragment shader can consume the
                // per-vertex (edgeDistance, attachmentTag) varying. Falls
                // back to the flat color pipeline when the path pipeline
                // failed to compile (e.g. shader not present in the
                // library), so the visual still renders.
                if(pathRenderPipelineState != nullptr){
                    usePathRenderPipeline = true;
                    pathStrokeColor   = strokeColor;
                    pathHasStrokeColor = hasStrokeColor;
                    pathFillColor    = fillColor;
                    pathHasFillColor  = hasFillColor;
                }
                break;
            }
            case VisualCommand::Shadow: {
                auto & _params = ((VisualCommandParams*)params)->shadowParams;
                const auto & shadow = _params.shadow;

                // Phase 6.3: shadow uses the SDF pipeline with a soft
                // falloff over `[-blur, +blur]` around the silhouette of
                // the (offset) underlying shape. Replaces the prior
                // offset+expand+tessellate path with a single 6-vertex
                // quad and a closed-form distance function.
                const float halfW = std::max(0.f, _params.shapeRect.w) * 0.5f;
                const float halfH = std::max(0.f, _params.shapeRect.h) * 0.5f;
                if(halfW <= 0.f || halfH <= 0.f) return;
                const float cx = _params.shapeRect.pos.x + halfW + shadow.x_offset;
                const float cy = _params.shapeRect.pos.y + halfH + shadow.y_offset;
                const float blur = std::max(0.f, shadow.blurAmount);
                const float cornerR = std::max(0.f, std::min(_params.cornerRadius,
                                                              std::min(halfW, halfH)));

                auto shadowColor = OmegaGTE::makeColor(shadow.color.r,
                                                       shadow.color.g,
                                                       shadow.color.b,
                                                       shadow.color.a * shadow.opacity);
                auto noStroke = OmegaGTE::FVec<4>::Create();

                // Kind 3 = shadow with rect / rounded-rect base
                // (`cornerR > 0` selects rounded). Kind 4 = ellipse base.
                const float kind = _params.isEllipse ? 4.f : 3.f;
                emitSdfPrimitive(cx, cy, halfW, halfH,
                                 cornerR, blur, kind,
                                 shadowColor, noStroke);
                return;
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
        else if(usePathRenderPipeline){
            // Path pipeline (Phase 6.4) — vertex layout `(pos, color, edgeTag)`.
            struct_size = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4,OMEGASL_FLOAT4,OMEGASL_FLOAT4});
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

        auto writePathVertexToBuffer = [&](OmegaGTE::GPoint3D & pt,
                                           OmegaGTE::FVec<4> color,
                                           float edgeDist,
                                           float attachmentTag){
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = pt.x;
            pos[1][0] = pt.y;
            pos[2][0] = pt.z;
            pos[3][0] = 1.f;
            applyTransform(pos);
            if(opacityMul < 1.f){
                color[3][0] *= opacityMul;
            }
            auto edgeTag = OmegaGTE::FVec<4>::Create();
            edgeTag[0][0] = edgeDist;
            edgeTag[1][0] = attachmentTag;
            edgeTag[2][0] = 0.f;
            edgeTag[3][0] = 0.f;
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat4(color);
            bufferWriter->writeFloat4(edgeTag);
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

        // Color match for path attachment tagging. Stroke triangles wear
        // attachments[0]'s color exactly; fill triangles wear
        // attachments[1]'s. Bit-exact equality is fine because both came
        // from the same `OmegaGTE::makeColor` call we made above.
        auto colorEq = [](const OmegaGTE::FVec<4> & a, const OmegaGTE::FVec<4> & b){
            return a[0][0] == b[0][0] && a[1][0] == b[1][0]
                && a[2][0] == b[2][0] && a[3][0] == b[3][0];
        };

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
                else if(usePathRenderPipeline){
                    auto useColor = [&fallbackColor](const std::optional<OmegaGTE::TETriangulationResult::AttachmentData> &att) -> OmegaGTE::FVec<4> {
                        if (!att) return fallbackColor;
                        const auto &c = att->color;
                        if (c[0][0] == 0.f && c[1][0] == 0.f && c[2][0] == 0.f && c[3][0] == 0.f)
                            return fallbackColor;
                        return c;
                    };
                    auto tagFor = [&](const OmegaGTE::FVec<4> & c){
                        // 0.0 = stroke, 1.0 = fill. Default to stroke when
                        // neither side was set so the tag is well-defined
                        // even for the legacy "single brush" drawPath path.
                        if(pathHasFillColor && colorEq(c, pathFillColor)){
                            return 1.f;
                        }
                        if(pathHasStrokeColor && colorEq(c, pathStrokeColor)){
                            return 0.f;
                        }
                        return pathHasStrokeColor ? 0.f : 1.f;
                    };
                    // Phase 6.4 placeholder: every triangulator-emitted
                    // vertex sits exactly on a silhouette of the band or
                    // path outline. With no skirt geometry the linear
                    // interpolation across the triangle stays at +1 and
                    // the fragment shader resolves to full coverage —
                    // matching the prior flat-color pipeline output.
                    constexpr float kInteriorEdgeDist = 1.f;
                    OmegaGTE::FVec<4> aColor = useColor(v.a.attachment);
                    OmegaGTE::FVec<4> bColor = useColor(v.b.attachment);
                    OmegaGTE::FVec<4> cColor = useColor(v.c.attachment);
                    writePathVertexToBuffer(v.a.pt, aColor, kInteriorEdgeDist, tagFor(aColor));
                    writePathVertexToBuffer(v.b.pt, bColor, kInteriorEdgeDist, tagFor(bColor));
                    writePathVertexToBuffer(v.c.pt, cColor, kInteriorEdgeDist, tagFor(cColor));
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
        else if(usePathRenderPipeline){
            frameRenderPass_.bindPathPipeline(scope);
            cb->bindResourceAtVertexShader(buffer,8);
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
