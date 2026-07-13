#include "WidgetTreeHost.h"
#include "../Composition/Compositor.h"

#include "AppWindowImpl.h"
#include "FrameBuilder.h"
#include "ViewImpl.h"   // §2.3a Focus M1: friend access to View::Impl::parent_ptr for the click-focus parent walk
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/LayoutManager.h"   // Phase 2: LayoutManager::minSize for aggregateMinSize
#include "omegaWTK/UI/OverlayHost.h"
#include "omegaWTK/UI/FocusManager.h"
#include "omegaWTK/UI/UIView.h"            // §2.3a T1: tooltip content authoring
#include "omegaWTK/Native/NativeTheme.h"   // §2.3a T1: theme colors for the tooltip
#include "omegaWTK/Composition/Brush.h"    // §2.3a T1: ColorBrush for tooltip fill
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>
#include <sstream>

namespace OmegaWTK {
    namespace {
        std::atomic<uint64_t> g_widgetTreeSyncLaneSeed {1};
        std::atomic<uint64_t> g_resizeSessionSeed {1};

        Composition::Compositor *globalCompositor(){
            static Composition::Compositor compositor;
            return &compositor;
        }
    }

    namespace Composition {
        void shutdownGlobalCompositor(){
            globalCompositor()->shutdown();
        }
    }

    namespace {

        struct ResizeTrackerTuning {
            float velocitySettlingEpsilon = 20.f;
            float accelerationSettlingEpsilon = 80.f;
            double deadPeriodMs = 120.0;
            float significantDeltaPx = 0.5f;
        };

        struct ResizeValidationTuning {
            bool enabled = false;
            bool failHard = false;
            double maxDropRatio = 0.60;
            std::uint64_t maxFailedPackets = 0;
            std::uint64_t maxEpochDrops = 12;
            std::uint64_t maxStaleCoordinatorPackets = 0;
            std::uint32_t minResizeSamples = 1;
        };

        static double readEnvDoubleClamp(const char *name,double fallback,double minValue,double maxValue){
            auto rawVar = OmegaCommon::getEnvVar(name);
            if(!rawVar.has_value() || rawVar->empty()){
                return fallback;
            }
            const char *raw = rawVar->c_str();
            char *endPtr = nullptr;
            const auto parsed = std::strtod(raw,&endPtr);
            if(endPtr == raw || !std::isfinite(parsed)){
                return fallback;
            }
            return std::clamp(parsed,minValue,maxValue);
        }

        static float readEnvFloatClamp(const char *name,float fallback,float minValue,float maxValue){
            return static_cast<float>(readEnvDoubleClamp(name,fallback,minValue,maxValue));
        }

        static bool readEnvBool(const char *name,bool fallback){
            auto raw = OmegaCommon::getEnvVar(name);
            if(!raw.has_value() || raw->empty()){
                return fallback;
            }
            return (*raw)[0] != '0';
        }

        static std::uint64_t readEnvU64Clamp(const char *name,std::uint64_t fallback,std::uint64_t minValue,std::uint64_t maxValue){
            auto rawVar = OmegaCommon::getEnvVar(name);
            if(!rawVar.has_value() || rawVar->empty()){
                return fallback;
            }
            const char *raw = rawVar->c_str();
            char *endPtr = nullptr;
            const auto parsed = std::strtoull(raw,&endPtr,10);
            if(endPtr == raw){
                return fallback;
            }
            return std::clamp<std::uint64_t>(parsed,minValue,maxValue);
        }

        static const ResizeTrackerTuning & resizeTrackerTuning(){
            static const ResizeTrackerTuning tuning = []{
                ResizeTrackerTuning cfg {};
                cfg.velocitySettlingEpsilon = readEnvFloatClamp(
                        "OMEGAWTK_RESIZE_VELOCITY_EPSILON",
                        cfg.velocitySettlingEpsilon,
                        1.f,
                        10000.f);
                cfg.accelerationSettlingEpsilon = readEnvFloatClamp(
                        "OMEGAWTK_RESIZE_ACCEL_EPSILON",
                        cfg.accelerationSettlingEpsilon,
                        1.f,
                        20000.f);
                cfg.deadPeriodMs = readEnvDoubleClamp(
                        "OMEGAWTK_RESIZE_DEAD_PERIOD_MS",
                        cfg.deadPeriodMs,
                        10.0,
                        2000.0);
                cfg.significantDeltaPx = readEnvFloatClamp(
                        "OMEGAWTK_RESIZE_SIGNIFICANT_DELTA_PX",
                        cfg.significantDeltaPx,
                        0.05f,
                        20.f);
                return cfg;
            }();
            return tuning;
        }


        static const char *resizePhaseName(ResizePhase phase){
            switch(phase){
                case ResizePhase::Idle:
                    return "Idle";
                case ResizePhase::Active:
                    return "Active";
                case ResizePhase::Settling:
                    return "Settling";
                case ResizePhase::Completed:
                    return "Completed";
                default:
                    return "Unknown";
            }
        }

        static double nowMs(){
            using namespace std::chrono;
            return duration<double,std::milli>(steady_clock::now().time_since_epoch()).count();
        }

        static float finiteOrZero(float value){
            return std::isfinite(value) ? value : 0.f;
        }
    }

    std::uint64_t ResizeDynamicsTracker::nextSessionId(){
        return g_resizeSessionSeed.fetch_add(1,std::memory_order_relaxed);
    }

    ResizeSessionState ResizeDynamicsTracker::begin(float width,float height,double tMs){
        const auto & tuning = resizeTrackerTuning();
        inSession = true;
        currentSessionId = nextSessionId();
        lastWidth = width;
        lastHeight = height;
        lastVelocity = 0.f;
        lastSignificantChangeMs = tMs;
        lastTick = std::chrono::steady_clock::now();
        ResizeSessionState state {};
        state.sessionId = currentSessionId;
        state.phase = ResizePhase::Active;
        state.sample.timestampMs = tMs;
        state.sample.width = width;
        state.sample.height = height;
        state.sample.velocityPxPerSec = 0.f;
        state.sample.accelerationPxPerSec2 = 0.f;
        (void)tuning;
        return state;
    }

    ResizeSessionState ResizeDynamicsTracker::update(float width,float height,double tMs){
        const auto & tuning = resizeTrackerTuning();
        if(!inSession){
            return begin(width,height,tMs);
        }
        auto nowTick = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<float>(nowTick - lastTick).count();
        const auto dw = width - lastWidth;
        const auto dh = height - lastHeight;
        const auto delta = std::sqrt((dw * dw) + (dh * dh));
        float velocity = 0.f;
        float acceleration = 0.f;
        if(dt > 1e-6f){
            velocity = delta / dt;
            acceleration = (velocity - lastVelocity) / dt;
        }
        velocity = finiteOrZero(velocity);
        acceleration = finiteOrZero(acceleration);
        if(delta >= tuning.significantDeltaPx){
            lastSignificantChangeMs = tMs;
        }

        ResizeSessionState state {};
        state.sessionId = currentSessionId;
        const bool settlingVelocity = std::fabs(velocity) <= tuning.velocitySettlingEpsilon;
        const bool settlingAcceleration = std::fabs(acceleration) <= tuning.accelerationSettlingEpsilon;
        const bool inDeadPeriod = (tMs - lastSignificantChangeMs) >= tuning.deadPeriodMs;
        if(settlingVelocity && settlingAcceleration){
            state.phase = inDeadPeriod ? ResizePhase::Completed : ResizePhase::Settling;
        }
        else {
            state.phase = ResizePhase::Active;
        }
        state.sample.timestampMs = tMs;
        state.sample.width = width;
        state.sample.height = height;
        state.sample.velocityPxPerSec = velocity;
        state.sample.accelerationPxPerSec2 = acceleration;

        lastTick = nowTick;
        lastWidth = width;
        lastHeight = height;
        lastVelocity = velocity;
        return state;
    }

