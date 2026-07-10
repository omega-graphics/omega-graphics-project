#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeTimer.h"   // §2.3a T1: tooltip hover-delay timer
#include "omegaWTK/UI/OverlayHost.h"       // §2.3a T1: OverlayHandle for the tooltip
#include <type_traits>
#include <cstdint>
#include <chrono>

#ifndef OMEGAWTK_UI_WIDGETTREEHOST_H
#define OMEGAWTK_UI_WIDGETTREEHOST_H

namespace OmegaWTK {

    namespace Native {
        class NativeItem;
        typedef SharedHandle<NativeItem> NativeItemPtr;
    }

    namespace Composition {
        class Compositor;
        class CompositorSurface;
        struct CompositeFrame;
        class ViewRenderTarget;
        struct ResizeGovernorMetadata;
    }


    class AppWindow;
    OMEGACOMMON_SHARED_CLASS(AppWindow);
    class Widget;
    OMEGACOMMON_SHARED_CLASS(Widget);
    class View;
    class NativeViewHost;
    class FrameBuilder;
    class OverlayHost;
    class FocusManager;
    enum class PaintReason : std::uint8_t;

    struct OMEGAWTK_EXPORT ResizeDynamicsSample {
        double timestampMs = 0.0;
        float width = 0.f;
        float height = 0.f;
        float velocityPxPerSec = 0.f;
        float accelerationPxPerSec2 = 0.f;
    };

    enum class OMEGAWTK_EXPORT ResizePhase : std::uint8_t {
        Idle,
        Active,
        Settling,
        Completed
    };

    struct OMEGAWTK_EXPORT ResizeSessionState {
        std::uint64_t sessionId = 0;
        ResizePhase phase = ResizePhase::Idle;
        ResizeDynamicsSample sample {};
        bool animatedTree = false;
    };

    class OMEGAWTK_EXPORT ResizeDynamicsTracker {
        std::uint64_t currentSessionId = 0;
        bool inSession = false;
        float lastWidth = 0.f;
        float lastHeight = 0.f;
        float lastVelocity = 0.f;
        double lastSignificantChangeMs = 0.0;
        std::chrono::steady_clock::time_point lastTick {};
        static std::uint64_t nextSessionId();
    public:
        ResizeSessionState begin(float width,float height,double tMs);
        ResizeSessionState update(float width,float height,double tMs);
        ResizeSessionState end(float width,float height,double tMs);
        bool active() const { return inSession; }
    };
    /**
     @brief Owns a widget tree. (Owns the Widget tree's Compositor, and the Compositor's Scheduler)
     @paragraph An instance of this class gets attached to an AppWindow directly and the root widget
     the host is assigned to the AppWindow.
     NOTE: The first WidgetTreeHost attached to an AppWindow provides the
     Compositor that manages composition for the window's single surface.
    */
    class OMEGAWTK_EXPORT WidgetTreeHost {
        /** The Widget Tree's Compositor.
         NOTE: The instance of this class that was first attached to an
         AppWindow provides the Compositor for the window's single surface.
         */
        Composition::Compositor * compositor;
        uint64_t syncLaneId;
        /// The Root Widget
        WidgetPtr root;
        /// Window's shared render target (Phase 3). Propagated to all
        /// Views in the tree during initWidgetTree().
        SharedHandle<Composition::ViewRenderTarget> windowRenderTarget_;
        /// Root native item for this window (Phase 5). Used by
        /// NativeViewHost to embed/unembed real native views.
        Native::NativeItemPtr rootNativeItem_;
        ResizeDynamicsTracker resizeTracker;
        ResizeSessionState lastResizeSessionState {};
        std::uint64_t resizeCoordinatorGeneration = 0;
        /// Phase G.5.4: true between `notifyWindowResizeBegin` and
        /// `notifyWindowResizeEnd` — a live resize drag is in progress.
        /// `FrameBuilder` reads it (via `isResizing()`) to mark each cached
        /// View's capture marker `dragActive`, enabling the backend's
        /// stretch-instead-of-re-render fast path.
        bool resizing_ = false;
        struct ResizeValidationSession {
            bool active = false;
            std::uint64_t sessionId = 0;
            std::uint32_t sampleCount = 0;
            double beginTimestampMs = 0.0;
            double endTimestampMs = 0.0;
            float peakVelocityPxPerSec = 0.f;
            float peakAccelerationPxPerSec2 = 0.f;
            std::uint64_t baseSubmittedPackets = 0;
            std::uint64_t basePresentedPackets = 0;
            std::uint64_t baseDroppedPackets = 0;
            std::uint64_t baseFailedPackets = 0;
            std::uint64_t baseEpochDrops = 0;
            std::uint64_t baseStaleCoordinatorPackets = 0;
        };
        ResizeValidationSession resizeValidationSession {};

