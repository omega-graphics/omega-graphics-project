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
    if(impl_->mode == PaintMode::Automatic &&
       impl_->options.invalidateOnResize &&
       treeHost != nullptr &&
       impl_->hasMounted){
        invalidate(PaintReason::Resize);
    }
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