    ResizeSessionState ResizeDynamicsTracker::end(float width,float height,double tMs){
        ResizeSessionState state = update(width,height,tMs);
        state.phase = ResizePhase::Completed;
        inSession = false;
        return state;
    }

    WidgetTreeHost::WidgetTreeHost():
    compositor(globalCompositor()),
    syncLaneId(g_widgetTreeSyncLaneSeed.fetch_add(1)),
    attachedToWindow(false),
    overlayHost_(Core::UniquePtr<OverlayHost>(new OverlayHost(*this))),
    // §2.3a F2: one FocusManager per host, live for the host's lifetime
    // (same construction pattern as overlayHost_ above).
    focusManager_(Core::UniquePtr<FocusManager>(new FocusManager()))
    {

    };

    WidgetTreeHost::~WidgetTreeHost(){
        // Widget-View-Paint-Lifecycle-Plan Tier D / D2 (2026-06-03):
        // the pre-4.8 `unobserveWidgetLayerTreesRecurse(root.get())`
        // call is gone (per-view LayerTree observation retired in
        // Phase 4.8; the method body was a no-op pending this sweep).
        //
        // Overlay-Z-Order-Plan O1: `overlayHost_` releases every
        // presented `WidgetPtr` via the `Core::UniquePtr` member
        // destructor below. Explicit `dismissAll()` is unnecessary
        // because the host's destructor already empties its
        // per-tier `Entry` vectors, which drops the shared widget
        // refcounts. Sequencing inside this body intentionally leaves
        // the field destruction to the compiler-generated tail.
        compositor = nullptr;
    };

    OverlayHost & WidgetTreeHost::overlayHost(){
        return *overlayHost_;
    }

    const OverlayHost & WidgetTreeHost::overlayHost() const {
        return *overlayHost_;
    }

    FocusManager & WidgetTreeHost::focusManager(){
        return *focusManager_;
    }

    const FocusManager & WidgetTreeHost::focusManager() const {
        return *focusManager_;
    }

    SharedHandle<WidgetTreeHost> WidgetTreeHost::Create(){
        return SharedHandle<WidgetTreeHost>(new WidgetTreeHost());
    };

    void WidgetTreeHost::initWidgetRecurse(Widget *parent){
        if(parent == nullptr){
            return;
        }
        // Phase 4.7.5: the FrameBuilder offset accumulator push is
        // gone (no per-widget submitView call to consume it any
        // more — `FrameBuilder::buildFrame` threads
        // `PaintContext.offset` through its own paint walker
        // instead). `parent->init()` marks the widget's dirty bits
        // and either runs `WidgetTreeHost::paintDirty()` inline (for
        // `immediate=true` from `Widget::init` /
        // `dispatchResize*ToHosts`) or defers via
        // `requestFrame()`; both paths flow through
        // `FrameBuilder::buildFrame`, which does not consult the
        // accumulator.
        parent->init();
        for(const auto & child : parent->childWidgets()){
            initWidgetRecurse(child.get());
        }
    }

    // Widget-View-Paint-Lifecycle-Plan Tier D / D2 (2026-06-03):
    // five Phase-4.7 / 4.8 no-op or zero-caller shims were removed
    // here in one sweep:
    //   * `observeWidgetLayerTreesRecurse` /
    //     `unobserveWidgetLayerTreesRecurse` — per-view LayerTree
    //     observation is gone (Phase 4.8 collapsed everything onto
    //     `AppWindow::Impl::windowLayerTree_`). The four call sites
    //     in `~WidgetTreeHost`, `initWidgetTree`, `setRoot` (×2) are
    //     dropped with the method bodies.
    //   * `invalidateWidgetRecurse` — never had any external caller
    //     after D0 verified (the resize path the D-tier wording
    //     guessed at had already been refactored away). D1 inlined
    //     its `Widget::executePaint` call transitionally; D2 finishes
    //     the symbol.
    //   * `paintDirtyRecurse` — vestigial since Phase 4.7.4; the
    //     central `FrameBuilder::buildFrame` walk runs from
    //     `paintDirty()` instead.
    //   * `beginResizeCoordinatorSessionRecurse` — Phase 4.5 retired
    //     the per-view `ViewResizeCoordinator` session state; the
    //     last call site was removed alongside that work, so this is
    //     pure dead code today.
    // `paintDirty()` (§0.3 #4) and the new D1 inlined `Widget`
    // entry points carry the full paint dispatch surface.

    void WidgetTreeHost::requestFrame(){
        if(ownerWindow_ != nullptr){
            ownerWindow_->requestFrame();
        }
    }

