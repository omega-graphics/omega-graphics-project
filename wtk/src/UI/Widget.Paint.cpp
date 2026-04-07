#include "WidgetImpl.h"

#include "WidgetTreeHost.h"
#include "omegaWTK/UI/View.h"

namespace OmegaWTK {

void Widget::executePaint(PaintReason reason,bool immediate){
    if(impl_->mode != PaintMode::Automatic){
        return;
    }
    if(reason == PaintReason::Initial && impl_->initialDrawComplete){
        return;
    }
    if(impl_->paintInProgress){
        if(impl_->options.coalesceInvalidates){
            impl_->hasPendingInvalidate = true;
            impl_->pendingPaintReason = reason;
            return;
        }
        if(!immediate){
            return;
        }
    }
    impl_->paintInProgress = true;
    if(treeHost != nullptr){
        auto desiredFrontend = treeHost->compPtr();
        auto desiredLane = treeHost->laneId();
        if(view->compositorProxy().getFrontendPtr() != desiredFrontend ||
           view->compositorProxy().getSyncLaneId() != desiredLane){
            view->compositorProxy().setFrontendPtr(desiredFrontend);
            view->compositorProxy().setSyncLaneId(desiredLane);
        }
    }
    PaintReason activeReason = reason;
    while(true){
        view->startCompositionSession();
        onPaint(activeReason);
        int submissions = 1;
        if(activeReason == PaintReason::Initial &&
           !impl_->initialDrawComplete &&
           impl_->options.autoWarmupOnInitialPaint){
            submissions = std::max<int>(1,impl_->options.warmupFrameCount);
        }
        view->submitPaintFrame(submissions);
        view->endCompositionSession();
        if(activeReason == PaintReason::Initial){
            impl_->initialDrawComplete = true;
        }
        if(!(impl_->options.coalesceInvalidates && impl_->hasPendingInvalidate)){
            break;
        }
        activeReason = impl_->pendingPaintReason;
        impl_->hasPendingInvalidate = false;
    }
    impl_->paintInProgress = false;
}

void Widget::init(){
    if(impl_->hasMounted){
        return;
    }
    onMount();
    impl_->hasMounted = true;
    view->enable();
    if(impl_->mode == PaintMode::Automatic){
        executePaint(PaintReason::Initial,true);
    }
}

void Widget::setPaintMode(PaintMode mode){
    impl_->mode = mode;
}

PaintMode Widget::paintMode() const{
    return impl_->mode;
}

void Widget::setPaintOptions(const PaintOptions &options){
    impl_->options = options;
}

const PaintOptions & Widget::paintOptions() const{
    return impl_->options;
}

void Widget::invalidate(PaintReason reason){
    executePaint(reason,false);
}

void Widget::invalidateNow(PaintReason reason){
    executePaint(reason,true);
}

void Widget::onThemeSetRecurse(Native::ThemeDesc &desc){
    onThemeSet(desc);
    if(impl_->mode == PaintMode::Automatic){
        invalidate(PaintReason::ThemeChanged);
    }
    for(const auto & child : childWidgets()){
        if(child != nullptr){
            child->onThemeSetRecurse(desc);
        }
    }
}

}
