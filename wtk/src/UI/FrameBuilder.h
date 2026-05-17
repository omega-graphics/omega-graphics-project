#ifndef OMEGAWTK_UI_FRAMEBUILDER_H
#define OMEGAWTK_UI_FRAMEBUILDER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Geometry.h"

#include <cstddef>
#include <vector>

namespace OmegaWTK {

class AppWindow;
class View;

namespace Composition {
    struct CompositeFrame;
}

// Tier 3 Phase 3.1/3.2: window-level frame driver.
//
// Phase 3.1 established the bracket: FrameBuilder owns the lifetime
// of the window-scoped composition session (open at beginFrame,
// close at endFrame). Per-view sessions opened by UIView::update /
// SVGView::paint / ScrollView still run as they do today and
// coexist with the window-level session.
//
// Phase 3.2 wires the first real window-scoped paint route: when
// `AppWindow::windowScopedPaint()` is on, `UIView::update` hands
// its DisplayList to `submitView(...)` instead of replaying into
// its per-view canvas. `endFrame()` walks the pending submissions
// in insertion order (tree order), stamps each view's window-offset
// onto the window canvas's current frame, replays the DisplayList
// into the window canvas, and sendFrame()s once per submission.
// The aggregated CompositeFrame is then deposited into the window
// surface.
//
// Per-view paint paths continue to deposit their own CompositeFrames
// in parallel during the transition; the flag is flipped on per
// scene so any regression is isolated. Phase 3.8 deletes the
// per-view canvases and removes the flag.
class FrameBuilder {
    AppWindow & window_;
    // Nesting depth. Defensive: an AppWindow-driven paint pass
    // (initWidgetTree, dispatchResize*ToHosts) may transitively run
    // another. Only the outermost pair does the session work.
    int depth_ = 0;
    // Visuals count on the window Canvas at the start of the
    // outermost beginFrame, used by endFrame to decide whether any
    // draws landed via the direct path (Phase 3.1) when no
    // submissions were queued via submitView.
    std::size_t baselineVisualCount_ = 0;

    // Phase 3.2: pending UIView submissions for this frame. Captured
    // by submitView() during the widget paint walk; drained by
    // endFrame() in insertion order (== tree order, since the widget
    // paint pass is pre-order today).
    struct PendingSubmission {
        Composition::Point2D windowOffset {0.f, 0.f};
        Composition::DisplayList list {};
    };
    std::vector<PendingSubmission> pending_;

    // Phase 3.4: window-offset accumulator threaded through the
    // widget paint walk. Replaces View::computeWindowOffset's
    // parent-chain walk: instead of each leaf view summing positions
    // up to the root at submit time, the tree walker
    // (WidgetTreeHost::initWidgetRecurse / invalidateWidgetRecurse)
    // pushes each visited view's absolute window offset on enter and
    // pops on exit, and the leaf-view paint code (UIView::update /
    // SVGView::paint) pushes one final entry for itself before
    // calling submitView. The stack top is therefore always the
    // current view's absolute window offset; submitView reads it via
    // the new View::computeWindowOffset wrapper. Empty stack ⇒ {0,0}.
    std::vector<Composition::Point2D> offsetStack_;

    // Phase 3.2: window-level CompositeFrame allocated at beginFrame
    // when the windowScopedPaint flag is on, attached to the
    // AppWindow's compositor proxy so the windowCanvas's pushFrame
    // has somewhere to deposit slices. Deposited into the window
    // surface at endFrame.
    SharedHandle<Composition::CompositeFrame> compositeFrame_;
    // True iff the windowScopedPaint flag was on at this frame's
    // beginFrame. Captured once so a mid-frame flag flip cannot
    // strand the CompositeFrame attached to the proxy.
    bool windowScopedPaintActive_ = false;

public:
    explicit FrameBuilder(AppWindow & window);
    ~FrameBuilder();

    FrameBuilder(const FrameBuilder &) = delete;
    FrameBuilder & operator=(const FrameBuilder &) = delete;

    // Open the window-level composition session for this frame.
    // Idempotent across nesting via the depth counter.
    void beginFrame();
    // Close the window-level composition session. Drains pending
    // submissions, flushes the window CompositeFrame, and detaches
    // the proxy.
    void endFrame();

    // Phase 3.2: record a UIView's DisplayList for replay into the
    // window canvas at endFrame. Captures the view's window-offset
    // at submission time via View::computeWindowOffset, which
    // (Phase 3.4) returns the FrameBuilder's accumulator top while
    // a frame is in flight.
    void submitView(View * view, Composition::DisplayList list);

    // Phase 3.4: accumulator API. The widget-tree walker and the
    // per-view paint code (UIView::update / SVGView::paint) push
    // an absolute window offset on enter and pop on exit; everyone
    // who needs a view's window offset reads it through
    // `View::computeWindowOffset`, which now wraps `currentOffset()`.
    Composition::Point2D currentOffset() const;
    /// True when at least one entry has been pushed onto the offset
    /// accumulator since the outermost beginFrame. `currentOffset()`
    /// returns {0,0} when this is false, which is not safe to use
    /// as a fall-through for `View::computeWindowOffset` — the
    /// wrapper consults this and falls back to the legacy walk
    /// when the stack is empty (e.g. callers like
    /// NativeViewHost::syncBounds that run inside a ScopedFrame
    /// but outside any walker push scope).
    bool hasOffsetOnStack() const { return !offsetStack_.empty(); }
    void pushOffset(Composition::Point2D absolute);
    void popOffset();

    // RAII helper: pushes `view`'s absolute window offset on
    // construction (parent.scrollOffsetContribution already factored
    // into `view->computeOffsetDeltaFromParent()`), pops on
    // destruction. Null-safe on both `fb` and `view` — a null
    // FrameBuilder skips the push/pop entirely (off-flag / not in a
    // frame); a null view pushes the current top unchanged so the
    // stack depth always mirrors recursion depth.
    struct ScopedViewOffset {
        FrameBuilder * fb;
        ScopedViewOffset(FrameBuilder * f, View * v);
        ~ScopedViewOffset();
        ScopedViewOffset(const ScopedViewOffset &) = delete;
        ScopedViewOffset & operator=(const ScopedViewOffset &) = delete;
    };

    // Phase 3.2: mirrors AppWindow::windowScopedPaint() but reads
    // the value captured at beginFrame, so callers inside the paint
    // walk see a stable answer for the whole frame.
    bool windowScopedPaint() const { return windowScopedPaintActive_; }

    // RAII wrapper for the typical bracket-an-AppWindow-paint-pass
    // call site. Null-safe so callers can guard on the FrameBuilder
    // pointer without an explicit branch.
    struct ScopedFrame {
        FrameBuilder * fb;
        explicit ScopedFrame(FrameBuilder * f): fb(f){ if(fb) fb->beginFrame(); }
        ~ScopedFrame(){ if(fb) fb->endFrame(); }
        ScopedFrame(const ScopedFrame &) = delete;
        ScopedFrame & operator=(const ScopedFrame &) = delete;
    };
};

} // namespace OmegaWTK

#endif