        /// Per-window surface mailbox (Phase A). Created by AppWindow.
        SharedHandle<Composition::CompositorSurface> windowSurface_;
        /// Tier 3 Phase 3.8: the window's frame driver, set by AppWindow
        /// after construction. Pre-D1 `Widget::executePaint` opened a
        /// `FrameBuilder::ScopedFrame` per immediate paint; Tier D / D1
        /// (2026-06-03) deleted executePaint, so the only ScopedFrame
        /// brackets today live on `AppWindow::flushFrame` (deferred path)
        /// and `WidgetTreeHost::paintDirty` (immediate path).
        FrameBuilder * frameBuilder_ = nullptr;
        /// Widget-View-Paint-Lifecycle-Plan Tier A: owning window, set
        /// by AppWindow alongside the frame builder. requestFrame()
        /// routes through it to the native run-loop coalescing hook.
        AppWindow * ownerWindow_ = nullptr;

        bool attachedToWindow;
        View * hoveredView_ = nullptr;

        /// Overlay-Z-Order-Plan O1 — in-window overlay slot. One per
        /// host (i.e. one per AppWindow / AppPanel). Constructed in
        /// the host ctor so `overlayHost()` is always live; destroyed
        /// before the host's own members so dismissAll's WidgetPtr
        /// releases run before the host tears down.
        Core::UniquePtr<OverlayHost> overlayHost_;

        /// §2.3a F2 — the per-window keyboard-focus authority. One per
        /// host, constructed in the host ctor so `focusManager()` is
        /// always live (same lifetime pattern as `overlayHost_`). The
        /// View tree reaches it via the host pointer threaded by
        /// `View::setTreeHostRecurse`; the input dispatcher (F3/M1) and
        /// `View::focus`/`blur` are its writers.
        Core::UniquePtr<FocusManager> focusManager_;

        /// §2.3a T1 — tooltip dispatcher state. The tooltip is a
        /// dispatcher-owned overlay: on a hover that settles over a
        /// widget with a non-empty `tooltip()`, `tooltipTimer_` arms a
        /// one-shot hover delay; when it fires, a small text overlay is
        /// presented at `lastCursorPos_ + {12,12}` (tier `Tooltip`,
        /// `absorbsHits = false`) and its handle stored in
        /// `currentTooltipHandle_`. `tooltipWidget_` is the widget the
        /// tooltip is pending / shown for — used to detect when the
        /// hovered widget changes so the tooltip re-arms. Any of hover
        /// change / cursor exit / mouse-down / key-down cancels it.
        OverlayHandle currentTooltipHandle_ {};
        Widget * tooltipWidget_ = nullptr;
        Native::NativeTimerPtr tooltipTimer_;
        Composition::Point2D lastCursorPos_ {0.f, 0.f};

        WidgetTreeHost();

        friend class AppWindowManager;
        friend class AppWindow;
        friend class Widget;
        friend class NativeViewHost;
        /// Overlay-Z-Order-Plan O1 — `OverlayHost::Impl` reads
        /// `ownerWindow_` (for the AppWindow's logical size used in
        /// edge-clamping) and `root` (fallback when no AppWindow is
        /// attached). Adding accessors for this single internal
        /// consumer would expand the public surface without benefit.
        friend class OverlayHost;
        // Tier 3 Phase 3.1: FrameBuilder reads compPtr()/laneId() to
        // wire the window-level proxy at beginFrame, same access
        // pattern `Widget::invalidate` / `Widget::invalidateNow` use
        // after Tier D / D1 retired `Widget::executePaint`.
        friend class FrameBuilder;

