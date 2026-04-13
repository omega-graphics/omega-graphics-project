#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "backend/RenderTarget.h"

#ifndef OMEGAWTK_COMPOSTION_COMPOSITOR_H
#define OMEGAWTK_COMPOSTION_COMPOSITOR_H

namespace OmegaWTK::Composition {
    struct CanvasEffect;

   class CompositorScheduler {
       Compositor *compositor;
   public:
        bool shutdown;

        void shutdownAndJoin();
        void processCommand(SharedHandle<CompositorCommand> & command,bool laneAdmissionBypassed = false);
        std::thread t;

       explicit CompositorScheduler(Compositor *compositor);
       ~CompositorScheduler();
   };


   struct CompareCommands {
        auto operator()(SharedHandle<Composition::CompositorCommand> & lhs,
                        SharedHandle<Composition::CompositorCommand> & rhs){
           // View commands first (highest structural priority)
           if(lhs->type == CompositorCommand::View && rhs->type != CompositorCommand::View) return true;
           if(rhs->type == CompositorCommand::View && lhs->type != CompositorCommand::View) return false;
           // Fallback deterministic ordering: compare types
           return static_cast<int>(lhs->type) < static_cast<int>(rhs->type);
        };
    };


    /**
     OmegaWTK's Composition Engine Frontend Interface
     */
    class Compositor : public LayerTreeObserver {

        OmegaCommon::Vector<LayerTree *> targetLayerTrees;

        RenderTargetStore renderTargetStore;

        std::mutex mutex;

        bool queueIsReady;

        std::condition_variable queueCondition;
        OmegaCommon::PriorityQueueHeap<SharedHandle<CompositorCommand>,CompareCommands> commandQueue;

        friend class CompositorClientProxy;
        friend class CompositorScheduler;

        SharedHandle<CompositorCommand> currentCommand;

        CompositorScheduler scheduler;

        /// Per-window surface mailboxes. Keyed by SharedHandle so the
        /// render target stays alive for as long as the compositor knows
        /// about it (matches RenderTargetStore keying).
        OmegaCommon::Map<SharedHandle<CompositionRenderTarget>,SharedHandle<CompositorSurface>> windowSurfaces_;

        /// Phase B frame trigger. Set by deposit callback, cleared at
        /// the top of each scheduler iteration before draining surfaces.
        std::atomic<bool> frameDirty_ {false};

        /// Wake the scheduler thread to drain window surfaces.
        void notifyFrameDirty();

        /// Drain all registered window surfaces and render any pending
        /// composite frame. Called from the scheduler loop on wake.
        void drainWindowSurfaces();

        /// Render a composite frame consumed from a window surface into
        /// the target's root visual.
        void renderCompositeFrame(const SharedHandle<CompositionRenderTarget> & target,
                                  const SharedHandle<CompositeFrame> & frame);

        friend class Layer;
        friend class LayerTree;
        friend class ::OmegaWTK::AppWindow;

        void executeCurrentCommand();
        void onQueueDrained();

    public:
        /// Stub retained for Animation API compatibility. Real lane
        /// telemetry was removed with the queue-based render path —
        /// this returns a default-constructed snapshot.
        struct LaneTelemetrySnapshot {
            std::uint64_t syncLaneId = 0;
            std::uint64_t firstPresentedPacketId = 0;
            std::uint64_t lastSubmittedPacketId = 0;
            std::uint64_t lastPresentedPacketId = 0;
            std::uint64_t droppedPacketCount = 0;
            std::uint64_t failedPacketCount = 0;
            unsigned inFlight = 0;
            bool startupStabilized = false;
        };

        LaneTelemetrySnapshot getLaneTelemetrySnapshot(std::uint64_t syncLaneId) const;

        void observeLayerTree(LayerTree *tree,std::uint64_t syncLaneId = 0);
        void unobserveLayerTree(LayerTree *tree);

        void registerWindowSurface(SharedHandle<CompositionRenderTarget> target,
                                   SharedHandle<CompositorSurface> surface);

        void scheduleCommand(SharedHandle<CompositorCommand> & command);

        void hasDetached(LayerTree *tree) override;
        void layerHasResized(Layer *layer) override;
        void layerHasDisabled(Layer *layer) override;
        void layerHasEnabled(Layer *layer) override;


        Compositor();
        ~Compositor();
    };

    struct BackendExecutionContext {

    };
}

#endif