    void WidgetTreeHost::paintDirty(){
        // Phase 4.7.4: one tree-wide `FrameBuilder::buildFrame` call
        // replaces the pre-4.7.4 per-widget walk
        // (`paintDirtyRecurse` → `flushPendingPaint` →
        // `executePaint` → `onPaint` → `UIView::update` →
        // `submitView`). buildFrame runs the dirty-bit-gated
        // Style / Layout / Paint passes (Phase 4.7.3) over the View
        // tree and submits one aggregated DisplayList per frame.
        // Tier D / D1 (2026-06-03): `Widget::executePaint` and
        // `Widget::flushPendingPaint` have now been deleted (D1
        // didn't restore any caller of them — `paintDirtyRecurse`
        // remains a no-op shim awaiting D2's sweep).
        if(root == nullptr || root->view == nullptr){
            return;
        }
        auto * fb = frameBuilder();
        if(fb == nullptr){
            return;
        }
        FrameBuilder::ScopedFrame frame(fb);

        // Overlay-Z-Order-Plan O2: when overlays are presented, the
        // composited frame must include the main tree's slice
        // alongside the overlays' slices. `FrameBuilder::buildFrame`
        // early-returns on a clean root (mask == 0); if a main-tree
        // dirty trigger missed (e.g. an animation tick scheduled
        // via the overlay subtree, not via a main-tree widget), the
        // main-tree call below would skip submission and the
        // deposited `CompositeFrame` would only carry overlay
        // slices — blanking the rest of the window. Force-paint the
        // main tree whenever overlays exist. Tier-5 subtree-region
        // gating will refine this; the cost today is one extra
        // pre-order paint walk per frame on the main tree, which is
        // the same work pre-O2 paintDirty did anyway.
        const bool hasOverlays = overlayHost_ != nullptr &&
                                 overlayHost_->isPresentingAny();
        if(hasOverlays){
            root->view->markDirty(View::Paint);
        }

        fb->buildFrame(*root->view);

        // Overlay-Z-Order-Plan O2: walk overlay subtrees after the
        // main tree, in tier paint order (Floating → Modal →
        // Tooltip → DragGhost — §2). FIFO within tier matches
        // §2's "OverlayHost::present call order" tie-break. Each
        // call to `buildFrame` appends a `CompositeFrame::WidgetSlice`
        // (see `CompositorClientProxy::submitDisplayList`), so the
        // deposited frame layers main → Floating → Modal → Tooltip →
        // DragGhost in submission order — overlay DrawOps land on
        // top because the backend renders slices in order.
        //
        // Each overlay is force-Paint-dirtied per the same rationale
        // as the main-tree guard above. Style / Layout dirty bits
        // were set at present-time (see `OverlayHost::present`) so
        // the FIRST paint runs all three passes; on subsequent
        // frames only Paint runs (Style / Layout were cleared at
        // the end of the previous `buildFrame`).
        if(overlayHost_ == nullptr){
            return;
        }
        static constexpr OverlayTier kPaintOrder[] = {
            OverlayTier::Floating,
            OverlayTier::Modal,
            OverlayTier::Tooltip,
            OverlayTier::DragGhost
        };
        for(auto tier : kPaintOrder){
            for(const auto & po : overlayHost_->overlaysForPaintIn(tier)){
                if(po.widget == nullptr || po.widget->view == nullptr){
                    continue;
                }
                // Overlay-Z-Order-Plan O2.1: drop shadow emitted as a
                // standalone one-op submission BEFORE the overlay's
                // own `buildFrame`. The shadow slice appends first,
                // the overlay's content slice appends second, so the
                // backend renders the shadow underneath the overlay
                // content. `cornerRadius` lets the shadow track the
                // overlay widget's visible silhouette — callers set
                // this in `OverlayOrnamentation` at present time.
                if(po.ornament.dropShadow){
                    fb->submitOverlayShadow(po.ornament.shadowParams,
                                            po.rect,
                                            po.ornament.cornerRadius);
                }
                po.widget->view->markDirty(View::Paint);
                fb->buildFrame(*po.widget->view);
            }
        }
    }

    // `paintDirtyRecurse` and `beginResizeCoordinatorSessionRecurse`
    // deleted in the Tier D / D2 sweep — see the explanatory block
    // above the relocated `requestFrame()` definition.

    bool WidgetTreeHost::detectAnimatedTreeRecurse(Widget *parent) const{
        if(parent == nullptr){
            return false;
        }
        if(parent->paintMode() != PaintMode::Automatic){
            return true;
        }
        for(const auto & child : parent->childWidgets()){
            if(detectAnimatedTreeRecurse(child.get())){
                return true;
            }
        }
        return false;
    }

    // UIView-Render-Redesign-Plan Phase F (2026-06-05): the
    // `anyWidgetOptsIntoResize` gate is gone. Every window resize
    // unconditionally runs `handleHostResize` (sizes the root view +
    // runs the widget layout pass) AND forces a full-tree repaint
    // independent of `DirtyBits`. See `markFullRepaintRecurse` and
    // `forceFullRepaint` below; called once per `notifyWindowResize*`
    // event after `handleHostResize` returns.
    //
    // Rationale: the platform stretches the existing surface to the
    // new size on resize, so any content rasterized at the old size
    // appears stretched until re-drawn at the new resolution.
    // Dirty-only repaint is wrong here — a "non-dirty" widget whose
    // model did not change still has stale, wrong-resolution pixels.

    namespace {
        // Mark Style|Layout|Paint dirty on every node in the view
        // subtree so `FrameBuilder::buildFrame` re-runs all three
        // passes over the whole tree. Mirrors the cascade-change
        // helper `markViewSubtreeDirty` in AppWindow.cpp; kept local
        // here because the call site (resize) is the only Phase F
        // consumer outside that file.
        constexpr std::uint8_t kFullRepaintBits =
            View::Style | View::Layout | View::Paint;

        // UIView-Render-Redesign Phase G.3.3: mark via
        // `markDirtyNoContentBump`, NOT `markDirty`. The resize repaint
        // must re-run every node's Style / Layout / Paint pass (the
        // platform stretches the old surface, so content must redraw at
        // the new resolution), but it must NOT bump each View's
        // `contentVersion`. The per-View content cache (G.3.2-rev2) keys
        // on `(nodeId, contentVersion, sizeBucket, scale)` and bumps
        // `contentVersion` on ANY `markDirty` bit — so a plain
        // `markDirty(Style|Layout|Paint)` here would invalidate every
        // cached View texture on every resize tick, defeating the cache.
        // Leaving `contentVersion` alone lets the cache's size bucket do
        // the size-diff invalidation by itself: a View whose pixel size
        // changed lands in a new size bucket -> cache miss -> recapture
        // at the new size, while a View whose size is unchanged hits and
        // re-blits at its (possibly new) window position.
        void markFullRepaintRecurse(View & view){
            view.markDirtyNoContentBump(kFullRepaintBits);
            for(auto * sv : view.subviews()){
                if(sv != nullptr){
                    markFullRepaintRecurse(*sv);
                }
            }
        }

        /// Is `target` `root` or one of its (transitive) sub-views? Pointer
        /// comparison only — `target` is never dereferenced, so it is safe on
        /// a pointer that may already be freed. Used to clear the dispatcher's
        /// cached non-owning View pointers when an overlay subtree is torn
        /// down (they would otherwise dangle into freed views).
        bool viewIsInSubtree(View * root, View * target){
            if(root == nullptr || target == nullptr){
                return false;
            }
            if(root == target){
                return true;
            }
            for(auto * sv : root->subviews()){
                if(viewIsInSubtree(sv, target)){
                    return true;
                }
            }
            return false;
        }
    }

    void WidgetTreeHost::forceFullRepaint(){
        if(root == nullptr || root->view == nullptr){
            return;
        }
        // 1. Mark the whole view subtree dirty so `buildFrame`'s
        //    dirty-bit gates open for all three passes (the Paint
        //    walker already visits the whole tree unconditionally;
        //    Style/Layout are gated by `(self|desc) & bit`).
        markFullRepaintRecurse(*root->view);
        // 2. Run the central paint walk synchronously. `paintDirty()`
        //    opens its own `FrameBuilder::ScopedFrame`, but the
        //    `dispatchResize*ToHosts` caller has one open already, and
        //    the depth counter makes nested ScopedFrames safe — only
        //    the outermost pair does the session work.
        paintDirty();
    }



    void WidgetTreeHost::propagateWindowRenderTargetRecurse(Widget *parent){
        if(parent == nullptr || windowRenderTarget_ == nullptr){
            return;
        }
        if(parent->view != nullptr){
            parent->view->setWindowRenderTarget(windowRenderTarget_);
        }
        for(const auto & child : parent->childWidgets()){
            propagateWindowRenderTargetRecurse(child.get());
        }
    }

