#include "WidgetTreeHost.h"
#include "../Composition/Compositor.h"

#include "AppWindowImpl.h"
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"
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
            const char *raw = std::getenv(name);
            if(raw == nullptr || raw[0] == '\0'){
                return fallback;
            }
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
            const char *raw = std::getenv(name);
            if(raw == nullptr || raw[0] == '\0'){
                return fallback;
            }
            return raw[0] != '0';
        }

        static std::uint64_t readEnvU64Clamp(const char *name,std::uint64_t fallback,std::uint64_t minValue,std::uint64_t maxValue){
            const char *raw = std::getenv(name);
            if(raw == nullptr || raw[0] == '\0'){
                return fallback;
            }
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
    attachedToWindow(false)
    {

    };

    WidgetTreeHost::~WidgetTreeHost(){
        if(compositor != nullptr && root != nullptr){
            unobserveWidgetLayerTreesRecurse(root.get());
        }
        compositor = nullptr;
    };

    SharedHandle<WidgetTreeHost> WidgetTreeHost::Create(){
        return SharedHandle<WidgetTreeHost>(new WidgetTreeHost());
    };

    void WidgetTreeHost::initWidgetRecurse(Widget *parent){
        parent->init();
        for(const auto & child : parent->childWidgets()){
            initWidgetRecurse(child.get());
        }
    }

    void WidgetTreeHost::observeWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        auto *rootTree = parent->view ? parent->view->getLayerTree() : nullptr;
        if(rootTree != nullptr){
            compositor->observeLayerTree(rootTree,syncLaneId);
        }
        for(const auto & child : parent->childWidgets()){
            observeWidgetLayerTreesRecurse(child.get());
        }
    }

    void WidgetTreeHost::unobserveWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        auto *rootTree = parent->view ? parent->view->getLayerTree() : nullptr;
        if(rootTree != nullptr){
            compositor->unobserveLayerTree(rootTree);
        }
        for(const auto & child : parent->childWidgets()){
            unobserveWidgetLayerTreesRecurse(child.get());
        }
    }

    void WidgetTreeHost::invalidateWidgetRecurse(Widget *parent,
                                                 PaintReason reason,
                                                 bool immediate){
        if(parent == nullptr){
            return;
        }
        if(parent->paintMode() == PaintMode::Automatic){
            if(immediate){
                parent->invalidateNow(reason);
            }
            else {
                parent->invalidate(reason);
            }
        }
        for(const auto & child : parent->childWidgets()){
            invalidateWidgetRecurse(child.get(),reason,immediate);
        }
    }

    void WidgetTreeHost::beginResizeCoordinatorSessionRecurse(Widget *parent,std::uint64_t sessionId){
        if(parent == nullptr || parent->view == nullptr){
            return;
        }
        parent->view->getResizeCoordinator().beginResizeSession(sessionId);
        for(const auto & child : parent->childWidgets()){
            beginResizeCoordinatorSessionRecurse(child.get(),sessionId);
        }
    }

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
        // Views before observing layer trees or initializing widgets,
        // so that compositor wiring uses the correct (shared) target.
        if(windowRenderTarget_ != nullptr){
            propagateWindowRenderTargetRecurse(root.get());
        }
        observeWidgetLayerTreesRecurse(root.get());
        root->setTreeHostRecurse(this);
        initWidgetRecurse(root.get());
        // Note: Widget::init() already calls executePaint(Initial, true) for
        // each Automatic-mode widget during initWidgetRecurse, so a second
        // repaint pass is not needed.  The redundant pass was generating
        // duplicate compositor commands during startup, causing the first
        // content frames to be overwritten by a late-arriving clear.
    }

    void WidgetTreeHost::notifyWindowResize(const Composition::Rect &rect){
        if(root != nullptr){
            root->handleHostResize(rect);
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
        beginResizeCoordinatorSessionRecurse(root.get(),lastResizeSessionState.sessionId);
        if(root != nullptr){
            root->handleHostResize(rect);
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
        if(root != nullptr){
            root->handleHostResize(rect);
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
                auto *exitParams = new Native::CursorExitParams();
                exitParams->position = pos;
                hoveredView_->emit(Native::NativeEventPtr(
                    new NativeEvent(NativeEvent::CursorExit,exitParams)));
            }
            hoveredView_ = target;
            if(hoveredView_ != nullptr){
                auto *enterParams = new Native::CursorEnterParams();
                enterParams->position = pos;
                hoveredView_->emit(Native::NativeEventPtr(
                    new NativeEvent(NativeEvent::CursorEnter,enterParams)));
            }
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
        if(root == widget){
            return;
        }
        if(root != nullptr && compositor != nullptr){
            unobserveWidgetLayerTreesRecurse(root.get());
        }
        root = widget;
        if(root != nullptr && compositor != nullptr){
            observeWidgetLayerTreesRecurse(root.get());
        }
    };
};
