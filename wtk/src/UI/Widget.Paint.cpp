#include "WidgetImpl.h"

#include "WidgetTreeHost.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorClient.h"

#include <iostream>

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
        // Tier 3 Phase 3.8: the window-level FrameBuilder owns the
        // CompositeFrame and the composition session. Bracketing each
        // paint with a ScopedFrame makes onPaint -> UIView::update /
        // SVGView::paint run with an active FrameBuilder, so they
        // submit their DisplayList into the one window-scoped frame.
        // Nested-safe via FrameBuilder's depth counter: a display or
        // resize pass already has the outer frame open and this inner
        // ScopedFrame just shares it; a standalone invalidate opens
        // the outermost frame here. Null-safe when there is no tree
        // host / frame builder yet (pre-attach paints).
        FrameBuilder::ScopedFrame frame(
            treeHost != nullptr ? treeHost->frameBuilder() : nullptr);

        view->startCompositionSession();
        onPaint(activeReason);
        int submissions = 1;
        if(activeReason == PaintReason::Initial &&
           !impl_->initialDrawComplete &&
           impl_->options.autoWarmupOnInitialPaint){
            submissions = std::max<int>(1,impl_->options.warmupFrameCount);
        }
        // submitPaintFrame is a no-op since Phase 3.8 / 3.9: UIView and
        // SVGView submit their DisplayList through the window-level
        // FrameBuilder during onPaint, and the per-view CanvasView (the
        // last override) is gone, so there is no per-view frame to push.
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
    // Widget-View-Paint-Lifecycle-Plan Tier A: deferred. Set the
    // view's dirty bits derived from the reason and ask the window to
    // flush a frame on the next run-loop turn. A burst of invalidates
    // between frames coalesces into one frame (the run-loop primitive
    // dedups the requests; the dirty bits accumulate).
    if(view != nullptr){
        uint8_t bits = View::Paint;
        if(reason == PaintReason::Resize)
            bits |= View::Layout;
        if(reason == PaintReason::ThemeChanged)
            bits |= View::Style | View::Layout;
        view->markDirty(bits);
    }
    impl_->deferredReason = reason;
    // Null treeHost: not attached yet. The initial paint after
    // attach covers display; the dirty bits stay set harmlessly.
    if(treeHost != nullptr){
        treeHost->requestFrame();
    }
}

void Widget::invalidateNow(PaintReason reason){
#ifndef NDEBUG
    std::cerr << "[OmegaWTK] Widget::invalidateNow() forces a synchronous "
                 "paint, bypassing the deferred frame lifecycle. Prefer "
                 "invalidate()." << std::endl;
#endif
    executePaint(reason,true);
}

void Widget::flushPendingPaint(){
    // Called by WidgetTreeHost::paintDirty during the window frame
    // flush, for widgets whose view has the Paint dirty bit set.
    // Clear the dirty bits *before* painting (Chromium clears
    // needs_paint_ at Paint() entry) so that a re-invalidation issued
    // from within onPaint re-sets them and schedules the next frame
    // rather than being wiped by this one.
    PaintReason reason = impl_->deferredReason;
    if(view != nullptr){
        view->clearDirtyBits();
    }
    executePaint(reason,false);
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
