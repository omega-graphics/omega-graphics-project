#include "FrameBuilder.h"

#include "AppWindowImpl.h"
#include "WidgetTreeHost.h"

#include <cassert>

#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"

namespace OmegaWTK {

namespace {
// Phase 3.2: AppWindow::activeFrameBuilder() reads this; FrameBuilder
// sets it during the outermost beginFrame/endFrame so UIView::update
// (and SVGView in Phase 3.3) can route their DisplayList submissions
// to the right FrameBuilder without holding a back-pointer to
// AppWindow. Paint runs on the UI thread; a single static is
// sufficient. If paint ever multi-threads, this becomes thread_local.
FrameBuilder * g_activeFrameBuilder = nullptr;
}

FrameBuilder::FrameBuilder(AppWindow & window): window_(window) {}

FrameBuilder::~FrameBuilder() = default;

void FrameBuilder::beginFrame(){
    if(depth_++ > 0){
        return;
    }

    auto * impl = window_.impl_.get();
    if(impl == nullptr){
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

    pending_.clear();
    // Phase 3.4: defensive — the accumulator should be empty at this
    // point (every ScopedViewOffset push is RAII-paired with a pop),
    // but an exception escaping a paint could leak entries from the
    // previous frame. Clear so currentOffset()'s "empty == fall back
    // to legacy" contract starts fresh.
    offsetStack_.clear();

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
    if(compositeFrame_ != nullptr &&
       !compositeFrame_->slices.empty() &&
       impl->windowSurface != nullptr){
        impl->windowSurface->deposit(compositeFrame_);
    }
    compositeFrame_.reset();

    g_activeFrameBuilder = nullptr;
}

void FrameBuilder::submitView(View * view, Composition::DisplayList list){
    if(view == nullptr){
        return;
    }
#ifndef NDEBUG
    // Tier 3 Phase 3.5: enforce balanced PushClip / PopClip pairs
    // per submission. Imbalance leaks scissor state across views in
    // the same window-scoped frame (the window canvas is shared);
    // catching it here is much easier than tracing a missing pop
    // from a misrendered subsequent view. Release builds skip the
    // walk — Canvas::popClip on an empty stack is a no-op, and
    // Canvas::nextFrame defensively clears its clip stack at frame
    // boundaries.
    int clipDepth = 0;
    for(const auto & op : list.ops()){
        if(op.type == Composition::DrawOp::PushClip) ++clipDepth;
        else if(op.type == Composition::DrawOp::PopClip) --clipDepth;
    }
    assert(clipDepth == 0 &&
           "FrameBuilder::submitView: DisplayList has unbalanced "
           "PushClip / PopClip pairs. Each PushClip must have a "
           "matching PopClip before submission.");
#endif
    PendingSubmission sub;
    // Phase 3.4: View::computeWindowOffset is now a wrapper that
    // returns currentOffset() while a FrameBuilder is active. The
    // caller (UIView::update / SVGView::paint) is expected to have
    // pushed `view`'s absolute offset onto the stack via
    // ScopedViewOffset just before this submitView call, so the
    // wrapper sees the leaf view's offset, not the enclosing
    // widget's.
    sub.windowOffset = view->computeWindowOffset();
    sub.list = std::move(list);
    pending_.push_back(std::move(sub));
}

// ---------------------------------------------------------------------------
// Phase 3.4: window-offset accumulator.
// ---------------------------------------------------------------------------

Composition::Point2D FrameBuilder::currentOffset() const {
    if(offsetStack_.empty()){
        return {0.f, 0.f};
    }
    return offsetStack_.back();
}

void FrameBuilder::pushOffset(Composition::Point2D absolute){
    offsetStack_.push_back(absolute);
}

void FrameBuilder::popOffset(){
    if(!offsetStack_.empty()){
        offsetStack_.pop_back();
    }
}

FrameBuilder::ScopedViewOffset::ScopedViewOffset(FrameBuilder * f, View * v): fb(f) {
    if(fb == nullptr){
        return;
    }
    // Compute the view's absolute window offset via the legacy
    // parent-chain walk and push that. The push pays O(depth) once
    // per push; reads through `View::computeWindowOffset` while the
    // push is live then cost O(1) — which is the point of the
    // accumulator: submitView et al. become O(1) lookups instead of
    // re-walking. Could in principle be optimized to
    // `currentOffset() + (view.rect.pos - parent.scrollContrib)`
    // when the stack already contains the immediate parent, but
    // that invariant only holds for direct widget-tree children —
    // sub-views nested inside another view (e.g. Phase32Widget's
    // innerView_ under leftView_) aren't visited by the widget
    // walker, so their parents wouldn't be on the stack. Using the
    // full walk keeps the push correct regardless of how the scene
    // composes views and widgets.
    Composition::Point2D abs = v != nullptr
        ? v->legacyComputeWindowOffset()
        : fb->currentOffset();
    fb->pushOffset(abs);
}

FrameBuilder::ScopedViewOffset::~ScopedViewOffset(){
    if(fb != nullptr){
        fb->popOffset();
    }
}

// Phase 3.2: static accessor lives on AppWindow but reads the
// FrameBuilder-internal slot, so the AppWindow header does not have
// to expose the static storage. Defined here for the same reason
// the slot lives here.
FrameBuilder * AppWindow::activeFrameBuilder(){
    return g_activeFrameBuilder;
}

} // namespace OmegaWTK
