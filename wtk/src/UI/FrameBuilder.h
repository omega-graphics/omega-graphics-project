#ifndef OMEGAWTK_UI_FRAMEBUILDER_H
#define OMEGAWTK_UI_FRAMEBUILDER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Geometry.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OmegaWTK {

class AppWindow;
class View;

namespace Composition {
    struct CompositeFrame;
}

// Widget-View-Paint-Lifecycle Tier B / B3 (§3.1–3.2): the strict per-
// frame phase order. Through Tier B each UIView::update() runs this
// sequence locally (flipping currentPhase_ via ScopedPhase); Tier D
// hoists it into FrameBuilder::buildFrame across the whole tree.
enum class FramePhase : std::uint8_t {
    Idle, Tick, Style, Layout, Paint, Commit
};

inline const char * phaseName(FramePhase phase){
    switch(phase){
        case FramePhase::Idle:   return "Idle";
        case FramePhase::Tick:   return "Tick";
        case FramePhase::Style:  return "Style";
        case FramePhase::Layout: return "Layout";
        case FramePhase::Paint:  return "Paint";
        case FramePhase::Commit: return "Commit";
    }
    return "?";
}

// Tier 3: window-level frame driver.
//
// FrameBuilder owns the lifetime of the window-scoped composition
// session (open at beginFrame, close at endFrame) and the single
// per-frame CompositeFrame. Every paint pass — display, resize, and
// each Widget::executePaint invalidate/init repaint — brackets its
// work with a ScopedFrame, so nested passes share one frame via the
// depth counter.
//
// `UIView::update` / `SVGView::paint` hand their DisplayList to
// `submitView(...)`. `endFrame()` walks the pending submissions in
// insertion order (tree order), stamps each view's window-offset
// onto the window canvas's current frame, replays the DisplayList
// into the window canvas, and sendFrame()s once per submission. The
// aggregated CompositeFrame is then deposited into the window
// surface.
//
// Phase 3.8 made this the only paint route: the per-view canvases and
// the OMEGAWTK_WINDOW_SCOPED_PAINT flag are gone.
class FrameBuilder {
    AppWindow & window_;
    // Nesting depth. Defensive: an AppWindow-driven paint pass
    // (initWidgetTree, dispatchResize*ToHosts) may transitively run
    // another. Only the outermost pair does the session work.
    int depth_ = 0;

    // Tier B / B3: the active lifecycle phase. UIView::update() flips
    // this around each of its ordered sub-phases via ScopedPhase; B5
    // wires the cross-phase assertions that consult it.
    FramePhase currentPhase_ = FramePhase::Idle;

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

    // Phase 3.2: window-level CompositeFrame allocated at beginFrame,
    // attached to the AppWindow's compositor proxy so the
    // windowCanvas's pushFrame has somewhere to deposit slices.
    // Deposited into the window
    // surface at endFrame.
    SharedHandle<Composition::CompositeFrame> compositeFrame_;

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

    // Tier B / B3: lifecycle-phase state. setPhase flips the active
    // phase; assertPhase is the debug-only check that B5's work-method
    // guards call (e.g. DisplayList::append only during Paint). Through
    // Tier B no work method calls assertPhase yet — B5 adds the teeth.
    FramePhase currentPhase() const { return currentPhase_; }
    void setPhase(FramePhase phase){ currentPhase_ = phase; }
    void assertPhase(FramePhase expected) const {
        assert(currentPhase_ == expected &&
               "FrameBuilder lifecycle phase violation");
        (void)expected;
    }

    // RAII: set the phase on construction, restore the previous phase on
    // destruction. Null-safe (a null FrameBuilder is a no-op). Restoring
    // the previous phase (rather than forcing Idle) keeps nesting honest.
    struct ScopedPhase {
        FrameBuilder * fb;
        FramePhase previous;
        ScopedPhase(FrameBuilder * f, FramePhase phase)
            : fb(f), previous(f != nullptr ? f->currentPhase_ : FramePhase::Idle) {
            if(fb != nullptr){ fb->currentPhase_ = phase; }
        }
        ~ScopedPhase(){ if(fb != nullptr){ fb->currentPhase_ = previous; } }
        ScopedPhase(const ScopedPhase &) = delete;
        ScopedPhase & operator=(const ScopedPhase &) = delete;
    };

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
