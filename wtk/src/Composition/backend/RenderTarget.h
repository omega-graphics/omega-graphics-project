// Composition backend coordinator.
//
// Defines `BackendRenderTargetContext`, the per-Layer object that owns one
// "render target" worth of compositor state, plus the `RenderTargetStore`
// lookup map used by the compositor thread and the `BackendCompRenderTarget`
// / `ViewPresentTarget` plain-data structs that wire visual trees to native
// surfaces. The context itself only holds the pieces unique to one logical
// surface (fence, native target handle, effect queue, transform/opacity,
// trace id, deferred buffer-release queue) and delegates everything else to:
//
//   - `BackingTextureSet` (Texture.h)        — offscreen pair, present blit,
//                                              gradient texture upload
//   - `FrameRenderPass`   (RenderPass.h)     — frame begin/end, viewport,
//                                              pipeline-bind tracking,
//                                              standalone-CB fallback
//   - `BackendCanvasEffectProcessor` (Effect.h) — per-effect compute work
//
// Every GPU resource (pools, pipelines, fences, effect processors) is
// vended by the process-wide `BackendResourceFactory`.

#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "Texture.h"
#include "RenderPass.h"
#include "Effect.h"
#include <chrono>
#include <cstdint>
#include <functional>

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
        BackingTextureSet textures_;
        FrameRenderPass frameRenderPass_;
        SharedHandle<BackendCanvasEffectProcessor> imageProcessor;
        OmegaCommon::Vector<CanvasEffect> effectQueue;
        OmegaCommon::Vector<std::pair<SharedHandle<OmegaGTE::GEBuffer>,std::size_t>> deferredBufferReleases;
        OmegaGTE::FMatrix<4,4> currentTransform = OmegaGTE::FMatrix<4,4>::Identity();
        float currentOpacity = 1.f;
        void rebuildBackingTarget();
    public:
        void clear(float r,float g,float b,float a);
        /// Open a frame-level render pass that clears to the given color.
        /// All subsequent renderToTarget() calls record into this pass.
        void beginFrame(float clearR, float clearG, float clearB, float clearA);
        /// Close the frame-level render pass and submit the command buffer.
        void endFrame();
        void renderToTarget(VisualCommand::Type type,void *params);
        void applyEffectToTarget(const CanvasEffect & effect);
        void setRenderTargetSize(Composition::Rect &rect);
        void setViewportOverride(float offsetX, float offsetY, float width, float height);
        void clearViewportOverride();
        SharedHandle<OmegaGTE::GENativeRenderTarget> & getNativeRenderTarget(){ return renderTarget; }
        SharedHandle<OmegaGTE::GEFence> & getFence(){ return fence; }
        unsigned getBackingWidth()  const { return textures_.backingWidth(); }
        unsigned getBackingHeight() const { return textures_.backingHeight(); }
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

        // Non-copyable, non-movable: FrameRenderPass and the texture set hold
        // references back into this object. Copying or moving would silently
        // dangle those references against the source's storage.
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