    void WidgetTreeHost::setWindowRenderTarget(SharedHandle<Composition::ViewRenderTarget> rt){
        windowRenderTarget_ = std::move(rt);
    }

    void WidgetTreeHost::setWindowSurface(SharedHandle<Composition::CompositorSurface> surface){
        windowSurface_ = std::move(surface);
    }

    void WidgetTreeHost::setRootNativeItem(Native::NativeItemPtr item){
        rootNativeItem_ = std::move(item);
    }

    void WidgetTreeHost::embedNativeItem(Native::NativeItemPtr item){
        if(rootNativeItem_ != nullptr && item != nullptr){
            rootNativeItem_->addChildNativeItem(item);
        }
    }

    void WidgetTreeHost::unembedNativeItem(Native::NativeItemPtr item){
        if(rootNativeItem_ != nullptr && item != nullptr){
            rootNativeItem_->removeChildNativeItem(item);
        }
    }

    void WidgetTreeHost::initWidgetTree(){
        // Phase 3: propagate the window's shared render target to all
        // Views before initializing widgets, so that compositor wiring
        // uses the correct (shared) target. Tier D / D2 (2026-06-03)
        // dropped the `observeWidgetLayerTreesRecurse(root.get())` call
        // that used to live here — per-view LayerTree observation is
        // gone (Phase 4.8).
        if(windowRenderTarget_ != nullptr){
            propagateWindowRenderTargetRecurse(root.get());
        }
        root->setTreeHostRecurse(this);
        // Tier 4 (first-paint stale-layout fix): size the tree to the
        // window *before* the initial paint walk. Without this, the root's
        // rect is still its constructor value (or zero) when
        // initWidgetRecurse paints, so a container's first
        // `layoutChildren` runs against a suspicious/unsized frame, bails
        // (leaving `needsLayout` set), and the children paint at the
        // origin — the window-resize that would relayout them arrives only
        // *after* this walk (notifyWindowResize), and never at all for a
        // tree with no resize-opted widgets. `handleHostResize` sizes the
        // root view + runs the widget layout (StackWidget::resize →
        // relayout → layoutChildren) but does not paint here (hasMounted
        // is still false), so the subsequent initWidgetRecurse paints once
        // with children already in their arranged positions.
        if(ownerWindow_ != nullptr && root != nullptr){
            const auto wr = ownerWindow_->getRect();
            const Composition::Rect rootRect{
                Composition::Point2D{0.f, 0.f}, wr.w, wr.h};
            root->handleHostResize(rootRect);
        }
        initWidgetRecurse(root.get());
        // Note: Widget::init() already calls executePaint(Initial, true) for
        // each Automatic-mode widget during initWidgetRecurse, so a second
        // repaint pass is not needed.  The redundant pass was generating
        // duplicate compositor commands during startup, causing the first
        // content frames to be overwritten by a late-arriving clear.
    }

