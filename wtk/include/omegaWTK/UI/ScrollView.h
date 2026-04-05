#ifndef OMEGAWTK_UI_SCROLLVIEW_H
#define OMEGAWTK_UI_SCROLLVIEW_H

#include "View.h"

namespace OmegaWTK {

class ScrollViewDelegate;

/// @brief ScrollView
class OMEGAWTK_EXPORT ScrollView : public View {
    SharedHandle<View> child;
    Core::Rect * childViewRect;
    ScrollViewDelegate *delegate = nullptr;
    bool hasDelegate();
    bool hasVerticalScrollBar,hasHorizontalScrollBar;
    friend class Widget;
public:
    explicit ScrollView(const Core::Rect & rect,
                        SharedHandle<View> child,
                        bool hasVerticalScrollBar,
                        bool hasHorizontalScrollBar,
                        ViewPtr parent = nullptr);
    OMEGACOMMON_CLASS("OmegaWTK.ScrollView")
    void toggleVerticalScrollBar();
    void toggleHorizontalScrollBar();
    void setDelegate(ScrollViewDelegate *_delegate);
};

class OMEGAWTK_EXPORT ScrollViewDelegate : public Native::NativeEventProcessor {
    void onRecieveEvent(Native::NativeEventPtr event);
    friend class ScrollView;
protected:
    ScrollView *scrollView;

    INTERFACE_METHOD void onScrollLeft() DEFAULT;
    INTERFACE_METHOD void onScrollRight() DEFAULT;
    INTERFACE_METHOD void onScrollDown() DEFAULT;
    INTERFACE_METHOD void onScrollUp() DEFAULT;
};

}

#endif
