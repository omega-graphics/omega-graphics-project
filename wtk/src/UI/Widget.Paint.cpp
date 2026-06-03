#include "WidgetImpl.h"

#include "WidgetTreeHost.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorClient.h"

#include <iostream>

namespace OmegaWTK {

namespace {

/// Widget-View-Paint-Lifecycle-Plan Tier D / D1 (2026-06-03):
/// pre-D1 each entry point ran through `Widget::executePaint`, which
/// derived View dirty bits from a `PaintReason`. With executePaint
/// gone, the three live entry points — `init()`, `invalidate()`, and
/// `invalidateNow()` — each share this small derivation:
///   * Paint always.
///   * Initial / ThemeChanged additionally require Style + Layout.
///   * Resize additionally requires Layout.
/// The new `invalidate()` / `invalidateNow()` and the kept `init()`
/// call this directly; the dispatcher (immediate vs deferred) is
/// then the call-site's choice rather than a method parameter on
/// executePaint.
uint8_t dirtyBitsForReason(PaintReason reason){
    uint8_t bits = View::Paint;
    switch(reason){
        case PaintReason::Initial:
        case PaintReason::ThemeChanged:
            bits |= View::Style | View::Layout;
            break;
        case PaintReason::Resize:
            bits |= View::Layout;
            break;
        case PaintReason::StateChanged:
            break;
    }
    return bits;
}

}

void Widget::init(){
    if(impl_->hasMounted){
        return;
    }
    onMount();
    impl_->hasMounted = true;
    view->enable();
    if(impl_->mode != PaintMode::Automatic){
        return;
    }
    // Widget-View-Paint-Lifecycle-Plan Tier D / D1 (2026-06-03):
    // inlined `executePaint(PaintReason::Initial, /*immediate=*/true)`.
    // The Initial path is always the first paint, so the prior
    // `initialDrawComplete` guard inside executePaint was redundant
    // with the `hasMounted` short-circuit above. Mark Style + Layout
    // + Paint, latch `initialDrawComplete`, then drive the central
    // FrameBuilder walk synchronously (matching the pre-D1
    // immediate-mode behavior `init` relied on for first-frame
    // visibility before the run-loop's first turn).
    if(view != nullptr){
        view->markDirty(dirtyBitsForReason(PaintReason::Initial));
    }
    impl_->initialDrawComplete = true;
    if(treeHost != nullptr){
        treeHost->paintDirty();
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
    //
    // Tier D / D1 (2026-06-03): no per-Widget `deferredReason`
    // bookkeeping anymore — the View's dirty bits already carry
    // everything `FrameBuilder::buildFrame` needs. Manual-mode
    // widgets still take the path (they can use invalidate to drive
    // the central paint; the old executePaint mode-guard only
    // applied to its own dispatch).
    if(view != nullptr){
        view->markDirty(dirtyBitsForReason(reason));
    }
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
    // Widget-View-Paint-Lifecycle-Plan Tier D / D1 (2026-06-03):
    // inlined `executePaint(reason, /*immediate=*/true)`. Manual-mode
    // widgets short-circuit (same pre-D1 guard inside executePaint).
    // Mark the same dirty bits the deferred path would, then run the
    // central paint synchronously via `treeHost->paintDirty()`. The
    // dead pre-D1 reentrancy state (`paintInProgress`,
    // `hasPendingInvalidate`, `pendingPaintReason`) is gone — Tier
    // B5's phase asserts already proved the reentrancy branch
    // unreachable.
    if(impl_->mode != PaintMode::Automatic){
        return;
    }
    if(reason == PaintReason::Initial && impl_->initialDrawComplete){
        return;
    }
    if(view != nullptr){
        view->markDirty(dirtyBitsForReason(reason));
    }
    if(reason == PaintReason::Initial){
        impl_->initialDrawComplete = true;
    }
    if(treeHost != nullptr){
        treeHost->paintDirty();
    }
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
