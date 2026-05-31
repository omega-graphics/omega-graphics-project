#include "WidgetImpl.h"

#include "WidgetTreeHost.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorClient.h"

#include <iostream>

namespace OmegaWTK {

void Widget::executePaint(PaintReason reason,bool immediate){
    // Phase 4.7.4: the entry-point switch. `executePaint` no longer
    // opens a ScopedFrame, dispatches `onPaint`, or routes per-widget
    // through the legacy submitView path. Its only job now is:
    //   1. Mark the appropriate dirty bits on the backing View
    //      (which propagate to the root via `markDirty`'s ancestor
    //      walk, Phase 4.7.3).
    //   2. Trigger the central per-window paint:
    //       - `immediate` ⇒ run `WidgetTreeHost::paintDirty()` inline
    //         (used by `invalidateNow` and the resize / initial-paint
    //         legacy entry points).
    //       - otherwise ⇒ ask the window to flush a frame on the next
    //         run-loop turn (`treeHost->requestFrame()`) — the normal
    //         deferred path.
    // The actual Style / Layout / Paint passes live in
    // `FrameBuilder::buildFrame`, which the frame flush calls once
    // per window with the root View. The pre-4.7.4 per-widget
    // `onPaint` → `UIView::update` → `submitView` flow is gone.
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

    if(view != nullptr){
        uint8_t bits = View::Paint;
        if(reason == PaintReason::Initial){
            bits |= View::Style | View::Layout;
        }
        else if(reason == PaintReason::Resize){
            bits |= View::Layout;
        }
        else if(reason == PaintReason::ThemeChanged){
            bits |= View::Style | View::Layout;
        }
        view->markDirty(bits);
    }

    if(reason == PaintReason::Initial){
        impl_->initialDrawComplete = true;
    }

    if(treeHost == nullptr){
        // Pre-attach paint — the dirty bits stay set; the first
        // tree-attached frame flush picks them up.
        return;
    }
    if(immediate){
        impl_->paintInProgress = true;
        treeHost->paintDirty();
        impl_->paintInProgress = false;
    }
    else {
        treeHost->requestFrame();
    }
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
