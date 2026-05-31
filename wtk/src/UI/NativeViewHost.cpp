#include "omegaWTK/UI/NativeViewHost.h"
#include "omegaWTK/UI/View.h"
#include "WidgetTreeHost.h"

namespace OmegaWTK {

NativeViewHost::NativeViewHost(Composition::Rect rect)
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

void NativeViewHost::onLayoutResolved(const Composition::Rect & finalRectPx){
    Widget::onLayoutResolved(finalRectPx);
    syncBounds();
}

void NativeViewHost::syncBounds(){
    if(!attached_ || embeddedItem_ == nullptr || view == nullptr){
        return;
    }
    // Phase 4.7.5: `computeWindowOffset` is gone (it was a thin
    // wrapper over the FrameBuilder accumulator which is also gone).
    // syncBounds fires from `onLayoutResolved`, OUTSIDE the central
    // `FrameBuilder::buildFrame` walk, so it cannot read
    // PaintContext.offset — the parent-chain walk via
    // `View::offsetFromRoot()` is the right hammer here.
    auto offset = view->offsetFromRoot();
    auto & r = view->getRect();
    embeddedItem_->resize(Composition::Rect{offset, r.w, r.h});
}

}
