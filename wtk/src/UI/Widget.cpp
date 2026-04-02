#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/Layout.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "../Composition/Compositor.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/WidgetTreeHost.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>


namespace OmegaWTK {

namespace {

static inline const char * geometryReasonLabel(GeometryChangeReason reason){
    switch(reason){
        case GeometryChangeReason::ParentLayout:
            return "ParentLayout";
        case GeometryChangeReason::ChildRequest:
            return "ChildRequest";
        case GeometryChangeReason::UserInput:
            return "UserInput";
    }
    return "Unknown";
}

static inline bool geometryTraceEnvEnabled(){
    static int cached = -1;
    if(cached >= 0){
        return cached == 1;
    }
    const auto * envValue = std::getenv("OMEGAWTK_GEOMETRY_TRACE");
    if(envValue == nullptr){
        cached = 0;
        return false;
    }
    const auto equalsIgnoreCase = [](const char *lhs,const char *rhs) -> bool {
        if(lhs == nullptr || rhs == nullptr){
            return false;
        }
        while(*lhs != '\0' && *rhs != '\0'){
            if(std::tolower(static_cast<unsigned char>(*lhs)) !=
               std::tolower(static_cast<unsigned char>(*rhs))){
                return false;
            }
            ++lhs;
            ++rhs;
        }
        return *lhs == '\0' && *rhs == '\0';
    };
    if(std::strcmp(envValue,"0") == 0 ||
       equalsIgnoreCase(envValue,"false") ||
       equalsIgnoreCase(envValue,"off") ||
       equalsIgnoreCase(envValue,"no")){
        cached = 0;
        return false;
    }
    cached = 1;
    return true;
}

static inline void geometryTraceLog(const char * phase,
                                    const Widget * widget,
                                    const Widget * parent,
                                    GeometryChangeReason reason,
                                    const Core::Rect & lhs,
                                    const Core::Rect & rhs,
                                    const Widget::GeometryTraceContext & syncCtx){
    if(!geometryTraceEnvEnabled()){
        return;
    }
    std::fprintf(stderr,
                 "[OmegaWTKGeometry] phase=%s lane=%llu packet=%llu widget=%p parent=%p reason=%s lhs={x:%.3f y:%.3f w:%.3f h:%.3f} rhs={x:%.3f y:%.3f w:%.3f h:%.3f}\n",
                 phase,
                 static_cast<unsigned long long>(syncCtx.syncLaneId),
                 static_cast<unsigned long long>(syncCtx.predictedPacketId),
                 static_cast<const void *>(widget),
                 static_cast<const void *>(parent),
                 geometryReasonLabel(reason),
                 lhs.pos.x,lhs.pos.y,lhs.w,lhs.h,
                 rhs.pos.x,rhs.pos.y,rhs.w,rhs.h);
}

}

Widget::Widget(Core::Rect rect):view(CanvasView::Create(rect)){
}

Widget::Widget(ViewPtr view):view(std::move(view)){
};

//Widget::Widget(Widget & widget):parent(std::move(widget.parent)),compositor(std::move(widget.compositor)),view(std::move(widget.view)){
//    
//};

void Widget::executePaint(PaintReason reason,bool immediate){
    if(mode != PaintMode::Automatic){
        return;
    }
    if(reason == PaintReason::Initial && initialDrawComplete){
        return;
    }
    if(paintInProgress){
        if(options.coalesceInvalidates){
            hasPendingInvalidate = true;
            pendingPaintReason = reason;
            return;
        }
        if(!immediate){
            return;
        }
    }
    paintInProgress = true;
    if(treeHost != nullptr){
        auto desiredFrontend = treeHost->compPtr();
        auto desiredLane = treeHost->laneId();
        if(view->proxy.getFrontendPtr() != desiredFrontend ||
           view->proxy.getSyncLaneId() != desiredLane){
            view->proxy.setFrontendPtr(desiredFrontend);
            view->proxy.setSyncLaneId(desiredLane);
        }
    }
    PaintReason activeReason = reason;
    while(true){
        view->startCompositionSession();
        onPaint(activeReason);
        int submissions = 1;
        if(activeReason == PaintReason::Initial &&
           !initialDrawComplete &&
           options.autoWarmupOnInitialPaint){
            submissions = std::max<int>(1,options.warmupFrameCount);
        }
        view->submitPaintFrame(submissions);
        view->endCompositionSession();
        if(activeReason == PaintReason::Initial){
            initialDrawComplete = true;
        }
        if(!(options.coalesceInvalidates && hasPendingInvalidate)){
            break;
        }
        activeReason = pendingPaintReason;
        hasPendingInvalidate = false;
    }
    paintInProgress = false;
}

void Widget::init(){
    if(hasMounted){
        return;
    }
    onMount();
    hasMounted = true;
    view->enable();
    if(mode == PaintMode::Automatic){
        executePaint(PaintReason::Initial,true);
    }
}

void Widget::setPaintMode(PaintMode mode){
    this->mode = mode;
}

PaintMode Widget::paintMode() const{
    return mode;
}

void Widget::setPaintOptions(const PaintOptions &options){
    this->options = options;
}

const PaintOptions & Widget::paintOptions() const{
    return options;
}

void Widget::invalidate(PaintReason reason){
    executePaint(reason,false);
}

void Widget::invalidateNow(PaintReason reason){
    executePaint(reason,true);
}

void Widget::handleHostResize(const Core::Rect &rect){
    auto oldRect = this->rect();
    view->resize(rect);
    auto & rootRect = view->getRect();
    auto newRect = rootRect;
    this->resize(newRect);

    LayoutContext layoutCtx {};
    layoutCtx.availableRectPx = rootRect;
    layoutCtx.dpiScale = 1.f;
    runWidgetLayout(*this, layoutCtx);

    WIDGET_NOTIFY_OBSERVERS_RESIZE(oldRect);
    if(mode == PaintMode::Automatic &&
       options.invalidateOnResize &&
       treeHost != nullptr &&
       hasMounted){
        invalidate(PaintReason::Resize);
    }
}

void Widget::onThemeSetRecurse(Native::ThemeDesc &desc){
    onThemeSet(desc);
    if(mode == PaintMode::Automatic){
        invalidate(PaintReason::ThemeChanged);
    }
    for(auto * child : childWidgets()){
        if(child != nullptr){
            child->onThemeSetRecurse(desc);
        }
    }
}


Core::Rect & Widget::rect(){
    return view->getRect();
};

bool Widget::requestRect(const Core::Rect &requested,GeometryChangeReason reason){
    auto oldRect = rect();
    auto syncCtx = geometryTraceContext();
    geometryTraceLog("proposal",this,parent,reason,oldRect,requested,syncCtx);
    if(parent == nullptr){
        setRect(requested);
        geometryTraceLog("commit",this,parent,reason,oldRect,rect(),syncCtx);
        return true;
    }

    GeometryProposal proposal {};
    proposal.requested = requested;
    proposal.reason = reason;
    auto clamped = parent->clampChildRect(*this,proposal);
    geometryTraceLog("clamp",this,parent,reason,requested,clamped,syncCtx);
    setRect(clamped);
    parent->onChildRectCommitted(*this,oldRect,rect(),reason);
    geometryTraceLog("commit",this,parent,reason,oldRect,rect(),syncCtx);
    return true;
}

void Widget::setRect(const Core::Rect &newRect){
    auto oldRect = rect();
    view->resize(newRect);
    auto & rootRect = view->getRect();
    auto updatedRect = rootRect;
    this->resize(updatedRect);
    WIDGET_NOTIFY_OBSERVERS_RESIZE(oldRect);
    if(mode == PaintMode::Automatic &&
       options.invalidateOnResize &&
       treeHost != nullptr &&
       hasMounted){
        invalidate(PaintReason::Resize);
    }
}

OmegaCommon::Vector<Widget *> Widget::childWidgets() const{
    return {};
}

void Widget::show(){

    view->enable();
    WIDGET_NOTIFY_OBSERVERS_SHOW();

};
void Widget::hide(){
    view->disable();
    WIDGET_NOTIFY_OBSERVERS_HIDE();
};

void Widget::addObserver(WidgetObserverPtr observer){
    if(!observer->hasAssignment) {
        observers.push_back(observer);
        observer->hasAssignment = true;
    };
};

void Widget::setTreeHostRecurse(WidgetTreeHost *host){
    treeHost = host;
    if(host != nullptr){
        // View::setFrontendRecurse handles per-view LayerTree observation.
        view->setFrontendRecurse(host->compPtr());
        view->setSyncLaneRecurse(host->laneId());
    }
    else {
        view->setFrontendRecurse(nullptr);
        view->setSyncLaneRecurse(0);
    }
    for(auto * c : childWidgets()){
        if(c != nullptr){
            c->setTreeHostRecurse(host);
        }
    };
};

void Widget::removeObserver(WidgetObserverPtr observerPtr){
    auto it = observers.begin();
    while(it != observers.end()){
        if(*it == observerPtr){
            observers.erase(it);
            observerPtr->hasAssignment = false;
            break;
        };
        ++it;
    };
};

void Widget::notifyObservers(Widget::WidgetEventType event_ty,Widget::WidgetEventParams params){
    for(auto & observer : observers){
        switch (event_ty) {
            case Show : {
                observer->onWidgetDidShow();
                break;
            };
            case Hide : {
                observer->onWidgetDidHide();
                break;
            };
            case Resize : {
                observer->onWidgetChangeSize(params.rect,view->rect);
                break;
            }
            case Attach : {
                observer->onWidgetAttach(params.widget);
                break;
            }
            case Detach : {
                observer->onWidgetDetach(params.widget);
                break;
            }
        }
    };
};

Core::Rect Widget::clampChildRect(const Widget &child,const GeometryProposal &proposal) const{
    (void)child;
    return proposal.requested;
}

void Widget::onChildRectCommitted(const Widget & child,
                                  const Core::Rect & oldRect,
                                  const Core::Rect & newRect,
                                  GeometryChangeReason reason){
    (void)child;
    (void)oldRect;
    (void)newRect;
    (void)reason;
}

void Widget::setLayoutStyle(const LayoutStyle & style){
    layoutStyle_ = style;
    hasExplicitLayoutStyle_ = true;
}

const LayoutStyle & Widget::layoutStyle() const {
    return layoutStyle_;
}

void Widget::setLayoutBehavior(LayoutBehaviorPtr behavior){
    layoutBehavior_ = std::move(behavior);
}

LayoutBehaviorPtr Widget::layoutBehavior() const {
    return layoutBehavior_;
}

void Widget::requestLayout(){
    if(parent != nullptr){
        parent->requestLayout();
    }
}

bool Widget::hasExplicitLayoutStyle() const {
    return hasExplicitLayoutStyle_;
}

View & Widget::viewRef(){
    return *view;
}

MeasureResult Widget::measureSelf(const LayoutContext & /*ctx*/){
    auto & r = rect();
    return {r.w, r.h};
}

void Widget::onLayoutResolved(const Core::Rect & finalRectPx){
    setRect(finalRectPx);
}

bool Widget::geometryTraceLoggingEnabled(){
    return geometryTraceEnvEnabled();
}

Widget::GeometryTraceContext Widget::geometryTraceContext() const{
    GeometryTraceContext ctx {};
    if(view == nullptr){
        return ctx;
    }

    ctx.syncLaneId = view->proxy.getSyncLaneId();
    auto diag = view->proxy.getSyncLaneDiagnostics();
    if(diag.lastSubmittedPacketId > 0){
        ctx.predictedPacketId = diag.lastSubmittedPacketId + 1;
    }
    else if(diag.lastPresentedPacketId > 0){
        ctx.predictedPacketId = diag.lastPresentedPacketId + 1;
    }
    else {
        ctx.predictedPacketId = 1;
    }
    return ctx;
}

Widget::~Widget(){
    parent = nullptr;
}


WidgetObserver::WidgetObserver():hasAssignment(false),widget(nullptr){

};

}
