#include "WidgetImpl.h"

#include "omegaWTK/UI/CanvasView.h"
#include "WidgetTreeHost.h"

#include <algorithm>
#include <utility>

namespace OmegaWTK {

Widget::Widget(Core::Rect rect):
    impl_(std::make_unique<Impl>()),
    view(CanvasView::Create(rect)){
}

Widget::Widget(ViewPtr view):
    impl_(std::make_unique<Impl>()),
    view(std::move(view)){
}

OmegaCommon::ArrayRef<WidgetPtr> Widget::childWidgets(){
    static OmegaCommon::Vector<WidgetPtr> empty;
    return empty;
}

void Widget::show(){
    view->enable();
    WIDGET_NOTIFY_OBSERVERS_SHOW();
}

void Widget::hide(){
    view->disable();
    WIDGET_NOTIFY_OBSERVERS_HIDE();
}

void Widget::addObserver(WidgetObserverPtr observer){
    if(!observer->hasAssignment) {
        impl_->observers.push_back(observer);
        observer->hasAssignment = true;
    }
}

void Widget::setTreeHostRecurse(WidgetTreeHost *host){
    treeHost = host;
    if(host != nullptr){
        // View::setFrontendRecurse handles per-view LayerTree observation.
        view->setFrontendRecurse(host->compPtr());
        view->setSyncLaneRecurse(host->laneId());
    }
    else {
        view->setFrontendRecurse(nullptr);
        view->setSyncLaneRecurse(0);
    }
    for(const auto & c : childWidgets()){
        if(c != nullptr){
            c->setTreeHostRecurse(host);
        }
    }
}

void Widget::removeObserver(WidgetObserverPtr observerPtr){
    auto it = impl_->observers.begin();
    while(it != impl_->observers.end()){
        if(*it == observerPtr){
            impl_->observers.erase(it);
            observerPtr->hasAssignment = false;
            break;
        }
        ++it;
    }
}

void Widget::notifyObservers(Widget::WidgetEventType event_ty,Widget::WidgetEventParams params){
    for(auto & observer : impl_->observers){
        switch (event_ty) {
            case Show : {
                observer->onWidgetDidShow();
                break;
            }
            case Hide : {
                observer->onWidgetDidHide();
                break;
            }
            case Resize : {
                observer->onWidgetChangeSize(params.rect,view->getRect());
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
    }
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

View & Widget::viewRef(){
    return *view;
}

Widget::~Widget(){
    parent = nullptr;
}

WidgetObserver::WidgetObserver():hasAssignment(false),widget(nullptr){
}

}
