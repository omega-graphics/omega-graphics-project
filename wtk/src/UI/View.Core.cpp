#include "ViewImpl.h"

#include "omegaWTK/UI/Layout.h"
#include "omegaWTK/UI/AppWindow.h"

#include <iostream>
#include <utility>

#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "../Composition/Compositor.h"
#include "../Composition/backend/ResourceFactory.h"
#include "FrameBuilder.h"

namespace OmegaWTK {

Composition::CompositorClientProxy & View::compositorProxy(){
    return impl_->proxy;
}

const Composition::CompositorClientProxy & View::compositorProxy() const{
    return impl_->proxy;
}

SharedHandle<Composition::ViewRenderTarget> & View::renderTargetHandle(){
    return impl_->renderTarget;
}

const SharedHandle<Composition::ViewRenderTarget> & View::renderTargetHandle() const{
    return impl_->renderTarget;
}

Composition::Rect & View::getRect(){
    return impl_->rect;
}

Composition::LayerTree * View::getLayerTree(){
    return impl_->ownLayerTree.get();
}

bool View::isRootView(){
    return impl_->parent_ptr == nullptr;
}

ViewResizeCoordinator & View::getResizeCoordinator(){
    return impl_->resizeCoordinator;
}

const ViewResizeCoordinator & View::getResizeCoordinator() const{
    return impl_->resizeCoordinator;
}

bool View::containsPoint(const Composition::Point2D &point) const{
    const auto &r = impl_->rect;
    return point.x >= r.pos.x && point.x < r.pos.x + r.w &&
           point.y >= r.pos.y && point.y < r.pos.y + r.h;
}


View::View(const Composition::Rect & rect,ViewPtr parent):
    impl_(std::make_unique<Impl>(
            *this,
            ViewInternal::sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f}),
            parent.get())){
    // Phase 3: View is purely virtual. No NativeItem, no per-View render
    // target, no pre-created visual resources. The window's render target
    // is propagated via setWindowRenderTarget() when the widget tree
    // attaches to an AppWindow.
    if(impl_->parent_ptr != nullptr) {
        parent->addSubView(this);
    }
}

bool View::hasDelegate(){
    return impl_->delegate != nullptr;
}

void View::setDelegate(ViewDelegate *_delegate){
    impl_->delegate = _delegate;
    impl_->delegate->view = this;
    setReciever(impl_->delegate);
}

void View::addSubView(View * view){
    if(view == nullptr){
        return;
    }
    for(auto *existing : impl_->subviews){
        if(existing == view){
            return;
        }
    }
    impl_->subviews.emplace_back(view);
    view->impl_->parent_ptr = this;
    impl_->resizeCoordinator.registerChild(view,{});
    // Newly created subviews must inherit compositor wiring immediately.
    view->setFrontendRecurse(compositorProxy().getFrontendPtr());
    view->setSyncLaneRecurse(compositorProxy().getSyncLaneId());
}

void View::removeSubView(View *view){
    auto it  = impl_->subviews.begin();
    while(it != impl_->subviews.end()){
        auto *v = *it;
        if(v == view){
            impl_->subviews.erase(it);
            impl_->resizeCoordinator.unregisterChild(view);
            view->impl_->parent_ptr = nullptr;
            return;
        }
        ++it;
    }
}

void View::resize(Composition::Rect newRect){
    auto sanitized = ViewInternal::sanitizeRect(newRect,impl_->rect);
    if(ViewInternal::sameRect(impl_->rect,sanitized)){
        return;
    }
    impl_->rect = sanitized;
    // Phase 3: View is purely virtual. Update the layer tree rect but
    // do not call through to any NativeItem — there isn't one.
    if(impl_->ownLayerTree != nullptr && impl_->ownLayerTree->getRootLayer() != nullptr){
        impl_->ownLayerTree->getRootLayer()->resize(impl_->rect);
    }
    // Phase 2.5: emit on the new rect *after* it's been committed and
    // the layer tree caught up, so subscribers (NativeViewHost et al)
    // observe a consistent post-resize state.
    onLayoutResolved.emit(impl_->rect);
}


