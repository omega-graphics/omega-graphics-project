#include "WidgetTreeHost.h"
#include "../Composition/Compositor.h"

#include "AppWindowImpl.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/OverlayHost.h"
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
    overlayHost_(Core::UniquePtr<OverlayHost>(new OverlayHost(*this)))
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
        fb->buildFrame(*root->view);
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

        void markFullRepaintRecurse(View & view){
            view.markDirty(kFullRepaintBits);
            for(auto * sv : view.subviews()){
                if(sv != nullptr){
                    markFullRepaintRecurse(*sv);
                }
            }
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
            if(childView.containsPoint(point)){
                Composition::Point2D localPoint {
                    point.x - childView.getRect().pos.x,
                    point.y - childView.getRect().pos.y
                };
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

    void WidgetTreeHost::dispatchInputEvent(Native::NativeEventPtr event){
        if(root == nullptr){
            return;
        }
        using Native::NativeEvent;

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
                // Keyboard events go to root widget for now.
                // TODO: route to focused widget once focus tracking exists.
                if(root->view != nullptr){
                    root->view->emit(event);
                }
                return;
            }
            default:
                return;
        }

        if(!hasPosition){
            return;
        }

        View *target = hitTest(pos);

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

        if(target != nullptr){
            target->emit(event);
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
    };
};
