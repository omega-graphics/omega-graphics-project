#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/Layout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
#include "../Composition/Compositor.h"
#include "../Composition/backend/ResourceFactory.h"

namespace OmegaWTK {

    namespace {
        // Per-view sync lane allocator. Each View gets its own lane so that
        // packets from independent Views don't block each other via lane admission.
        static std::atomic<uint64_t> g_viewSyncLaneSeed {1000};
    }

    namespace {
#if defined(TARGET_MACOS)
        constexpr float kMaxViewDimension = 8192.f;
#else
        constexpr float kMaxViewDimension = 16384.f;
#endif

        static inline bool finiteFloat(float value){
            return std::isfinite(value);
        }

        static inline bool suspiciousDimensionPair(float w,float h){
            if(!finiteFloat(w) || !finiteFloat(h) || w <= 0.f || h <= 0.f){
                return true;
            }
            const float maxDim = std::max(w,h);
            const float minDim = std::min(w,h);
            if(maxDim >= (kMaxViewDimension * 0.5f) && minDim <= 2.f){
                return true;
            }
            if(maxDim >= 1024.f && minDim > 0.f){
                const float aspect = maxDim / minDim;
                if(aspect > 256.f){
                    return true;
                }
            }
            return false;
        }

        static inline Core::Rect sanitizeRect(const Core::Rect & candidate,const Core::Rect & fallback){
            Core::Rect saneFallback = fallback;
            if(!finiteFloat(saneFallback.pos.x)){
                saneFallback.pos.x = 0.f;
            }
            if(!finiteFloat(saneFallback.pos.y)){
                saneFallback.pos.y = 0.f;
            }
            if(!finiteFloat(saneFallback.w) || saneFallback.w <= 0.f){
                saneFallback.w = 1.f;
            }
            if(!finiteFloat(saneFallback.h) || saneFallback.h <= 0.f){
                saneFallback.h = 1.f;
            }
            saneFallback.w = std::clamp(saneFallback.w,1.f,kMaxViewDimension);
            saneFallback.h = std::clamp(saneFallback.h,1.f,kMaxViewDimension);
            if(suspiciousDimensionPair(saneFallback.w,saneFallback.h)){
                saneFallback.w = 1.f;
                saneFallback.h = 1.f;
            }

            Core::Rect sanitized = candidate;
            if(!finiteFloat(sanitized.pos.x)){
                sanitized.pos.x = saneFallback.pos.x;
            }
            if(!finiteFloat(sanitized.pos.y)){
                sanitized.pos.y = saneFallback.pos.y;
            }
            if(!finiteFloat(sanitized.w) || sanitized.w <= 0.f){
                sanitized.w = saneFallback.w;
            }
            if(!finiteFloat(sanitized.h) || sanitized.h <= 0.f){
                sanitized.h = saneFallback.h;
            }
            sanitized.w = std::clamp(sanitized.w,1.f,kMaxViewDimension);
            sanitized.h = std::clamp(sanitized.h,1.f,kMaxViewDimension);
            if(suspiciousDimensionPair(sanitized.w,sanitized.h)){
                sanitized.w = saneFallback.w;
                sanitized.h = saneFallback.h;
            }
            return sanitized;
        }

        static inline bool sameRect(const Core::Rect & a,const Core::Rect & b){
            constexpr float kEpsilon = 0.001f;
            return std::fabs(a.pos.x - b.pos.x) <= kEpsilon &&
                   std::fabs(a.pos.y - b.pos.y) <= kEpsilon &&
                   std::fabs(a.w - b.w) <= kEpsilon &&
                   std::fabs(a.h - b.h) <= kEpsilon;
        }

        static inline float clampAxis(float value,float minValue,float maxValue){
            if(maxValue < minValue){
                maxValue = minValue;
            }
            return std::clamp(value,minValue,maxValue);
        }
    }

    void ViewResizeCoordinator::attachView(View * parent){
        parentView = parent;
    }

    void ViewResizeCoordinator::registerChild(View * child,const ChildResizeSpec & spec){
        if(child == nullptr){
            return;
        }
        auto & state = childState[child];
        state.spec = spec;
        state.baselineChildRect = child->getRect();
        if(parentView != nullptr){
            state.baselineParentRect = parentView->getRect();
        }
        state.hasBaseline = true;
    }

