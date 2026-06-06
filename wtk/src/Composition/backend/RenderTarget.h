// Composition backend coordinator.
//
// Defines `BackendRenderTargetContext`, the per-Layer object that owns one
// "render target" worth of compositor state, plus the `RenderTargetStore`
// lookup map used by the compositor thread and the `BackendCompRenderTarget`
// / `ViewPresentTarget` plain-data structs that wire visual trees to native
// surfaces. The context owns:
//   - the native render target handle and its sizing (post-Phase-4
//     collapse: the dead offscreen pair / present blit / `BackingTextureSet`
//     have been deleted; sizing math now lives directly here)
//   - the per-frame fence
//   - the tessellation engine context bound to the native target
//   - the per-blurred-layer scratch surfaces
//   - the deferred buffer-release queue for buffer-pool reuse
//   - the per-element transform / opacity state
//
// `FrameRenderPass` (RenderPass.h) drives frame begin/end, viewport,
// pipeline-bind tracking, and the per-layer scratch redirect; the stateless
// `BackendCanvasEffectProcessor` (Effect.h) records the per-effect compute
// work for blur. Every GPU resource (pools, pipelines, fences, the shared
// effect processor) is vended by the process-wide
// `BackendResourceFactory`.

#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/Geometry.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "BlurScratch.h"
#include "RenderPass.h"
#include "Effect.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#ifndef OMEGAWTK_COMPOSITION_BACKEND_RENDERTARGETSTORE_H
#define OMEGAWTK_COMPOSITION_BACKEND_RENDERTARGETSTORE_H


namespace OmegaWTK::Composition {

    /// Tier 3 Phase 3.7: a single native-content carve-out recorded
    /// by `BackendRenderTargetContext::renderToTarget` for the
    /// platform tree to consume at flush time. `destRect` is in
    /// *backing pixel* coordinates (canvas-local origin + slice
    /// window offset, scaled by `renderScale`) so the platform tree
    /// can hand it straight to the native layer's geometry setter
    /// without re-doing the transform. `hostId` is the opaque
    /// identifier the NativeViewHost-Adoption pipeline assigns to
    /// each embedded native item; the platform tree uses it to look
    /// up the CALayer / DComp visual / Wayland subsurface that owns
    /// the region. `zOrderHint` is ascending = later / on-top.
    // §2.14 Pass 1 retired `BackendNativeContentRegion` and the
    // sibling `BackendVisualTree::applyNativeContentCarveouts` drain
    // hook. Pre-§2.14 the slice loop's `case PrimitiveOp::NativeContent`
    // recorded a region into `BackendRenderTargetContext::
    // pendingNativeContent_`; the compositor's frame worker drained it
    // after present and the per-platform tree translated each record
    // into a CALayer / DComp visual / X11 child window. The drain hook
    // never had a consumer (NativeViewHost-Adoption-Plan Phase V2/G2
    // would have populated the hostId → native-layer registry), so the
    // recording was dead infrastructure on every backend; §2.14 Pass 2
    // will use `Native::NativeContentNode` for the same purpose,
    // routed through `Native::VisualTree` directly without a per-frame
    // drain.

    enum class BackendSubmissionStatus : std::uint8_t {
        Completed,
        Error,
        Timeout,
        Dropped
    };

    struct BackendSubmissionTelemetry {
        std::uint64_t syncLaneId = 0;
        std::uint64_t syncPacketId = 0;
        std::chrono::steady_clock::time_point submitTimeCpu {};
        std::chrono::steady_clock::time_point completeTimeCpu {};
        std::chrono::steady_clock::time_point presentTimeCpu {};
        double gpuStartTimeSec = 0.0;
        double gpuEndTimeSec = 0.0;
        BackendSubmissionStatus status = BackendSubmissionStatus::Completed;
    };

    using BackendSubmissionCompletionHandler =
            std::function<void(const BackendSubmissionTelemetry &)>;

    class BackendRenderTargetContext {
        std::uint64_t traceResourceId = 0;
        SharedHandle<OmegaGTE::GEFence> fence;
        SharedHandle<OmegaGTE::GECommandQueue> commandQueue_;
        SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;

