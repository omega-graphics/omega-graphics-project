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

    /// Frame worker thread. Sleeps on the compositor's queueCondition
    /// until either shutdown is requested or frameDirty_ is set by a
    /// CompositorSurface deposit. On wake, drains all registered
    /// window surfaces.
    class CompositorFrameWorker {
        Compositor *compositor;
    public:
        bool shutdown;
        std::thread t;

        void shutdownAndJoin();

        explicit CompositorFrameWorker(Compositor *compositor);
        ~CompositorFrameWorker();
    };


    /**
     OmegaWTK's Composition Engine Frontend Interface
     */
    /// Shut down the process-wide Compositor (the Meyers singleton
    /// returned by globalCompositor() inside WidgetTreeHost.cpp).
    /// Calls Compositor::shutdown() — see the doc on that method for
    /// the failure mode this prevents. Called from AppInst::doShutdown
    /// between closeAllWindows() and CleanupEngine().
    OMEGAWTK_EXPORT void shutdownGlobalCompositor();

    class Compositor : public LayerTreeObserver {

        OmegaCommon::Vector<LayerTree *> targetLayerTrees;

        RenderTargetStore renderTargetStore;

        std::mutex mutex;
        std::condition_variable queueCondition;

        friend class CompositorClientProxy;
        friend class CompositorFrameWorker;

        CompositorFrameWorker frameWorker;

        /// Per-window surface mailboxes. Keyed by SharedHandle so the
        /// render target stays alive for as long as the compositor knows
        /// about it (matches RenderTargetStore keying).
        OmegaCommon::Map<SharedHandle<CompositionRenderTarget>,SharedHandle<CompositorSurface>> windowSurfaces_;

        /// Frame trigger. Set by deposit callback, cleared at the top
        /// of each frame worker iteration before draining surfaces.
        std::atomic<bool> frameDirty_ {false};

        /// Wake the frame worker thread.
        void notifyFrameDirty();

        /// Drain all registered window surfaces and render any pending
        /// composite frame. Called from the frame worker on wake.
        void drainWindowSurfaces();

        /// Render a composite frame consumed from a window surface into
        /// the target's root visual.
        void renderCompositeFrame(const SharedHandle<CompositionRenderTarget> & target,
                                  const SharedHandle<CompositeFrame> & frame);

        friend class Layer;
        friend class LayerTree;
        friend class ::OmegaWTK::AppWindow;

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

        /// Tear down the compositor's GE-holding state without
        /// destroying the object itself. Mirrors ~Compositor (stops
        /// the frame worker, waits for GPU idle, drops observed
        /// LayerTrees, clears renderTargetStore) AND empties
        /// windowSurfaces_. Required by AppInst::doShutdown because
        /// the compositor is a Meyers singleton (see
        /// globalCompositor() in WidgetTreeHost.cpp) whose
        /// destructor would otherwise run at atexit, AFTER
        /// OmegaGTE::Close has nulled out gte.graphicsEngine — at
        /// which point releasing the held SharedHandle<GEBuffer> /
        /// SharedHandle<GETexture> entries would Release D3D12MA
        /// allocations against a freed allocator and corrupt the
        /// heap. Safe to call once; subsequent calls just see empty
        /// state and no-op.
        void shutdown();

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
