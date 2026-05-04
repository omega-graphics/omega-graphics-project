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
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/Geometry.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "BlurScratch.h"
#include "RenderPass.h"
#include "Effect.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#ifndef OMEGAWTK_COMPOSITION_BACKEND_RENDERTARGETSTORE_H
#define OMEGAWTK_COMPOSITION_BACKEND_RENDERTARGETSTORE_H


namespace OmegaWTK::Composition {

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

        void compositeScratchOntoFrame(LayerBlurScratch & scratch,
                                       const Composition::Rect & destBounds,
                                       const Composition::Point2D & windowOffset);
    public:
        /// Open a frame-level render pass that clears to the given color.
        /// All subsequent renderToTarget() calls record into this pass.
        void beginFrame(float clearR, float clearG, float clearB, float clearA);
        /// Close the frame-level render pass and submit the command buffer.
        void endFrame();
        void renderToTarget(VisualCommand::Type type,void *params);
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
        SharedHandle<OmegaGTE::GEFence> & getFence(){ return fence; }
        unsigned getBackingWidth()  const { return backingWidth_; }
        unsigned getBackingHeight() const { return backingHeight_; }
        float renderScale() const { return renderScale_; }
        const Composition::Rect & renderTargetSize() const { return renderTargetSize_; }
        SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> & tessellationContext(){ return tessellationContext_; }
        void releaseDeferredBuffers();
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

    class BackendVisualTree;

    /// Construction-time output for the native render target created by
    /// makeRootVisual().  With Phase A-1 the native target is also passed
    /// into BackendRenderTargetContext, so this struct is retained only for
    /// the visual tree creation API.  It will be removed with Phase B.
    struct ViewPresentTarget {
        SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget;
        unsigned backingWidth = 1;
        unsigned backingHeight = 1;
    };

    struct BackendCompRenderTarget {
        SharedHandle<BackendVisualTree> visualTree;
        OmegaCommon::Map<Layer *,BackendRenderTargetContext *> surfaceTargets;
        ViewPresentTarget viewPresentTarget;
    };

    struct RenderTargetStore {
     private:
        void cleanTargets(LayerTree *tree);
    public:
        void cleanTreeTargets(LayerTree *tree);
        void removeRenderTarget(const SharedHandle<CompositionRenderTarget> & target);
        OmegaCommon::Map<SharedHandle<CompositionRenderTarget>,BackendCompRenderTarget> store = {};
    };

};

#endif