        // Native-target sizing (collapsed in from BackingTextureSet in
        // Phase 4.2). The logical rect is what the slice / canvas sees;
        // backingWidth/Height is the pixel resolution the GPU rasterizes
        // at (logical * renderScale, clamped to hardware limits).
        Composition::Rect renderTargetSize_;
        float    renderScale_   = 1.0f;
        unsigned backingWidth_  = 1;
        unsigned backingHeight_ = 1;
        SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> tessellationContext_;

#ifdef _WIN32
        // Win32 swap-chain resize is GPU-side work that must run on the
        // same thread as frame recording / commit / present, because
        // `IDXGISwapChain3::ResizeBuffers` releases the back-buffer
        // ID3D12Resources whose raw pointers the compositor worker's
        // command lists bake in at record time. Calling it from the GUI
        // thread (where `setRenderTargetSize` arrives via WM_SIZE →
        // syncNativePresentLayer) frees buffers the worker is mid-
        // recording against, which D3D12 catches as error 924
        // ("render target resource has been freed") and the nvidia
        // driver eventually segfaults on. The GUI thread stashes the
        // requested backing dims here; the worker thread drains them at
        // the top of `beginFrame` (single-owner of frame lifecycle, and
        // `resizeSwapChain`'s internal `commitToGPUAndWait` safely
        // drains the previous frame's submissions before releasing
        // back-buffers).
        std::atomic<bool>     pendingSwapChainResize_ {false};
        std::atomic<unsigned> pendingSwapChainW_      {0};
        std::atomic<unsigned> pendingSwapChainH_      {0};
        void applyPendingSwapChainResize();
#endif

        FrameRenderPass frameRenderPass_;
        SharedHandle<BackendCanvasEffectProcessor> imageProcessor;
        OmegaCommon::Vector<std::pair<SharedHandle<OmegaGTE::GEBuffer>,std::size_t>> deferredBufferReleases;
        OmegaGTE::FMatrix<4,4> currentTransform = OmegaGTE::FMatrix<4,4>::Identity();
        float currentOpacity = 1.f;
        /// Per-blurred-layer scratch surfaces, keyed by Layer*. Created on
        /// first blurred draw for a layer; resized when bounds change. Live
        /// for the lifetime of the context (compositor handles cleanup of
        /// dead layers via `RenderTargetStore::cleanTreeTargets`).
        OmegaCommon::Map<Layer *, std::unique_ptr<LayerBlurScratch>> layerScratches;

        /// Pure dimension math: sanitize the logical rect, clamp it to the
        /// engine's max texture dimension, and recompute backingWidth /
        /// backingHeight from it. Does not touch the GPU.
        void recomputeBackingDimensions();

        /// Re-acquire the tessellation engine context bound to the native
        /// target. Emits a `ResourceTrace::ResizeRebuild` event so memory
        /// inspection sees the rebuild boundary.
        void rebuildBackingTarget();

        /// Lazily acquire the tessellation engine context bound to the
        /// native target (Phase 6.8). Returns true on success, false if
        /// creation failed or there is no native target. Frames that
        /// only emit SDF primitives never call this and never allocate
        /// a tessellation context.
        bool ensureTessellationContext();

        /// Composite the per-layer blur scratch back onto the frame. Since
        /// the absolute-coords paint change (2026-05-29) the slice's ops are
        /// rendered into the (window-sized) scratch at their absolute window
        /// position, so the scratch is composited 1:1 at the origin over the
        /// full window — no per-slice offset. `destBounds` is the full-window
        /// slice bounds (the scratch / viewport extent).
        void compositeScratchOntoFrame(LayerBlurScratch & scratch,
                                       const Composition::Rect & destBounds);

