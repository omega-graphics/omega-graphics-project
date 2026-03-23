#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
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
                                         OmegaCommon::Vector<CanvasEffect> & effects) ABSTRACT;
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
        Core::Rect renderTargetSize;
        float renderScale = 1.0f;
        unsigned backingWidth = 1;
        unsigned backingHeight = 1;
        OmegaCommon::Vector<CanvasEffect> effectQueue;
        OmegaCommon::Vector<std::pair<SharedHandle<OmegaGTE::GEBuffer>,std::size_t>> deferredBufferReleases;
        SharedHandle<OmegaGTE::GETexture> committedTexture;
        void rebuildBackingTarget();
        void createGradientTexture(bool linearOrRadial,Gradient & gradient,OmegaGTE::GRect & rect,SharedHandle<OmegaGTE::GETexture> & dest);
    public:
        bool hasPendingContent = false;
        void clear(float r,float g,float b,float a);
        void renderToTarget(VisualCommand::Type type,void *params);
        void applyEffectToTarget(const CanvasEffect & effect);
        void setRenderTargetSize(Core::Rect &rect);
        SharedHandle<OmegaGTE::GENativeRenderTarget> & getNativeRenderTarget(){ return renderTarget; }
        SharedHandle<OmegaGTE::GEFence> & getFence(){ return fence; }
        SharedHandle<OmegaGTE::GETexture> getCommittedTexture(){ return committedTexture; }
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
        explicit BackendRenderTargetContext(Core::Rect & rect,
                                            SharedHandle<OmegaGTE::GENativeRenderTarget> & renderTarget,
                                            float renderScale = 1.0f);
        ~BackendRenderTargetContext();
    };

    class BackendVisualTree;



    struct BackendCompRenderTarget {
        SharedHandle<BackendVisualTree> visualTree;
        OmegaCommon::Map<Layer *,BackendRenderTargetContext *> surfaceTargets;
        bool needsPresent = false;
    };



    void compositeAndPresentTarget(BackendCompRenderTarget & compTarget);

    struct RenderTargetStore {
     private:
        void cleanTargets(LayerTree *tree,LayerTree::Limb *limb);
    public:
        void cleanTreeTargets(LayerTree *tree);
        void removeRenderTarget(const SharedHandle<CompositionRenderTarget> & target);
        void presentAllPending();
        OmegaCommon::Map<SharedHandle<CompositionRenderTarget>,BackendCompRenderTarget> store = {};
    };

};

#endif
