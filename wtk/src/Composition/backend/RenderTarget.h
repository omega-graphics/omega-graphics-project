#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Core/GTEHandle.h"
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

    INTERFACE BackendCanvasEffectProcessor {
    public:
        explicit BackendCanvasEffectProcessor(SharedHandle<OmegaGTE::GEFence> & fence):fence(fence){

        };
        SharedHandle<OmegaGTE::GEFence> fence;
      INTERFACE_METHOD void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,
                                         SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,
                                         OmegaCommon::Vector<CanvasEffect> & effects,
                                         unsigned texWidth,
                                         unsigned texHeight) ABSTRACT;
      static SharedHandle<BackendCanvasEffectProcessor> Create(SharedHandle<OmegaGTE::GEFence> & fence);
      virtual ~BackendCanvasEffectProcessor() = default;
    };

    class BackendRenderTargetContext {
        std::uint64_t traceResourceId = 0;
        SharedHandle<OmegaGTE::GETexture> targetTexture;
        SharedHandle<OmegaGTE::GETexture> effectTexture;
        SharedHandle<OmegaGTE::GEFence> fence;
        SharedHandle<OmegaGTE::GETextureRenderTarget> preEffectTarget;
        SharedHandle<OmegaGTE::GETextureRenderTarget> effectTarget;
        SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
        SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> tessellationEngineContext;
        SharedHandle<BackendCanvasEffectProcessor> imageProcessor;
        Composition::Rect renderTargetSize;
        float renderScale = 1.0f;
        unsigned backingWidth = 1;
        unsigned backingHeight = 1;
        OmegaCommon::Vector<CanvasEffect> effectQueue;
        OmegaCommon::Vector<std::pair<SharedHandle<OmegaGTE::GEBuffer>,std::size_t>> deferredBufferReleases;
        OmegaGTE::FMatrix<4,4> currentTransform = OmegaGTE::FMatrix<4,4>::Identity();
        float currentOpacity = 1.f;
        struct ViewportOverride {
            bool active = false;
            float offsetX = 0.f;
            float offsetY = 0.f;
            float width = 0.f;
            float height = 0.f;
        };
        ViewportOverride viewportOverride_;
        SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> frameCB_;
        bool frameActive_ = false;
        bool lastPipelineWasTexture_ = false;
        bool renderingToNative_ = false;
        void rebuildBackingTarget();
        void createGradientTexture(bool linearOrRadial,Gradient & gradient,OmegaGTE::GRect & rect,SharedHandle<OmegaGTE::GETexture> & dest);
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
        unsigned getBackingWidth() const { return backingWidth; }
        unsigned getBackingHeight() const { return backingHeight; }
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
