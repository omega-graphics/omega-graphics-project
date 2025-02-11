#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/WidgetTreeHost.h"



namespace OmegaWTK {


Widget::Widget(const Core::Rect & rect,WidgetPtr parent):parent(parent){
    layerTree = std::make_shared<Composition::LayerTree>();
    rootView = SharedHandle<CanvasView>(new CanvasView(rect,layerTree.get(),nullptr));
    // std::cout << "Constructing View for Widget" << std::endl;
    if(parent != nullptr) {
        parent->rootView->addSubView(this->rootView.get());
        parent->children.push_back(SharedHandle<Widget>(this));
    }
//    std::cout << "RenderTargetPtr:" << rootView->renderTarget.get() << std::endl;
};

//Widget::Widget(Widget & widget):parent(std::move(widget.parent)),compositor(std::move(widget.compositor)),rootView(std::move(widget.rootView)){
//    
//};

void Widget::onThemeSetRecurse(Native::ThemeDesc &desc){
    onThemeSet(desc);
    for(auto & child : children){
        child->onThemeSetRecurse(desc);
    }
}

SharedHandle<View> Widget::makeCanvasView(const Core::Rect & rect,ViewPtr parent){
    return SharedHandle<CanvasView>(new CanvasView(rect,layerTree.get(),parent));
};

// SharedHandle<TextView> Widget::makeTextView(const Core::Rect & rect,View *parent){
//     return SharedHandle<TextView>(new TextView(rect,layerTree.get(),parent,false));
// };

SharedHandle<SVGView> Widget::makeSVGView(const Core::Rect & rect,ViewPtr parent){
    return SharedHandle<SVGView>(new SVGView(rect,layerTree.get(),parent));
}

SharedHandle<VideoView> Widget::makeVideoView(const Core::Rect & rect,ViewPtr parent){
    return SharedHandle<VideoView>(new VideoView(rect,layerTree.get(),parent));
};

Core::Rect & Widget::rect(){
    return rootView->getRect();
};

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
    treeHost = host;
    rootView->setFrontendRecurse(host->compPtr());
    for(auto c : children){
        c->setTreeHostRecurse(host);
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

void Widget::removeChildWidget(WidgetPtr ptr){
    for(auto it = children.begin();it != children.end();it++){
        if(ptr == *it){
            rootView->removeSubView(ptr->rootView.get());
            children.erase(it);
            ptr->notifyObservers(Detach,{WidgetPtr(this)});
            ptr->layerTree->notifyObserversOfWidgetDetach();
            break;
        };
    };
};

void Widget::setParentWidget(WidgetPtr widget){
    assert(widget != nullptr && "Cannot set Widget as child of a null Widget");

    if(parent != nullptr){
        parent->removeChildWidget(std::shared_ptr<Widget>(this));
    }
    parent = widget;
    parent->children.push_back(std::shared_ptr<Widget>(this));
    setTreeHostRecurse(widget->treeHost);
    parent->rootView->addSubView(rootView.get());
    notifyObservers(Attach,{widget});
};

Widget::~Widget(){
    parent->removeChildWidget(std::shared_ptr<Widget>(this));
}


WidgetObserver::WidgetObserver():hasAssignment(false),widget(nullptr){

};

}
