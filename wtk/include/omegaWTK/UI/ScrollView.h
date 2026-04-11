#ifndef OMEGAWTK_UI_SCROLLVIEW_H
#define OMEGAWTK_UI_SCROLLVIEW_H

#include "View.h"

namespace OmegaWTK {

namespace Composition {
    class Layer;
    class Canvas;
}

class ScrollViewDelegate;

/// @brief A fully virtual scroll container (Phase 3g).
///
/// Owns a content child View and tracks a scroll offset. Content is
/// clipped to the ScrollView's visible bounds via the compositor's
/// scissor rect. Scroll bar indicators are composited Layers drawn
/// by Canvas commands.
class OMEGAWTK_EXPORT ScrollView : public View {
    SharedHandle<View> child;
    Composition::Point2D scrollOffset {0.f, 0.f};
    ScrollViewDelegate *delegate = nullptr;
    bool hasVerticalScrollBar,hasHorizontalScrollBar;
    bool hasDelegate() override;

    /// Scroll bar overlay layers and canvases.
    SharedHandle<Composition::Layer> vScrollBarLayer;
    SharedHandle<Composition::Layer> hScrollBarLayer;
    SharedHandle<Composition::Canvas> vScrollBarCanvas;
    SharedHandle<Composition::Canvas> hScrollBarCanvas;

    void paintScrollBars();

    /// Internal event processor for default scroll handling when no
    /// delegate is set.
    struct DefaultScrollHandler : public Native::NativeEventProcessor {
        ScrollView *owner = nullptr;
        void onRecieveEvent(Native::NativeEventPtr event) override;
    };
    DefaultScrollHandler defaultHandler;

    friend class Widget;
public:
    explicit ScrollView(const Composition::Rect & rect,
                        SharedHandle<View> child,
                        bool hasVerticalScrollBar,
                        bool hasHorizontalScrollBar,
                        ViewPtr parent = nullptr);
    OMEGACOMMON_CLASS("OmegaWTK.ScrollView")

    void setDelegate(ScrollViewDelegate *_delegate);

    /// Returns the current scroll offset.
    const Composition::Point2D & getScrollOffset() const { return scrollOffset; }

    /// Sets the scroll offset directly. Repaints scroll bar indicators.
    void setScrollOffset(const Composition::Point2D & offset);

    /// Returns the scroll offset contribution for computeWindowOffset().
    /// Child Views inside this ScrollView subtract this from their
    /// window offset so content appears translated by the scroll amount.
    Composition::Point2D scrollOffsetContribution() const override;
};

class OMEGAWTK_EXPORT ScrollViewDelegate : public Native::NativeEventProcessor {
    void onRecieveEvent(Native::NativeEventPtr event);
    friend class ScrollView;
protected:
    ScrollView *scrollView;

    /// Called when scroll wheel input is received. deltaX and deltaY
    /// are pixel deltas (positive = content moves right/down).
    INTERFACE_METHOD void onScrollWheel(float deltaX, float deltaY) DEFAULT;
};

}

#endif
