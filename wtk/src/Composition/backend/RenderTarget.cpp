

#include "RenderTarget.h"
#include "TexturePool.h"
#include "BufferPool.h"
#include "FencePool.h"
#include "MainThreadDispatch.h"
#include "Pipeline.h"
#include "BitmapTextureCache.h"
#include "GlyphAtlas.h"
#include "ResourceFactory.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "GeometryConvert.h"
#include "ResourceTrace.h"
#include "ContentCache.h"
#include "TessellationCache.h"
#include "ViewContentCache.h"
#include "../TextShapingCache.h"   // G.4: process-wide cache stats snapshot

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
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

        // OMEGAWTK_TRACE_TEXT=1 turns on the text-render-path trace
        // lines. `emitTextSubRun` has many silent early-exits (null
        // font, empty subrun, pipeline missing, every-glyph-lookup-miss,
        // null atlas texture, null buffers, null CB); without this trace
        // a stalled text path is invisible — the screen just shows
        // nothing. Same env var the font engines use.
        bool textTraceEnabled() {
            static const bool enabled = []() {
                auto e = OmegaCommon::getEnvVar("OMEGAWTK_TRACE_TEXT");
                return e.has_value() && !e->empty() && (*e)[0] != '0';
            }();
            return enabled;
        }

        // Sizing math, collapsed in from BackingTextureSet in Phase 4.2.
        // The backing texture is always the swap chain drawable on the
        // always-direct path; these helpers only sanitize / scale the
        // logical rect and clamp to engine limits.
        constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
        constexpr float kRenderScaleFloor = 1.f;
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

    // Phase G.1: tessellation-cache PIMPL. Defined here (not in
    // `RenderTarget.h`) so the `ContentCache<…, TETriangulationResult>`
    // instantiation only needs `<omegaGTE/TE.h>`'s full definition inside
    // this translation unit — RTC consumers stay free of the GTE math
    // surface. The byte limit is left at 0 (entry count is the only cap,
    // per the plan's "LRU cap: 1024 entries" decision); a real byte cap is
    // unnecessary because the cached value is a vector of `Polygon`
    // structs whose total weight is bounded by the entry count plus the
    // typical mesh size.
    // Phase G.5.1b: the cached value grows from the bare mesh to the mesh
    // plus a persistent GPU vertex buffer and the per-draw state baked into
    // it. `drawTriangulatedResult` binds `vertexBuf` directly (skipping the
    // per-vertex re-encode) when a hit's `(transform, opacity, size,
    // pipeline)` still match `baked*`; otherwise it encodes into a fresh
    // pooled buffer and replaces the held one. The buffer is never
    // overwritten in place — reuse is read-only across frames, so no
    // in-flight GPU read is ever raced.
    struct TessellationCacheEntry {
        OmegaGTE::TETriangulationResult  mesh;
        SharedHandle<OmegaGTE::GEBuffer> vertexBuf;     // null until first encode
        OmegaGTE::FMatrix<4,4> bakedTransform = OmegaGTE::FMatrix<4,4>::Identity();
        float       bakedOpacity = 1.f;
        std::size_t bakedBytes   = 0;       // encoded byte count = vertexCount * struct_size
        bool        bakedPath    = false;   // which vertex layout (path vs color) was baked
        bool        hasBaked     = false;
    };

    struct TessellationCacheState {
        ContentCache<TessellationCacheKey, TessellationCacheEntry> cache;
        TessellationCacheState()
            : cache(ContentCacheConfig::inst().tessellationCacheEntries,
                    /*byteLimit*/0) {}
    };

    // Phase G.3.0: per-View content-cache PIMPL. Defined here for the
    // same reason as `TessellationCacheState`: the `ContentCache<…,
    // ViewCacheEntry>` instantiation references `SharedHandle<GETexture>`
    // and we keep the GTE surface scoped to this TU. Entry limit is left
    // unbounded (0); the byte limit is the plan-locked
    // `ContentCacheConfig::inst().contentCacheBytes` (default 64 MB,
    // overridable via `OMEGAWTK_CONTENT_CACHE_BYTES`) since the entries
    // hold GPU textures whose CPU-side struct cost is trivial but whose
    // GPU footprint is what matters. G.3.0 ships the slot only — no
    // `OnEvict` callback yet; G.5's persistent-handle work attaches one
    // when the texture-pool round-trip should happen on eviction.
    struct ContentCacheState {
        ContentCache<ViewCacheKey, ViewCacheEntry> cache;
        ContentCacheState()
            : cache(/*entryLimit*/0,
                    ContentCacheConfig::inst().contentCacheBytes) {}
    };

