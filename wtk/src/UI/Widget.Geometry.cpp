#include "WidgetImpl.h"
#include "omegaWTK/UI/View.h"

namespace OmegaWTK {

Composition::Rect & Widget::rect(){
    return view->getRect();
}

bool Widget::requestRect(const Composition::Rect &requested,GeometryChangeReason reason){
    auto oldRect = rect();
    auto syncCtx = geometryTraceContext();
    WidgetInternal::geometryTraceLog("proposal",this,parent,reason,oldRect,requested,syncCtx);
    if(parent == nullptr){
        setRect(requested);
        WidgetInternal::geometryTraceLog("commit",this,parent,reason,oldRect,rect(),syncCtx);
        return true;
    }

    GeometryProposal proposal {};
    proposal.requested = requested;
    proposal.reason = reason;
    auto clamped = parent->clampChildRect(*this,proposal);
    WidgetInternal::geometryTraceLog("clamp",this,parent,reason,requested,clamped,syncCtx);
    setRect(clamped);
    parent->onChildRectCommitted(*this,oldRect,rect(),reason);
    WidgetInternal::geometryTraceLog("commit",this,parent,reason,oldRect,rect(),syncCtx);
    return true;
}

void Widget::setRect(const Composition::Rect &newRect){
    auto oldRect = rect();
    view->resize(newRect);
    auto & rootRect = view->getRect();
    auto updatedRect = rootRect;
    this->resize(updatedRect);
    WIDGET_NOTIFY_OBSERVERS_RESIZE(oldRect);
    if(impl_->mode == PaintMode::Automatic &&
       impl_->options.invalidateOnResize &&
       treeHost != nullptr &&
       impl_->hasMounted){
        invalidate(PaintReason::Resize);
    }
}

bool Widget::geometryTraceLoggingEnabled(){
    return WidgetInternal::geometryTraceEnvEnabled();
}

Widget::GeometryTraceContext Widget::geometryTraceContext() const{
    GeometryTraceContext ctx {};
    if(view == nullptr){
        return ctx;
    }

    ctx.syncLaneId = view->compositorProxy().getSyncLaneId();
    auto diag = view->compositorProxy().getSyncLaneDiagnostics();
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

}