    void ViewResizeCoordinator::updateChildSpec(View * child,const ChildResizeSpec & spec){
        if(child == nullptr){
            return;
        }
        auto entry = childState.find(child);
        if(entry == childState.end()){
            registerChild(child,spec);
            return;
        }
        entry->second.spec = spec;
    }

    void ViewResizeCoordinator::unregisterChild(View * child){
        if(child == nullptr){
            return;
        }
        childState.erase(child);
    }

    void ViewResizeCoordinator::beginResizeSession(std::uint64_t sessionId){
        activeSessionId = sessionId;
        (void)activeSessionId;
        Core::Rect parentBaseline {Core::Position{0.f,0.f},1.f,1.f};
        if(parentView != nullptr){
            parentBaseline = parentView->getRect();
        }
        for(auto & entry : childState){
            auto * child = entry.first;
            if(child == nullptr){
                continue;
            }
            auto & state = entry.second;
            state.baselineParentRect = parentBaseline;
            state.baselineChildRect = child->getRect();
            state.hasBaseline = true;
        }
    }

    Core::Rect ViewResizeCoordinator::clampRectToParent(const Core::Rect & requested,
                                                        const Core::Rect & parentContentRect,
                                                        const ChildResizeSpec & spec){
        auto fallback = sanitizeRect(parentContentRect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
        auto parent = sanitizeRect(parentContentRect,fallback);
        auto output = sanitizeRect(requested,parent);

        const float minWidth = std::max(1.f,std::isfinite(spec.clamp.minWidth) ? spec.clamp.minWidth : 1.f);
        const float minHeight = std::max(1.f,std::isfinite(spec.clamp.minHeight) ? spec.clamp.minHeight : 1.f);
        const float maxWidth = std::isfinite(spec.clamp.maxWidth) ? spec.clamp.maxWidth : kMaxViewDimension;
        const float maxHeight = std::isfinite(spec.clamp.maxHeight) ? spec.clamp.maxHeight : kMaxViewDimension;

        if(spec.resizable){
            output.w = clampAxis(output.w,minWidth,std::max(minWidth,maxWidth));
            output.h = clampAxis(output.h,minHeight,std::max(minHeight,maxHeight));
        }
        else {
            output.w = std::max(1.f,output.w);
            output.h = std::max(1.f,output.h);
        }

        output.w = std::min(output.w,std::max(1.f,parent.w));
        output.h = std::min(output.h,std::max(1.f,parent.h));

        const float minX = parent.pos.x;
        const float minY = parent.pos.y;
        const float maxX = parent.pos.x + std::max(0.f,parent.w - output.w);
        const float maxY = parent.pos.y + std::max(0.f,parent.h - output.h);
        output.pos.x = clampAxis(output.pos.x,minX,maxX);
        output.pos.y = clampAxis(output.pos.y,minY,maxY);
        return output;
    }

    Core::Rect ViewResizeCoordinator::resolveChildRect(View * child,
                                                       const Core::Rect & requested,
                                                       const Core::Rect & parentContentRect){
        if(child == nullptr){
            return requested;
        }
        auto stateIt = childState.find(child);
        if(stateIt == childState.end()){
            registerChild(child,{});
            stateIt = childState.find(child);
            if(stateIt == childState.end()){
                return requested;
            }
        }

        auto & state = stateIt->second;
        auto output = sanitizeRect(requested,child->getRect());
        auto parent = sanitizeRect(parentContentRect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f});

        switch(state.spec.policy){
            case ChildResizePolicy::Fill: {
                if(state.spec.resizable){
                    const float gx = std::max(0.f,state.spec.growWeightX);
                    const float gy = std::max(0.f,state.spec.growWeightY);
                    output.w = parent.w * (gx > 0.f ? std::min(1.f,gx) : 1.f);
                    output.h = parent.h * (gy > 0.f ? std::min(1.f,gy) : 1.f);
                    output.pos.x = parent.pos.x;
                    output.pos.y = parent.pos.y;
                }
                break;
            }
            case ChildResizePolicy::Proportional: {
                if(state.spec.resizable && state.hasBaseline &&
                   state.baselineParentRect.w > 0.f &&
                   state.baselineParentRect.h > 0.f){
                    const float scaleX = parent.w / state.baselineParentRect.w;
                    const float scaleY = parent.h / state.baselineParentRect.h;
                    output.w = state.baselineChildRect.w * scaleX;
                    output.h = state.baselineChildRect.h * scaleY;
                    output.pos.x = parent.pos.x + (state.baselineChildRect.pos.x - state.baselineParentRect.pos.x) * scaleX;
                    output.pos.y = parent.pos.y + (state.baselineChildRect.pos.y - state.baselineParentRect.pos.y) * scaleY;
                }
                break;
            }
            case ChildResizePolicy::Fixed:
            case ChildResizePolicy::FitContent:
            default:
                break;
        }

        output = clampRectToParent(output,parent,state.spec);
        return output;
    }

