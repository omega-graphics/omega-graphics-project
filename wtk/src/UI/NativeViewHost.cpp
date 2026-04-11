#include "omegaWTK/UI/NativeViewHost.h"
#include "omegaWTK/UI/View.h"
#include "WidgetTreeHost.h"

namespace OmegaWTK {

NativeViewHost::NativeViewHost(Core::Rect rect)
    : Widget(rect){}

NativeViewHost::~NativeViewHost(){
    if(attached_){
        detach();
    }
}

void NativeViewHost::attach(Native::NativeItemPtr nativeItem){
    if(attached_){
        detach();
    }
    embeddedItem_ = std::move(nativeItem);
    if(treeHost != nullptr && embeddedItem_ != nullptr){
        treeHost->embedNativeItem(embeddedItem_);
        attached_ = true;
        syncBounds();
    }
}

void NativeViewHost::detach(){
    if(attached_ && treeHost != nullptr && embeddedItem_ != nullptr){
        treeHost->unembedNativeItem(embeddedItem_);
    }
    attached_ = false;
    embeddedItem_.reset();
}

bool NativeViewHost::hasAttachedItem() const {
    return attached_;
}

void NativeViewHost::onMount(){
    // If attach() was called before this widget was added to the tree,
    // perform the deferred attachment now that treeHost is available.
    if(embeddedItem_ != nullptr && !attached_ && treeHost != nullptr){
        treeHost->embedNativeItem(embeddedItem_);
        attached_ = true;
        syncBounds();
    }
}

void NativeViewHost::onLayoutResolved(const Core::Rect & finalRectPx){
    Widget::onLayoutResolved(finalRectPx);
    syncBounds();
}

void NativeViewHost::syncBounds(){
    if(!attached_ || embeddedItem_ == nullptr || view == nullptr){
        return;
    }
    auto offset = view->computeWindowOffset();
    auto & r = view->getRect();
    embeddedItem_->resize(Core::Rect{offset, r.w, r.h});
}

}
