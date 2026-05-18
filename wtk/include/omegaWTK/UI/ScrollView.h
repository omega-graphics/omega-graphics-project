#ifndef OMEGAWTK_UI_SCROLLVIEW_H
#define OMEGAWTK_UI_SCROLLVIEW_H

#include "View.h"

namespace OmegaWTK {

namespace Composition {
    class DisplayList;
}

class ScrollViewDelegate;

/// @brief A fully virtual scroll container.
///
/// Tier 3 Phase 3.6: ScrollView is the first `PushClip` producer and
/// the first consumer of the FrameBuilder offset accumulator's
/// `contentOffset()` hook. The two prior overlay scroll-bar `Layer`s
/// and their per-canvas paint paths are gone — the bars are now
/// declarative styling resolved into `RoundedRect` `DrawOp`s at paint
/// time inside `paintOverlay()`. Content clipping flows through
/// `paint()` emitting `DrawOp::PushClip(getRect())` and
/// `paintOverlay()` emitting `DrawOp::PopClip()` so the FrameBuilder
/// can bracket descendant draws between them on the shared window
/// canvas (per-frame balanced — see `FrameBuilder::submitView`).
class OMEGAWTK_EXPORT ScrollView : public View {
    SharedHandle<View> child;
    Composition::Point2D scrollOffset {0.f, 0.f};
    ScrollViewDelegate *delegate = nullptr;
    bool hasVerticalScrollBar,hasHorizontalScrollBar;
    bool hasDelegate() override;

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

    /// Sets the scroll offset directly. Marks the view dirty so the
    /// next paint pass re-emits ops with shifted descendants.
    void setScrollOffset(const Composition::Point2D & offset);

    /// Tier 3 Phase 3.6: the offset applied to children's positions
    /// when arranging them. Returns `-scrollOffset_` so the
    /// FrameBuilder offset accumulator (Phase 3.4 stack) folds it in
    /// when entering this subtree, yielding scroll-shifted descendant
    /// rects in layout — the 2D translation never has to become a
    /// transform op (per §3.2, `DisplayList::PushTransform` is
    /// reserved for 3D effects).
    Composition::Point2D contentOffset() const override;

    /// Tier 3 Phase 3.6: layerization tag — `true` requests the
    /// compositor to give this subtree its own composition layer. The
    /// tag is a no-op for Tier 3 (content re-rasterizes every frame);
    /// a future compositor-thread scrolling pass reads it.
    bool wantsLayer() const override;

    /// Tier 3 Phase 3.6: emit the *pre-children* draw ops for this
    /// ScrollView's paint into `list`. Appends a single
    /// `DrawOp::PushClip(getRect())` so descendant draws ending up in
    /// later submissions land inside the scissor on the shared window
    /// canvas. The matching `PopClip` is emitted by `paintOverlay()`
    /// after children finish; the caller is responsible for ordering
    /// the two so the per-frame `PushClip` / `PopClip` count balances
    /// (FrameBuilder enforces per-submission balance, so paint() and
    /// paintOverlay() are intended to be emitted into the SAME
    /// `DisplayList` with the content ops appended between them, or
    /// emitted into two separate submissions whose pair-balance the
    /// caller verifies).
    void paint(Composition::DisplayList & list) const;

    /// Tier 3 Phase 3.6: emit the *post-children* draw ops for this
    /// ScrollView's paint into `list`. Appends `DrawOp::PopClip()`
    /// first, then up to two `DrawOp::RoundedRect`s for the visible
    /// scroll bars (computed from the current scroll offset, the
    /// view's bounds, and the content child's bounds). The bars
    /// emit *outside* the clip, so they always render even when the
    /// content extends beyond the viewport.
    void paintOverlay(Composition::DisplayList & list) const;
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
