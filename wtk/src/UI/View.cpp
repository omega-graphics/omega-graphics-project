#include "omegaWTK/UI/View.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"

namespace OmegaWTK {

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

    View::View(const Core::Rect & rect,Composition::LayerTree *layerTree,ViewPtr parent):
        renderTarget(std::make_shared<Composition::ViewRenderTarget>(
                Native::make_native_item(
                        sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})))),
        proxy(std::static_pointer_cast<Composition::CompositionRenderTarget>(renderTarget)),
        widgetLayerTree(layerTree),
        parent_ptr(parent.get()),
        rect(sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})){

        resizeCoordinator.attachView(this);

        layerTreeLimb = widgetLayerTree->createLimb(this->rect);
        renderTarget->getNativePtr()->setLayerTreeLimb(layerTreeLimb.get());
        renderTarget->getNativePtr()->event_emitter = this;
        
        if(parent_ptr) {
            parent->addSubView(this);
            layerTree->addChildLimb(layerTreeLimb,parent->layerTreeLimb.get());
        }
        else
            layerTree->setRootLimb(layerTreeLimb);
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
    if(layerTreeLimb != nullptr){
        // Preserve positioned layer rect so child visuals keep stack/layout offsets.
        layerTreeLimb->getRootLayer()->resize(rect);
    }
};

View::View(const Core::Rect & rect,Native::NativeItemPtr nativeItem,Composition::LayerTree *layerTree,ViewPtr parent):
rect(sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})),
widgetLayerTree(layerTree),
renderTarget(std::make_shared<Composition::ViewRenderTarget>(nativeItem)),
proxy(std::static_pointer_cast<Composition::CompositionRenderTarget>(renderTarget)),
parent_ptr(parent.get()){
    resizeCoordinator.attachView(this);
    if(renderTarget != nullptr && renderTarget->getNativePtr() != nullptr){
        renderTarget->getNativePtr()->resize(this->rect);
    }
    if(parent_ptr) {
        parent->addSubView(this);
    };
};

SharedHandle<Composition::Layer> View::makeLayer(Core::Rect rect){
    auto layer = std::make_shared<Composition::Layer>(rect);
    layer->parentLimb = layerTreeLimb.get();
    layerTreeLimb->addLayer(layer);
    return layer;
};

SharedHandle<Composition::Canvas> View::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
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

View::~View(){
    std::cout << "View will destruct" << std::endl;
};

void View::setFrontendRecurse(Composition::Compositor *frontend){
    proxy.setFrontendPtr(frontend);
    for(auto *subView : subviews){
        if(subView != nullptr){
            subView->setFrontendRecurse(frontend);
        }
    };
};

void View::setSyncLaneRecurse(uint64_t syncLaneId){
    proxy.setSyncLaneId(syncLaneId);
    for(auto *subView : subviews){
        if(subView != nullptr){
            subView->setSyncLaneRecurse(syncLaneId);
        }
    }
}

void View::setResizeGovernorMetadataRecurse(const Composition::ResizeGovernorMetadata & metadata,
                                            std::uint64_t coordinatorGeneration){
    proxy.setResizeGovernorMetadata(metadata,coordinatorGeneration);
    for(auto *subView : subviews){
        if(subView != nullptr){
            subView->setResizeGovernorMetadataRecurse(metadata,coordinatorGeneration);
        }
    }
}
    
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

ScrollView::ScrollView(const Core::Rect & rect, SharedHandle<View> child, bool hasVerticalScrollBar, bool hasHorizontalScrollBar, Composition::LayerTree *layerTree, ViewPtr parent):
        View(rect,Native::make_native_item(rect,Native::ScrollItem),layerTree,parent),
        child(child),
        childViewRect(child ? &child->getRect() : nullptr),
        delegate(nullptr),
        hasHorizontalScrollBar(hasHorizontalScrollBar), hasVerticalScrollBar(hasVerticalScrollBar){
    renderTarget->getNativePtr()->event_emitter = this;
    Native::NativeItemPtr ptr = renderTarget->getNativePtr();
    if(child != nullptr){
        ptr->setClippedView(child->renderTarget->getNativePtr());
    }
    if(hasHorizontalScrollBar)
        ptr->toggleHorizontalScrollBar(hasHorizontalScrollBar);
    if(hasVerticalScrollBar)
        ptr->toggleVerticalScrollBar(hasVerticalScrollBar);
};

bool ScrollView::hasDelegate(){
    return delegate != nullptr;
};

void ScrollView::toggleVerticalScrollBar(){
    hasVerticalScrollBar = !hasVerticalScrollBar;
    renderTarget->getNativePtr()->toggleVerticalScrollBar(hasVerticalScrollBar);
}

void ScrollView::toggleHorizontalScrollBar(){
    hasHorizontalScrollBar = !hasHorizontalScrollBar;
    renderTarget->getNativePtr()->toggleHorizontalScrollBar(hasHorizontalScrollBar);
}

