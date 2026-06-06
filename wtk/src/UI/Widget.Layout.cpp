#include "WidgetImpl.h"
#include "omegaWTK/UI/View.h"

namespace OmegaWTK {

void Widget::handleHostResize(const Composition::Rect &rect){
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
    // UIView-Render-Redesign-Plan Phase F (2026-06-05): the pre-Phase-F
    // `invalidate(PaintReason::Resize)` call here is gone. Resize now
    // force-repaints the whole tree at the `WidgetTreeHost` level
    // (`WidgetTreeHost::forceFullRepaint` called from the three
    // `notifyWindowResize*` paths after `handleHostResize`); a per-widget
    // `invalidate` would double-request a frame (the host already drove
    // a synchronous repaint inside the resize ScopedFrame). The
    // `PaintOptions::invalidateOnResize` field stays so existing callers
    // remain source-compatible, but is no longer a relayout/repaint gate
    // for the host-resize path. `Widget::setRect` (Widget.Geometry.cpp)
    // still honors it for programmatic, non-host-driven geometry changes.
}

void Widget::setLayoutStyle(const LayoutStyle & style){
    impl_->layoutStyle_ = style;
    impl_->hasExplicitLayoutStyle_ = true;
}

const LayoutStyle & Widget::layoutStyle() const {
    return impl_->layoutStyle_;
}

void Widget::setLayoutBehavior(LayoutBehaviorPtr behavior){
    impl_->layoutBehavior_ = std::move(behavior);
}

LayoutBehaviorPtr Widget::layoutBehavior() const {
    return impl_->layoutBehavior_;
}

void Widget::requestLayout(){
    if(parent != nullptr){
        parent->requestLayout();
    }
}

bool Widget::hasExplicitLayoutStyle() const {
    return impl_->hasExplicitLayoutStyle_;
}

MeasureResult Widget::measureSelf(const LayoutContext & /*ctx*/){
    auto & r = rect();
    return {r.w, r.h};
}

void Widget::onLayoutResolved(const Composition::Rect & finalRectPx){
    setRect(finalRectPx);
}

}
