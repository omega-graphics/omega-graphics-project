#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "../Composition/Compositor.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/UI/WidgetTreeHost.h"

#include <algorithm>


namespace OmegaWTK {

PaintContext::PaintContext(Widget *widget,SharedHandle<Composition::Canvas> mainCanvas,PaintReason reason):
widget(widget),
mainCanvas(mainCanvas),
paintReason(reason),
paintBounds(widget != nullptr ?
            Core::Rect{
                    Core::Position{0.f,0.f},
                    widget->rect().w,
                    widget->rect().h
            } :
            Core::Rect{{0.f,0.f},0.f,0.f}){

}

const Core::Rect & PaintContext::bounds() const{
    return paintBounds;
}

PaintReason PaintContext::reason() const{
    return paintReason;
}

Composition::Canvas & PaintContext::rootCanvas(){
    return *mainCanvas.get();
}

SharedHandle<Composition::Canvas> PaintContext::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
    return widget->rootView->makeCanvas(targetLayer);
}

void PaintContext::clear(const Composition::Color &color){
    auto & background = rootCanvas().getCurrentFrame()->background;
    background.r = color.r;
    background.g = color.g;
    background.b = color.b;
    background.a = color.a;
}

void PaintContext::drawRect(const Core::Rect &rect,const SharedHandle<Composition::Brush> &brush){
    auto _rect = rect;
    auto _brush = brush;
    rootCanvas().drawRect(_rect,_brush);
}

void PaintContext::drawRoundedRect(const Core::RoundedRect &rect,const SharedHandle<Composition::Brush> &brush){
    auto _rect = rect;
    auto _brush = brush;
    rootCanvas().drawRoundedRect(_rect,_brush);
}

void PaintContext::drawImage(const SharedHandle<Media::BitmapImage> &img,const Core::Rect &rect){
    auto _rect = rect;
    auto _img = img;
    rootCanvas().drawImage(_img,_rect);
}

void PaintContext::drawText(const UniString &text,
                            const SharedHandle<Composition::Font> &font,
                            const Core::Rect &rect,
                            const Composition::Color &color,
                            const Composition::TextLayoutDescriptor &layoutDesc){
    auto _rect = rect;
    auto _font = font;
    rootCanvas().drawText(text,_font,_rect,color,layoutDesc);
}

void PaintContext::drawText(const UniString &text,
                            const SharedHandle<Composition::Font> &font,
                            const Core::Rect &rect,
                            const Composition::Color &color){
    auto _rect = rect;
    auto _font = font;
    rootCanvas().drawText(text,_font,_rect,color);
}


Widget::Widget(const Core::Rect & rect,WidgetPtr parent):parent(parent.get()){
    layerTree = std::make_shared<Composition::LayerTree>();
    rootView = SharedHandle<CanvasView>(new CanvasView(rect,layerTree.get(),nullptr));
    // std::cout << "Constructing View for Widget" << std::endl;
    if(parent != nullptr) {
        parent->rootView->addSubView(this->rootView.get());
        parent->children.push_back(this);
    }
//    std::cout << "RenderTargetPtr:" << rootView->renderTarget.get() << std::endl;
};

//Widget::Widget(Widget & widget):parent(std::move(widget.parent)),compositor(std::move(widget.compositor)),rootView(std::move(widget.rootView)){
//    
//};

SharedHandle<Composition::Canvas> Widget::getRootPaintCanvas(){
    if(rootPaintCanvas == nullptr){
        auto rootLayer = rootView->getLayerTreeLimb()->getRootLayer();
        rootPaintCanvas = rootView->makeCanvas(rootLayer);
    }
    return rootPaintCanvas;
}

void Widget::executePaint(PaintReason reason,bool immediate){
    if(mode != PaintMode::Automatic){
        return;
    }
    if(paintInProgress){
        if(options.coalesceInvalidates){
            hasPendingInvalidate = true;
            pendingPaintReason = reason;
            return;
        }
        if(!immediate){
            return;
        }
    }
    paintInProgress = true;
    if(treeHost != nullptr){
        auto desiredFrontend = treeHost->compPtr();
        auto desiredLane = treeHost->laneId();
        if(rootView->proxy.getFrontendPtr() != desiredFrontend ||
           rootView->proxy.getSyncLaneId() != desiredLane){
            rootView->setFrontendRecurse(desiredFrontend);
            rootView->setSyncLaneRecurse(desiredLane);
        }
    }
    else if(parent != nullptr && parent->rootView != nullptr){
        auto inheritedFrontend = parent->rootView->proxy.getFrontendPtr();
        auto inheritedLane = parent->rootView->proxy.getSyncLaneId();
        if(inheritedFrontend != nullptr &&
           (rootView->proxy.getFrontendPtr() != inheritedFrontend ||
            rootView->proxy.getSyncLaneId() != inheritedLane)){
            rootView->setFrontendRecurse(inheritedFrontend);
            rootView->setSyncLaneRecurse(inheritedLane);
        }
    }
    PaintReason activeReason = reason;
    while(true){
        auto canvas = getRootPaintCanvas();
        PaintContext context(this,canvas,activeReason);
        rootView->startCompositionSession();
        onPaint(context,activeReason);
        int submissions = 1;
        if(activeReason == PaintReason::Initial &&
           !initialDrawComplete &&
           options.autoWarmupOnInitialPaint){
            submissions = std::max<int>(1,options.warmupFrameCount);
        }
        for(int i = 0; i < submissions; i++){
            canvas->sendFrame();
        }
        rootView->endCompositionSession();
        if(activeReason == PaintReason::Initial){
            initialDrawComplete = true;
        }
        if(!(options.coalesceInvalidates && hasPendingInvalidate)){
            break;
        }
        activeReason = pendingPaintReason;
        hasPendingInvalidate = false;
    }
    paintInProgress = false;
}