BackendRenderTargetContext::BackendRenderTargetContext(Composition::Rect & rect,
        SharedHandle<OmegaGTE::GENativeRenderTarget> &renderTargetIn,
        SharedHandle<OmegaGTE::GECommandQueue> commandQueueIn,
        float renderScaleValue):
        fence(fencePool() != nullptr ? fencePool()->acquire() : gte.graphicsEngine->makeFence()),
        commandQueue_(std::move(commandQueueIn)),
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

    // Phase G.1: allocate the tessellation cache PIMPL eagerly. Allocating
    // unconditionally (rather than gating on `OMEGAWTK_CONTENT_CACHE_ENABLED`)
    // keeps the destructor's invocation of `~TessellationCacheState` valid
    // across both build modes; the macro only gates whether the hot path
    // inside `renderVectorPathSegmented` consults the cache.
    tessellationCacheState_ = std::make_unique<TessellationCacheState>();
    // Phase G.5.1b: when an entry leaves the cache (LRU eviction or same-key
    // replacement) hand its persistent vertex buffer to the completion-gated
    // recycler rather than freeing it inline — the evicted entry could (in a
    // pathological >cap-paths-in-one-frame case) still be read by an
    // in-flight frame, and the gate releases it to `BufferPool` only after
    // the GPU is done. Safe destruction order: `deferredBufferReleases` is
    // declared before `tessellationCacheState_`, so it outlives the cache's
    // teardown clear() that fires this callback.
    tessellationCacheState_->cache.setOnEvict(
            [this](const TessellationCacheKey &, TessellationCacheEntry & e){
                if(e.vertexBuf != nullptr){
                    deferredBufferReleases.push_back({std::move(e.vertexBuf), e.bakedBytes});
                }
            });

    // Phase G.3.0: allocate the content cache PIMPL eagerly, same
    // policy as the tessellation cache. G.3.0 only lands the slot; the
    // FrameBuilder integration in G.3.1 + G.3.2 will exercise it.
    contentCacheState_ = std::make_unique<ContentCacheState>();
    // Phase G.5.1b follow-up: when a content-cache entry leaves the cache
    // (LRU eviction or same-key replacement) hand its persistent blit-quad
    // vertex buffer to the completion-gated recycler — same rationale and
    // same safe destruction order as the tessellation `OnEvict` above
    // (`deferredBufferReleases` is declared before `contentCacheState_`, so
    // it outlives the cache's teardown clear() that fires this callback).
    // The entry's `texture` handle is left to drop its reference as today;
    // the texture-pool round-trip on eviction is G.5.2's job and will extend
    // this same callback.
    contentCacheState_->cache.setOnEvict(
            [this](const ViewCacheKey &, ViewCacheEntry & e){
                if(e.blitQuadBuf != nullptr){
                    deferredBufferReleases.push_back({std::move(e.blitQuadBuf), e.blitQuadBytes});
                }
            });

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
    // Phase G.5.1: return the completion-gated batches too. Release to the
    // pool (which outlives this per-window context) rather than letting the
    // handles free here — on backends where the GPU completion never fired
    // the buffers may still be in flight, and the pool keeps them alive
    // (same keep-alive contract as the deferred list above).
    for(auto & batch : pendingReleaseBatches_){
        for(auto & entry : batch.buffers){
            if(bufferPool() != nullptr && entry.first){
                bufferPool()->release(std::move(entry.first), entry.second);
            }
        }
    }
    pendingReleaseBatches_.clear();
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
            // Defer the native swap-chain resize to the worker thread's
            // `beginFrame`. The actual resize releases backing resources
            // (DXGI ID3D12Resources / VkImage handles) that the worker is
            // mid-recording against; the worker is sole owner of frame
            // lifecycle so it can safely device-idle / commitToGPUAndWait
            // before tearing them down. On Metal, `GENativeRenderTarget::
            // resizeSwapChain` is a no-op default (CAMetalLayer auto-resizes
            // off the layer bounds), so the drain runs but does nothing.
            pendingSwapChainW_.store(newW, std::memory_order_relaxed);
            pendingSwapChainH_.store(newH, std::memory_order_relaxed);
            pendingSwapChainResize_.store(true, std::memory_order_release);
        }
    }

    void BackendRenderTargetContext::applyPendingSwapChainResize(){
        // Acquire-load pairs with the release-store in setRenderTargetSize
        // so the new W/H reads below see the values the GUI thread wrote
        // before flipping the flag. `exchange` clears the pending bit
        // so a concurrent setRenderTargetSize that fires *after* this
        // load can re-arm a fresh request for the next frame.
        if(!pendingSwapChainResize_.exchange(false, std::memory_order_acq_rel)){
            return;
        }
        const unsigned w = pendingSwapChainW_.load(std::memory_order_relaxed);
        const unsigned h = pendingSwapChainH_.load(std::memory_order_relaxed);
        resizeSwapChain(w, h);
    }

    void BackendRenderTargetContext::applySetClip(const Core::Optional<Composition::Rect> & clipRect){
        // Tier 3 Phase 3.5: translate the resolved canvas-local clip
        // rect into target pixel coordinates and apply via the GTE
        // scissor. Empty Optional restores the slice's natural
        // scissor (the viewport override, or the full backing when
        // no override is active). Issued as a tiny draw scope so the
        // call lands on the active frame / scratch command buffer.
        SharedHandle<OmegaGTE::GEFence> noFence;
        auto scope = frameRenderPass_.beginDraw(noFence);
        if(scope.cb == nullptr){
            return;
        }

        const float scale = renderScale_;
        const auto & vp = frameRenderPass_.viewportOverride();

        float sliceX, sliceY, sliceW, sliceH;
        if(vp.active){
            sliceX = vp.offsetX * scale;
            sliceY = vp.offsetY * scale;
            sliceW = vp.width   * scale;
            sliceH = vp.height  * scale;
        } else {
            sliceX = 0.f;
            sliceY = 0.f;
            sliceW = static_cast<float>(backingWidth_);
            sliceH = static_cast<float>(backingHeight_);
        }

        OmegaGTE::GEScissorRect sr {};
        if(clipRect.has_value()){
            // Clip is canvas-local; canvas origin maps to (vp.offsetX,
            // vp.offsetY) on the window — add the slice's window
            // offset to lift the clip into window-pixel space.
            const float clipX = (clipRect->pos.x + vp.offsetX) * scale;
            const float clipY = (clipRect->pos.y + vp.offsetY) * scale;
            const float clipW = clipRect->w * scale;
            const float clipH = clipRect->h * scale;

            // Intersect with the slice's natural scissor so a clip
            // overflowing the slice cannot enlarge what gets drawn.
            const float ix = std::max(sliceX, clipX);
            const float iy = std::max(sliceY, clipY);
            const float irRight  = std::min(sliceX + sliceW, clipX + clipW);
            const float irBottom = std::min(sliceY + sliceH, clipY + clipH);
            if(irRight <= ix || irBottom <= iy){
                // Empty intersection — degenerate scissor culls all
                // subsequent draws, which is the correct visual.
                sr = {ix, iy, 0.f, 0.f};
            } else {
                sr = {ix, iy, irRight - ix, irBottom - iy};
            }
        } else {
            sr = {sliceX, sliceY, sliceW, sliceH};
        }
        scope.cb->setScissorRects({sr});
        frameRenderPass_.endDraw(scope);
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

void BackendRenderTargetContext::resizeSwapChain(unsigned int newBackingWidth, unsigned int newBackingHeight) {
    if (renderTarget != nullptr)
        renderTarget->resizeSwapChain(newBackingWidth, newBackingHeight);
}
#ifdef _WIN32
void BackendRenderTargetContext::waitForGPU() {
    if (renderTarget != nullptr)
        renderTarget->waitForGPU();
}
#endif

void BackendRenderTargetContext::beginFrame(float clearR, float clearG, float clearB, float clearA) {
    // Drain any pending swap-chain resize request queued by
    // `setRenderTargetSize` on the GUI thread. We are on the compositor
    // worker thread here, sole owner of this context's frame lifecycle,
    // and the previous frame's submissions are either retired or about
    // to be drained by `resizeSwapChain`'s internal device-idle /
    // commitToGPUAndWait — so releasing the old back-buffers / image
    // views cannot race with an in-recording command list / buffer.
    // Cross-platform now (was Windows-only until the Vulkan
    // GEVulkanNativeRenderTarget::resizeSwapChain path landed); the
    // Metal default override is a no-op since CAMetalLayer auto-resizes.
    applyPendingSwapChainResize();
    // Phase G.5.1: return buffers whose owning frame has completed on the
    // GPU back to the pool, so this frame's `acquire`s hit instead of
    // allocating. Drained here (compositor thread) — never from the GPU
    // completion callback — to keep all `BufferPool` access single-threaded.
    drainCompletedBufferReleases();
    frameRenderPass_.begin(clearR, clearG, clearB, clearA);
    // §2.14 Pass 1 retired `pendingNativeContent_` — see the
    // header's comment at the former
    // `BackendNativeContentRegion` site.
}

void BackendRenderTargetContext::endFrame() {
    frameRenderPass_.end();
#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
    // Phase G.4: one presented frame closed. Drive the periodic
    // content-cache telemetry (no-op unless OMEGAWTK_CONTENT_CACHE_STATS
    // is set). `endFrame` is the per-window, once-per-frame boundary the
    // Compositor calls after recording every slice (Compositor.cpp).
    ++frameCounter_;
    reportContentCacheStats();
#endif
}

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
void BackendRenderTargetContext::reportContentCacheStats() {
    if(!ContentCacheConfig::inst().cacheStatsEnabled){
        return;
    }
    // Every 60th presented frame. `frameCounter_` is pre-incremented in
    // endFrame, so the first report lands on frame 60.
    if(frameCounter_ == 0 || (frameCounter_ % 60) != 0){
        return;
    }

    auto hitRate = [](const ContentCacheStats & s) -> double {
        const std::uint64_t total = s.hits + s.misses;
        if(total == 0){
            return 0.0;
        }
        return 100.0 * static_cast<double>(s.hits) / static_cast<double>(total);
    };

    // Per-RTC caches (tessellation G.1, per-View content G.3) live on
    // this context; the text-shaping cache (G.2) is process-wide, copied
    // out under its own lock via snapshot(). Counters are cumulative
    // since process start (the cache never calls resetCounters in
    // production), so hit-rate is the lifetime average, not a per-window
    // rate.
    const ContentCacheStats & tess    = tessellationCacheState_->cache.stats();
    const ContentCacheStats & content = contentCacheState_->cache.stats();
    const ContentCacheStats   text    = TextShapingCache::inst().snapshot().stats;

    auto line = [&](std::ostream & os, const char * name, const ContentCacheStats & s){
        os << "  " << name
           << " hits=" << s.hits
           << " miss=" << s.misses
           << " evict=" << s.evictions
           << " entries=" << s.entries
           << " bytes=" << s.currentBytes
           << " peakBytes=" << s.peakBytes
           << " hit=" << std::fixed << std::setprecision(1) << hitRate(s) << "%\n";
    };

    std::ostringstream os;
    os << "[ContentCacheStats] rtc=" << traceResourceId
       << " frame=" << frameCounter_ << "\n";
    line(os, "tessellation", tess);
    line(os, "content     ", content);
    line(os, "textShaping ", text);
    // Phase G.5.1: the per-RTC buffer pool. Its hit rate is the direct
    // signal that the completion-gated recycling is working — before the
    // fix it sat near 0% (every draw allocated, nothing was ever
    // returned); after warmup it should climb as freed buffers come back
    // for reuse. `pending` counts batches still waiting on GPU completion.
    if(auto * pool = bufferPool()){
        const BufferPool::Stats ps = pool->stats();
        const std::uint64_t total = ps.poolHits + ps.poolMisses;
        const double rate = total == 0 ? 0.0
                : 100.0 * static_cast<double>(ps.poolHits) / static_cast<double>(total);
        os << "  bufferPool   hits=" << ps.poolHits
           << " miss=" << ps.poolMisses
           << " pooledBytes=" << ps.totalPooledBytes
           << " pending=" << pendingReleaseBatches_.size()
           << " hit=" << std::fixed << std::setprecision(1) << rate << "%\n";
    }
    // Phase G.5.1b: persistent tessellation-buffer reuse. `reuse` = draws that
    // bound a cache entry's held vertex buffer and skipped the per-vertex
    // re-encode; `reencode` = draws that had to (re)encode into a fresh
    // buffer (first paint, or transform/opacity/size changed). A high reuse
    // ratio is the win — static cached shapes stop re-encoding every frame.
    {
        const std::uint64_t tessTotal = tessReuseHits_ + tessReencodes_;
        const double reuseRate = tessTotal == 0 ? 0.0
                : 100.0 * static_cast<double>(tessReuseHits_) / static_cast<double>(tessTotal);
        os << "  tessBuffer   reuse=" << tessReuseHits_
           << " reencode=" << tessReencodes_
           << " reuseRate=" << std::fixed << std::setprecision(1) << reuseRate << "%\n";
    }
    // Phase G.5.1b follow-up: persistent content-cache blit-quad reuse.
    // `reuse` = cache-hit (or capture-end) blits that re-bound the entry's
    // held fullscreen-quad and skipped the six-vertex re-encode; `reencode`
    // = blits that had to (re)encode into a fresh quad (first capture, or
    // the dest/viewport/transform changed). A high reuse ratio is the win —
    // a static cached View's repeated blits stop re-encoding their quad.
    {
        const std::uint64_t blitTotal = blitQuadReuseHits_ + blitQuadReencodes_;
        const double blitReuseRate = blitTotal == 0 ? 0.0
                : 100.0 * static_cast<double>(blitQuadReuseHits_) / static_cast<double>(blitTotal);
        os << "  blitQuad     reuse=" << blitQuadReuseHits_
           << " reencode=" << blitQuadReencodes_
           << " reuseRate=" << std::fixed << std::setprecision(1) << blitReuseRate << "%\n";
    }
    std::cerr << os.str();
}
#endif

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
            const Composition::Rect & destBounds){
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

        // Absolute-coords (2026-05-29): the slice's ops were rendered into the
        // scratch at their absolute window position (renderToTarget computes
        // NDC against the full-window renderTargetSize_, and the scratch pass
        // viewport is (0,0,sw,sh) with sw,sh = the full-window slice extent),
        // so the scratch already holds the layer's content where it belongs.
        // Composite it 1:1 at the origin over the full window — applying the
        // old per-slice windowOffset here would double-shift the content. The
        // unit NDC quad spans [-1..1]; this viewport maps it onto the whole
        // window. (A view-sized scratch + offset blit is a Phase E blur-rework
        // efficiency concern, not a correctness one.)
        const float scale = renderScale_;
        OmegaGTE::GEViewport vp {};
        vp.x = 0.f;
        vp.y = 0.f;
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
        cb->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 6, 0);

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
            // Phase 7: WTK pos.y is top-edge (Y-down). NDC Y is up on
            // Metal/D3D12 and flipped via negative-height viewport on
            // Vulkan native, so map y=0 → NDC +1.
            pos[0][0] = (2.f * x) / viewportW - 1.f;
            pos[1][0] = 1.f - (2.f * y) / viewportH;
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
        cb->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 6, 0);
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
            SharedHandle<OmegaGTE::GEFence> textureFence,
            ViewCacheEntry * blitCacheEntry){
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

        const bool hasTransform = !(currentTransform == OmegaGTE::FMatrix<4,4>::Identity());

        // Phase G.5.1b follow-up: when the caller passes a content-cache
        // entry, the six-vertex quad it last encoded is held on the entry.
        // The vertices bake the dest rect, the viewport, and (when active)
        // the current transform; the UVs ride alongside. So a blit whose
        // dest/viewport/UVs are unchanged AND that runs under an identity
        // transform (matching the bake) re-uses the held buffer's GPU
        // contents verbatim — bind it and skip the encode. Any active
        // transform on either side, or any geometry change, re-encodes into
        // a fresh buffer (the held buffer is never overwritten in place, so
        // an in-flight GPU read of it is never raced). This mirrors the
        // tessellation cache's hold-and-replace. The transform is gated on
        // a single no-transform flag rather than comparing the full matrix
        // so `ViewCacheEntry` stays free of the GTE math surface.
        const bool canReuseQuad = blitCacheEntry != nullptr
                && blitCacheEntry->blitQuadBaked
                && blitCacheEntry->blitQuadBuf != nullptr
                && blitCacheEntry->blitQuadBytes == vertexBytes
                && !hasTransform
                && blitCacheEntry->blitBakedNoTransform
                && blitCacheEntry->blitDestX     == destRect.pos.x
                && blitCacheEntry->blitDestY     == destRect.pos.y
                && blitCacheEntry->blitDestW     == destRect.w
                && blitCacheEntry->blitDestH     == destRect.h
                && blitCacheEntry->blitViewportW == viewportW
                && blitCacheEntry->blitViewportH == viewportH
                && blitCacheEntry->blitUMin      == uMin
                && blitCacheEntry->blitVMin      == vMin
                && blitCacheEntry->blitUMax      == uMax
                && blitCacheEntry->blitVMax      == vMax;

        SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;
        if(canReuseQuad){
            vertexBuffer = blitCacheEntry->blitQuadBuf;   // bind the held quad; no encode
        }
        else {
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
        }

        // Per-draw uniform buffer: tintColor (16 bytes). Always re-encoded
        // per blit (it folds in the current opacity), so it stays on the
        // pooled per-draw path — only the geometry quad is held on the entry.
        // Released after frame completes via deferredBufferReleases.
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
            // A freshly-acquired (non-reused) vertex buffer that never got
            // bound goes back to the pool; a reused one belongs to the entry.
            if(!canReuseQuad && bufferPool() != nullptr && vertexBuffer){
                bufferPool()->release(std::move(vertexBuffer), vertexBytes);
            }
            return;
        }

        const float opacityMul = std::clamp(currentOpacity, 0.f, 1.f);

        // Phase G.5.1b follow-up: encode the six quad vertices only when we
        // did NOT reuse the entry's held buffer (a reused buffer already
        // holds correct contents).
        if(!canReuseQuad){
            bufferWriter->setOutputBuffer(vertexBuffer);
            auto writeVertex = [&](float x, float y, float u, float v){
                auto pos = OmegaGTE::FVec<4>::Create();
                // Phase 7: WTK Y-down → NDC Y-up (mirrors emitSdfPrimitive).
                pos[0][0] = (2.f * x) / viewportW - 1.f;
                pos[1][0] = 1.f - (2.f * y) / viewportH;
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
        }

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
            // Don't release a reused vertex buffer — it belongs to the entry.
            if(bufferPool() != nullptr){
                if(!canReuseQuad && vertexBuffer){
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
        cb->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 6, 0);
        frameRenderPass_.endDraw(scope);

        // Phase G.5.1b follow-up: decide what happens to the vertex buffer.
        if(canReuseQuad){
            // Bound the entry's held quad; it stays for the next blit.
            ++blitQuadReuseHits_;
        }
        else if(blitCacheEntry != nullptr){
            // Re-encoded into a fresh quad for a cached View: adopt it onto
            // the entry and hand the previous buffer to the completion-gated
            // recycler (an earlier frame may still be reading it).
            ++blitQuadReencodes_;
            if(blitCacheEntry->blitQuadBuf != nullptr
               && blitCacheEntry->blitQuadBuf != vertexBuffer){
                deferredBufferReleases.push_back(
                        {std::move(blitCacheEntry->blitQuadBuf), blitCacheEntry->blitQuadBytes});
            }
            blitCacheEntry->blitQuadBuf          = vertexBuffer;
            blitCacheEntry->blitQuadBytes        = vertexBytes;
            blitCacheEntry->blitBakedNoTransform = !hasTransform;
            blitCacheEntry->blitDestX            = destRect.pos.x;
            blitCacheEntry->blitDestY            = destRect.pos.y;
            blitCacheEntry->blitDestW            = destRect.w;
            blitCacheEntry->blitDestH            = destRect.h;
            blitCacheEntry->blitViewportW        = viewportW;
            blitCacheEntry->blitViewportH        = viewportH;
            blitCacheEntry->blitUMin             = uMin;
            blitCacheEntry->blitVMin             = vMin;
            blitCacheEntry->blitUMax             = uMax;
            blitCacheEntry->blitVMax             = vMax;
            blitCacheEntry->blitQuadBaked        = true;
        }
        else if(bufferPool() != nullptr && vertexBuffer){
            // Non-cache blit: recycle the quad per the foundation.
            deferredBufferReleases.push_back({std::move(vertexBuffer), vertexBytes});
        }

        // The per-draw params/tint buffer always recycles per the foundation.
        if(bufferPool() != nullptr && paramsBuffer){
            deferredBufferReleases.push_back({std::move(paramsBuffer), paramsStride});
        }
    }

    void BackendRenderTargetContext::emitTextSubRun(
            const Composition::TextSubRun & subRun,
            const Composition::Rect & rect,
            const Composition::Color & color){
        if(textTraceEnabled()){
            std::cout << "[wtk-text] emitTextSubRun ENTER"
                      << " font=" << (void *)subRun.resolvedFont.get()
                      << " glyphs=" << subRun.glyphIds.size()
                      << " rect=(" << rect.pos.x << "," << rect.pos.y
                      << " " << rect.w << "x" << rect.h << ")"
                      << " color=(" << color.r << "," << color.g
                      << "," << color.b << "," << color.a << ")"
                      << std::endl;
        }
        if(subRun.resolvedFont == nullptr){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: resolvedFont == nullptr"
                          << std::endl;
            }
            return;
        }
        if(subRun.glyphIds.empty() ||
           subRun.glyphIds.size() != subRun.positions.size()){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: glyphIds.size="
                          << subRun.glyphIds.size()
                          << " positions.size=" << subRun.positions.size()
                          << " (empty or mismatched)" << std::endl;
            }
            return;
        }

        auto & pipelines = pipelineRegistry();
        auto bufferWriter = pipelines.bufferWriter();
        auto textPipeline = pipelines.text();
        if(bufferWriter == nullptr || textPipeline == nullptr || renderTarget == nullptr){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: pipeline-missing"
                          << " bufferWriter=" << (bufferWriter == nullptr ? "NULL" : "ok")
                          << " textPipeline=" << (textPipeline == nullptr ? "NULL" : "ok")
                          << " renderTarget=" << (renderTarget == nullptr ? "NULL" : "ok")
                          << std::endl;
            }
            return;
        }

        GlyphAtlas & atlas = subRun.resolvedFont->atlas();
        if(textTraceEnabled()){
            std::cout << "[wtk-text] emitTextSubRun atlas=" << (void *)&atlas
                      << " atlasTexture=" << (atlas.texture() == nullptr ? "NULL" : "ok")
                      << " fontMode="
                      << (subRun.resolvedFont->mode() == Font::Mode::MSDF ? "MSDF" : "BitmapFallback")
                      << std::endl;
        }

        // Author one quad (6 vertices) per resident glyph. Atlas
        // population already happened on the paint-recording thread
        // (`Font::ensureGlyphsResident`), so the render path only
        // `lookup`s — a glyph that failed to rasterize / pack is simply
        // absent and skipped (chunk-2 append-only atlas contract — no
        // eviction, no panic). `ensureGlyph` must not run here: it
        // uploads a texture tile, which is illegal inside the frame
        // render pass.
        struct QuadVertex { float x, y, u, v; };
        OmegaCommon::Vector<QuadVertex> verts;
        verts.reserve(subRun.glyphIds.size() * 6);

        std::size_t lookupMisses = 0;
        std::size_t zeroSizeSkips = 0;
        for(std::size_t i = 0; i < subRun.glyphIds.size(); ++i){
            const std::uint32_t gid = subRun.glyphIds[i];
            const AtlasGlyph * g = atlas.lookup(gid);
            if(g == nullptr){
                ++lookupMisses;
                if(textTraceEnabled()){
                    std::cout << "[wtk-text]   atlas.lookup MISS gid=" << gid
                              << " (glyph not resident — ensureGlyphsResident likely failed)"
                              << std::endl;
                }
                continue;
            }
            if(g->fWidth <= 0.f || g->fHeight <= 0.f){
                ++zeroSizeSkips;
                if(textTraceEnabled()){
                    std::cout << "[wtk-text]   skip zero-sized glyph gid=" << gid
                              << " fW=" << g->fWidth << " fH=" << g->fHeight
                              << std::endl;
                }
                continue;
            }

            // Phase-3.5 sub-pixel quad authoring. The glyph metrics
            // carry the bbox top-left corner relative to the pen
            // (`fLeft, fTop`) and the bbox dimensions in canvas pixels
            // (`fWidth, fHeight`). Phase 2.5 rounded pen positions to
            // the integer pixel grid; Phase 3.5 drops the round so
            // accumulated `penX + fLeft` deviations don't surface as
            // uneven inter-glyph spacing on long runs. The MSDF
            // fragment shader's smoothstep + the atlas's bilinear
            // sampling produce correct fractional coverage along the
            // glyph edge at sub-pixel quad positions.
            const float penX = rect.pos.x + subRun.positions[i].x;
            const float penY = rect.pos.y + subRun.positions[i].y;
            const float minX = penX + g->fLeft;
            const float minY = penY - g->fTop;
            const float maxX = minX + g->fWidth;
            const float maxY = minY + g->fHeight;

            if(textTraceEnabled()){
                std::cout << "[wtk-text] QUAD gid=" << gid
                          << " pos=(" << subRun.positions[i].x << "," << subRun.positions[i].y << ")"
                          << " penY=" << penY
                          << " fTop=" << g->fTop
                          << " fHeight=" << g->fHeight
                          << " uv.v=[" << g->v0 << "," << g->v1 << "]"
                          << " canvasY=[" << minY << "," << maxY << "]" << std::endl;
            }

            // UV pairing (Phase 2.5): canvas-top ↔ `v0` (smaller V =
            // top of the atlas tile), canvas-bottom ↔ `v1`. The
            // rasterize callback writes the tile top-row-first, the
            // atlas upload is a straight `copyBytes` (no per-row
            // flip), and this pairing carries canvas-top through to
            // top-of-glyph end-to-end. Zero implicit flips.
            verts.push_back({minX, minY, g->u0, g->v0});
            verts.push_back({maxX, minY, g->u1, g->v0});
            verts.push_back({minX, maxY, g->u0, g->v1});
            verts.push_back({maxX, minY, g->u1, g->v0});
            verts.push_back({maxX, maxY, g->u1, g->v1});
            verts.push_back({minX, maxY, g->u0, g->v1});
        }

        if(verts.empty()){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: verts.empty()"
                          << " — every glyph lookup failed or was zero-sized."
                          << " misses=" << lookupMisses
                          << " zeroSized=" << zeroSizeSkips
                          << " (of " << subRun.glyphIds.size() << " glyphIds)"
                          << std::endl;
            }
            return;
        }

        auto atlasTexture = atlas.texture();
        if(atlasTexture == nullptr){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: atlas.texture() == nullptr"
                          << " (atlas never allocated its GPU texture — first ensureGlyph likely failed)"
                          << std::endl;
            }
            return;
        }
        if(textTraceEnabled()){
            std::cout << "[wtk-text] emitTextSubRun authoring "
                      << verts.size() << " verts (" << (verts.size() / 6)
                      << " quads); atlasTex=" << (void *)atlasTexture.get()
                      << std::endl;
        }

        const float viewportW = std::max(1.f, renderTargetSize_.w);
        const float viewportH = std::max(1.f, renderTargetSize_.h);
        const bool hasTransform = !(currentTransform == OmegaGTE::FMatrix<4,4>::Identity());
        const float opacityMul = std::clamp(currentOpacity, 0.f, 1.f);

        // Vertex buffer: verts × (float4 pos, float4 uvPad) — same
        // layout as the bitmap pipeline.
        const std::size_t vertexStride = OmegaGTE::omegaSLStructStride(
                {OMEGASL_FLOAT4, OMEGASL_FLOAT4});
        const std::size_t vertexBytes  = vertexStride * verts.size();
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
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: vertexBuffer alloc failed"
                          << " bytes=" << vertexBytes
                          << " (bufferPool=" << (bufferPool() != nullptr ? "ok" : "NULL") << ")"
                          << std::endl;
            }
            return;
        }

        // Per-draw uniform buffer: textColor + reserved outline params
        // (Phase 6.7.3 surface).
        const std::size_t paramsStride = OmegaGTE::omegaSLStructStride(
                {OMEGASL_FLOAT4, OMEGASL_FLOAT4});
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
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: paramsBuffer alloc failed"
                          << std::endl;
            }
            if(bufferPool() != nullptr && vertexBuffer){
                bufferPool()->release(std::move(vertexBuffer), vertexBytes);
            }
            return;
        }

        bufferWriter->setOutputBuffer(vertexBuffer);
        for(const auto & vtx : verts){
            auto pos = OmegaGTE::FVec<4>::Create();
            // Phase 7: WTK Y-down → NDC Y-up (mirrors emitSdfPrimitive).
            pos[0][0] = (2.f * vtx.x) / viewportW - 1.f;
            pos[1][0] = 1.f - (2.f * vtx.y) / viewportH;
            pos[2][0] = 0.f;
            pos[3][0] = 1.f;
            if(hasTransform){
                pos = currentTransform * pos;
            }
            auto uvPad = OmegaGTE::FVec<4>::Create();
            uvPad[0][0] = vtx.u;
            uvPad[1][0] = vtx.v;
            uvPad[2][0] = 0.f;
            uvPad[3][0] = 0.f;
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat4(uvPad);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        }
        bufferWriter->flush();

        bufferWriter->setOutputBuffer(paramsBuffer);
        auto textColor = OmegaGTE::makeColor(color.r, color.g, color.b,
                                             color.a * opacityMul);
        auto outlineReserved = OmegaGTE::FVec<4>::Create();
        bufferWriter->structBegin();
        bufferWriter->writeFloat4(textColor);
        bufferWriter->writeFloat4(outlineReserved);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();
        bufferWriter->flush();

        SharedHandle<OmegaGTE::GEFence> noFence;
        auto scope = frameRenderPass_.beginDraw(noFence);
        if(scope.cb == nullptr){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] emitTextSubRun SKIP: beginDraw returned null CB"
                          << " — most likely the frame's render pass is not active"
                          << " (frameRenderPass_.begin() not called, or already ended)."
                          << std::endl;
            }
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

        frameRenderPass_.bindTextPipeline(scope);
        cb->bindResourceAtVertexShader(vertexBuffer, 12);
        cb->bindResourceAtFragmentShader(paramsBuffer, 13);
        cb->bindResourceAtFragmentShader(atlasTexture, 14);
        cb->drawPolygons(OmegaGTE::GECommandBuffer::Triangle,
                         (unsigned)verts.size(), 0);
        frameRenderPass_.endDraw(scope);

        if(textTraceEnabled()){
            std::cout << "[wtk-text] emitTextSubRun DRAW issued: "
                      << verts.size() << " verts (" << (verts.size() / 6)
                      << " quads)" << std::endl;
        }

        if(bufferPool() != nullptr){
            if(vertexBuffer){
                deferredBufferReleases.push_back({std::move(vertexBuffer), vertexBytes});
            }
            if(paramsBuffer){
                deferredBufferReleases.push_back({std::move(paramsBuffer), paramsStride});
            }
        }
    }

    // Phase G.3.1: cache-target machinery. Mirrors the blur scratch
    // setup almost line-for-line — the scratch-pass mechanism in
    // `FrameRenderPass` already does the heavy lifting (suspend the
    // window-target pass, switch the recording CB to a new render
    // pass on the scratch target, resume on close). The difference is
    // the lifetime of the captured texture: the blur path consumes
    // and discards it within the same frame; G.3.1 hands the texture
    // over to the per-RTC content cache so subsequent frames can
    // (G.3.2) blit it instead of re-painting.
    bool BackendRenderTargetContext::beginCacheTarget(unsigned widthPx, unsigned heightPx,
                                                      float originXLogical, float originYLogical){
        if(widthPx == 0 || heightPx == 0){
            return false;
        }
        if(!frameRenderPass_.active()){
            return false;
        }
        if(frameRenderPass_.scratchActive()){
            // Already inside a scratch pass (blur, or a malformed
            // double-capture). Surface as failure so the caller treats
            // the capture as aborted; subsequent ops keep recording
            // onto whatever pass is open.
            return false;
        }

        auto * pool = BackendResourceFactory::instance().texturePool();
        TexturePoolKey key {};
        key.width       = widthPx;
        key.height      = heightPx;
        key.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
        key.usage       = OmegaGTE::GETexture::RenderTarget;

        SharedHandle<OmegaGTE::GETexture> texture;
        if(pool != nullptr){
            texture = pool->acquire(key);
        }
        if(texture == nullptr){
            OmegaGTE::TextureDescriptor desc {};
            desc.usage        = OmegaGTE::GETexture::RenderTarget;
            desc.storage_opts = OmegaGTE::Shared;
            desc.width        = widthPx;
            desc.height       = heightPx;
            desc.pixelFormat  = OmegaGTE::PixelFormat::BGRA8Unorm;
            texture = gte.graphicsEngine->makeTexture(desc);
        }
        if(texture == nullptr){
            return false;
        }

        auto target = gte.graphicsEngine->makeTextureRenderTarget({true, texture});
        if(target == nullptr){
            if(pool != nullptr){
                pool->release(std::move(texture), key);
            }
            return false;
        }

        auto * fp = fencePool();
        SharedHandle<OmegaGTE::GEFence> fence = (fp != nullptr)
                ? fp->acquire()
                : gte.graphicsEngine->makeFence();

        // Capture viewport: window-sized, offset by -(viewOrigin × scale)
        // so the View's absolute-window-coord ops (NDC authored against
        // the window in every emit path) land at the View-sized
        // texture's origin. Scissor = the texture dims, clipping the
        // rest of the window away.
        const float scale = renderScale_;
        const float vpX = -originXLogical * scale;
        const float vpY = -originYLogical * scale;
        const float vpW = static_cast<float>(backingWidth_);
        const float vpH = static_cast<float>(backingHeight_);

        frameRenderPass_.beginCapturePass(target, widthPx, heightPx, vpX, vpY, vpW, vpH);
        if(!frameRenderPass_.scratchActive()){
            // beginCapturePass refused (no active frame, or already
            // inside another scratch). Roll back the allocations.
            if(pool != nullptr){
                pool->release(std::move(texture), key);
            }
            if(fp != nullptr && fence){
                fp->release(std::move(fence));
            }
            return false;
        }

        captureTexture_ = std::move(texture);
        captureTarget_  = std::move(target);
        captureFence_   = std::move(fence);
        return true;
    }

    SharedHandle<OmegaGTE::GETexture> BackendRenderTargetContext::endCacheTarget(){
        if(captureTexture_ == nullptr){
            return {};
        }
        frameRenderPass_.endScratchPass();
        frameRenderPass_.resumeFrameAfterScratch(captureFence_);

        auto texture = std::move(captureTexture_);
        captureTarget_.reset();
        // Keep `captureFence_` until the bitmap blit has been emitted —
        // the blit's `emitBitmapPrimitive` consumes it as the
        // texture-wait fence. The caller resets it after that.
        return texture;
    }

    void BackendRenderTargetContext::renderBlurredSlice(
            const CompositeFrame::WidgetSlice & slice){
        if(slice.targetLayer == nullptr || !slice.targetLayer->hasBlur()){
            for(auto & op : slice.ops.ops()){
                renderToTarget(op.type, (void *)&op.params);
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
            for(auto & op : slice.ops.ops()){
                renderToTarget(op.type, (void *)&op.params);
            }
            return;
        }
        auto & scratch = *scratchSlot;

        // Suspend the frame pass and start a fresh pass on the scratch
        // target. While the scratch pass is open, beginDraw records onto
        // the scratch CB; the renderToTarget vertex math uses
        // renderTargetSize_ (the full-window logical size) to compute NDC,
        // and the scratch's GPU viewport is (0,0,sw,sh) with sw,sh = the
        // full-window slice extent — so the slice's absolute-coords ops land
        // in the scratch at their absolute window position (matching the
        // origin-aligned composite below).
        auto scratchTarget = scratch.sourceTarget();
        frameRenderPass_.beginScratchPass(scratchTarget, sw, sh);
        if(!frameRenderPass_.scratchActive()){
            // Couldn't start scratch pass (no active frame). Fall back to
            // direct render so the layer still appears.
            for(auto & op : slice.ops.ops()){
                renderToTarget(op.type, (void *)&op.params);
            }
            return;
        }

        // Reset transient transform/opacity inside the scratch so prior
        // slices' SetTransform/SetOpacity don't bleed in.
        const auto savedTransform = currentTransform;
        const float savedOpacity  = currentOpacity;
        currentTransform = OmegaGTE::FMatrix<4,4>::Identity();
        currentOpacity   = 1.f;

        for(auto & op : slice.ops.ops()){
            renderToTarget(op.type, (void *)&op.params);
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
                                             commandQueue_,
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
        compositeScratchOntoFrame(scratch, slice.bounds);
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
        // beginFrame/endFrame. Commit the queue, then present the swap chain.
        if(commandQueue_ != nullptr){
            commandQueue_->commitToGPU();
        }
        renderTarget->present();
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

    void BackendRenderTargetContext::enqueueFrameBufferReleases(
            SharedHandle<OmegaGTE::GECommandBuffer> & cb){
        // Phase G.5.1: nothing acquired this frame → nothing to gate.
        if(deferredBufferReleases.empty() || cb == nullptr){
            return;
        }
        // Heap-owned flag so the completion callback can outlive this
        // context: the callback captures only this `shared_ptr`, never
        // `this` or the pool. Flipped on GPU completion; read on the
        // compositor thread in `drainCompletedBufferReleases`.
        auto done = std::make_shared<std::atomic<bool>>(false);
        cb->setCompletionHandler([done](const OmegaGTE::GECommandBufferCompletionInfo &){
            done->store(true, std::memory_order_release);
        });
        PendingReleaseBatch batch;
        batch.buffers = std::move(deferredBufferReleases);
        batch.done    = std::move(done);
        deferredBufferReleases.clear();
        pendingReleaseBatches_.push_back(std::move(batch));
    }

    void BackendRenderTargetContext::drainCompletedBufferReleases(){
        // Front-to-back: command buffers complete in submission order on
        // the single render queue, so the front batch is the oldest. Stop
        // at the first batch whose GPU work has not finished. On backends
        // whose `setCompletionHandler` is the base no-op (D3D12 / Vulkan
        // today) the flag never flips, so batches accumulate until
        // teardown — the pre-G.5.1 behavior, no recycling but no leak past
        // the context's lifetime.
        auto * pool = bufferPool();
        while(!pendingReleaseBatches_.empty() &&
              pendingReleaseBatches_.front().done->load(std::memory_order_acquire)){
            auto batch = std::move(pendingReleaseBatches_.front());
            pendingReleaseBatches_.pop_front();
            if(pool != nullptr){
                for(auto & entry : batch.buffers){
                    if(entry.first){
                        pool->release(std::move(entry.first), entry.second);
                    }
                }
            }
        }
    }


    template<class ParamsT>
    void BackendRenderTargetContext::renderPrimitiveImpl(PrimitiveOp op, ParamsT *params) {
        // Phase 6.8 / Tier 4 §4.0: this body only prepares per-primitive
        // work. SDF primitives, bitmaps, text, and state ops are self-
        // contained (they call an emit* helper and return); the gradient-
        // fill Rect/RoundedRect cases triangulate and fall through to
        // `drawTriangulatedResult`, which owns the pipeline/buffer/draw
        // tail (and re-derives the pipeline states it needs).
        if(renderTarget == nullptr){
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

        bool useTextureRenderPipeline = false;
        float textureCoordDenomW = 1.f;
        float textureCoordDenomH = 1.f;

        SharedHandle<OmegaGTE::GETexture> texturePaint;
        SharedHandle<OmegaGTE::GEFence> textureFence;

        switch (op) {
            case PrimitiveOp::Rect : {
                auto & _params = params->rectParams;
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
            case PrimitiveOp::Bitmap : {
                // Phase 6.6: bitmap drawing dispatches through the dedicated
                // bitmap pipeline — hardcoded 6-vertex quad authored inline,
                // no triangulator round-trip, mip-chain texture sourced from
                // the process-wide BitmapTextureCache, optional sub-rect UV
                // and RGBA tint via per-draw uniform buffer.
                auto & _params = params->bitmapParams;

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
            case PrimitiveOp::RoundedRect : {
                auto & _params = params->roundedRectParams;
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
            case PrimitiveOp::Ellipse : {
                auto & _params = params->ellipseParams;
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
            // VectorPath is handled in the public overloads (it needs the
            // segmented form `Path::decomposeForDraw` produces, not a
            // params sub-struct shared with VisualCommand) — see
            // renderVectorPathSegmented.
            case PrimitiveOp::Shadow: {
                auto & _params = params->shadowParams;
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
            case PrimitiveOp::SetTransform: {
                auto & _params = params->transformMatrix;
                currentTransform = toGTEMatrix(_params);
                return;
            }
            case PrimitiveOp::SetOpacity: {
                currentOpacity = params->opacityValue;
                return;
            }
            // SetClip / PushClip / PopClip are handled in the public
            // overloads (VisualCommand's pre-resolved clipRect vs. the
            // DrawOp backend clip stack) — see pushDrawOpClip/popDrawOpClip.
            case PrimitiveOp::NativeContent: {
                // §2.14 Pass 1 retired the carve-out recording. The
                // platform-tree side of the pre-§2.14 plan
                // (CALayer / DComp visual / X11 child window
                // ordering via `applyNativeContentCarveouts`) is
                // replaced in §2.14 Pass 2 by
                // `Native::NativeContentNode` +
                // `Native::VisualTree::reconfigureContentNode`, which
                // runs inside `NativeViewHost::onLayoutResolved`
                // rather than from the per-frame drain. The
                // alpha-clear + AABB-cull semantics around
                // `DrawOp::NativeContent` are preserved upstream of
                // this switch; the recording branch here was the
                // only piece tied to the retired drain hook.
                (void)params;
                return;
            }
            case PrimitiveOp::TextRun: {
                // Phase 6.7-c3: iterate the sub-runs (chunk 3 emits a
                // single sub-run; chunk 4 lights up multi-atlas runs)
                // and issue one draw call per sub-run against its
                // resolved font's MSDF glyph atlas.
                auto & _params = params->textRunParams;
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] renderToTarget TextRun: "
                              << _params.subRuns.size() << " sub-runs, rect=("
                              << _params.rect.pos.x << "," << _params.rect.pos.y
                              << " " << _params.rect.w << "x" << _params.rect.h << ")"
                              << std::endl;
                }
                for(const auto & subRun : _params.subRuns){
                    emitTextSubRun(subRun, _params.rect, _params.color);
                }
                return;
            }
            default:
                return;
        }

        // Only the gradient-fill Rect / RoundedRect cases reach here
        // (they triangulate into `result` and request the texture
        // pipeline). The shared draw tail owns the rest.
        drawTriangulatedResult(result, useTextureRenderPipeline, false,
                               textureCoordDenomW, textureCoordDenomH,
                               OmegaGTE::FVec<4>::Create(), false,
                               OmegaGTE::FVec<4>::Create(), false,
                               texturePaint, textureFence);
    }

    void BackendRenderTargetContext::drawTriangulatedResult(
            OmegaGTE::TETriangulationResult & result,
            bool useTextureRenderPipeline,
            bool usePathRenderPipeline,
            float textureCoordDenomW,
            float textureCoordDenomH,
            const OmegaGTE::FVec<4> & pathStrokeColor,
            bool pathHasStrokeColor,
            const OmegaGTE::FVec<4> & pathFillColor,
            bool pathHasFillColor,
            SharedHandle<OmegaGTE::GETexture> texturePaint,
            SharedHandle<OmegaGTE::GEFence> textureFence,
            TessellationCacheEntry * cacheEntry) {
        auto & pipelines = pipelineRegistry();
        auto bufferWriter = pipelines.bufferWriter();
        auto renderPipelineState = pipelines.color();
        auto textureRenderPipelineState = pipelines.texture();
        auto pathRenderPipelineState = pipelines.path();
        if(bufferWriter == nullptr || renderTarget == nullptr){
            return;
        }
        std::size_t struct_size;

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

        // Phase G.5.1b: reuse the cache entry's persistent vertex buffer when
        // its baked state still describes what we'd encode this frame. The
        // vertex bytes bake `currentTransform` + `currentOpacity` (and the
        // pipeline picks the layout), and the mesh + colors are fixed by the
        // cache key — so a match on all four means the buffer's GPU contents
        // are already correct and we can bind without re-encoding. Reuse is
        // read-only (the buffer is never overwritten in place), so concurrent
        // in-flight frames reading the same buffer is safe.
        const bool canReuse = cacheEntry != nullptr
                && cacheEntry->hasBaked
                && cacheEntry->vertexBuf != nullptr
                && cacheEntry->bakedBytes == requiredBytes
                && cacheEntry->bakedPath  == usePathRenderPipeline
                && cacheEntry->bakedOpacity == currentOpacity
                && cacheEntry->bakedTransform == currentTransform;

        SharedHandle<OmegaGTE::GEBuffer> buffer;
        if(canReuse){
            buffer = cacheEntry->vertexBuf;   // bind the held buffer; no encode
        }
        else {
            if(bufferPool()){
                buffer = bufferPool()->acquire(requiredBytes, struct_size);
            }
            else {
                OmegaGTE::BufferDescriptor bufferDesc {OmegaGTE::BufferDescriptor::Upload,requiredBytes,struct_size};
                buffer = gte.graphicsEngine->makeBuffer(bufferDesc);
            }
            bufferWriter->setOutputBuffer(buffer);
        }

        // Acquire the command buffer for this draw. The scope wraps the
        // frame's CB (or the per-layer scratch's CB when a scratch pass is
        // open). Texture-fence handling (mid-pass end+notify+restart) is
        // bundled into beginDraw(). On the always-direct path
        // renderToTarget is only invoked inside an active frame, so a null
        // scope CB indicates a contract violation; short-circuit to avoid
        // recording onto an undefined target.
        auto scope = frameRenderPass_.beginDraw(textureFence);
        if(scope.cb == nullptr){
            // Don't release a reused buffer — it belongs to the cache entry,
            // not to this draw. A freshly-acquired (non-reused) buffer that
            // never got bound goes back to the pool.
            if(!canReuse && bufferPool() != nullptr && buffer){
                bufferPool()->release(std::move(buffer), requiredBytes);
            }
            return;
        }
        auto & cb = scope.cb;

        unsigned startVertexIndex = 0;

        // Phase G.5.1b: the per-vertex encode runs only when we did NOT reuse
        // the entry's buffer (a reused buffer already holds correct contents).
        if(!canReuse){
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

        for(auto & v : result.mesh.vertexPolygons){
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

        // Flush vertex data before draw calls
        bufferWriter->flush();
        } // end if(!canReuse) — encode block

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

        {
            auto & m = result.mesh;
            OmegaGTE::GECommandBuffer::PolygonType topology;
            if(m.topology == OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangleStrip){
                topology = OmegaGTE::GECommandBuffer::TriangleStrip;
            }
            else {
                topology = OmegaGTE::GECommandBuffer::Triangle;
            }
            cb->drawPolygons(topology, m.vertexCount(), startVertexIndex);
            startVertexIndex += m.vertexCount();
        }

        frameRenderPass_.endDraw(scope);

        // Phase G.5.1b: decide what happens to `buffer` after the draw.
        if(canReuse){
            // Bound the entry's held buffer; it stays on the entry for the
            // next frame. Nothing to release.
            ++tessReuseHits_;
        }
        else if(cacheEntry != nullptr){
            // Re-encoded into a fresh buffer for a cached shape: adopt it onto
            // the entry and hand the entry's previous buffer to the
            // completion-gated recycler (it was read by an earlier frame, so
            // it must not return to the pool until the GPU retires it).
            ++tessReencodes_;
            if(cacheEntry->vertexBuf != nullptr && cacheEntry->vertexBuf != buffer){
                deferredBufferReleases.push_back(
                        {std::move(cacheEntry->vertexBuf), cacheEntry->bakedBytes});
            }
            cacheEntry->vertexBuf      = buffer;
            cacheEntry->bakedTransform = currentTransform;
            cacheEntry->bakedOpacity   = currentOpacity;
            cacheEntry->bakedBytes     = requiredBytes;
            cacheEntry->bakedPath      = usePathRenderPipeline;
            cacheEntry->hasBaked       = true;
        }
        else if(bufferPool() && buffer){
            // Non-cached draw (gradient-fill path): recycle per the foundation.
            deferredBufferReleases.push_back({std::move(buffer), requiredBytes});
        }
    }

    void BackendRenderTargetContext::renderVectorPathSegmented(
            const Core::SharedPtr<OmegaGTE::GVectorPath2D> & path,
            float strokeWidth, bool contour, bool fill,
            const Core::SharedPtr<Brush> & strokeBrush,
            const Core::SharedPtr<Brush> & fillBrush) {
        if(path == nullptr || path->size() < 2){
            return;
        }
        if(!ensureTessellationContext()) return;

        OmegaGTE::GEViewport viewPort {};
        viewPort.x = viewPort.y = viewPort.nearDepth = 0.f;
        viewPort.farDepth = 1.f;
        viewPort.width = renderTargetSize_.w;
        viewPort.height = renderTargetSize_.h;

        // First attachment: stroke color.
        auto strokeColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
        bool hasStrokeColor = false;
        if(strokeBrush != nullptr && strokeBrush->type == Brush::Type::Color){
            strokeColor = OmegaGTE::makeColor(strokeBrush->color.r,
                                              strokeBrush->color.g,
                                              strokeBrush->color.b,
                                              strokeBrush->color.a);
            hasStrokeColor = true;
        }

        // Second attachment: fill color.
        auto fillColor = OmegaGTE::FVec<4>::Create();
        bool hasFillColor = false;
        if(fill && fillBrush != nullptr && fillBrush->type == Brush::Type::Color){
            fillColor = OmegaGTE::makeColor(fillBrush->color.r,
                                            fillBrush->color.g,
                                            fillBrush->color.b,
                                            fillBrush->color.a);
            hasFillColor = true;
        }

        // Phase 6.4: route the dual-attachment mesh through the path
        // pipeline when available, else fall back to the flat color
        // pipeline (same logic the old VectorPath case carried).
        const bool usePathRenderPipeline = (pipelineRegistry().path() != nullptr);

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
        // Phase G.1: cache lookup. The key captures everything
        // `triangulateSync` consumes plus the stroke/fill colors that
        // `drawTriangulatedResult` reads back out of the mesh
        // attachments. A cache hit skips `triangulateSync` entirely and
        // draws from the cached mesh.
        TessellationCacheKey key;
        const auto pathHashPair = hashPath2D(*path);
        key.pathHash    = pathHashPair.first;
        key.pointCount  = pathHashPair.second;
        key.wBucket     = bucketDim(renderTargetSize_.w);
        key.hBucket     = bucketDim(renderTargetSize_.h);
        key.strokeWidth = strokeWidth;
        key.flagsBits   = 0;
        if(contour){
            key.flagsBits |= TessellationCacheKey::FlagContour;
        }
        if(fill){
            key.flagsBits |= TessellationCacheKey::FlagFill;
        }
        if(hasStrokeColor){
            key.flagsBits |= TessellationCacheKey::FlagHasStrokeColor;
        }
        key.strokeRGBA = packRGBA(strokeColor[0][0], strokeColor[1][0],
                                  strokeColor[2][0], strokeColor[3][0]);
        if(hasFillColor){
            key.flagsBits |= TessellationCacheKey::FlagHasFillColor;
            key.fillRGBA  = packRGBA(fillColor[0][0], fillColor[1][0],
                                     fillColor[2][0], fillColor[3][0]);
        }

        auto * cacheState = tessellationCacheState_.get();
        if(cacheState != nullptr){
            if(auto * hit = cacheState->cache.find(key)){
                // G.5.1b: draw through the entry — reuses its persistent
                // vertex buffer when the baked state still matches, else
                // re-encodes into a fresh one and re-adopts it.
                drawTriangulatedResult(hit->mesh, false, usePathRenderPipeline,
                                       1.f, 1.f,
                                       strokeColor, hasStrokeColor,
                                       fillColor, hasFillColor,
                                       nullptr, nullptr, hit);
                return;
            }
        }
#endif

        auto te_params = OmegaGTE::TETriangulationParams::GraphicsPath2D(*path,
                                                                         strokeWidth,
                                                                         contour,
                                                                         fill);
        te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(strokeColor));
        if(hasFillColor){
            te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(fillColor));
        }

        auto result = tessellationContext_->triangulateSync(te_params,
                                                            OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,
                                                            &viewPort);

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
        // Phase G.1 + G.5.1b: cache the just-computed mesh and draw THROUGH
        // the inserted entry, so this frame's encode populates the entry's
        // persistent vertex buffer for the next frame to reuse. Byte cost is
        // a telemetry-only estimate (the polygon vector dominates); the cache
        // caps on entry count, not bytes. `insert` may evict the LRU tail
        // (its `OnEvict` recycles that entry's buffer) but returns a stable
        // pointer to the new front entry, which no further cache mutation
        // touches before the draw.
        if(cacheState != nullptr){
            const std::size_t entryBytes =
                    result.mesh.vertexPolygons.size()
                    * sizeof(OmegaGTE::TETriangulationResult::TEMesh::Polygon);
            TessellationCacheEntry entry;
            entry.mesh = std::move(result);
            auto * entryPtr = cacheState->cache.insert(key, std::move(entry), entryBytes);
            drawTriangulatedResult(entryPtr->mesh, false, usePathRenderPipeline,
                                   1.f, 1.f,
                                   strokeColor, hasStrokeColor,
                                   fillColor, hasFillColor,
                                   nullptr, nullptr, entryPtr);
            return;
        }
#endif

        drawTriangulatedResult(result, false, usePathRenderPipeline,
                               1.f, 1.f,
                               strokeColor, hasStrokeColor,
                               fillColor, hasFillColor,
                               nullptr, nullptr);
    }

    void BackendRenderTargetContext::pushDrawOpClip(const Composition::Rect & rect){
        // Mirrors Canvas::pushClip: intersect the incoming rect with the
        // current top of stack and apply the effective scissor. The
        // intersection bookkeeping lived in Canvas (deleted in 4.2); the
        // backend now owns it for the DrawOp path.
        Composition::Rect effective = rect;
        if(!drawOpClipStack_.empty()){
            const auto & top = drawOpClipStack_.back();
            const float left   = std::max(top.pos.x, rect.pos.x);
            const float topEdge= std::max(top.pos.y, rect.pos.y);
            const float right  = std::min(top.pos.x + top.w, rect.pos.x + rect.w);
            const float bottom = std::min(top.pos.y + top.h, rect.pos.y + rect.h);
            if(right > left && bottom > topEdge){
                effective = Composition::Rect{
                    Composition::Point2D{left, topEdge},
                    right - left, bottom - topEdge};
            }
            else {
                // Empty intersection: degenerate scissor at the current
                // top's origin (all draws culled while active), matching
                // Canvas::pushClip's behavior so a matching pop restores.
                effective = Composition::Rect{top.pos, 0.f, 0.f};
            }
        }
        drawOpClipStack_.push_back(effective);
        applySetClip(Core::Optional<Composition::Rect>{effective});
    }

    void BackendRenderTargetContext::popDrawOpClip(){
        if(drawOpClipStack_.empty()){
            // Imbalanced pop — treat as no-op so the scissor state stays
            // consistent (matches Canvas::popClip).
            return;
        }
        drawOpClipStack_.pop_back();
        if(drawOpClipStack_.empty()){
            applySetClip(Core::Optional<Composition::Rect>{});
        }
        else {
            applySetClip(Core::Optional<Composition::Rect>{drawOpClipStack_.back()});
        }
    }

    void BackendRenderTargetContext::renderToTarget(DrawOp::Type type, void *params){
#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
        // G.3.2 hit skip: a Begin marker found the cache and emitted
        // its Bitmap blit; the wrapped ops are now redundant, so drop
        // every op until the matching End marker clears the flag. End
        // is the *only* op allowed through while skipping; everything
        // else (including nested Begin markers, if any slipped past
        // the per-View walker's invariant) is silently ignored.
        if(captureSkipping_ && type != DrawOp::EndCacheCapture){
            return;
        }
#endif
        switch(type){
            case DrawOp::Rect:
                renderPrimitiveImpl(PrimitiveOp::Rect, (DrawOp::Params*)params); return;
            case DrawOp::RoundedRect:
                renderPrimitiveImpl(PrimitiveOp::RoundedRect, (DrawOp::Params*)params); return;
            case DrawOp::Ellipse:
                renderPrimitiveImpl(PrimitiveOp::Ellipse, (DrawOp::Params*)params); return;
            case DrawOp::Bitmap:
                renderPrimitiveImpl(PrimitiveOp::Bitmap, (DrawOp::Params*)params); return;
            case DrawOp::Shadow:
                renderPrimitiveImpl(PrimitiveOp::Shadow, (DrawOp::Params*)params); return;
            case DrawOp::TextRun:
                renderPrimitiveImpl(PrimitiveOp::TextRun, (DrawOp::Params*)params); return;
            case DrawOp::SetTransform:
                renderPrimitiveImpl(PrimitiveOp::SetTransform, (DrawOp::Params*)params); return;
            case DrawOp::SetOpacity:
                renderPrimitiveImpl(PrimitiveOp::SetOpacity, (DrawOp::Params*)params); return;
            case DrawOp::NativeContent:
                renderPrimitiveImpl(PrimitiveOp::NativeContent, (DrawOp::Params*)params); return;
            case DrawOp::VectorPath: {
                if(params == nullptr) return;
                auto & p = ((DrawOp::Params*)params)->pathParams;
                if(p.path == nullptr) return;
                // Tier 4 §4.0: rehome Canvas::drawPath's decomposition.
                Core::SharedPtr<Brush> strokeBrush;
                float strokeWidth = 0.f;
                if(p.border.has_value()){
                    strokeBrush = p.border->brush;
                    strokeWidth = static_cast<float>(p.border->width);
                }
                for(auto & seg : p.path->decomposeForDraw(strokeBrush, strokeWidth)){
                    renderVectorPathSegmented(seg.path, seg.strokeWidth, seg.contour,
                                              seg.fill, seg.strokeBrush, seg.fillBrush);
                }
                return;
            }
            case DrawOp::PushClip: {
                if(params == nullptr) return;
                pushDrawOpClip(((DrawOp::Params*)params)->pushClipParams.rect);
                return;
            }
            case DrawOp::PopClip:
                popDrawOpClip();
                return;
            case DrawOp::PushTransform:
            case DrawOp::PopTransform:
                // Scoped 3D-effect transform — no producer yet (same as the
                // Tier 3 replay no-op). Lands when a producer appears.
                return;
#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
            case DrawOp::BeginCacheCapture: {
                if(params == nullptr) return;
                auto & p = ((DrawOp::Params*)params)->beginCacheCaptureParams;
                ++captureDepth_;
                if(captureDepth_ != 1){
                    // Nested marker. The G.3.2 per-View walker
                    // guarantees this should not happen — markers are
                    // emitted around each View's own paint only, with
                    // children recursing as siblings afterwards — but
                    // we tolerate it defensively. The inner pair is
                    // dropped; the outer pair retains control.
                    return;
                }

                // The marker rect is the View's WINDOW rect (pos =
                // absolute window origin, w/h = view size). Build the
                // lookup key. The cache lives on the render thread (same
                // thread as renderToTarget), so the lookup is
                // unsynchronized. The size bucket keys on the view SIZE,
                // not its window position — a View that moved but kept
                // its content + size still hits.
                ViewCacheKey key {};
                key.nodeId         = p.nodeId;
                key.contentVersion = p.contentVersion;
                key.wBucket        = (p.rect.w > 0.f)
                        ? static_cast<std::uint32_t>(std::lround(p.rect.w))
                        : 0u;
                key.hBucket        = (p.rect.h > 0.f)
                        ? static_cast<std::uint32_t>(std::lround(p.rect.h))
                        : 0u;
                std::memcpy(&key.renderScaleBits, &renderScale_, sizeof(key.renderScaleBits));

                ViewCacheEntry * hit = nullptr;
                if(contentCacheState_ != nullptr){
                    hit = contentCacheState_->cache.find(key);
                }

                if(hit != nullptr && hit->texture != nullptr){
                    // HIT: blit the View-sized cached texture at the
                    // View's CURRENT window rect (p.rect) — not the
                    // stored capture position, since the View may have
                    // moved while keeping the same content + size.
                    // `captureSkipping_` then drops the wrapped ops until
                    // the matching End.
                    auto tint = OmegaGTE::FVec<4>::Create();
                    tint[0][0] = 1.f; tint[1][0] = 1.f;
                    tint[2][0] = 1.f; tint[3][0] = 1.f;
                    // G.5.1b follow-up: blit THROUGH the hit entry so it
                    // re-binds the persistent quad buffer populated by the
                    // capture-end composite (or an earlier hit) and skips
                    // the six-vertex re-encode when the dest/viewport are
                    // unchanged.
                    emitBitmapPrimitive(p.rect,
                                        0.f, 0.f, 1.f, 1.f,
                                        tint,
                                        hit->texture,
                                        SharedHandle<OmegaGTE::GEFence>{},
                                        hit);
                    captureSkipping_ = true;
                    return;
                }

                // MISS: open a View-sized cache target. The capture pass
                // uses a window-sized viewport offset by -(view origin)
                // so the wrapped ops (absolute window coords) land at the
                // texture origin clipped to the view bounds.
                const unsigned widthPx  = scaledExtent(p.rect.w, renderScale_);
                const unsigned heightPx = scaledExtent(p.rect.h, renderScale_);
                if(widthPx == 0 || heightPx == 0 ||
                   !beginCacheTarget(widthPx, heightPx, p.rect.pos.x, p.rect.pos.y)){
                    captureDepth_ = 0;
                    return;
                }
                captureKeyNodeId_         = p.nodeId;
                captureKeyContentVersion_ = p.contentVersion;
                // Capture rect = the View's window rect. Used as the
                // composite blit dest on End and stored on the cache
                // entry.
                captureRect_              = p.rect;
                return;
            }
            case DrawOp::EndCacheCapture: {
                if(captureDepth_ == 0){
                    // Unmatched End (Begin disarmed or never fired).
                    captureSkipping_ = false;
                    return;
                }
                --captureDepth_;
                if(captureDepth_ != 0){
                    // Outer marker still active.
                    return;
                }

                // Outermost End. Two branches: skipping (hit path,
                // nothing to do beyond clearing the flag) versus
                // capturing (miss path, close + insert + composite).
                if(captureSkipping_){
                    captureSkipping_ = false;
                    return;
                }

                auto texture = endCacheTarget();
                if(texture == nullptr){
                    captureFence_.reset();
                    return;
                }

                ViewCacheKey key {};
                key.nodeId         = captureKeyNodeId_;
                key.contentVersion = captureKeyContentVersion_;
                key.wBucket        = (captureRect_.w > 0.f)
                        ? static_cast<std::uint32_t>(std::lround(captureRect_.w))
                        : 0u;
                key.hBucket        = (captureRect_.h > 0.f)
                        ? static_cast<std::uint32_t>(std::lround(captureRect_.h))
                        : 0u;
                std::memcpy(&key.renderScaleBits, &renderScale_, sizeof(key.renderScaleBits));

                // G.5.1b follow-up: insert returns a stable pointer to the
                // live entry so the composite blit below can draw THROUGH it
                // — that first blit populates the entry's persistent quad
                // buffer, which subsequent cache-hit blits then reuse. The
                // `insert` may evict the LRU tail (its `OnEvict` recycles
                // that entry's quad buffer) but the returned pointer is to
                // the new front entry, untouched by any further cache
                // mutation before the draw.
                ViewCacheEntry * insertedEntry = nullptr;
                if(contentCacheState_ != nullptr){
                    ViewCacheEntry entry {};
                    entry.texture        = texture;
                    entry.rasterizedSize = captureRect_;
                    const std::size_t pxW = static_cast<std::size_t>(
                            scaledExtent(captureRect_.w, renderScale_));
                    const std::size_t pxH = static_cast<std::size_t>(
                            scaledExtent(captureRect_.h, renderScale_));
                    const std::size_t bytes = pxW * pxH * 4;
                    insertedEntry = contentCacheState_->cache.insert(key, std::move(entry), bytes);
                }

                auto tint = OmegaGTE::FVec<4>::Create();
                tint[0][0] = 1.f; tint[1][0] = 1.f;
                tint[2][0] = 1.f; tint[3][0] = 1.f;
                emitBitmapPrimitive(captureRect_,
                                    0.f, 0.f, 1.f, 1.f,
                                    tint,
                                    texture,
                                    captureFence_,
                                    insertedEntry);

                captureFence_.reset();
                captureKeyNodeId_         = 0;
                captureKeyContentVersion_ = 0;
                captureRect_              = Composition::Rect{};
                return;
            }
#else
            case DrawOp::BeginCacheCapture:
            case DrawOp::EndCacheCapture:
                // Content cache compiled out — markers are inert.
                return;
#endif
            default:
                return;
        }
    }

    // §2.14 Pass 1 retired RenderTargetStore — its cleanTargets /
    // cleanTreeTargets / removeRenderTarget impls were the legacy
    // per-Layer BackendRenderTargetContext cache. The per-Visual RTC
    // is now owned by Compositor::nativeAttachedTrees_ and lives only
    // for the AppWindow's lifetime; the dead-layer scratch purge that
    // ran here was the only consumer of the per-LayerTree
    // bookkeeping.

}