        /// Emit a single SDF primitive draw call (Phase 6). The shape is
        /// described in shape-local coordinates: `cx, cy` is the center
        /// in logical (canvas) space; `halfW, halfH` are the half-extents
        /// of the actual silhouette; `cornerRadius` is the corner radius
        /// for `RoundedRect` / shadow-rounded base; `widthOrBlur` is the
        /// stroke width for fills (centered band, 0 = no stroke) or the
        /// blur extent for shadows. `kindCode` selects the SDF formula
        /// (0=Rect, 1=RoundedRect, 2=Ellipse, 3=ShadowRect/RoundedRect,
        /// 4=ShadowEllipse). Authors a 6-vertex unit-quad covering the
        /// silhouette plus AA / stroke / blur padding, plus a small
        /// per-draw uniform buffer carrying the flat shape parameters.
        void emitSdfPrimitive(float cx, float cy,
                              float halfW, float halfH,
                              float cornerRadius,
                              float widthOrBlur,
                              float kindCode,
                              OmegaGTE::FVec<4> fillColor,
                              OmegaGTE::FVec<4> strokeColor);

        /// Emit a single bitmap quad through the Phase 6.6 bitmap pipeline.
        /// `destRect` is the canvas-space destination; `(uMin,vMin)` /
        /// `(uMax,vMax)` are the texture sample coordinates for the dest
        /// rect's corners (already normalized to [0..1] by the caller —
        /// the caller computes them from an optional source-rect against
        /// the texture's pixel dimensions). `tint` is multiplied onto the
        /// sampled color; pass `(1,1,1,1)` for straight passthrough.
        /// The texture is an already-uploaded `GETexture` (typically from
        /// the process-wide `BitmapTextureCache`); `textureFence` is an
        /// optional fence to wait on before sampling.
        void emitBitmapPrimitive(const Composition::Rect & destRect,
                                 float uMin, float vMin,
                                 float uMax, float vMax,
                                 OmegaGTE::FVec<4> tint,
                                 SharedHandle<OmegaGTE::GETexture> texture,
                                 SharedHandle<OmegaGTE::GEFence> textureFence);

        /// Emit one MSDF text sub-run through the Phase 6.7.2 text
        /// pipeline (Phase 6.7-c3). Authors a 6-vertex quad per resident
        /// glyph against `subRun.resolvedFont`'s glyph atlas — glyphs
        /// not yet resident are rasterized on demand via
        /// `GlyphAtlas::ensureGlyph`; glyphs that fail to rasterize or
        /// pack are silently skipped (chunk-2 append-only atlas
        /// contract). All glyphs of the sub-run share one vertex buffer
        /// and one draw call. `rect.pos` offsets the layout-relative
        /// glyph positions into canvas space; `color` (× current
        /// opacity) rides a per-draw uniform buffer at fragment slot 13;
        /// the atlas texture binds at fragment slot 14.
        void emitTextSubRun(const Composition::TextSubRun & subRun,
                            const Composition::Rect & rect,
                            const Composition::Color & color);

        /// Tier 3 Phase 3.5: apply or clear the GPU scissor for the
        /// current draw scope, given the effective clip rect already
        /// resolved by `Canvas::pushClip`'s intersection stack.
        /// Translates the rect from canvas-local (= slice-window-
        /// local for the current slice) into target pixel space
        /// using the current viewport override and renderScale, then
        /// intersects with the slice's natural scissor. Empty Optional
        /// reinstates the natural scissor.
        void applySetClip(const Core::Optional<Composition::Rect> & clipRect);

        // §2.14 Pass 1 retired `pendingNativeContent_` and its
        // recording branch in `renderToTarget` (`case PrimitiveOp::
        // NativeContent` no longer feeds a per-frame queue here). The
        // alpha-clear + AABB-cull semantics of `DrawOp::NativeContent`
        // are preserved; only the carve-out producer is gone.
        // `Native::VisualTree`'s reconfigureContentNode (Pass 2) will
        // replace the role this queue would have played.