void Widget::init(){
    if(hasMounted){
        return;
    }
    onMount();
    hasMounted = true;
    rootView->enable();
    if(mode == PaintMode::Automatic){
        executePaint(PaintReason::Initial,true);
    }
}

void Widget::setPaintMode(PaintMode mode){
    this->mode = mode;
}

PaintMode Widget::paintMode() const{
    return mode;
}

void Widget::setPaintOptions(const PaintOptions &options){
    this->options = options;
}

const PaintOptions & Widget::paintOptions() const{
    return options;
}

void Widget::invalidate(PaintReason reason){
    executePaint(reason,false);
}

void Widget::invalidateNow(PaintReason reason){
    executePaint(reason,true);
}

void Widget::handleHostResize(const Core::Rect &rect){
    auto oldRect = this->rect();
    rootView->resize(rect);
    auto & rootRect = rootView->getRect();
    auto newRect = rootRect;
    this->resize(newRect);
    WIDGET_NOTIFY_OBSERVERS_RESIZE(oldRect);
    if(mode == PaintMode::Automatic &&
       options.invalidateOnResize &&
       treeHost != nullptr &&
       hasMounted){
        invalidate(PaintReason::Resize);
    }
}

void Widget::onThemeSetRecurse(Native::ThemeDesc &desc){
    onThemeSet(desc);
    if(mode == PaintMode::Automatic){
        invalidate(PaintReason::ThemeChanged);
    }
    for(auto & child : children){
        if(child != nullptr){
            child->onThemeSetRecurse(desc);
        }
    }
}

SharedHandle<View> Widget::makeCanvasView(const Core::Rect & rect,ViewPtr parent){
    return SharedHandle<CanvasView>(new CanvasView(rect,layerTree.get(),parent));
};

SharedHandle<ScrollView> Widget::makeScrollView(const Core::Rect & rect,
                                                ViewPtr child,
                                                bool hasVerticalScrollBar,
                                                bool hasHorizontalScrollBar,
                                                ViewPtr parent){
    assert(child != nullptr && "Cannot create ScrollView with null child View");
    return SharedHandle<ScrollView>(new ScrollView(rect,
                                                   child,
                                                   hasVerticalScrollBar,
                                                   hasHorizontalScrollBar,
                                                   layerTree.get(),
                                                   parent));
}

// SharedHandle<TextView> Widget::makeTextView(const Core::Rect & rect,View *parent){
//     return SharedHandle<TextView>(new TextView(rect,layerTree.get(),parent,false));
// };

SharedHandle<SVGView> Widget::makeSVGView(const Core::Rect & rect,ViewPtr parent){
    return SharedHandle<SVGView>(new SVGView(rect,layerTree.get(),parent));
}

SharedHandle<VideoView> Widget::makeVideoView(const Core::Rect & rect,ViewPtr parent){
    return SharedHandle<VideoView>(new VideoView(rect,layerTree.get(),parent));
};

SharedHandle<UIView> Widget::makeUIView(const Core::Rect & rect,ViewPtr parent,UIViewTag tag){
    return SharedHandle<UIView>(new UIView(rect,layerTree.get(),parent,tag));
}

Core::Rect & Widget::rect(){
    return rootView->getRect();
};

bool Widget::requestRect(const Core::Rect &requested,GeometryChangeReason reason){
    auto oldRect = rect();
    if(parent == nullptr){
        setRect(requested);
        return true;
    }

    GeometryProposal proposal {};
    proposal.requested = requested;
    proposal.reason = reason;
    auto clamped = parent->clampChildRect(*this,proposal);
    setRect(clamped);
    parent->onChildRectCommitted(*this,oldRect,rect(),reason);
    return true;
}

void Widget::setRect(const Core::Rect &newRect){
    auto oldRect = rect();
    rootView->resize(newRect);
    auto & rootRect = rootView->getRect();
    auto updatedRect = rootRect;
    this->resize(updatedRect);
    WIDGET_NOTIFY_OBSERVERS_RESIZE(oldRect);
    if(mode == PaintMode::Automatic &&
       options.invalidateOnResize &&
       treeHost != nullptr &&
       hasMounted){
        invalidate(PaintReason::Resize);
    }
}