    void ViewResizeCoordinator::resolve(const Core::Rect & parentContentRect){
        for(auto & entry : childState){
            auto * child = entry.first;
            if(child == nullptr){
                continue;
            }
            auto resolved = resolveChildRect(child,child->getRect(),parentContentRect);
            if(!sameRect(resolved,child->getRect())){
                child->resize(resolved);
            }
        }
    }

    void View::preCreateVisualResources(){
        if(renderTarget == nullptr){
            return;
        }
        Composition::BackendResourceFactory factory;
        Composition::ViewPresentTarget presentTarget {};
        auto bundle = factory.createVisualTreeForView(renderTarget, rect, presentTarget);
        preCreatedVisualTree_ = std::make_unique<Composition::PreCreatedVisualTreeData>(
                Composition::PreCreatedVisualTreeData{std::move(bundle), std::move(presentTarget)});
        Composition::PreCreatedResourceRegistry::store(
                renderTarget.get(), preCreatedVisualTree_.get());
    }

    View::View(const Core::Rect & rect,ViewPtr parent):
        renderTarget(std::make_shared<Composition::ViewRenderTarget>(
                Native::make_native_item(
                        sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})))),
        proxy(std::static_pointer_cast<Composition::CompositionRenderTarget>(renderTarget)),
        ownLayerTree(std::make_shared<Composition::LayerTree>(
                sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f}))),
        parent_ptr(parent.get()),
        rect(sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})){

        resizeCoordinator.attachView(this);

        renderTarget->getNativePtr()->setLayerTreeLimb(ownLayerTree.get());
        renderTarget->getNativePtr()->event_emitter = this;

        preCreateVisualResources();

        if(parent_ptr) {
            parent->addSubView(this);
        }
    };
//    View::View(const Core::Rect & rect,View *parent):
//        renderTarget(std::make_shared<Composition::ViewRenderTarget>(Native::make_native_item(rect))),
//        proxy(std::static_pointer_cast<Composition::CompositionRenderTarget>(renderTarget)),
//        widgetLayerTree(nullptr),
//        parent_ptr(parent),
//        rect(rect){
//
//        renderTarget->getNativePtr()->event_emitter = this;
//        if(parent_ptr) {
//            parent->addSubView(this);
//        };
//    };
    bool View::hasDelegate(){
        return delegate != nullptr;
    };
    void View::setDelegate(ViewDelegate *_delegate){
        delegate = _delegate;
        delegate->view = this;
        setReciever(delegate);
    };
void View::addSubView(View * view){
    if(view == nullptr){
        return;
    }
    for(auto *existing : subviews){
        if(existing == view){
            return;
        }
    }
    subviews.emplace_back(view);
    view->parent_ptr = this;
    resizeCoordinator.registerChild(view,{});
    renderTarget->getNativePtr()->addChildNativeItem(view->renderTarget->getNativePtr());
    // Newly created subviews must inherit compositor wiring immediately.
    view->setFrontendRecurse(proxy.getFrontendPtr());
    view->setSyncLaneRecurse(proxy.getSyncLaneId());
    };
void View::removeSubView(View *view){
    auto it  = subviews.begin();
    while(it != subviews.end()){
        auto *v = *it;
        if(v == view){
            subviews.erase(it);
            resizeCoordinator.unregisterChild(view);
            renderTarget->getNativePtr()->removeChildNativeItem(view->renderTarget->getNativePtr());
            view->parent_ptr = nullptr;
            return;
        }
        ++it;
        };
    };

void View::resize(Core::Rect newRect){
    auto sanitized = sanitizeRect(newRect,rect);
    if(sameRect(rect,sanitized)){
        return;
    }
    rect = sanitized;
    renderTarget->getNativePtr()->resize(rect);
    // Update the native present layer (CAMetalLayer / swap chain) geometry
    // immediately on the main thread so it is always in sync with the view.
    {
        Core::Rect localRect {Core::Position{0.f,0.f},sanitized.w,sanitized.h};
        renderTarget->getNativePtr()->resizeNativeLayer(localRect,0.f);
    }
    if(ownLayerTree != nullptr && ownLayerTree->getRootLayer() != nullptr){
        ownLayerTree->getRootLayer()->resize(rect);
    }
};