        /// Tier 4 §4.0: neutral primitive selector. Lets the per-primitive
        /// rasterization body be shared (as a template over the params
        /// struct type) between the `VisualCommand` and `DrawOp` dispatch
        /// overloads with zero duplicated SDF / tessellation / bitmap /
        /// text code — both param structs expose identically-named
        /// sub-structs (Tier 2 §2.0). Clip ops and `VectorPath` are *not*
        /// here: they access type-specific members and need stack / per-
        /// segment semantics, so the two overloads handle them directly.
        enum class PrimitiveOp : std::uint8_t {
            Rect, RoundedRect, Ellipse, Bitmap, Shadow, TextRun,
            SetTransform, SetOpacity, NativeContent
        };

        /// Shared rasterization body (was the switch in the old
        /// `renderToTarget(VisualCommand::Type,…)`). `params` points at a
        /// `VisualCommand::Params` or a `DrawOp::Params`; the body only
        /// touches sub-structs common to both. Self-contained SDF / bitmap
        /// / text / state ops return inside the switch; the gradient-fill
        /// Rect / RoundedRect cases fall through to `drawTriangulatedResult`.
        template<class ParamsT>
        void renderPrimitiveImpl(PrimitiveOp op, ParamsT *params);

        /// Shared draw tail: pipeline select + vertex authoring + draw for
        /// an already-triangulated mesh. Called by the gradient-fill
        /// primitives (`renderPrimitiveImpl`) and once per segment by
        /// `renderVectorPathSegmented`. (Was the post-switch body of the
        /// old `renderToTarget`.)
        void drawTriangulatedResult(OmegaGTE::TETriangulationResult & result,
                                    bool useTextureRenderPipeline,
                                    bool usePathRenderPipeline,
                                    float textureCoordDenomW,
                                    float textureCoordDenomH,
                                    const OmegaGTE::FVec<4> & pathStrokeColor,
                                    bool pathHasStrokeColor,
                                    const OmegaGTE::FVec<4> & pathFillColor,
                                    bool pathHasFillColor,
                                    SharedHandle<OmegaGTE::GETexture> texturePaint,
                                    SharedHandle<OmegaGTE::GEFence> textureFence);

        /// Triangulate + draw one path segment (the former `VectorPath`
        /// case body, taking concrete args). The `VisualCommand::VectorPath`
        /// case calls it once with its already-segmented params; the
        /// `DrawOp::VectorPath` case calls it once per segment produced by
        /// `Path::decomposeForDraw` (Tier 4 §4.0 — the decomposition that
        /// lived in `Canvas::drawPath`).
        void renderVectorPathSegmented(
            const Core::SharedPtr<OmegaGTE::GVectorPath2D> & path,
            float strokeWidth, bool contour, bool fill,
            const Core::SharedPtr<Brush> & strokeBrush,
            const Core::SharedPtr<Brush> & fillBrush);

