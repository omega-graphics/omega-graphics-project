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
        SharedHandle<OmegaGTE::GETexture> targetTexture;
        SharedHandle<OmegaGTE::GETexture> effectTexture;
        SharedHandle<OmegaGTE::GEFence> fence;
        SharedHandle<OmegaGTE::GETextureRenderTarget> preEffectTarget;
        SharedHandle<OmegaGTE::GETextureRenderTarget> effectTarget;
        SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
        SharedHandle<OmegaGTE::OmegaTessellationEngineContext> tessellationEngineContext;
        SharedHandle<BackendCanvasEffectProcessor> imageProcessor;
        Core::Rect renderTargetSize;
        float renderScale = 1.0f;
        unsigned backingWidth = 1;
        unsigned backingHeight = 1;
        OmegaCommon::Vector<CanvasEffect> effectQueue;
        void rebuildBackingTarget();
        void createGradientTexture(bool linearOrRadial,Gradient & gradient,OmegaGTE::GRect & rect,SharedHandle<OmegaGTE::GETexture> & dest);
    public:
        void clear(float r,float g,float b,float a);
        void renderToTarget(VisualCommand::Type type,void *params);
        void applyEffectToTarget(const CanvasEffect & effect);
        void setRenderTargetSize(Core::Rect &rect);
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
    };

    class BackendVisualTree;



    struct BackendCompRenderTarget {
        SharedHandle<BackendVisualTree> visualTree;
        OmegaCommon::Map<Layer *,BackendRenderTargetContext *> surfaceTargets;
    };



    struct RenderTargetStore {
     private:
        void cleanTargets(LayerTree *tree,LayerTree::Limb *limb);
    public:
        void cleanTreeTargets(LayerTree *tree);
        OmegaCommon::Map<SharedHandle<CompositionRenderTarget>,BackendCompRenderTarget> store = {};
    };

};

#endif
