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
class AnimationScheduler;

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
// each `Widget::invalidate` / `Widget::invalidateNow` / `Widget::init`
// repaint (pre-Tier-D/D1 these all routed through `Widget::executePaint`)
// — brackets its work with a ScopedFrame, so nested passes share one
// frame via the depth counter.
//
// `UIView::update` / `SVGView::paint` hand their DisplayList to
// `submitView(...)`. `endFrame()` walks the pending submissions in
// insertion order (tree order), stamps each view's window-offset
// onto the window canvas's current frame, replays the DisplayList
// into the window canvas, and sendFrame()s once per submission. The
// aggregated CompositeFrame is then deposited into the window
// surface.
//
// Phase 3.8 made this the only paint route: the per-view canvases
// are gone.
class FrameBuilder {
    AppWindow & window_;
    // Nesting depth. Defensive: an AppWindow-driven paint pass
    // (initWidgetTree, dispatchResize*ToHosts) may transitively run
    // another. Only the outermost pair does the session work.
    int depth_ = 0;

    // Tier 4 Phase 4.3: monotonically increasing frame counter, stamped
    // into the FrameTime handed to AnimationScheduler::tick at the start
    // of each outermost frame. Stands in for the (not-yet-built) frame
    // pacer's frame index.
    std::uint32_t frameIndex_ = 0;

    // UIView-Render-Redesign Phase G.3.2 (rev. 2026-06-17): whether this
    // window has presented its first composite frame yet. Used by the
    // cache-eligibility check to skip the cache until the first frame is
    // on screen. On a brand-new window the native swap chain hasn't
    // settled yet, and the begin-native-pass / begin-scratch-pass /
    // resume-native-pass cycle that the cache uses produces a blank first
    // frame on Metal (the native pass A between `beginFrame` and the
    // scratch transition has no draws on it, and Metal does not flush its
    // Clear action into a present-visible state before the resume's
    // LoadPreserve reads it). The cache kicks in from the second
    // presented frame onwards.
    //
    // This is a per-PRESENTED-frame flag, not a per-`buildFrame` counter.
    // `endFrame` sets it (only on a real deposit) AFTER every `buildFrame`
    // for that frame has run, so every slice composing one presented frame
    // sees the same value. A per-`buildFrame` counter flipped mid-frame and
    // let one presented frame carry both a direct slice and an all-miss
    // cache slice of the same content — a full opaque double-composite on
    // the initial frame (see the G.3.2-rev2 follow-up in the plan doc).
    bool firstFramePresented_ = false;

    // Tier B / B3: the active lifecycle phase. UIView::update() flips
    // this around each of its ordered sub-phases via ScopedPhase; B5
    // wires the cross-phase assertions that consult it.
    FramePhase currentPhase_ = FramePhase::Idle;

    // Phase 4.7.1+: `buildFrame` assembles one window-wide
    // DisplayList per frame and queues it here. Pre-4.7.5 this was
    // populated per UIView/SVGView by `submitView` during the widget
    // paint walk; post-4.7.5 only `buildFrame` writes here.
    struct PendingSubmission {
        Composition::Point2D windowOffset {0.f, 0.f};
        Composition::DisplayList list {};
    };
    std::vector<PendingSubmission> pending_;

    // Phase 4.7.5: the offset accumulator (`offsetStack_` +
    // `ScopedViewOffset`) is deleted. The central
    // `FrameBuilder::buildFrame` walk threads `PaintContext.offset`
    // through its own pre-order traversal, so the stack was redundant
    // once the per-widget `submitView` flow that consumed it was
    // retired in 4.7.4.

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

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D6.3 (2026-06-03):
    /// the owning window. `StyleResolver` reaches through this from
    /// `AppWindow::activeFrameBuilder()` to read
    /// `window().styleSheets()` during Phase 2. Lifetime matches the
    /// FrameBuilder's, which matches the AppWindow's.
    AppWindow & window() const { return window_; }

    // Open the window-level composition session for this frame.
    // Idempotent across nesting via the depth counter.
    void beginFrame();
    // Close the window-level composition session. Drains pending
    // submissions, flushes the window CompositeFrame, and detaches
    // the proxy.
    void endFrame();

    // Phase 4.7.5: `submitView` is deleted. `buildFrame` writes the
    // window-wide DisplayList directly to `pending_` itself; no
    // per-view callers remain. The accumulator API
    // (`currentOffset` / `hasOffsetOnStack` / `pushOffset` /
    // `popOffset` / `ScopedViewOffset`) is deleted alongside it —
    // PaintContext.offset is the per-walk offset now.

    // Phase 4.7.1: the centralised Paint walk. Allocates one window-
    // wide `DisplayList`, threads a `PaintContext` (whose `offset`
    // tracks the running absolute window position) down the View tree
    // rooted at `root`, calls `view->paint(pc)` at each node (the
    // per-node hook emits its own draw ops; recursion lives here, not
    // in `paint`), and submits the aggregated DL as a single pending
    // submission whose `windowOffset == {0,0}` — UIView's paint code
    // bakes `pc.offset` into every emitted rect, so the DL is already
    // in absolute window coords and the submitter only needs the
    // window viewport bounds at flush time. As of Phase 4.7.2, the
    // walk also runs the dirty-bit-gated Style and Layout passes
    // before Paint.
    void buildFrame(View & root);

    /// Overlay-Z-Order-Plan O2.1 — emit a one-op overlay-chrome
    /// submission containing a single `DrawOp::Shadow`. The op's
    /// `shapeRect` is the overlay's already-resolved window-space
    /// rect; the slice's `windowOffset` is `{0, 0}` so the op lands
    /// in absolute window coordinates, matching every other slice
    /// the FrameBuilder produces. Call this *before* the overlay's
    /// own `buildFrame(overlay->view)` so the shadow slice appends
    /// to `pending_` first and the backend renders the shadow
    /// underneath the overlay content. No-op if not inside a
    /// `beginFrame`/`endFrame` bracket. The `cornerRadius` parameter
    /// lets the caller match the overlay widget's visible corner
    /// rounding so the shadow tracks the silhouette; pass `0.f`
    /// for a rectangular overlay.
    void submitOverlayShadow(
        const Composition::LayerEffect::DropShadowParams & shadow,
        const Composition::Rect & shapeRect,
        float cornerRadius);

    // Phase 4.4 (Block 2): per-window AnimationScheduler accessor for the
    // animation surfaces (View::applyLayoutDelta, UIView::applyLayoutDelta,
    // UIView::Impl::startOrUpdateAnimation, the applyAnimated*/animatedValue
    // readers). FrameBuilder is the natural broker — it already owns the
    // active-frame lookup the call sites use to find the right window's
    // scheduler. Returns null if the window has no scheduler (defensive;
    // AppWindow always stands one up in Phase 4.3).
    AnimationScheduler * animationScheduler() const;

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
