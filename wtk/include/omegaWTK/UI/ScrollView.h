#ifndef OMEGAWTK_UI_SCROLLVIEW_H
#define OMEGAWTK_UI_SCROLLVIEW_H

#include "View.h"
#include "omegaWTK/Composition/Animation.h"   // E4: AnimationHandle (fling)

namespace OmegaWTK {

namespace Composition {
    class DisplayList;
}

class ScrollViewDelegate;
class AnimationScheduler;   // E4: per-window animation driver (UI-private)

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

    // E3 scroll-bar drag state. `draggingThumb_` is set between an
    // LMouseDown on a thumb and the matching LMouseUp; `dragVertical_`
    // records which bar; `dragGrab_` is the pointer-to-thumb-origin gap
    // along the drag axis (window coords) captured at grab so the thumb
    // does not jump under the cursor.
    bool  draggingThumb_ = false;
    bool  dragVertical_  = false;
    float dragGrab_      = 0.f;

    // E4 fling-momentum state. Velocity (offset px/sec) is sampled across
    // the drag; on release a decelerating tween carries the offset to a
    // projected landing point. `flingAnim_` is the in-flight tween,
    // cancelled on any new user input and in the destructor.
    float  dragLastOffset_  = 0.f;
    double dragLastTimeSec_ = 0.0;
    float  dragVelocity_    = 0.f;
    Composition::AnimationHandle flingAnim_ {};

    // E5: timestamp of the last discrete-wheel tick, for computing wheel
    // velocity so a mouse wheel (no OS momentum) gets an app-side fling.
    // A trackpad on an OS-momentum platform (macOS) is left alone.
    double wheelLastTimeSec_ = 0.0;

    // E5 (richer contract): trackpad-gesture velocity tracking for platforms
    // that stream NO OS momentum (GTK/libinput). While `trackpadGesture_` is
    // true we are mid two-finger scroll: each `Changed` event EMA-accumulates
    // the offset velocity, and the terminating `Ended` (is_stop) event flings
    // with it. On an OS-momentum platform these fields stay untouched — the
    // ScrollView defers to the OS stream and never enters this path.
    bool   trackpadGesture_     = false;
    bool   trackpadVertical_    = false;
    float  trackpadVelocity_    = 0.f;
    float  trackpadLastOffset_  = 0.f;
    double trackpadLastTimeSec_ = 0.0;

    /// E1: the thumb rectangle for one axis in this view's LOCAL space, or
    /// a zero-size rect when that axis does not overflow (no bar). Shared
    /// by `paintAfterChildren` (drawing) and the drag hit-test.
    Composition::Rect thumbLocalRect(bool vertical);
    /// E1: which thumb (if any) a window-space point lands on. Returns 1
    /// for the vertical thumb, 2 for the horizontal, 0 for neither.
    int hitTestThumb(const Composition::Point2D & windowPoint);
    /// E3: the full bar track strip for one axis in LOCAL space (the thumb
    /// slides within this), or zero-size when that axis has no bar. Used to
    /// tell a track click from a content click.
    Composition::Rect trackLocalRect(bool vertical);
    /// E3: set the scroll offset so the thumb's top/left sits at
    /// `pointerAxisWindow - grab` along the drag axis (window coords),
    /// clamped to the track. Shared by thumb-drag and track-click-jump.
    void dragThumbTo(bool vertical, float pointerAxisWindow, float grab);
    /// E3: handle an LMouseDown / CursorMove / LMouseUp for the scroll-bar
    /// drag interaction. Kept off `DefaultScrollHandler::onRecieveEvent` to
    /// keep the wheel/key path readable.
    void handleDragPointer(Native::NativeEventPtr event);
    /// E4: the window's AnimationScheduler, or nullptr if not attached.
    AnimationScheduler * scheduler();
    /// E4: cancel any in-flight momentum tween (called on new user input).
    void cancelFling();
    /// E4: start a decelerating momentum tween from the current offset
    /// given the release velocity (offset px/sec) along the drag axis.
    void startFling(bool vertical, float velocity);

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

    /// V3 (ScrollView-4.7-Integration-Plan): the pre-children paint hook.
    /// Emits a single `DrawOp::PushClip` at this view's absolute window
    /// rect so descendant draws are scissored to the viewport. The
    /// matching `PopClip` is emitted by `paintAfterChildren` once all
    /// subviews have painted. (Replaces the orphaned Tier-3
    /// `paint(DisplayList&)` / `paintOverlay(DisplayList&)` pair, which
    /// the Phase 4.7 walker never called — it dispatches the virtual
    /// `View::paint(PaintContext&)`.)
    void paint(Composition::PaintContext & pc) override;

    /// V3: the post-children paint hook. Emits the `DrawOp::PopClip` that
    /// closes the clip opened in `paint`. (V4 will also emit the scroll
    /// bars here, outside the clip.)
    void paintAfterChildren(Composition::PaintContext & pc) override;

    /// V3: a ScrollView clips its descendants, so its subtree paints live
    /// (the cache walker must not split the clip bracket across capture
    /// markers). See `View::clipsContentSubtree`.
    bool clipsContentSubtree() const override;

    /// E4: cancels any in-flight momentum tween so the scheduler never
    /// fires its `this`-capturing callback after the view is gone.
    ~ScrollView() override;
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