bool Widget::acceptsChildWidget(const Widget *child) const{
    return child != nullptr && child != this;
}

void Widget::show(){

    rootView->enable();
    WIDGET_NOTIFY_OBSERVERS_SHOW();

};
void Widget::hide(){
    rootView->disable();
    WIDGET_NOTIFY_OBSERVERS_HIDE();
};

void Widget::addObserver(WidgetObserverPtr observer){
    if(!observer->hasAssignment) {
        observers.push_back(observer);
        observer->hasAssignment = true;
    };
};

void Widget::setTreeHostRecurse(WidgetTreeHost *host){
    auto *previousHost = treeHost;
    if(previousHost != nullptr && previousHost != host && layerTree != nullptr){
        auto *previousComp = previousHost->compPtr();
        if(previousComp != nullptr){
            previousComp->unobserveLayerTree(layerTree.get());
        }
    }

    treeHost = host;
    if(host != nullptr){
        auto *comp = host->compPtr();
        if(comp != nullptr && layerTree != nullptr){
            comp->observeLayerTree(layerTree.get(),host->laneId());
        }
        rootView->setFrontendRecurse(host->compPtr());
        rootView->setSyncLaneRecurse(host->laneId());
    }
    else {
        rootView->setFrontendRecurse(nullptr);
        rootView->setSyncLaneRecurse(0);
    }
    for(auto c : children){
        if(c != nullptr){
            c->setTreeHostRecurse(host);
        }
    };
};

void Widget::removeObserver(WidgetObserverPtr observerPtr){
    auto it = observers.begin();
    while(it != observers.end()){
        if(*it == observerPtr){
            observers.erase(it);
            observerPtr->hasAssignment = false;
            break;
        };
        ++it;
    };
};

void Widget::notifyObservers(Widget::WidgetEventType event_ty,Widget::WidgetEventParams params){
    for(auto & observer : observers){
        switch (event_ty) {
            case Show : {
                observer->onWidgetDidShow();
                break;
            };
            case Hide : {
                observer->onWidgetDidHide();
                break;
            };
            case Resize : {
                observer->onWidgetChangeSize(params.rect,rootView->rect);
                break;
            }
            case Attach : {
                observer->onWidgetAttach(params.widget);
                break;
            }
            case Detach : {
                observer->onWidgetDetach(params.widget);
                break;
            }
        }
    };
};

void Widget::onChildAttached(Widget *child){
    (void)child;
}

void Widget::onChildDetached(Widget *child){
    (void)child;
}

Core::Rect Widget::clampChildRect(const Widget &child,const GeometryProposal &proposal) const{
    (void)child;
    return proposal.requested;
}

void Widget::onChildRectCommitted(const Widget & child,
                                  const Core::Rect & oldRect,
                                  const Core::Rect & newRect,
                                  GeometryChangeReason reason){
    (void)child;
    (void)oldRect;
    (void)newRect;
    (void)reason;
}

void Widget::removeChildWidget(Widget *ptr){
    if(ptr == nullptr){
        return;
    }
    for(auto it = children.begin();it != children.end();it++){
        if(ptr == *it){
            rootView->removeSubView(ptr->rootView.get());
            onChildDetached(ptr);
            children.erase(it);
            ptr->parent = nullptr;
            ptr->setTreeHostRecurse(nullptr);
            ptr->notifyObservers(Detach,{});
            ptr->layerTree->notifyObserversOfWidgetDetach();
            break;
        };
    };
};

void Widget::setParentWidgetImpl(Widget *widget,WidgetPtr widgetHandle){
    assert(widget != nullptr && "Cannot set Widget as child of a null Widget");
    if(parent == widget){
        return;
    }
    if(!widget->acceptsChildWidget(this)){
        return;
    }
    if(parent != nullptr){
        parent->removeChildWidget(this);
    }
    parent = widget;
    parent->children.push_back(this);
    parent->onChildAttached(this);
    setTreeHostRecurse(widget->treeHost);
    parent->rootView->addSubView(rootView.get());
    notifyObservers(Attach,{widgetHandle});
}

void Widget::setParentWidget(WidgetPtr widget){
    assert(widget != nullptr && "Cannot set Widget as child of a null Widget");
    setParentWidgetImpl(widget.get(),widget);
};

void Widget::setParentWidget(Widget *widget){
    setParentWidgetImpl(widget,{});
};

void Widget::detachFromParent(){
    if(parent != nullptr){
        parent->removeChildWidget(this);
    }
}

Widget::~Widget(){
    for(auto child : children){
        if(child != nullptr){
            child->parent = nullptr;
        }
    }
    children.clear();
    if(parent != nullptr){
        parent->removeChildWidget(this);
        parent = nullptr;
    }
}


WidgetObserver::WidgetObserver():hasAssignment(false),widget(nullptr){

};

}
