#include "omegaWTK/UI/ScrollView.h"

namespace OmegaWTK {

    ScrollView::ScrollView(const Core::Rect & rect, SharedHandle<View> child, bool hasVerticalScrollBar, bool hasHorizontalScrollBar, ViewPtr parent):
            View(rect,Native::make_native_item(rect,Native::ScrollItem),parent),
            child(child),
            childViewRect(child ? &child->getRect() : nullptr),
            delegate(nullptr),
            hasVerticalScrollBar(hasVerticalScrollBar){
                
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

};