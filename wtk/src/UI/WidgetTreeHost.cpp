#include "omegaWTK/UI/WidgetTreeHost.h"
#include "../Composition/Compositor.h"

#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"
#include <atomic>
#include <cmath>
#include <sstream>

namespace OmegaWTK {
    namespace {
        std::atomic<uint64_t> g_widgetTreeSyncLaneSeed {1};
        std::atomic<uint64_t> g_resizeSessionSeed {1};

        Composition::Compositor *globalCompositor(){
            static Composition::Compositor compositor;
            return &compositor;
        }

        constexpr float kVelocitySettlingEpsilon = 20.f;
        constexpr float kAccelerationSettlingEpsilon = 80.f;

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

        static const char *paintReasonName(PaintReason reason){
            switch(reason){
                case PaintReason::Initial:
                    return "Initial";
                case PaintReason::StateChanged:
                    return "StateChanged";
                case PaintReason::Resize:
                    return "Resize";
                case PaintReason::ThemeChanged:
                    return "ThemeChanged";
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
        inSession = true;
        currentSessionId = nextSessionId();
        lastWidth = width;
        lastHeight = height;
        lastVelocity = 0.f;
        lastTick = std::chrono::steady_clock::now();
        ResizeSessionState state {};
        state.sessionId = currentSessionId;
        state.phase = ResizePhase::Active;
        state.sample.timestampMs = tMs;
        state.sample.width = width;
        state.sample.height = height;
        state.sample.velocityPxPerSec = 0.f;
        state.sample.accelerationPxPerSec2 = 0.f;
        return state;
    }

    ResizeSessionState ResizeDynamicsTracker::update(float width,float height,double tMs){
        if(!inSession){
            return begin(width,height,tMs);
        }
        auto nowTick = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<float>(nowTick - lastTick).count();
        float velocity = 0.f;
        float acceleration = 0.f;
        if(dt > 1e-6f){
            const auto dw = width - lastWidth;
            const auto dh = height - lastHeight;
            const auto delta = std::sqrt((dw * dw) + (dh * dh));
            velocity = delta / dt;
            acceleration = (velocity - lastVelocity) / dt;
        }
        velocity = finiteOrZero(velocity);
        acceleration = finiteOrZero(acceleration);

        ResizeSessionState state {};
        state.sessionId = currentSessionId;
        state.phase = (std::fabs(velocity) <= kVelocitySettlingEpsilon &&
                       std::fabs(acceleration) <= kAccelerationSettlingEpsilon)
                              ? ResizePhase::Settling
                              : ResizePhase::Active;
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
        for(auto & child : parent->children){
            initWidgetRecurse(child);
        }
    }

    void WidgetTreeHost::observeWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        if(parent->layerTree != nullptr){
            compositor->observeLayerTree(parent->layerTree.get(),syncLaneId);
        }
        for(auto & child : parent->children){
            observeWidgetLayerTreesRecurse(child);
        }
    }

    void WidgetTreeHost::unobserveWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        if(parent->layerTree != nullptr){
            compositor->unobserveLayerTree(parent->layerTree.get());
        }
        for(auto & child : parent->children){
            unobserveWidgetLayerTreesRecurse(child);
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
        for(auto & child : parent->children){
            invalidateWidgetRecurse(child,reason,immediate);
        }
    }

    void WidgetTreeHost::beginResizeCoordinatorSessionRecurse(Widget *parent,std::uint64_t sessionId){
        if(parent == nullptr || parent->rootView == nullptr){
            return;
        }
        parent->rootView->getResizeCoordinator().beginResizeSession(sessionId);
        for(auto & child : parent->children){
            beginResizeCoordinatorSessionRecurse(child,sessionId);
        }
    }

    bool WidgetTreeHost::detectAnimatedTreeRecurse(Widget *parent) const{
        if(parent == nullptr){
            return false;
        }
        if(parent->paintMode() != PaintMode::Automatic){
            return true;
        }
        for(auto & child : parent->children){
            if(detectAnimatedTreeRecurse(child)){
                return true;
            }
        }
        return false;
    }

    void WidgetTreeHost::flushAuthoritativeResizeFrame(){
        if(root == nullptr){
            return;
        }
        staticSuspendVerification.authoritativeFlushCount += 1;
        invalidateWidgetRecurse(root.get(),PaintReason::Resize,true);
    }

    void WidgetTreeHost::notePaintDeferredDuringResize(PaintReason reason,bool immediate){
        if(!staticResizeSuspendActive){
            return;
        }
        staticSuspendVerification.deferredPaintCount += 1;
        staticSuspendVerification.lastDeferredReason = reason;
        if(reason == PaintReason::Resize){
            staticSuspendVerification.deferredResizePaintCount += 1;
        }
        if(immediate){
            staticSuspendVerification.deferredImmediatePaintCount += 1;
        }
    }