        void initWidgetRecurse(Widget *parent);
        void propagateWindowRenderTargetRecurse(Widget *parent);
        // Widget-View-Paint-Lifecycle-Plan Tier D / D2 (2026-06-03):
        // five Phase-4.7/4.8 no-op / zero-caller shims removed —
        // `observeWidgetLayerTreesRecurse`,
        // `unobserveWidgetLayerTreesRecurse`,
        // `invalidateWidgetRecurse`, `paintDirtyRecurse`,
        // `beginResizeCoordinatorSessionRecurse`. The window-level
        // FrameBuilder walk (entered via `paintDirty()` which lives
        // on) handles every paint dispatch the per-widget walks used
        // to chain through.
        void applyResizeGovernorMetadata(const Composition::ResizeGovernorMetadata & metadata);
        bool detectAnimatedTreeRecurse(Widget *parent) const;
        // UIView-Render-Redesign-Plan Phase F (2026-06-05): the
        // pre-Phase-F `anyWidgetOptsIntoResize` short-circuit (which
        // skipped the per-widget `handleHostResize` walk when no widget
        // had set `PaintOptions::invalidateOnResize`) is gone. Resize
        // unconditionally relayouts and force-repaints the whole tree,
        // so the gate has no remaining caller.
        View * hitTestWidget(Widget *widget,const Composition::Point2D &point) const;
        /// Overlay-Z-Order-Plan O3 §5.1 — overlay hit-test precedence.
        /// Asks the OverlayHost for the topmost absorbing overlay under
        /// `point` (window-space) and descends into its view subtree,
        /// returning the deepest hit View (or the overlay root when the
        /// point lands on the overlay itself with no deeper child).
        /// Returns nullptr when no overlay absorbs the point, so the
        /// dispatcher falls through to the main tree.
        View * hitTestOverlay(const Composition::Point2D &point) const;

        /// Overlay-Z-Order-Plan (O1/O2 render gap) — make a presented
        /// overlay renderable. An overlay lives outside the main tree, so
        /// `present` never runs the `initWidgetTree` wiring; without this
        /// the overlay's Views have no window render target and no host,
        /// and `buildFrame` cannot rasterize them. `mountOverlay`
        /// propagates the shared render target and threads the host down
        /// the overlay subtree (mirroring `initWidgetTree`), sizing it to
        /// the already-committed anchor rect. `unmountOverlay` un-threads
        /// the host on dismiss. Called by `OverlayHost` (friend).
        void mountOverlay(Widget * overlay);
        void unmountOverlay(Widget * overlay);

        /// §2.3a T1 tooltip helpers.
        ///   `owningWidgetOf` — walk a hit View up to the nearest View
        ///     whose `ownerWidget()` is set (tooltip is a per-Widget prop).
        ///   `scheduleTooltip` — arm the one-shot hover-delay timer for a
        ///     widget known to carry a non-empty tooltip.
        ///   `presentTooltip` — build + present the tooltip overlay now.
        ///   `cancelTooltip` — stop the timer and dismiss any shown tooltip.
        Widget * owningWidgetOf(View * v) const;
        void scheduleTooltip(Widget * owner);
        void presentTooltip(const OmegaCommon::String & text,
                            const Composition::Point2D & at);
        void cancelTooltip();

        void initWidgetTree();
        Composition::Compositor *compPtr(){return compositor;};
        uint64_t laneId() const { return syncLaneId; }
    public:
        OMEGACOMMON_CLASS("OmegaWTK.UI.WidgetTreeHost")


        static SharedHandle<WidgetTreeHost> Create();

        /**
         @brief Set the root of the Widget tree.
         @param[in] widget The root of the Widget Tree
        */
        void setRoot(WidgetPtr widget);

        /// Resize-Clamping Plan Phase 2: the aggregate minimum size (in
        /// logical dp) of the current root widget's view tree — the
        /// smallest the window's content can be without overflowing.
        /// Delegates to the root View's `LayoutManager::minSize` (sum on
        /// the main axis, max on the cross axis, recursing into nested
        /// containers). Writes {1,1} when there is no root. `AppWindow`
        /// forwards the result to `NativeWindow::setMinSize`.
        void aggregateMinSize(float & outWidthDp, float & outHeightDp) const;

        /**
         @brief Attach this host to an AppWindow
         @param[in] window The AppWindow to attach to.
         @paragraph
         If this instance of this class is the first to be attached to the AppWindow specified,
         its Compositor will be used to manage composition for the window's single surface.
        */
        void attachToWindow(AppWindow * window);

