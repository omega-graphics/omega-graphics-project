#include "WidgetImpl.h"

#include "omegaWTK/UI/View.h"
#include "WidgetTreeHost.h"

#include <algorithm>
#include <utility>

namespace OmegaWTK {

Widget::Widget(Composition::Rect rect):
    impl_(std::make_unique<Impl>()),
    view(View::Create(rect)){
    // §2.3a T1: back-link the root view to this widget so the hover
    // dispatcher can resolve a hit View up to its owning Widget (for
    // tooltip lookup). Only the widget's own root view is tagged; nested
    // element / sub-views keep a null owner and the dispatcher walks the
    // parent chain to the nearest non-null.
    view->setOwnerWidget(this);
}

Widget::Widget(ViewPtr view):
    impl_(std::make_unique<Impl>()),
    view(std::move(view)){
    // §2.3a T1: see the rect ctor above. `this->view` because the
    // parameter shadows the member.
    this->view->setOwnerWidget(this);
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
    // §2.3a F2: thread the host pointer down this widget's View subtree
    // so `View::focus`/`blur` can reach `host->focusManager()`. `host`
    // may be null (detach), which clears the pointer and makes those
    // calls no-ops — matching the setFrontend/syncLane null handling.
    view->setTreeHostRecurse(host);
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

void Widget::setTooltip(const OmegaCommon::String & text){
    impl_->tooltip_ = text;
}

void Widget::clearTooltip(){
    impl_->tooltip_.clear();
    // §2.3a T1: if the tooltip is on screen for this widget right now,
    // take it down immediately rather than waiting for the next hover
    // change. `treeHost` is null when the widget is detached (nothing
    // could be showing), so this is a no-op then.
    if(treeHost != nullptr){
        treeHost->dismissTooltipFor(this);
    }
}

const OmegaCommon::String & Widget::tooltip() const{
    return impl_->tooltip_;
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

Composition::Rect Widget::clampChildRect(const Widget &child,const GeometryProposal &proposal) const{
    (void)child;
    return proposal.requested;
}

void Widget::onChildRectCommitted(const Widget & child,
                                  const Composition::Rect & oldRect,
                                  const Composition::Rect & newRect,
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