    void WidgetTreeHost::emitStaticSuspendVerificationSummary(bool flushIssued){
        if(!staticSuspendVerification.active){
            return;
        }
        const bool hasUpdates = staticSuspendVerification.resizeUpdateCount > 0;
        const bool pass = !hasUpdates ||
                          (staticSuspendVerification.deferredPaintCount > 0 &&
                           staticSuspendVerification.authoritativeFlushCount > 0 &&
                           flushIssued);
        std::ostringstream stream;
        stream << "SliceCVerify lane=" << syncLaneId
               << " id=" << staticSuspendVerification.sessionId
               << " updates=" << staticSuspendVerification.resizeUpdateCount
               << " deferred=" << staticSuspendVerification.deferredPaintCount
               << " deferredResize=" << staticSuspendVerification.deferredResizePaintCount
               << " deferredImmediate=" << staticSuspendVerification.deferredImmediatePaintCount
               << " flushCount=" << staticSuspendVerification.authoritativeFlushCount
               << " flushIssued=" << (flushIssued ? 1 : 0)
               << " lastReason=" << paintReasonName(staticSuspendVerification.lastDeferredReason)
               << " result=" << (pass ? "PASS" : "WARN");
        OMEGAWTK_DEBUG(stream.str());
        staticSuspendVerification.active = false;
    }

    void WidgetTreeHost::initWidgetTree(){
        observeWidgetLayerTreesRecurse(root.get());
        root->setTreeHostRecurse(this);
        initWidgetRecurse(root.get());
        auto repaintRecurse = [&](auto &&self,Widget *widget) -> void {
            if(widget == nullptr){
                return;
            }
            if(widget->paintMode() == PaintMode::Automatic){
                widget->invalidateNow(PaintReason::Initial);
            }
            for(auto & child : widget->children){
                self(self,child);
            }
        };
        repaintRecurse(repaintRecurse,root.get());
    }

    void WidgetTreeHost::notifyWindowResize(const Core::Rect &rect){
        if(root != nullptr){
            root->handleHostResize(rect);
        }
        lastResizeSessionState = resizeTracker.update(rect.w,rect.h,nowMs());
        if(staticSuspendVerification.active){
            staticSuspendVerification.resizeUpdateCount += 1;
        }
        if(staticResizeSuspendActive){
            pendingAuthoritativeResizeFrame = true;
        }
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " v=" << lastResizeSessionState.sample.velocityPxPerSec
               << " a=" << lastResizeSessionState.sample.accelerationPxPerSec2;
        OMEGAWTK_DEBUG(stream.str());
    }

    void WidgetTreeHost::notifyWindowResizeBegin(const Core::Rect &rect){
        lastResizeSessionState = resizeTracker.begin(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        staticResizeSuspendActive = !lastResizeSessionState.animatedTree;
        pendingAuthoritativeResizeFrame = staticResizeSuspendActive;
        staticSuspendVerification = {};
        staticSuspendVerification.active = staticResizeSuspendActive;
        staticSuspendVerification.sessionId = lastResizeSessionState.sessionId;
        staticSuspendVerification.lastDeferredReason = PaintReason::StateChanged;
        if(staticResizeSuspendActive){
            beginResizeCoordinatorSessionRecurse(root.get(),lastResizeSessionState.sessionId);
        }
        if(root != nullptr){
            // Apply host geometry after suspend policy is armed so the first
            // live-resize pass cannot submit an unsynchronized frame.
            root->handleHostResize(rect);
        }
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " policy=" << (staticResizeSuspendActive ? "StaticSuspend" : "AnimatedGoverned");
        OMEGAWTK_DEBUG(stream.str());
    }

    void WidgetTreeHost::notifyWindowResizeEnd(const Core::Rect &rect){
        if(root != nullptr){
            root->handleHostResize(rect);
        }
        lastResizeSessionState = resizeTracker.end(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        const bool shouldFlush = staticResizeSuspendActive && pendingAuthoritativeResizeFrame;
        staticResizeSuspendActive = false;
        pendingAuthoritativeResizeFrame = false;
        if(shouldFlush){
            flushAuthoritativeResizeFrame();
        }
        emitStaticSuspendVerificationSummary(shouldFlush);
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " v=" << lastResizeSessionState.sample.velocityPxPerSec
               << " a=" << lastResizeSessionState.sample.accelerationPxPerSec2
               << " flush=" << (shouldFlush ? "authoritative" : "none");
        OMEGAWTK_DEBUG(stream.str());
    }

    bool WidgetTreeHost::shouldSuspendPaintDuringResize() const{
        return staticResizeSuspendActive;
    }

    void WidgetTreeHost::attachToWindow(AppWindow * window){
        if(!attachedToWindow) {
            attachedToWindow = true;
            window->_add_widget(root.get());
            window->proxy.setFrontendPtr(compositor);
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
