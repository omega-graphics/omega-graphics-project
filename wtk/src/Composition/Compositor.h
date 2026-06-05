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
#include "omegaWTK/Native/NativeVisualTree.h"

#include <unordered_map>

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

        std::mutex mutex;
        std::condition_variable queueCondition;

        friend class CompositorClientProxy;
        friend class CompositorFrameWorker;

        CompositorFrameWorker frameWorker;

        /// Per-window surface mailboxes. Keyed by SharedHandle so the
        /// render target stays alive for as long as the compositor knows
        /// about it (matches RenderTargetStore keying).
        OmegaCommon::Map<SharedHandle<CompositionRenderTarget>,SharedHandle<CompositorSurface>> windowSurfaces_;

        /// §2.14 Pass 1 — per-window attached visual tree (Native side)
        /// + its lazily-bound root render-target context.
        ///
        /// Replaces `PreCreatedResourceRegistry` for backends that have
        /// migrated to `Native::VisualTree`. macOS and Win32 still use
        /// the legacy path until their §2.14 migration lands; the
        /// renderer checks this map first and falls back to the legacy
        /// `PreCreatedResourceRegistry` lookup on miss.
        ///
        /// `rootContext` is built lazily on first frame: the binder
        /// (`tryBindRootVisual`) returns nullptr until the platform
        /// surface is realized (Linux X11 Window not yet live), at
        /// which point the compositor retries on every frame. This
        /// mirrors the pre-§2.14 `resolveDeferredNativeTarget` loop.
        struct NativeAttachedTree {
            SharedHandle<Native::VisualTree> tree;
            std::unique_ptr<BackendRenderTargetContext> rootContext;
        };
        std::unordered_map<CompositionRenderTarget *, NativeAttachedTree> nativeAttachedTrees_;

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

        /// §2.14 Pass 1 — attach a per-window `Native::VisualTree` to
        /// the compositor. AppWindow calls this from `setRootWidget`
        /// after constructing the tree via `Native::make_native_visual_tree`.
        /// The compositor stores the tree and lazily binds its root
        /// visual to a `BackendRenderTargetContext` on first render
        /// (via the per-backend `tryBindRootVisual`).
        ///
        /// Replaces `PreCreatedResourceRegistry::store` for migrated
        /// backends. Single attach per render target; re-attaching
        /// replaces.
        void attachVisualTree(SharedHandle<Native::VisualTree> tree,
                              SharedHandle<CompositionRenderTarget> target);

        /// §2.14 Pass 1 — drop the attached tree + its RTC. Called
        /// from `~AppWindow` before the tree handle drops so the
        /// compositor releases its RTC (which holds GTE resources)
        /// while GTE is still live.
        void detachVisualTree(CompositionRenderTarget * target);

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