View::View(const Core::Rect & rect,Native::NativeItemPtr nativeItem,ViewPtr parent):
renderTarget(std::make_shared<Composition::ViewRenderTarget>(nativeItem)),
proxy(std::static_pointer_cast<Composition::CompositionRenderTarget>(renderTarget)),
ownLayerTree(std::make_shared<Composition::LayerTree>(
        sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f}))),
parent_ptr(parent.get()),
rect(sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})){
    resizeCoordinator.attachView(this);
    if(renderTarget != nullptr && renderTarget->getNativePtr() != nullptr){
        renderTarget->getNativePtr()->resize(this->rect);
    }
    preCreateVisualResources();
    if(parent_ptr) {
        parent->addSubView(this);
    };
};

SharedHandle<Composition::Layer> View::makeLayer(Core::Rect rect){
    auto layer = std::make_shared<Composition::Layer>(rect);
    layer->parentTree = ownLayerTree.get();
    ownLayerTree->addLayer(layer);
    return layer;
};

SharedHandle<Composition::Canvas> View::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
    // Each View owns its own render target and visual tree.
    return std::shared_ptr<Composition::Canvas>(new Composition::Canvas(proxy,*targetLayer));
}

void View::startCompositionSession(){
    if(proxy.getFrontendPtr() == nullptr && parent_ptr != nullptr){
        auto parentFrontend = parent_ptr->proxy.getFrontendPtr();
        if(parentFrontend != nullptr){
            proxy.setFrontendPtr(parentFrontend);
            auto parentLane = parent_ptr->proxy.getSyncLaneId();
            if(parentLane != 0){
                proxy.setSyncLaneId(parentLane);
            }
        }
    }
    proxy.beginRecord();
}

void View::endCompositionSession(){
    proxy.endRecord();
}

void View::enable() {
    renderTarget->getNativePtr()->enable();
}

void View::disable() {
    renderTarget->getNativePtr()->disable();
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
    if(preCreatedVisualTree_ != nullptr && renderTarget != nullptr){
        Composition::PreCreatedResourceRegistry::remove(renderTarget.get());
    }
    std::cout << "View will destruct" << std::endl;
};

void View::setFrontendRecurse(Composition::Compositor *frontend){
    auto *previousFrontend = proxy.getFrontendPtr();
    if(previousFrontend != nullptr && previousFrontend != frontend && ownLayerTree != nullptr){
        previousFrontend->unobserveLayerTree(ownLayerTree.get());
    }
    proxy.setFrontendPtr(frontend);
    if(frontend != nullptr && ownLayerTree != nullptr){
        frontend->observeLayerTree(ownLayerTree.get(),proxy.getSyncLaneId());
    }
    for(auto *subView : subviews){
        if(subView != nullptr){
            subView->setFrontendRecurse(frontend);
        }
    };
};

void View::setSyncLaneRecurse(uint64_t syncLaneId){
    // Each View gets its own sync lane so that per-view LayerTree isolation
    // extends to the compositor's lane admission system. Packets from
    // independent Views no longer block each other's budget/inFlight counters.
    auto ownLaneId = g_viewSyncLaneSeed.fetch_add(1);
    proxy.setSyncLaneId(ownLaneId);
    for(auto *subView : subviews){
        if(subView != nullptr){
            subView->setSyncLaneRecurse(ownLaneId);
        }
    }
}

// setResizeGovernorMetadataRecurse removed (Phase 3).

// Composition::Compositor * View::getWidgetCompositor(){
//     return widgetLayerTree->widgetCompositor;
// };


ViewDelegate::ViewDelegate(){};

void ViewDelegate::setForwardDelegate(ViewDelegate *delegate){
    forwardDelegate = delegate;
};

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
        };
        case NativeEvent::RMouseUp : {
            onRightMouseUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onRightMouseUp(event);
            }
            break;
        }
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
                auto sanitized = sanitizeRect(params->rect,view->getRect());
                view->getRect() = sanitized;
            }
            break;
        }
        
        default:
            break;
    }
};




};