void ScrollView::setDelegate(ScrollViewDelegate *_delegate){
    if(_delegate == nullptr){
        delegate = nullptr;
        setReciever(nullptr);
        return;
    }
    delegate = _delegate;
    delegate->scrollView = this;
    setReciever(delegate);
};

void ScrollViewDelegate::onRecieveEvent(Native::NativeEventPtr event){
    if(event == nullptr){
        return;
    }
    switch(event->type){
        case Native::NativeEvent::ScrollLeft:
            onScrollLeft();
            break;
        case Native::NativeEvent::ScrollRight:
            onScrollRight();
            break;
        case Native::NativeEvent::ScrollUp:
            onScrollUp();
            break;
        case Native::NativeEvent::ScrollDown:
            onScrollDown();
            break;
        default:
            break;
    }
};

// SharedHandle<ClickableViewHandler> ClickableViewHandler::Create() {
//     return (SharedHandle<ClickableViewHandler>)new ClickableViewHandler();
// }

// void ClickableViewHandler::onClick(std::function<void()> handler) {
//     click_handler = std::move(handler);
// }

// void ClickableViewHandler::onHoverBegin(std::function<void()> handler) {
//     hover_begin_handler = handler;
// }

// void ClickableViewHandler::onHoverEnd(std::function<void()> handler) {
//     hover_end_handler = handler;
// }

// void ClickableViewHandler::onPress(std::function<void()> handler) {
//     press_handler = handler;
// }

// void ClickableViewHandler::onRelease(std::function<void()> handler) {
//     release_handler = handler;
// }

// void ClickableViewHandler::onMouseEnter(Native::NativeEventPtr event) {
//     hover_begin_handler();
// }

// void ClickableViewHandler::onMouseExit(Native::NativeEventPtr event) {
//     hover_end_handler();
// }

// void ClickableViewHandler::onLeftMouseDown(Native::NativeEventPtr event) {
//     press_handler();
//     click_handler();
// }

// void ClickableViewHandler::onLeftMouseUp(Native::NativeEventPtr event) {
//     release_handler();
// }






// SharedHandle<Composition::Brush> cursorBrush = Composition::ColorBrush({Composition::Color::Black8});

// TextView::TextView(const Core::Rect &rect,Composition::LayerTree * layerTree,View *parent, bool io) :
// View(rect,layerTree,parent),
// textRect(
//         Composition::TextRect::Create(
//         rect,
//         Composition::TextLayoutDescriptor {
//             Composition::TextLayoutDescriptor::LeftUpper,
//             Composition::TextLayoutDescriptor::WrapByWord
//         })),
//         rootLayerCanvas(makeCanvas(getLayerTreeLimb()->getRootLayer())),
//         str(),
//         editMode(io),cursorLayer(makeLayer({{0,0},100,100})),
//         cursorCanvas(makeCanvas(cursorLayer)){

        
// }

// void TextView::moveTextCursorToMousePoint(Core::Position & pos){
    
// }

// void TextView::enableCursor(){
//     if(font){
//         cursorLayer->setEnabled(true);
//         auto &r = cursorLayer->getLayerRect();
//         cursorCanvas->drawRect(r,cursorBrush);
//         cursorCanvas->sendFrame();
//     }
// }

// void TextView::disableCursor(){
//     if(font){
//         cursorLayer->setEnabled(false);
//     }
// }

// void TextView::pushChar(Unicode32Char &ch) {
//     str.append(ch);
// }

// void TextView::popChar() {
//     str.truncate(str.length() - 1);
// }

// void TextView::commitChanges() {
//     auto run = Composition::GlyphRun::fromUStringAndFont(str,font);
//     textRect->drawRun(run,Composition::Color::create8Bit(Composition::Color::Eight::Black8));
//     auto img = textRect->toBitmap();
//     rootLayerCanvas->drawGETexture(img.s,getRect(),img.textureFence);
//     rootLayerCanvas->sendFrame();
// }

// void TextView::updateFont(SharedHandle<Composition::Font> &font) {
//     this->font = font;
// }

// void TextView::setContent(const UChar *str){
//     this->str.setTo(str,u_strlen(str));
//     commitChanges();
// }

// TextViewDelegate::TextViewDelegate(TextView *view) : ViewDelegate(){
//     view->setDelegate(this);
//     clickHandler = std::make_unique<ClickableViewHandler>();
//     setForwardDelegate(clickHandler.get());
//      clickHandler->onClick([&](){
//          auto _v = (TextView *)this->view;
//         Core::Position pos {};
//         _v->startCompositionSession();
//         _v->moveTextCursorToMousePoint(pos);
//         _v->enableCursor();
//         _v->endCompositionSession();
//     });
// }


// void TextViewDelegate::onKeyDown(Native::NativeEventPtr event){
//     auto _v = (TextView *)this->view;
//     UChar32 ch;
//     _v->startCompositionSession();
//     _v->pushChar(ch);
//     _v->commitChanges();
//     _v->endCompositionSession();
// }

// void TextViewDelegate::onKeyUp(Native::NativeEventPtr event){
    
// }





};
