#include "ViewImpl.h"

#include "omegaWTK/UI/Layout.h"

#include <atomic>
#include <iostream>
#include <utility>

#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "../Composition/Compositor.h"
#include "../Composition/backend/ResourceFactory.h"

namespace OmegaWTK {

namespace {

// Per-view sync lane allocator. Each View gets its own lane so that
// packets from independent Views don't block each other via lane admission.
static std::atomic<uint64_t> g_viewSyncLaneSeed {1000};

}

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

Core::Rect & View::getRect(){
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

void View::preCreateVisualResources(){
    if(renderTargetHandle() == nullptr){
        return;
    }
    Composition::BackendResourceFactory factory;
    Composition::ViewPresentTarget presentTarget {};
    auto bundle = factory.createVisualTreeForView(renderTargetHandle(), impl_->rect, presentTarget);
    impl_->preCreatedVisualTree_ = std::make_unique<Composition::PreCreatedVisualTreeData>(
            Composition::PreCreatedVisualTreeData{std::move(bundle), std::move(presentTarget)});
    Composition::PreCreatedResourceRegistry::store(
            renderTargetHandle().get(), impl_->preCreatedVisualTree_.get());
}

View::View(const Core::Rect & rect,ViewPtr parent):
    impl_(std::make_unique<Impl>(
            *this,
            std::make_shared<Composition::ViewRenderTarget>(
                Native::make_native_item(
                    ViewInternal::sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f}))),
            ViewInternal::sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f}),
            parent.get())){

    renderTargetHandle()->getNativePtr()->setLayerTreeLimb(impl_->ownLayerTree.get());
    renderTargetHandle()->getNativePtr()->event_emitter = this;

    preCreateVisualResources();

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
    renderTargetHandle()->getNativePtr()->addChildNativeItem(view->renderTargetHandle()->getNativePtr());
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
            renderTargetHandle()->getNativePtr()->removeChildNativeItem(view->renderTargetHandle()->getNativePtr());
            view->impl_->parent_ptr = nullptr;
            return;
        }
        ++it;
    }
}

void View::resize(Core::Rect newRect){
    auto sanitized = ViewInternal::sanitizeRect(newRect,impl_->rect);
    if(ViewInternal::sameRect(impl_->rect,sanitized)){
        return;
    }
    impl_->rect = sanitized;
    renderTargetHandle()->getNativePtr()->resize(impl_->rect);
    // Update the native present layer (CAMetalLayer / swap chain) geometry
    // immediately on the main thread so it is always in sync with the view.
    {
        Core::Rect localRect {Core::Position{0.f,0.f},sanitized.w,sanitized.h};
        renderTargetHandle()->getNativePtr()->resizeNativeLayer(localRect,0.f);
    }
    if(impl_->ownLayerTree != nullptr && impl_->ownLayerTree->getRootLayer() != nullptr){
        impl_->ownLayerTree->getRootLayer()->resize(impl_->rect);
    }
}

View::View(const Core::Rect & rect,Native::NativeItemPtr nativeItem,ViewPtr parent):
    impl_(std::make_unique<Impl>(
            *this,
            std::make_shared<Composition::ViewRenderTarget>(nativeItem),
            ViewInternal::sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f}),
            parent.get())){
    if(renderTargetHandle() != nullptr && renderTargetHandle()->getNativePtr() != nullptr){
        renderTargetHandle()->getNativePtr()->resize(impl_->rect);
    }
    preCreateVisualResources();
    if(impl_->parent_ptr != nullptr) {
        parent->addSubView(this);
    }
}

SharedHandle<Composition::Layer> View::makeLayer(Core::Rect rect){
    auto layer = std::make_shared<Composition::Layer>(rect);
    layer->parentTree = impl_->ownLayerTree.get();
    impl_->ownLayerTree->addLayer(layer);
    return layer;
}

SharedHandle<Composition::Canvas> View::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
    // Each View owns its own render target and visual tree.
    return std::shared_ptr<Composition::Canvas>(new Composition::Canvas(compositorProxy(),*targetLayer));
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
    compositorProxy().beginRecord();
}

void View::endCompositionSession(){
    compositorProxy().endRecord();
}

void View::enable() {
    renderTargetHandle()->getNativePtr()->enable();
}

void View::disable() {
    renderTargetHandle()->getNativePtr()->disable();
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

View::~View(){
    if(impl_->preCreatedVisualTree_ != nullptr && renderTargetHandle() != nullptr){
        Composition::PreCreatedResourceRegistry::remove(renderTargetHandle().get());
    }
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
    (void)syncLaneId;
    // Each View gets its own sync lane so that per-view LayerTree isolation
    // extends to the compositor's lane admission system. Packets from
    // independent Views no longer block each other's budget/inFlight counters.
    auto ownLaneId = g_viewSyncLaneSeed.fetch_add(1);
    compositorProxy().setSyncLaneId(ownLaneId);
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setSyncLaneRecurse(ownLaneId);
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