    void WidgetTreeHost::notifyWindowResize(const Composition::Rect &rect){
        // UIView-Render-Redesign-Plan Phase F (2026-06-05):
        // unconditionally relayout the whole widget tree, then force a
        // full-tree repaint independent of DirtyBits. The caller
        // (dispatchResizeToHosts) brackets this in a
        // `FrameBuilder::ScopedFrame`; `forceFullRepaint`'s nested
        // `paintDirty()` shares the same frame via the depth counter.
        if(root != nullptr){
            root->handleHostResize(rect);
            forceFullRepaint();
        }
        lastResizeSessionState = resizeTracker.update(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        resizeCoordinatorGeneration += 1;
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " gen=" << resizeCoordinatorGeneration;
        OMEGAWTK_DEBUG(stream.str());
    }

    void WidgetTreeHost::notifyWindowResizeBegin(const Composition::Rect &rect){
        resizing_ = true;   // G.5.4: live drag in progress
        lastResizeSessionState = resizeTracker.begin(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        resizeCoordinatorGeneration += 1;
        // Phase 4.5: the session walk is a no-op (the coordinator it
        // primed is gone). The recursive call dropped here saves a
        // full-tree walk on every resize-begin. `resizeCoordinatorGeneration`
        // stays — it is still consumed by the resize tracker / diagnostics.
        //
        // UIView-Render-Redesign-Plan Phase F (2026-06-05):
        // unconditional relayout + force-full-tree-repaint. See
        // `notifyWindowResize` above for the rationale.
        if(root != nullptr){
            root->handleHostResize(rect);
            forceFullRepaint();
        }
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " gen=" << resizeCoordinatorGeneration;
        OMEGAWTK_DEBUG(stream.str());
    }

    void WidgetTreeHost::notifyWindowResizeEnd(const Composition::Rect &rect){
        // UIView-Render-Redesign-Plan Phase F (2026-06-05):
        // unconditional relayout + force-full-tree-repaint. See
        // `notifyWindowResize` above for the rationale.
        resizing_ = false;   // G.5.4: drag ended → next paint re-captures crisp
        if(root != nullptr){
            root->handleHostResize(rect);
            forceFullRepaint();
        }
        lastResizeSessionState = resizeTracker.end(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        resizeCoordinatorGeneration += 1;
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " gen=" << resizeCoordinatorGeneration;
        OMEGAWTK_DEBUG(stream.str());
    }

    View * WidgetTreeHost::hitTest(const Composition::Point2D &point) const{
        if(root == nullptr){
            return nullptr;
        }
        return hitTestWidget(root.get(),point);
    }

    View * WidgetTreeHost::hitTestWidget(Widget *widget,const Composition::Point2D &point) const{
        if(widget == nullptr){
            return nullptr;
        }
        auto children = widget->childWidgets();
        // Walk children in reverse z-order (last child = frontmost).
        for(auto i = children.size(); i > 0; --i){
            auto &child = children[i - 1];
            if(child == nullptr || child->view == nullptr){
                continue;
            }
            View &childView = child->viewRef();
            // The viewport clip: a point outside `childView`'s own rect
            // never descends into its subtree. For a ScrollView this rect
            // is the viewport, so content scrolled past the viewport edges
            // is unreachable — events stop reporting for children that have
            // moved outside the scroll rect.
            if(childView.containsPoint(point)){
                Composition::Point2D localPoint {
                    point.x - childView.getRect().pos.x,
                    point.y - childView.getRect().pos.y
                };
                // ScrollableContainer-Implementation-Plan §6.2 / S4: fold the
                // child view's `contentOffset()` into the descent, mirroring
                // the FrameBuilder paint walker's per-descent fold. A
                // ScrollView returns `-scrollOffset` here, so subtracting it
                // shifts the hit point in the same direction the content
                // moved — the child hit rects track the scrolled content
                // instead of landing on their un-scrolled positions. For a
                // non-scrolling view `contentOffset()` is {0,0} (no effect).
                const auto co = childView.contentOffset();
                localPoint.x -= co.x;
                localPoint.y -= co.y;
                View *hit = hitTestWidget(child.get(),localPoint);
                if(hit != nullptr){
                    return hit;
                }
            }
        }
        if(widget->view != nullptr){
            return widget->view.get();
        }
        return nullptr;
    }

    void WidgetTreeHost::captureMouse(View * view){
        capturedView_ = view;
        // E2 native follow-up: install an OS-level pointer grab so a drag
        // keeps receiving motion after the cursor leaves the window. No-op on
        // macOS / GTK4 (implicit button grab); calls SetCapture() on Win32.
        if(rootNativeItem_ != nullptr){
            rootNativeItem_->setPointerCapture(true);
        }
    }

    void WidgetTreeHost::releaseMouse(){
        capturedView_ = nullptr;
        if(rootNativeItem_ != nullptr){
            rootNativeItem_->setPointerCapture(false);
        }
    }

    namespace {
        /// View-tree hit test for the overlay path: walk `root`'s VIEW
        /// subtree (not the widget tree) in reverse z-order and return the
        /// deepest view whose rect contains `point` (in `root`'s content
        /// space). Overlays can host raw sub-views that back no child
        /// Widget — a `ContextMenu`'s per-item rows are `UIView` sub-views
        /// made with `makeSubView`, invisible to `hitTestWidget`'s
        /// `childWidgets()` descent, so their hover / click delegates never
        /// fired. Walking the view tree reaches them; and because a child
        /// Widget's root view IS a sub-view of its parent's view, this also
        /// covers the child-Widget case (a future Popover / Modal Container)
        /// with no extra work. Returns nullptr when nothing deeper than
        /// `root` claims the point (the caller then returns `root` itself).
        View * hitTestOverlayViewSubtree(View * root,
                                         const Composition::Point2D & point){
            if(root == nullptr){
                return nullptr;
            }
            auto subs = root->subviews();
            for(auto i = subs.size(); i > 0; --i){
                View * child = subs[i - 1];
                if(child == nullptr){
                    continue;
                }
                // `containsPoint` tests `point` (in the child's parent =
                // `root`'s content space) against the child's own rect.
                if(child->containsPoint(point)){
                    const auto co = child->contentOffset();
                    const Composition::Point2D childLocal {
                        point.x - child->getRect().pos.x - co.x,
                        point.y - child->getRect().pos.y - co.y
                    };
                    View * deeper = hitTestOverlayViewSubtree(child, childLocal);
                    return deeper != nullptr ? deeper : child;
                }
            }
            return nullptr;
        }
    }

    View * WidgetTreeHost::hitTestOverlay(const Composition::Point2D &point) const{
        if(overlayHost_ == nullptr){
            return nullptr;
        }
        Widget * ow = overlayHost_->absorbingOverlayAt(point);
        if(ow == nullptr){
            return nullptr;
        }
        // Overlay roots carry a window-space rect (set by
        // `OverlayHost::present` via `setRect`), so translate `point`
        // into the overlay's own content space before descending — exactly
        // as `hitTest` passes a window-space point straight to the root
        // descent because the main-tree root sits at the window origin.
        // The descent walks the VIEW tree (not the widget tree) so an
        // overlay's raw sub-views are reachable; it returns the overlay's
        // own view when no deeper sub-view claims the point, so a bare
        // overlay background still hit-tests to itself.
        View & ov = ow->viewRef();
        Composition::Point2D local {
            point.x - ov.getRect().pos.x - ov.contentOffset().x,
            point.y - ov.getRect().pos.y - ov.contentOffset().y
        };
        View * hit = hitTestOverlayViewSubtree(&ov, local);
        return hit != nullptr ? hit : &ov;
    }

    namespace {
    // §2.3a T1 — the tooltip's content is a UI-layer widget (not a
    // Widgets-library `Label`): the dispatcher lives in the UI module,
    // which has no dependency on the Widgets library, so the tooltip is
    // built directly on a `UIView` here. A rounded background box plus a
    // single centered text element, styled from the current OS theme.
    // Content is authored in the constructor (an overlay is presented,
    // never `onMount`-ed, so there is no mount hook to build it in).
    class TooltipContentWidget : public Widget {
    protected:
        // Required override (NativeThemeObserver is abstract). The tooltip
        // is short-lived and re-created on each present with the current
        // theme, so it does not react to a live theme flip.
        void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    public:
        TooltipContentWidget(const Composition::Rect & rect,
                             const OmegaCommon::UString & text,
                             const Native::ThemeDesc & theme)
            : Widget(ViewPtr(new UIView(rect, nullptr, "tooltip"))){
            auto & uv = viewAs<UIView>();
            auto & lv2 = uv.layoutV2();
            lv2.clear();

            // bg — full-rect rounded rectangle. Element coords are in the
            // widget's rect space (the Layout pass normalizes them to the
            // view's local bounds), matching how Button authors its "bg".
            {
                Composition::RoundedRect bg{};
                bg.pos = rect.pos;
                bg.w = rect.w;
                bg.h = rect.h;
                bg.rad_x = 4.f;
                bg.rad_y = 4.f;
                UIElementLayoutSpec spec;
                spec.tag = "bg";
                spec.shape = Shape::RoundedRect(bg);
                lv2.element(spec);
            }
            // label — the tooltip text, horizontally inset, single line.
            // Inset matches the width budget's `hpad` in `presentTooltip`.
            {
                const float pad = 8.f;
                UIElementLayoutSpec spec;
                spec.tag = "label";
                spec.text = text;
                spec.textRect = Composition::Rect{
                    Composition::Point2D{rect.pos.x + pad, rect.pos.y},
                    std::max(0.f, rect.w - pad * 2.f), rect.h};
                lv2.element(spec);
            }

            auto ss = Style::Create();
            ss->elementBrush("bg",
                Composition::ColorBrush(theme.colors.controlBackground));
            ss->elementBorder("bg", theme.colors.separator, 1.f);
            // No explicit font: the tooltip text renders at the same body
            // default as any Label (theme `defaultSize`). The box width in
            // `presentTooltip` is calibrated to that size.
            ss->textColor("label", theme.colors.foreground);
            ss->textAlignment("label",
                Composition::TextLayoutDescriptor::MiddleCenter);
            ss->textWrapping("label",
                Composition::TextLayoutDescriptor::None);
            uv.setStyle(ss);
        }
    };
    } // namespace

    void WidgetTreeHost::mountOverlay(Widget * overlay){
        if(overlay == nullptr){
            return;
        }
        // Mirror the `initWidgetTree` wiring for a subtree presented
        // outside the main tree: give its Views the window's shared
        // render target, then thread the host (which also wires the
        // compositor frontend + sync lane) down the subtree so
        // `buildFrame` can rasterize it. The overlay's root rect was
        // already committed by `OverlayHost::present` (`setRect`).
        if(windowRenderTarget_ != nullptr){
            propagateWindowRenderTargetRecurse(overlay);
        }
        overlay->setTreeHostRecurse(this);
        // Mark the ENTIRE overlay subtree dirty, not just the root. The
        // main tree gets this for free via `initWidgetTree` /
        // `forceFullRepaint`; an overlay is mounted on its own, and
        // `buildFrame`'s Style + Layout passes prune any subtree whose
        // own `(dirtyBits | descendantDirty) & passBit == 0`. If only the
        // root were marked (as `OverlayHost::present` does), an overlay's
        // child sub-views — a ContextMenu's per-item rows, a Popover's
        // child widgets — would be visited by the Paint pass (which walks
        // the whole tree) but SKIPPED by Style/Layout, so they paint with
        // unresolved styles and unlaid-out elements: invisible. Recursing
        // here fixes every overlay that hosts sub-views, not just the
        // widget that first hit it.
        if(overlay->view != nullptr){
            markFullRepaintRecurse(*overlay->view);
        }
    }

    void WidgetTreeHost::unmountOverlay(Widget * overlay){
        if(overlay == nullptr){
            return;
        }
        // The input dispatcher caches non-owning View pointers across events
        // — `hoveredView_` (for enter/exit synthesis) and `capturedView_`
        // (for a drag). If either points into this overlay's subtree, it
        // dangles the instant the overlay's `WidgetPtr` is released (right
        // after this in the dismiss path), and the NEXT motion event
        // dereferences it — `hoveredView_->setPseudoClassBits(...)` — for a
        // use-after-free. The overlay's views are still alive here (erase
        // happens after `unmountOverlay` returns), so clear the pointers now:
        // drop the stale hover pseudo-class and null them so the dispatcher
        // re-establishes hover from scratch on the next move. (Focus is the
        // sibling case, handled by O4's popAndRestore + teardown guard.)
        if(overlay->view != nullptr){
            View * ov = overlay->view.get();
            if(hoveredView_ != nullptr && viewIsInSubtree(ov, hoveredView_)){
                hoveredView_->setPseudoClassBits(0x01U, false);
                hoveredView_ = nullptr;
            }
            if(capturedView_ != nullptr && viewIsInSubtree(ov, capturedView_)){
                capturedView_ = nullptr;
            }
        }
        overlay->setTreeHostRecurse(nullptr);
    }

    Widget * WidgetTreeHost::owningWidgetOf(View * v) const{
        for(; v != nullptr; v = v->impl_->parent_ptr){
            if(Widget * w = v->ownerWidget()){
                return w;
            }
        }
        return nullptr;
    }

    void WidgetTreeHost::scheduleTooltip(Widget * owner){
        if(owner == nullptr){
            return;
        }
        tooltipWidget_ = owner;
        const OmegaCommon::String text = owner->tooltip();
        const Composition::Point2D at = lastCursorPos_;
        // One-shot 500ms hover delay (macOS / Qt convention). Capturing
        // `this` is safe: the host outlives its overlays, and the timer is
        // stopped in `cancelTooltip` before either is torn down. `text` /
        // `at` are captured by value so a later cursor move cannot mutate
        // the pending tooltip's content or anchor.
        tooltipTimer_ = Native::make_native_timer(0.5f, false,
            [this, text, at](){
                this->presentTooltip(text, at);
            });
    }

    void WidgetTreeHost::presentTooltip(const OmegaCommon::String & text,
                                        const Composition::Point2D & at){
        if(overlayHost_ == nullptr || text.empty()){
            return;
        }
        // Widen ASCII → UString for the text element (mirrors Button's
        // iconToken widening). v0 tooltips are short ASCII; full UTF-8
        // decoding is a follow-up if non-ASCII tooltips are wanted.
        OmegaCommon::UString utext(text.begin(), text.end());

        // The tooltip text renders at the same default size as a Label
        // (no explicit font), so size the box against the theme's body
        // `defaultSize`. Empirically the engine advances ~0.55·pt per
        // glyph for mixed text; allocate a wider ~0.72·pt so the box
        // comfortably encloses the string, plus padding on both sides.
        // Height is one line plus vertical padding. Precise text-
        // measurement-based sizing is deferred to T2 — see the Tooltip-v2
        // plan. Width is clamped so a pathological string can't produce a
        // giant box; the overlay host edge-clamps the final anchored rect
        // against the window.
        const Native::ThemeDesc theme = Native::queryCurrentTheme();
        const float fontPt = theme.typography.defaultSize;
        const float hpad  = 8.f;
        const float vpad  = 5.f;
        const float charW = fontPt * 0.72f;
        const float lineH = fontPt * 1.5f;
        float w = (hpad * 2.f) + (charW * static_cast<float>(text.size()));
        w = std::min(std::max(w, 40.f), 420.f);
        const float h = (vpad * 2.f) + lineH;

        const Composition::Rect rect{Composition::Point2D{0.f, 0.f}, w, h};
        auto tip = std::make_shared<TooltipContentWidget>(rect, utext, theme);

        OverlayAnchor anchor;
        anchor.mode = OverlayAnchor::Mode::AtPoint;
        anchor.point = Composition::Point2D{at.x + 12.f, at.y + 12.f};

        OverlayDismissPolicy policy;
        policy.absorbsHits     = false;  // hover / click through the tooltip
        policy.clickOutside    = false;  // dispatcher owns tooltip lifecycle
        policy.escapeKey       = false;
        policy.windowDeactivate = false;
        policy.anchorDestroyed = false;

        OverlayOrnamentation ornament;   // drop shadow on by default
        ornament.cornerRadius = 4.f;     // match the tooltip bg rounding

        currentTooltipHandle_ = overlayHost_->present(
            tip, OverlayTier::Tooltip, anchor, policy, ornament);
    }

    void WidgetTreeHost::cancelTooltip(){
        if(tooltipTimer_ != nullptr){
            tooltipTimer_->stop();
            tooltipTimer_ = nullptr;
        }
        if(currentTooltipHandle_.valid() && overlayHost_ != nullptr){
            overlayHost_->dismiss(currentTooltipHandle_);
            currentTooltipHandle_ = OverlayHandle{};
        }
        tooltipWidget_ = nullptr;
    }

    void WidgetTreeHost::dismissTooltipFor(Widget * widget){
        if(widget != nullptr && tooltipWidget_ == widget){
            cancelTooltip();
        }
    }

    void WidgetTreeHost::dispatchInputEvent(Native::NativeEventPtr event){
        if(root == nullptr){
            return;
        }
        using Native::NativeEvent;

        // Drain any overlay that closed itself from within its own delegate
        // (a ContextMenu item click) AFTER this whole dispatch has unwound —
        // at every exit path. Tearing the overlay down synchronously inside
        // the delegate frees the View + ViewDelegate still being dispatched
        // (use-after-free); deferring to this scope-guard runs the teardown
        // with the delegate/emit stack fully popped. Covers the KeyDown
        // (Enter-activate) exits as well as the positional (mouse-up) ones.
        struct DeferredDismissDrain {
            OverlayHost * host;
            ~DeferredDismissDrain(){
                if(host != nullptr){
                    host->drainDeferredDismissals();
                }
            }
        } deferredDismissDrain{overlayHost_.get()};

        // Extract position for positional events.
        Composition::Point2D pos {};
        bool hasPosition = false;

        switch(event->type){
            case NativeEvent::LMouseDown:
            case NativeEvent::LMouseUp:
            case NativeEvent::RMouseDown:
            case NativeEvent::RMouseUp: {
                auto *p = static_cast<Native::MouseEventParams *>(event->params);
                if(p != nullptr){ pos = p->position; hasPosition = true; }
                break;
            }
            case NativeEvent::CursorMove: {
                auto *p = static_cast<Native::CursorMoveParams *>(event->params);
                if(p != nullptr){ pos = p->position; hasPosition = true; }
                break;
            }
            case NativeEvent::CursorEnter: {
                auto *p = static_cast<Native::CursorEnterParams *>(event->params);
                if(p != nullptr){ pos = p->position; hasPosition = true; }
                break;
            }
            case NativeEvent::ScrollWheel: {
                auto *p = static_cast<Native::ScrollParams *>(event->params);
                if(p != nullptr){ pos = p->position; hasPosition = true; }
                break;
            }
            case NativeEvent::CursorExit: {
                // §2.3a T1: the cursor left the window — take down any
                // pending or shown tooltip.
                cancelTooltip();
                // Cursor left the window — send exit to hovered view.
                if(hoveredView_ != nullptr){
                    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.4
                    // (2026-06-03): clear the Hover bit on the View
                    // leaving the cursor's reach so the next Style
                    // pass re-resolves without `:hover`.
                    hoveredView_->setPseudoClassBits(0x01U, false);
                    hoveredView_->emit(event);
                    hoveredView_ = nullptr;
                }
                return;
            }
            case NativeEvent::KeyDown:
            case NativeEvent::KeyUp: {
                // Native-API-Completion-Proposal §2.3a Focus F3
                // (2026-06-25): route keyboard events to the View the
                // FocusManager has selected, replacing the pre-F2
                // root-broadcast. When nothing holds focus the event is
                // dropped — a no-focus state means no consumer, which is
                // the real condition the broadcast was masking (the root
                // widget is not itself a keyboard target).
                //
                // F4 (2026-07-01): Tab / Shift-Tab traversal interception.
                // A bare Tab moves focus forward, Shift+Tab backward, and
                // the event is consumed — it never reaches the focused
                // view's delegate. Doing this *before* delegate dispatch
                // is deliberate (the plan's step 1/2 ordering): a focused
                // TextInput must not be able to swallow Tab and trap
                // traversal. Both KeyDown and the matching KeyUp are
                // swallowed so a widget never sees a lone Tab release; the
                // actual focus move happens once, on the KeyDown.
                if(event->type == NativeEvent::KeyDown){
                    // §2.3a T1: any key press dismisses a pending / shown
                    // tooltip (macOS / Qt behavior — typing hides the hint).
                    cancelTooltip();
                    auto *kp = static_cast<Native::KeyDownParams *>(event->params);
                    // Overlay-Z-Order-Plan O3 §5.3: Escape dismisses the
                    // topmost presented overlay that opts into `escapeKey`
                    // and is consumed before it can reach the focused
                    // view. Intercepted here (before Tab / delegate
                    // dispatch) for the same reason Tab is: a focused
                    // TextInput must not be able to swallow Escape and
                    // trap an open popover. Only when no overlay claims
                    // the key does it fall through — so Escape can still
                    // cancel a field when nothing is presented.
                    if(kp != nullptr && kp->code == Native::KeyCode::Escape){
                        if(overlayHost_ != nullptr &&
                           overlayHost_->dismissTopmostForEscape()){
                            return;
                        }
                    }
                    if(kp != nullptr && kp->code == Native::KeyCode::Tab){
                        if(kp->modifiers.shift){
                            focusManager_->focusPrevious();
                        } else {
                            focusManager_->focusNext();
                        }
                        return;
                    }
                } else { // KeyUp
                    auto *kp = static_cast<Native::KeyUpParams *>(event->params);
                    if(kp != nullptr && kp->code == Native::KeyCode::Tab){
                        return;
                    }
                }
                if(View * focused = focusManager_->focusedView()){
                    focused->emit(event);
                }
                return;
            }
            default:
                return;
        }

        if(!hasPosition){
            return;
        }

        // ScrollView-Interaction-Enhancements-Plan E2: pointer-capture
        // short-circuit. While a view holds capture (an in-progress drag),
        // the button/move stream belongs to it — deliver straight to the
        // captured view and skip hit-testing + hover synthesis. The
        // captured view releases on its own LMouseUp. ScrollWheel and the
        // synthesized enter/exit are unaffected (they are not part of this
        // set), so a wheel mid-drag still reaches its normal target.
        if(capturedView_ != nullptr &&
           (event->type == NativeEvent::LMouseDown ||
            event->type == NativeEvent::LMouseUp ||
            event->type == NativeEvent::CursorMove)){
            capturedView_->emit(event);
            return;
        }

        // §2.3a T1: track the latest cursor position so a tooltip is
        // anchored where the cursor rested, and so an intra-widget move
        // updates the anchor without restarting the hover timer.
        lastCursorPos_ = pos;

        // §2.3a T1: a mouse-down anywhere takes down a pending / shown
        // tooltip (you clicked — the hint's job is done).
        if(event->type == NativeEvent::LMouseDown ||
           event->type == NativeEvent::RMouseDown){
            cancelTooltip();
        }

        // Overlay-Z-Order-Plan O3 §5.1: hit-test precedence. An
        // absorbing overlay under the cursor claims the event before
        // the main tree. Non-absorbing overlays (tooltips, drag-ghosts)
        // return nullptr here, so hover / clicks pass straight through
        // to the main tree ("click through a tooltip").
        View * overlayTarget = hitTestOverlay(pos);

        // Overlay-Z-Order-Plan O3 §5.2: click-outside dismissal. On a
        // mouse-down, dismiss every presented overlay whose clickOutside
        // policy is set and whose bounds exclude the point (topmost
        // first). When the down did not land inside an absorbing overlay
        // and at least one overlay was dismissed, the gesture is consumed
        // — the click that closes a popover must not also activate a
        // widget beneath it. A down inside an absorbing overlay still
        // runs the sweep (to close other, stale overlays) but is then
        // delivered to that overlay rather than consumed.
        if(event->type == NativeEvent::LMouseDown ||
           event->type == NativeEvent::RMouseDown){
            if(overlayHost_ != nullptr){
                const bool dismissed = overlayHost_->dismissClickOutside(pos);
                if(overlayTarget == nullptr && dismissed){
                    return;
                }
            }
        }

        View *target = overlayTarget != nullptr ? overlayTarget : hitTest(pos);

        // Synthesize CursorEnter/CursorExit when the hovered view changes.
        if(target != hoveredView_){
            if(hoveredView_ != nullptr){
                // Widget-View-Paint-Lifecycle-Plan Tier D / D6.4
                // (2026-06-03): clear Hover on the View losing focus.
                hoveredView_->setPseudoClassBits(0x01U, false);
                auto *exitParams = new Native::CursorExitParams();
                exitParams->position = pos;
                hoveredView_->emit(Native::NativeEventPtr(
                    new NativeEvent(NativeEvent::CursorExit,exitParams)));
            }
            hoveredView_ = target;
            if(hoveredView_ != nullptr){
                // Tier D / D6.4 (2026-06-03): set Hover on the new target.
                hoveredView_->setPseudoClassBits(0x01U, true);
                auto *enterParams = new Native::CursorEnterParams();
                enterParams->position = pos;
                hoveredView_->emit(Native::NativeEventPtr(
                    new NativeEvent(NativeEvent::CursorEnter,enterParams)));
            }

            // §2.3a C1: commit the effective cursor shape for the new
            // topmost hovered view to the single OS cursor sink. Resolve by
            // walking up from the hit target to the nearest ancestor that
            // declared a shape (CSS / Qt cursor inheritance); fall back to
            // Arrow when nothing in the chain set one, or when the cursor is
            // over the window background (target == nullptr, so the walk
            // does not run). The dispatcher is the sole writer of the OS
            // cursor (§2.2) — Views only declare via View::setCursorShape.
            // Uses friend access to View::Impl (cursorShape_ / parent_ptr),
            // the same seam M1's click-focus parent walk uses just below.
            if(ownerWindow_ != nullptr){
                Native::CursorShape shape = Native::CursorShape::Arrow;
                for(View * v = target; v != nullptr; v = v->impl_->parent_ptr){
                    if(v->impl_->cursorShape_.has_value()){
                        shape = *v->impl_->cursorShape_;
                        break;
                    }
                }
                ownerWindow_->commitCursorShape(shape);
            }

            // §2.3a T1: tooltip hover tracking. Resolve the new hovered
            // View to its owning Widget; only when that owner *changes*
            // do we re-arm. Moving between a widget's own sub-views (a
            // Button's label vs. its background) resolves to the same
            // owner, so the pending / shown tooltip is left alone. A real
            // owner change cancels the old tooltip and arms a fresh
            // hover-delay timer when the new owner carries a tooltip. This
            // sits inside the hovered-View-changed branch (same site as
            // C1) so it runs once per hover transition, not per move.
            Widget * newOwner = owningWidgetOf(target);
            if(newOwner != tooltipWidget_){
                cancelTooltip();
                if(newOwner != nullptr && !newOwner->tooltip().empty()){
                    scheduleTooltip(newOwner);
                }
            }
        }

        // Widget-View-Paint-Lifecycle-Plan Tier D / D6.4 (2026-06-03):
        // Pressed bit on left-mouse transitions. Down sets, Up clears
        // — on whichever view is the current target (so a button you
        // press then drag off does not stay pressed on the original).
        // PseudoClass::Pressed == 0x02.
        if(event->type == NativeEvent::LMouseDown && target != nullptr){
            target->setPseudoClassBits(0x02U, true);
        }
        else if(event->type == NativeEvent::LMouseUp && target != nullptr){
            target->setPseudoClassBits(0x02U, false);
        }

        // Native-API-Completion-Proposal §2.3a Focus M1 (2026-06-25):
        // mouse-click focus. On a left mouse-down, walk up from the hit
        // view to the nearest click-focusable ancestor and make it the
        // focused view *before* the mouseDown reaches the delegate — the
        // AppKit / Qt order, and the reason a non-focusable Label clicked
        // inside a Button focuses the Button rather than nothing.
        // FocusReason::Mouse is deliberate: it suppresses the focus ring
        // (only keyboard traversal raises it — see the F-table's
        // isKeyboardReason gate). When no ancestor is click-focusable, the
        // click resigns focus from the current holder (clearFocus) — the
        // standard "click outside a field commits and defocuses it" behavior
        // users expect from a text field. (The walk uses friend access to
        // View::Impl::parent_ptr — there is no public parent accessor on View.)
        //
        // Nuance for later: a *focus-neutral* control (e.g. a formatting
        // toolbar button that must not steal focus from the text being
        // edited) would want to opt out of this clearFocus. WTK has no such
        // opt-out yet and TextInput is currently the only focusable widget, so
        // clearing on miss is correct today; revisit when focus-neutral
        // controls land (a per-View "declines focus steal" flag on the walk).
        if(event->type == NativeEvent::LMouseDown && target != nullptr){
            View * clickFocusTarget = nullptr;
            for(View * v = target; v != nullptr; v = v->impl_->parent_ptr){
                if(v->isClickFocusable()){
                    clickFocusTarget = v;
                    break;
                }
            }
            if(clickFocusTarget != nullptr){
                focusManager_->setFocus(clickFocusTarget, FocusReason::Mouse);
            } else {
                focusManager_->clearFocus();
            }
        }

        if(target != nullptr){
            // ScrollView-4.7-Integration-Plan V2.1 + V2.2: route ScrollWheel
            // and the mouse-button events through the bubbling dispatch so
            // an event that lands on a deep leaf reaches an ancestor handler
            // (a wheel over a band → the ScrollView; a click a leaf ignores
            // → an interactive ancestor). Consumers set `event->handled`
            // (Invariant A) — a Button absorbs its click, the ScrollView its
            // wheel — so propagation stops at the innermost handler. Hover
            // motion (CursorMove/CursorEnter) stays deepest-hit `emit`: hover
            // is driven by the WidgetTreeHost's own enter/exit synthesis
            // above, not by bubbling these to ancestors.
            switch(event->type){
                case NativeEvent::ScrollWheel:
                case NativeEvent::LMouseDown:
                case NativeEvent::LMouseUp:
                case NativeEvent::RMouseDown:
                case NativeEvent::RMouseUp:
                    target->dispatchEvent(event);
                    break;
                default:
                    target->emit(event);
                    break;
            }
        }
    }

    void WidgetTreeHost::attachToWindow(AppWindow * window){
        if(!attachedToWindow) {
            attachedToWindow = true;
            window->impl_->proxy.setFrontendPtr(compositor);
        }
    };

    void WidgetTreeHost::setRoot(WidgetPtr widget){
        // Widget-View-Paint-Lifecycle-Plan Tier D / D2 (2026-06-03):
        // the pre-4.8 observe/unobserve bracket around the assignment
        // is gone — per-view LayerTree observation retired in Phase 4.8,
        // and the methods themselves were deleted alongside this call
        // site. The compositor short-circuit (`compositor != nullptr`)
        // is no longer needed because nothing happens conditionally on
        // it anymore.
        if(root == widget){
            return;
        }
        root = widget;
        // §2.3a F4: keep the FocusManager's traversal root in lockstep
        // with the host's root widget — this is the single point where
        // `root` changes, so syncing here means `focusNext`/`focusPrevious`
        // never walk a stale tree. A null root (widget cleared) leaves the
        // manager with nothing to traverse.
        focusManager_->setRoot(widget != nullptr ? &widget->viewRef() : nullptr);
    };

    void WidgetTreeHost::aggregateMinSize(float & outWidthDp, float & outHeightDp) const {
        outWidthDp  = 1.f;
        outHeightDp = 1.f;
        if(root == nullptr){
            return;
        }
        View & rootView = root->viewRef();
        LayoutManager * mgr = rootView.layoutManager();
        if(mgr == nullptr){
            return;
        }
        const LayoutSize ms = mgr->minSize(rootView);
        outWidthDp  = std::max(1.f, ms.w);
        outHeightDp = std::max(1.f, ms.h);
    }
};