SharedHandle<Composition::Layer> View::makeLayer(Composition::Rect rect){
    auto layer = std::make_shared<Composition::Layer>(rect);
    layer->parentTree = impl_->ownLayerTree.get();
    impl_->ownLayerTree->addLayer(layer);
    return layer;
}

SharedHandle<Composition::Canvas> View::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
    return std::shared_ptr<Composition::Canvas>(new Composition::Canvas(compositorProxy(),*targetLayer,this));
}

void View::startCompositionSession(){
    if(compositorProxy().getFrontendPtr() == nullptr && impl_->parent_ptr != nullptr){
        auto parentFrontend = impl_->parent_ptr->compositorProxy().getFrontendPtr();
        if(parentFrontend != nullptr){
            compositorProxy().setFrontendPtr(parentFrontend);
            auto parentLane = impl_->parent_ptr->compositorProxy().getSyncLaneId();
            if(parentLane != 0){
                compositorProxy().setSyncLaneId(parentLane);
            }
        }
    }
}

void View::endCompositionSession(){
}

void View::enable() {
    impl_->enabled_ = true;
}

void View::disable() {
    impl_->enabled_ = false;
}

void View::applyLayoutDelta(const LayoutDelta & delta,
                            const LayoutTransitionSpec & spec){
    if(!spec.enabled || spec.durationSec <= 0.f || delta.changedProperties.empty()){
        resize(delta.toRectPx);
        return;
    }

    auto viewAnimator = SharedHandle<Composition::ViewAnimator>(
        new Composition::ViewAnimator(compositorProxy()));

    int dx = static_cast<int>(delta.toRectPx.pos.x - delta.fromRectPx.pos.x);
    int dy = static_cast<int>(delta.toRectPx.pos.y - delta.fromRectPx.pos.y);
    int dw = static_cast<int>(delta.toRectPx.w - delta.fromRectPx.w);
    int dh = static_cast<int>(delta.toRectPx.h - delta.fromRectPx.h);

    if(dx == 0 && dy == 0 && dw == 0 && dh == 0){
        resize(delta.toRectPx);
        return;
    }

    unsigned durationMs = static_cast<unsigned>(spec.durationSec * 1000.f);
    auto curve = spec.curve;
    if(curve == nullptr){
        curve = Composition::AnimationCurve::Linear();
    }
    viewAnimator->resizeTransition(dx,dy,dw,dh,durationMs,curve);
}

void View::setWindowRenderTarget(SharedHandle<Composition::ViewRenderTarget> windowRT){
    impl_->renderTarget = std::move(windowRT);
    compositorProxy().setRenderTarget(
            std::static_pointer_cast<Composition::CompositionRenderTarget>(impl_->renderTarget));
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setWindowRenderTarget(impl_->renderTarget);
        }
    }
}

float View::getRenderScale() const {
    if(impl_ != nullptr && impl_->renderTarget != nullptr){
        return impl_->renderTarget->getRenderScale();
    }
    return 1.f;
}

Composition::Point2D View::legacyComputeWindowOffset() const{
    Composition::Point2D offset {0.f, 0.f};
    const View *v = this;
    while(v != nullptr){
        offset.x += v->impl_->rect.pos.x;
        offset.y += v->impl_->rect.pos.y;
        // Tier 3 Phase 3.6: fold in the parent's `contentOffset()`
        // (defaults to {0,0}; ScrollView overrides to -scrollOffset_).
        // Sign convention flipped from the pre-3.6 path —
        // `contentOffset` is *added*, whereas the old
        // `scrollOffsetContribution` was subtracted, but the net effect
        // for ScrollView is identical (its contentOffset is the
        // negation of its prior scrollOffsetContribution). Both the
        // off-flag direct callers of `legacyComputeWindowOffset` and
        // the on-flag accumulator (which seeds itself from this walk
        // via `FrameBuilder::ScopedViewOffset`) get scroll-shifted
        // descendant offsets through this one site.
        if(v->impl_->parent_ptr != nullptr){
            auto co = v->impl_->parent_ptr->contentOffset();
            offset.x += co.x;
            offset.y += co.y;
        }
        v = v->impl_->parent_ptr;
    }
    return offset;
}