        /// Tier 4 §4.0: backend-owned clip stack for the `DrawOp` path.
        /// Replaces the intersection bookkeeping `Canvas::pushClip` /
        /// `popClip` owned (Canvas is deleted in 4.2). `pushDrawOpClip`
        /// intersects the incoming rect with the current top and applies
        /// the effective scissor via `applySetClip`; `popDrawOpClip`
        /// restores the prior top (or the natural scissor when empty).
        void pushDrawOpClip(const Composition::Rect & rect);
        void popDrawOpClip();
        OmegaCommon::Vector<Composition::Rect> drawOpClipStack_;
    public:
        /// Open a frame-level render pass that clears to the given color.
        /// All subsequent renderToTarget() calls record into this pass.
        void beginFrame(float clearR, float clearG, float clearB, float clearA);
        /// Close the frame-level render pass and submit the command buffer.
        void endFrame();
        /// Tier 4: the sole GPU dispatch entry point — one arm per
        /// `DrawOp::Type`. Shares all rasterization across variants via
        /// `renderPrimitiveImpl` (the 9 shape/state variants),
        /// `applySetClip` + the backend clip stack (`PushClip`/`PopClip`),
        /// and `renderVectorPathSegmented` (`VectorPath`). (The
        /// `VisualCommand` overload was deleted in 4.2.)
        void renderToTarget(DrawOp::Type type,void *params);
        /// Reset the per-element transform and opacity so the next slice or
        /// frame starts from identity / opaque. The compositor calls this at
        /// each slice boundary so a `SetTransform` / `SetOpacity` left
        /// dangling at the end of one slice does not bleed into the next.
        void resetElementState();
        /// Render a slice whose target layer carries blur. Routes the
        /// slice's primitives through a per-layer scratch surface, applies
        /// the layer's blur effects via the GPU effect processor, and
        /// composites the result onto the frame's command buffer at the
        /// slice's window position. Falls back to the direct path when the
        /// scratch can't be allocated.
        void renderBlurredSlice(const CompositeFrame::WidgetSlice & slice);
        /// Drop any per-layer scratch entries owned by this context whose
        /// keys are not present in `liveLayers`. Called by the compositor's
        /// tree-cleanup path when layers are removed.
        void purgeDeadLayerScratches(const OmegaCommon::Vector<Layer *> & liveLayers);
        void setRenderTargetSize(Composition::Rect &rect);
        void setViewportOverride(float offsetX, float offsetY, float width, float height);
        void clearViewportOverride();
        SharedHandle<OmegaGTE::GENativeRenderTarget> & getNativeRenderTarget(){ return renderTarget; }
        SharedHandle<OmegaGTE::GECommandQueue> & commandQueue(){ return commandQueue_; }
        SharedHandle<OmegaGTE::GEFence> & getFence(){ return fence; }
        unsigned getBackingWidth()  const { return backingWidth_; }
        unsigned getBackingHeight() const { return backingHeight_; }
        float renderScale() const { return renderScale_; }
        const Composition::Rect & renderTargetSize() const { return renderTargetSize_; }
        SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> & tessellationContext(){ return tessellationContext_; }
        void releaseDeferredBuffers();

        // §2.14 Pass 1 retired the carve-out drain accessors —
        // `pendingNativeContent()` / `clearPendingNativeContent()` —
        // alongside `pendingNativeContent_` itself. See the comment
        // at `BackendNativeContentRegion`'s former definition above.
#ifdef _WIN32
        /// Resize swap chain after waiting for GPU; use instead of calling ResizeBuffers on the swap chain directly.
        void resizeSwapChain(unsigned int backingWidth, unsigned int backingHeight);
        /// Wait for this context's native target GPU work to complete. Call after commit() to avoid cross-context texture pool races.
        void waitForGPU();
#endif
        /**
         Commit all queued render jobs to GPU.
        */
        void commit();
        void commit(std::uint64_t syncLaneId,
                    std::uint64_t syncPacketId,
                    std::chrono::steady_clock::time_point submitTimeCpu,
                    BackendSubmissionCompletionHandler completionHandler);

        /**
            Create a BackendRenderTarget Context
            @param renderTarget
        */
        explicit BackendRenderTargetContext(Composition::Rect & rect,
                                            SharedHandle<OmegaGTE::GENativeRenderTarget> & renderTarget,
                                            SharedHandle<OmegaGTE::GECommandQueue> commandQueue,
                                            float renderScale = 1.0f);
        ~BackendRenderTargetContext();

        // Non-copyable, non-movable: FrameRenderPass holds a reference back
        // into this object. Copying or moving would silently dangle that
        // reference against the source's storage.
        BackendRenderTargetContext(const BackendRenderTargetContext &) = delete;
        BackendRenderTargetContext & operator=(const BackendRenderTargetContext &) = delete;
        BackendRenderTargetContext(BackendRenderTargetContext &&) = delete;
        BackendRenderTargetContext & operator=(BackendRenderTargetContext &&) = delete;
    };

    // §2.14 Pass 1 retired:
    //   - `BackendVisualTree` (the legacy abstract tree; replaced by
    //     `Native::VisualTree`)
    //   - `ViewPresentTarget` (no consumer once `createVisualTreeForView`
    //     went)
    //   - `BackendCompRenderTarget` (replaced by
    //     `Compositor::NativeAttachedTree` keyed by render target)
    //   - `RenderTargetStore` / `cleanTreeTargets` (per-Layer surface
    //     cache built by the same legacy fallback)

};

#endif
