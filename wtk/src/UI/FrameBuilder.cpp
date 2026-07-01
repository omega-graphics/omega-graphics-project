#include "FrameBuilder.h"

#include "AppWindowImpl.h"
#include "WidgetTreeHost.h"
#include "omegaWTK/UI/Widget.h"   // D7.2 auto-pump uses Widget::invalidate

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

#include "omegaGTE/GE.h"  // OmegaGTE::isDebugLayerEnabled() — gates [WTK_RP] traces
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/App.h"   // Tier 2: AppInst::resolveWindowSurfaceColor
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/LayoutManager.h"   // Phase 4.7.2: Layout pass invokes node.layoutManager()->measure/arrange.

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
// G.3.2 paint walker needs `ContentCacheConfig` (env-driven cache
// limits, in particular the min-size eligibility threshold).
// (AnimationScheduler.h is already included above for the
// hasAnyAnimationFor() check that powers eligibility rule #1.)
#include "../Composition/backend/ContentCache.h"
#endif

namespace OmegaWTK {

namespace {
// Phase 3.2: AppWindow::activeFrameBuilder() reads this; FrameBuilder
// sets it during the outermost beginFrame/endFrame so UIView::update
// (and SVGView in Phase 3.3) can route their DisplayList submissions
// to the right FrameBuilder without holding a back-pointer to
// AppWindow. Paint runs on the UI thread; a single static is
// sufficient. If paint ever multi-threads, this becomes thread_local.
FrameBuilder * g_activeFrameBuilder = nullptr;

// Tier 4 Phase 4.3: monotonic clock for the FrameTime stamp handed to
// AnimationScheduler::tick. Interim stand-in for the frame pacer
// (Frame-Pacing-Plan); steady_clock is monotonic, which is all the
// scheduler's elapsed-time math needs.
std::uint64_t steadyFrameClockNs(){
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

FrameBuilder::FrameBuilder(AppWindow & window): window_(window) {}

FrameBuilder::~FrameBuilder() = default;

void FrameBuilder::beginFrame(){
    if(depth_++ > 0){
        return;
    }
    if(OmegaGTE::isDebugLayerEnabled()){
        std::cerr << "[WTK_RP] FrameBuilder::beginFrame: entered (depth=1)" << std::endl;
    }

    auto * impl = window_.impl_.get();
    if(impl == nullptr){
        if(OmegaGTE::isDebugLayerEnabled()){
            std::cerr << "[WTK_RP] FrameBuilder::beginFrame: impl is null, aborting" << std::endl;
        }
        return;
    }
    auto * treeHost = impl->widgetTreeHost.get();
    if(treeHost != nullptr){
        if(impl->proxy.getFrontendPtr() == nullptr){
            impl->proxy.setFrontendPtr(treeHost->compPtr());
        }
        auto desiredLane = treeHost->laneId();
        if(desiredLane != 0 && impl->proxy.getSyncLaneId() != desiredLane){
            impl->proxy.setSyncLaneId(desiredLane);
        }
    }

    // Tier 4 Phase 4.3 (Block 2): Tick phase. Advance the per-window
    // animation scheduler once per outermost frame, before any paint
    // work, so its side table is fresh when Paint reads it (wired in
    // 4.4). Additive today — the scheduler's active set is empty until
    // UIView routes onto it, so this is a no-op-over-empty for now.
    if(impl->animationScheduler_){
        ScopedPhase tickPhase(this, FramePhase::Tick);
        // Phase H: the per-window FramePacer is now the single owner of
        // the FrameTime clock + frame index — it stamps the predicted
        // presentation time (vsync-aligned) when OMEGAWTK_VSYNC_PACING is
        // on, and an OS-monotonic now() otherwise, closing Phase 4.3's
        // `steady_clock` + per-FrameBuilder-counter stand-in. The local
        // `steadyFrameClockNs()` + `frameIndex_` fallback survives only for
        // the defensive case where the pacer is somehow absent.
        const FrameTime frameTime = (impl->framePacer_ != nullptr)
            ? impl->framePacer_->beginFrameTime()
            : FrameTime{steadyFrameClockNs(), frameIndex_++};
        impl->animationScheduler_->tick(frameTime);

        // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
        // scheduler-side auto-pump. While the active set is non-empty,
        // the tick has just sampled fresh values into the side table —
        // but `buildFrame`'s `rootMask == 0` gate skips the paint pass
        // when no widget marked itself dirty, and the deprecated
        // `Widget::onPaint(PaintReason)` pump (formerly the self-loop
        // path) is no longer dispatched (Phase 4.7.4 cutover). Without
        // this pump, every scheduler-driven animation — `animateElement`,
        // `tweenProperty`, the D7.2 `transition` hook — runs one tick
        // after registration and then freezes at t≈0.
        //
        // Two parts:
        //   1. Mark the widget tree's root view `Paint`-dirty so
        //      `buildFrame` continues into the Paint pass this frame.
        //      The paint walker visits every subview unconditionally —
        //      marking only the root is enough to refresh the whole
        //      tree (per-node dirty culling is a Tier 5 follow-up).
        //   2. Request another vsync turn so the next frame fires its
        //      own tick + paint. Naturally winds down once
        //      `propertyAnims` / `callbackAnims` go empty.
        //
        // Documented under D8 as the canonical replacement for the
        // `BlueRectWidget::onPaint` self-pump in
        // `wtk/tests/ContainerClampAnimationTest/main.cpp`.
        const auto schedStats = impl->animationScheduler_->stats();
        if(schedStats.activeProperty + schedStats.activeCallback > 0){
            // `Widget::invalidate(StateChanged)` is the public surface that
            // both marks the view's Paint bit AND calls
            // `treeHost->requestFrame()` — equivalent to two separate
            // calls (markDirty + window_.requestFrame) but avoids
            // touching Widget's protected `view` field. The
            // `dirtyBitsForReason(StateChanged)` derivation maps to
            // exactly `Paint` (no Style / Layout) which is what we want:
            // animations write the scheduler side table, Paint reads it,
            // Style / Layout do not need to re-run.
            auto * treeHost = impl->widgetTreeHost.get();
            if(treeHost != nullptr && treeHost->root != nullptr){
                treeHost->root->invalidate(PaintReason::StateChanged);
            }
            else {
                window_.requestFrame();
            }
        }
    }

    pending_.clear();
    // Phase 4.7.5: the offset accumulator is gone — `buildFrame`
    // threads `PaintContext.offset` through its walker instead.

    // Phase 3.8: window-scoped paint is the only path. Allocate the
    // window-level CompositeFrame and attach it to the AppWindow's
    // proxy. windowCanvas's pushFrame (during endFrame's replay loop)
    // deposits slices here; endFrame hands the frame off to the
    // window surface.
    compositeFrame_ = std::make_shared<Composition::CompositeFrame>();
    impl->proxy.setActiveCompositeFrame(compositeFrame_.get());

    g_activeFrameBuilder = this;
}

void FrameBuilder::endFrame(){
    if(depth_ == 0){
        // endFrame without a matching beginFrame — defensive no-op.
        return;
    }
    if(--depth_ > 0){
        return;
    }

    auto * impl = window_.impl_.get();
    if(impl == nullptr){
        g_activeFrameBuilder = nullptr;
        return;
    }

    // Tier 4 §4.1: pack each submission's DisplayList straight into the
    // window CompositeFrame via the proxy — no Canvas / CanvasFrame
    // bridge, no DisplayListReplay. Each slice carries the live
    // window-offset (the GPU viewport origin — captured fresh in
    // submitView, so there is no stale CanvasFrame::rect snapshot in the
    // path) plus the window-sized, local-origin bounds (the viewport
    // extent). The backend flush dispatches the slice's DrawOps via the
    // Phase 4.0 renderToTarget(DrawOp::Type) switch. windowCanvas_ is now
    // bypassed (deleted in 4.2).
    const Composition::Rect windowBounds{
        Composition::Point2D{0.f, 0.f},
        window_.getRect().w, window_.getRect().h};
    for(auto & sub : pending_){
        impl->proxy.submitDisplayList(std::move(sub.list),
                                      sub.windowOffset, windowBounds);
    }

    pending_.clear();

    // Detach the CompositeFrame from the proxy before depositing
    // so any post-endFrame draws (there shouldn't be any) do
    // not silently re-enter this frame.
    impl->proxy.setActiveCompositeFrame(nullptr);
    const std::size_t sliceCount = compositeFrame_ ? compositeFrame_->slices.size() : 0;
    const bool hasSurface = (impl->windowSurface != nullptr);
    const bool willDeposit = (compositeFrame_ != nullptr && sliceCount > 0 && hasSurface);
    if(OmegaGTE::isDebugLayerEnabled()){
        std::cerr << "[WTK_RP] FrameBuilder::endFrame: slices=" << sliceCount
                  << " hasSurface=" << (hasSurface ? 1 : 0)
                  << " willDeposit=" << (willDeposit ? 1 : 0)
                  << " pendingSubmissions=" << pending_.capacity()
                  << std::endl;
    }
    if(willDeposit){
        impl->windowSurface->deposit(compositeFrame_);
        // G.3.2 first-paint warmup guard: the first frame actually put on
        // screen is a full direct render (cache skipped); the content
        // cache engages from the next presented frame. Flip the flag only
        // on a real deposit and only AFTER every `buildFrame` for this
        // frame has run, so all slices composing this frame took the same
        // (direct) branch — preventing the direct+cache double-composite
        // that a per-`buildFrame` counter produced on the initial frame.
        firstFramePresented_ = true;
    }
    compositeFrame_.reset();

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 fixup (2026-06-04):
    // late auto-pump check. The pre-Style auto-pump in `beginFrame`
    // catches "an animation was already running when this frame
    // started." But transitions registered DURING this frame's Style
    // phase (via `StyleResolver::applyTransitions` →
    // `scheduler.transition<T>` → `tweenProperty`) only become active
    // AFTER the early auto-pump's `stats()` snapshot. Without this
    // late check, the very first cell-change-triggered transition
    // registers a tween that never gets ticked, because no further
    // requestFrame ever fires. Re-check the scheduler state at the
    // end of every outermost frame and schedule another vsync if
    // anything is active — the next frame's `beginFrame` auto-pump
    // then takes over the per-frame pump duty.
    if(impl->animationScheduler_){
        const auto schedStats = impl->animationScheduler_->stats();
        if(schedStats.activeProperty + schedStats.activeCallback > 0){
            window_.requestFrame();
        }
    }

    g_activeFrameBuilder = nullptr;
}

// ---------------------------------------------------------------------------
// Phase 4.7.1: centralised Paint walk. Replaces the
// per-UIView::update → submitView dance with one window-wide
// DisplayList built by a single top-down tree walk. Lives alongside
// `submitView` through 4.7.3; 4.7.4 makes this the only driver.
// ---------------------------------------------------------------------------

namespace {

// Style-pass walker — pre-order, dirty-bit gated. Phase 4.7.2 / 4.7.3.
//
// Calls `node.resolveStyles()` only when this node's own `dirtyBits()`
// has `Style` set; recurses into children only when either this
// node's `dirtyBits()` or its propagated `descendantDirty()` mask
// carries `Style`, so subtrees with no Style dirtiness short-circuit
// at the parent.
void styleSubtree(View & node){
    const uint8_t self = node.dirtyBits();
    const uint8_t desc = node.descendantDirty();
    if((self & View::Style) != 0){
        node.resolveStyles();
    }
    if(((self | desc) & View::Style) == 0){
        return;
    }
    for(auto * child : node.subviews()){
        if(child != nullptr){
            styleSubtree(*child);
        }
    }
}

// Layout-pass walker — pre-order, dirty-bit gated. Phase 4.7.2 / 4.7.3.
//
// At each gated node (self.dirtyBits & Layout):
//   1. `node.layoutManager()->measure(node, nodeLocalRect)` — the manager
//      reads each child's preferred / cached size (FlexLayout has its
//      own per-child cache; the other 4.5 managers stub measure).
//   2. `node.layoutManager()->arrange(node, nodeLocalRect)` — writes each
//      child's final rect via `child->resize(...)`.
//   3. `node.arrangeContent()` — intra-node element layout (UIView
//      overrides to resolve element rects from `UIViewLayoutV2`).
//
// Subtree descent gates on `(self | desc) & Layout` — subtrees with a
// clean Layout mask are skipped entirely.
//
// The plan's "Measure bottom-up then Arrange top-down" two-pass shape
// is folded into a single pre-order walk for 4.7.2: each manager's
// `measure` already consults children via cached preferred sizes
// (FlexLayout reads `child->getRect()` + its per-child cache), so an
// explicit bottom-up walk does not add information. A future manager
// that needs parent-uses-child-measured-size will reintroduce the
// split.
//
// Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
// the parameter is `nodeRectInParent` — the node's rect expressed in
// its parent's coordinate space. Recursion passes `child->getRect()`
// which is also in this node's space. BEFORE dispatching to the
// manager, we derive `nodeLocalRect = {{0,0}, w, h}` so child
// placement happens in the node's LOCAL coordinate space (origin 0).
// Pre-D8 this derivation lived inside each manager
// (`AbsoluteLayout` / `FillLayout` / `StackLayout` all had a
// per-manager workaround); promoting it to the walker removes the
// trap from any future manager subclass and lets the per-manager
// workarounds collapse to direct use of the incoming rect.
void layoutSubtree(View & node, const Composition::Rect & nodeRectInParent){
    const uint8_t self = node.dirtyBits();
    const uint8_t desc = node.descendantDirty();
    if((self & View::Layout) != 0){
        if(auto * mgr = node.layoutManager()){
            const Composition::Rect nodeLocalRect{
                Composition::Point2D{0.f, 0.f},
                nodeRectInParent.w,
                nodeRectInParent.h
            };
            mgr->measure(node, nodeLocalRect);
            mgr->arrange(node, nodeLocalRect);
        }
        node.arrangeContent();
    }
    if(((self | desc) & View::Layout) == 0){
        return;
    }
    for(auto * child : node.subviews()){
        if(child != nullptr){
            layoutSubtree(*child, child->getRect());
        }
    }
}

// Frame-end dirty-clear walker. Phase 4.7.3.
//
// Visits every node that could possibly have a dirty bit (gated by
// `(self | desc) != 0`) and calls `clearDirtyBits()` to zero both the
// self mask and the propagated descendant mask. Run after the Paint
// pass so the next frame starts with a clean tree.
void clearDirtySubtree(View & node){
    const uint8_t self = node.dirtyBits();
    const uint8_t desc = node.descendantDirty();
    if((self | desc) == 0){
        return;
    }
    for(auto * child : node.subviews()){
        if(child != nullptr){
            clearDirtySubtree(*child);
        }
    }
    node.clearDirtyBits();
}

// Paint-pass walker — pre-order. Phase 4.7.1 (unchanged in 4.7.2).
//
// Visits each node, calls `paint(pc)` (per-node hook that emits this
// node's draw ops — no recursion inside paint), then descends into
// `subviews()`, updating `pc.offset` by `child.rect.pos +
// node.contentOffset()` per descent so each child sees its absolute
// window position in `pc.offset` — the same math
// `View::legacyComputeWindowOffset` performed inside
// `ScopedViewOffset` pre-4.7. Offset is saved / restored at each
// level so siblings see the parent's accumulated offset, not the
// trailing sibling's.
void paintSubtree(View & node, Composition::PaintContext & pc){
    node.paint(pc);

    const auto parentOffset = pc.offset;
    const auto contentOff   = node.contentOffset();
    for(auto * child : node.subviews()){
        if(child == nullptr){
            continue;
        }
        const auto & cr = child->getRect();
        pc.offset.x = parentOffset.x + contentOff.x + cr.pos.x;
        pc.offset.y = parentOffset.y + contentOff.y + cr.pos.y;
        paintSubtree(*child, pc);
    }
    pc.offset = parentOffset;
    // ScrollView-4.7-Integration-Plan V3: post-order hook, with pc.offset
    // restored to this node's own absolute offset. ScrollView emits its
    // PopClip here to close the PushClip its paint() opened.
    node.paintAfterChildren(pc);
}

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
// UIView-Render-Redesign-Plan §G.3.2-rev2 — per-View view-sized cache.
//
// Same pre-order walk as `paintSubtree`, but each eligible View wraps
// its OWN `paint(pc)` (not its subtree) in a `BeginCacheCapture` /
// `EndCacheCapture` pair, then recurses into children. Because each
// View wraps only its own paint, the markers never nest — the parent's
// End fires before the first child's Begin. The backend resolves hit
// vs miss at replay time: hit → blit the View-sized cached texture at
// the View's window rect and skip the wrapped ops; miss → capture the
// wrapped ops into a View-sized texture (window-sized viewport offset
// by the View's origin), cache + composite.
//
// Eligibility (size + animation) is decided here; `DrawOp::NativeContent`
// eligibility is deferred (no cheap View-level signal yet — see the
// plan note). Painting is direct into `pc.displayList`; no side list
// (a side list earlier masked the contentVersion freeze bug and is
// treated with suspicion).
void paintSubtreeWithCache(View & node,
                           Composition::PaintContext & pc,
                           AnimationScheduler * animScheduler,
                           std::uint32_t minSizePx,
                           bool dragActive){
    // ScrollView-4.7-Integration-Plan V3: a clip-producing view (ScrollView)
    // paints its WHOLE subtree live. Its paint() opens a PushClip and
    // paintAfterChildren() closes it; if any node inside that bracket went
    // through the per-View cache, a cache HIT would skip the wrapped ops —
    // dropping the PushClip while the PopClip still ran (scissor imbalance),
    // and a MISS would switch render targets mid-bracket. Recursing the
    // subtree through the non-cache `paintSubtree` keeps the clip bracket
    // free of capture markers. (Re-enabling the cache under a clip is the
    // tracked V3.1 follow-up.)
    if(node.clipsContentSubtree()){
        node.paint(pc);
        const auto parentOffset = pc.offset;
        const auto contentOff   = node.contentOffset();
        for(auto * child : node.subviews()){
            if(child == nullptr){
                continue;
            }
            const auto & cr = child->getRect();
            pc.offset.x = parentOffset.x + contentOff.x + cr.pos.x;
            pc.offset.y = parentOffset.y + contentOff.y + cr.pos.y;
            paintSubtree(*child, pc);
        }
        pc.offset = parentOffset;
        node.paintAfterChildren(pc);
        return;
    }

    const auto nodeRect = node.getRect();
    bool eligible = nodeRect.w >= static_cast<float>(minSizePx)
                 && nodeRect.h >= static_cast<float>(minSizePx);
    // §G.3.2 eligibility rule #1: never cache a view mid-animation — its
    // tween frames must render live. `View::isAnimating` checks the view
    // node id AND (for `UIView`) every per-element node id; the bare
    // `hasAnyAnimationFor(node.nodeId())` missed element-level animations
    // (drop shadow, per-element style transitions), which left animating
    // views cached and frozen on their start frame.
    if(eligible && animScheduler != nullptr
       && node.isAnimating(*animScheduler)){
        eligible = false;
    }

    if(eligible){
        // The View's window rect: pos = accumulated absolute window
        // offset (pc.offset), size = the View's own rect dims. The
        // backend uses pos for the capture-viewport offset + blit dest,
        // size for the texture dims + size bucket.
        //
        // §G.3.4: inflate that rect by how far this View's paint bleeds
        // past its layout rect (drop-shadow offset + blur). Because the
        // backend drives texture size, capture-viewport offset, size
        // bucket, AND blit dest entirely from this rect, inflating it here
        // is the whole fix — the shadow is captured into the (larger)
        // texture and blitted in its correct window position instead of
        // being scissored away to the layout rect. Zero bleed (the common
        // case) leaves the rect exactly as before.
        const View::PaintBleed bleed = node.paintBleed();
        const Composition::Rect windowRect{
                Composition::Point2D{pc.offset.x - bleed.left,
                                     pc.offset.y - bleed.top},
                nodeRect.w + bleed.left + bleed.right,
                nodeRect.h + bleed.top  + bleed.bottom};
        // §G.5.4: tag the marker with the live-drag state so the backend can
        // stretch this View's prior texture instead of re-rendering it.
        pc.displayList.append(Composition::DrawOp::makeBeginCacheCapture(
                node.nodeId(), node.contentVersion(), windowRect, dragActive));
        node.paint(pc);
        pc.displayList.append(Composition::DrawOp::makeEndCacheCapture(node.nodeId()));
    }
    else {
        node.paint(pc);
    }

    const auto parentOffset = pc.offset;
    const auto contentOff   = node.contentOffset();
    for(auto * child : node.subviews()){
        if(child == nullptr){
            continue;
        }
        const auto & cr = child->getRect();
        pc.offset.x = parentOffset.x + contentOff.x + cr.pos.x;
        pc.offset.y = parentOffset.y + contentOff.y + cr.pos.y;
        paintSubtreeWithCache(*child, pc, animScheduler, minSizePx, dragActive);
    }
    pc.offset = parentOffset;
    // V3 post-order hook (no-op for every node except a clip producer,
    // which is handled by the bypass branch above — kept here for parity
    // with the non-cache walker and any future after-children emitter).
    node.paintAfterChildren(pc);
}
#endif

} // namespace

void FrameBuilder::buildFrame(View & root){
    // Phase 4.7.2 + 4.7.3: the central per-frame loop. Three passes in
    // order — Style → Layout → Paint — each gated by the root's union
    // dirty mask (`root.dirtyBits() | root.descendantDirty()`):
    //   - Style runs only when `Style` is set somewhere in the tree.
    //   - Layout runs only when `Layout` is set somewhere in the tree.
    //   - Paint runs when ANY bit is set anywhere — a Style or Layout
    //     change implies the next frame must re-paint, plus an
    //     explicit `Paint` dirty (e.g. animation tick) triggers paint
    //     on its own.
    // Each pass walker prunes subtrees whose `(dirtyBits |
    // descendantDirty) & passBit == 0`, so a Paint-only animation tick
    // skips Style and Layout entirely and Paint visits only the
    // dirty branch.
    //
    // After all passes, `clearDirtySubtree` zeroes every dirty bit so
    // the next frame starts clean. Caller must mark the appropriate
    // bits before invoking `buildFrame` (Widget::invalidate does this
    // today; Phase 4.7.4 makes that the only entry path).

    const uint8_t rootMask = root.dirtyBits() | root.descendantDirty();
    if(rootMask == 0){
        // Nothing to do — the tree is clean. Return without
        // submitting an empty DisplayList so `endFrame` does not
        // push a no-op slice into the window compositeFrame.
        return;
    }

    const auto rootRect = root.getRect();

    if((rootMask & View::Style) != 0){
        ScopedPhase stylePhase(this, FramePhase::Style);
        styleSubtree(root);
        // Native-Theme-Application-Plan Tier 2 (2026-07-01): with the
        // root's styles freshly resolved, recompute the window surface
        // (clear) color per the §3 priority chain and stash it on the
        // window for the compositor to read at frame open. Gated on the
        // Style bit — a theme change forces it dirty via
        // applyCascadeChange, so the clear tracks OS light/dark.
        if(auto * app = AppInst::inst(); app != nullptr){
            window_.setSurfaceColor(app->resolveWindowSurfaceColor(window_));
        }
    }

    if((rootMask & View::Layout) != 0){
        ScopedPhase layoutPhase(this, FramePhase::Layout);
        layoutSubtree(root, rootRect);
    }

    // Paint pass — one window-wide DisplayList. UIView::paint bakes
    // `pc.offset` into every emitted rect, so the DL is already in
    // absolute window coords; the flush submits with
    // `windowOffset == {0,0}` and the GPU viewport is the whole window.
    // Paint walker visits the whole tree (no subtree pruning at the
    // node level — region-based dirty culling is Tier 5).
    Composition::DisplayList dl{};
    Composition::PaintContext pc{dl};
    pc.offset.x = rootRect.pos.x;
    pc.offset.y = rootRect.pos.y;
    {
        ScopedPhase paintPhase(this, FramePhase::Paint);
#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
        // UIView-Render-Redesign-Plan §G.3.2-rev2 (2026-06-09): per-View
        // view-sized cache. `paintSubtreeWithCache` wraps each eligible
        // View's own paint in a Begin/End marker pair (view-sized
        // capture on the backend); a hover that changes one View
        // re-captures only that View while siblings blit from cache.
        //
        // First-presented-frame warmup guard: on a brand-new window the
        // native swap chain hasn't settled, and the begin/scratch/resume
        // render-pass cycle the capture uses can produce a blank first
        // frame on Metal. Skip the cache until the first frame is on
        // screen (direct render), engage from the second presented frame.
        //
        // The gate is per-PRESENTED-frame (`firstFramePresented_`), NOT
        // per-`buildFrame`: a single presented frame can be composed of
        // several `buildFrame` calls (root + overlays in `paintDirty`,
        // and the startup initial-paint + realize/resize repaint), all
        // accumulated into one `compositeFrame_`. A per-`buildFrame`
        // counter flipped mid-frame and let that frame carry both a direct
        // slice and an all-miss cache slice of the same content — a full
        // opaque double-composite on the initial frame. `endFrame` flips
        // this flag only after a real deposit, so every slice in the first
        // presented frame takes the same (direct) branch here.
        if(firstFramePresented_){
            const auto & cfg = Composition::ContentCacheConfig::inst();
            // §G.5.4: a live resize drag tags each cached View's marker so the
            // backend can stretch its prior texture instead of re-rendering.
            auto * tHost = window_.impl_->widgetTreeHost.get();
            const bool dragActive = tHost != nullptr && tHost->isResizing();
            paintSubtreeWithCache(root, pc, animationScheduler(), cfg.cacheMinSizePx, dragActive);
        }
        else {
            paintSubtree(root, pc);
        }
#else
        paintSubtree(root, pc);
#endif
    }

    PendingSubmission sub;
    sub.windowOffset = {0.f, 0.f};
    sub.list         = std::move(dl);
    pending_.push_back(std::move(sub));

    clearDirtySubtree(root);

    // G.3.2 first-paint guard: the warmup flag (`firstFramePresented_`)
    // is flipped in `endFrame` on a real deposit — NOT here — so the gate
    // is per-presented-frame, not per-`buildFrame`. See the eligibility
    // check above and the `firstFramePresented_` doc in FrameBuilder.h.
}

// Phase 4.7.5: `submitView`, the offset-accumulator API
// (`currentOffset` / `pushOffset` / `popOffset` / `ScopedViewOffset`),
// and the legacy `View::computeWindowOffset` /
// `legacyComputeWindowOffset` paths are gone. `buildFrame` writes
// directly into `pending_` itself, and threads `PaintContext.offset`
// through its own walker. The replay path in `endFrame` is unchanged
// — it still drains `pending_` and submits each entry's DisplayList
// via the proxy.

void FrameBuilder::submitOverlayShadow(
    const Composition::LayerEffect::DropShadowParams & shadow,
    const Composition::Rect & shapeRect,
    float cornerRadius){
    // Overlay-Z-Order-Plan O2.1: one-op overlay-chrome submission.
    // No-op outside of `beginFrame`/`endFrame` brackets — pending_
    // is drained at endFrame, so submissions outside that window
    // would never reach the compositor anyway and would silently
    // accumulate. Matching the dirty-bit-gated buildFrame
    // semantics of "no slice if there's nothing to render," but
    // here gated on lifecycle rather than mask: callers (today
    // only `WidgetTreeHost::paintDirty`) are expected to be inside
    // a `ScopedFrame`.
    if(depth_ == 0){
        return;
    }
    Composition::DisplayList dl{};
    dl.append(Composition::DrawOp{shadow, shapeRect, cornerRadius,
                                  /*isEllipse=*/false});
    PendingSubmission sub;
    sub.windowOffset = {0.f, 0.f};
    sub.list         = std::move(dl);
    pending_.push_back(std::move(sub));
}

// Phase 3.2: static accessor lives on AppWindow but reads the
// FrameBuilder-internal slot, so the AppWindow header does not have
// to expose the static storage. Defined here for the same reason
// the slot lives here.
AnimationScheduler * FrameBuilder::animationScheduler() const {
    // Phase 4.4: animation callers reach the per-window scheduler through
    // the FrameBuilder. The fields live on AppWindow::Impl (Phase 4.3),
    // alongside FrameBuilder itself; this accessor is the single hop.
    return window_.impl_->animationScheduler_.get();
}

FrameBuilder * AppWindow::activeFrameBuilder(){
    return g_activeFrameBuilder;
}

} // namespace OmegaWTK