        /// Set the window's shared render target. Called by AppWindow
        /// before initWidgetTree() so the render target can be propagated
        /// to all Views in the tree (Phase 3, single-surface rendering).
        void setWindowRenderTarget(SharedHandle<Composition::ViewRenderTarget> rt);

        /// Set the window's compositor surface mailbox (Phase A).
        /// Called by AppWindow during creation.
        void setWindowSurface(SharedHandle<Composition::CompositorSurface> surface);

        /// Tier 3 Phase 3.8: set / read the window's frame driver.
        /// Called by AppWindow after the FrameBuilder is constructed.
        void setFrameBuilder(FrameBuilder * fb){ frameBuilder_ = fb; }
        FrameBuilder * frameBuilder() const { return frameBuilder_; }

        /// Widget-View-Paint-Lifecycle-Plan Tier A: owning window, set
        /// by AppWindow. Used by requestFrame().
        void setOwnerWindow(AppWindow * window){ ownerWindow_ = window; }
        /// Ask the owning window to flush a frame on the next run-loop
        /// turn (coalesced). Called by the deferred Widget::invalidate.
        void requestFrame();
        /// Repaint every widget whose view has the Paint dirty bit set.
        /// Invoked by AppWindow::flushFrame inside one FrameBuilder
        /// ScopedFrame.
        void paintDirty();

        /// UIView-Render-Redesign-Plan Phase F (2026-06-05): force a
        /// full-tree repaint independent of `DirtyBits`. Marks every
        /// node in the view subtree Style|Layout|Paint dirty and runs
        /// the central paint walk synchronously via `paintDirty()`.
        /// Called by `notifyWindowResize*` after `handleHostResize`
        /// so resize always re-rasterizes every widget at the new
        /// size (the platform stretches the existing surface, so
        /// dirty-only repaint would leave non-dirty content visibly
        /// stretched). Cheap once Phase G's content cache lands;
        /// always correct.
        void forceFullRepaint();

        /// Set the root native item for this window (Phase 5).
        /// Called by AppWindow during setRootWidget().
        void setRootNativeItem(Native::NativeItemPtr item);

        /// Embed a real native view as a child of the window's root
        /// native view. Used by NativeViewHost (Phase 5).
        void embedNativeItem(Native::NativeItemPtr item);

        /// Remove a previously embedded native view from the window's
        /// root native view. Used by NativeViewHost (Phase 5).
        void unembedNativeItem(Native::NativeItemPtr item);

        void notifyWindowResizeBegin(const Composition::Rect & rect);
        void notifyWindowResize(const Composition::Rect & rect);
        void notifyWindowResizeEnd(const Composition::Rect & rect);
        /// Phase G.5.4: whether a live resize drag is currently in progress.
        [[nodiscard]] bool isResizing() const { return resizing_; }

        /// Walk the virtual widget tree and return the deepest View whose
        /// bounds contain `point` (in window-relative coordinates).
        View * hitTest(const Composition::Point2D &point) const;

        /// Dispatch a native input event (mouse, keyboard) through the
        /// virtual widget tree via hit testing. Mouse events are routed
        /// to the View under the cursor; keyboard events go to the root.
        void dispatchInputEvent(Native::NativeEventPtr event);

        /// Overlay-Z-Order-Plan O1 — in-window overlay slot. Returns
        /// a reference because the host always owns exactly one
        /// `OverlayHost` (constructed in this host's ctor, destroyed
        /// in this host's dtor). Widgets that need to present
        /// tooltips, popovers, menus, or modals call
        /// `treeHost->overlayHost().present(...)`.
        OverlayHost & overlayHost();
        const OverlayHost & overlayHost() const;

        /// §2.3a T1 — dismiss the tooltip if it is currently pending or
        /// shown for `widget`. Called by `Widget::clearTooltip` so a
        /// programmatic clear takes the popup down immediately instead of
        /// waiting for the next hover change. No-op when a different
        /// widget's tooltip (or none) is active.
        void dismissTooltipFor(Widget * widget);

        /// §2.3a F2 — the per-window keyboard-focus authority. Returns a
        /// reference because the host always owns exactly one
        /// `FocusManager` (constructed in this host's ctor, destroyed in
        /// its dtor), matching `overlayHost()`. `View::focus`/`blur` and
        /// the input dispatcher reach the manager through this accessor.
        FocusManager & focusManager();
        const FocusManager & focusManager() const;

        ~WidgetTreeHost();
    };
};

#endif