Composition::Point2D View::computeWindowOffset() const{
    // Tier 3 Phase 3.4: while an AppWindow-driven paint pass is in
    // flight AND the offset accumulator has a value pushed for this
    // view, the FrameBuilder's accumulator is the source of truth.
    // The widget tree walker pushes the widget's view; UIView::update
    // / SVGView::paint push the leaf view. Callers that resolve a
    // view's offset inside a ScopedFrame but outside any walker push
    // scope (e.g. NativeViewHost::syncBounds firing from
    // onLayoutResolved during handleHostResize) get the legacy
    // parent-chain walk so the answer matches the off-flag path.
    if(auto * fb = AppWindow::activeFrameBuilder();
       fb != nullptr && fb->hasOffsetOnStack()){
        return fb->currentOffset();
    }
    return legacyComputeWindowOffset();
}

Composition::Point2D View::scrollOffsetContribution() const{
    return {0.f, 0.f};
}

Composition::Point2D View::contentOffset() const{
    return {0.f, 0.f};
}

bool View::wantsLayer() const{
    return false;
}

bool View::isEnabled() const{
    return impl_->enabled_;
}

View::~View(){
    // Phase 3: per-View PreCreatedVisualTreeData removed.
    // The window's visual tree is managed by AppWindow::Impl.
    std::cout << "View will destruct" << std::endl;
}

void View::setFrontendRecurse(Composition::Compositor *frontend){
    auto *previousFrontend = compositorProxy().getFrontendPtr();
    if(previousFrontend != nullptr && previousFrontend != frontend && impl_->ownLayerTree != nullptr){
        previousFrontend->unobserveLayerTree(impl_->ownLayerTree.get());
    }
    compositorProxy().setFrontendPtr(frontend);
    if(frontend != nullptr && impl_->ownLayerTree != nullptr){
        frontend->observeLayerTree(impl_->ownLayerTree.get(),compositorProxy().getSyncLaneId());
    }
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setFrontendRecurse(frontend);
        }
    }
}

void View::setSyncLaneRecurse(uint64_t syncLaneId){
    // Tier 1 (UIView-Render-Redesign-Plan §4): all Views in a window share
    // one sync lane. WidgetTreeHost owns the lane; this method propagates
    // it down the View subtree without re-allocation.
    compositorProxy().setSyncLaneId(syncLaneId);
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setSyncLaneRecurse(syncLaneId);
        }
    }
}

ViewDelegate::ViewDelegate(){};

void ViewDelegate::setForwardDelegate(ViewDelegate *delegate){
    forwardDelegate = delegate;
}

ViewDelegate::~ViewDelegate(){};

void ViewDelegate::onRecieveEvent(Native::NativeEventPtr event){
    using Native::NativeEvent;
    switch (event->type) {
//        case NativeEvent::HasLoaded : {
//            viewHasLoaded(event);
//            break;
//        }
        case NativeEvent::CursorEnter : {
            onMouseEnter(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onMouseEnter(event);
            }
            break;
        }
        case NativeEvent::CursorExit : {
            onMouseExit(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onMouseExit(event);
            }
            break;
        }
        case NativeEvent::LMouseDown : {
            onLeftMouseDown(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onLeftMouseDown(event);
            }
            break;
        }
        case NativeEvent::LMouseUp : {
            onLeftMouseUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onLeftMouseUp(event);
            }
            break;
        }
        case NativeEvent::RMouseDown : {
            onRightMouseDown(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onRightMouseDown(event);
            }
            break;
        }
        case NativeEvent::RMouseUp : {
            onRightMouseUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onRightMouseUp(event);
            }
            break;
        };
        case NativeEvent::KeyDown : {
            onKeyDown(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onKeyDown(event);
            }
            break;
        };
        case NativeEvent::KeyUp : {
            onKeyUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onKeyUp(event);
            }
            break;
        }
        case Native::NativeEvent::ViewResize : {
            auto params = (Native::ViewResize *)event->params;
            if(params != nullptr){
                auto sanitized = ViewInternal::sanitizeRect(params->rect,view->getRect());
                view->getRect() = sanitized;
            }
            break;
        }

        default:
            break;
    }
}

}
