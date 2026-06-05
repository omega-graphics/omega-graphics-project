#include "ViewImpl.h"

#include "omegaWTK/UI/Layout.h"
#include "omegaWTK/UI/LayoutManager.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/Composition/DisplayList.h"   // Phase 4.7.0: PaintContext

#include <iostream>
#include <utility>

#include "omegaWTK/Composition/CompositorClient.h"
#include "../Composition/Compositor.h"
#include "../Composition/backend/ResourceFactory.h"
#include "AnimationScheduler.h"
#include "FrameBuilder.h"

namespace OmegaWTK {

Composition::CompositorClientProxy & View::compositorProxy(){
    return impl_->proxy;
}

const Composition::CompositorClientProxy & View::compositorProxy() const{
    return impl_->proxy;
}

SharedHandle<Composition::ViewRenderTarget> & View::renderTargetHandle(){
    return impl_->renderTarget;
}

const SharedHandle<Composition::ViewRenderTarget> & View::renderTargetHandle() const{
    return impl_->renderTarget;
}

Composition::Rect & View::getRect(){
    return impl_->rect;
}

// Phase 4.8: `getLayerTree()` deleted alongside `Impl::ownLayerTree`.
// The window-level `AppWindow::Impl::windowLayerTree_` is the single
// tree; UIView DisplayLists flow into it via
// `FrameBuilder::buildFrame` → `endFrame` → `proxy.submitDisplayList`.

void View::markDirty(uint8_t bits){
    // Phase 4.7.3: self mask gets `bits`, ancestor chain gets `bits`
    // OR-ed into their descendant-dirty masks so the root mask is the
    // union of every dirty bit in the subtree. `FrameBuilder::buildFrame`
    // gates per-pass execution off `root.dirtyBits() |
    // root.descendantDirty()` (run the pass), and prunes subtree
    // descents off `node.dirtyBits() | node.descendantDirty()` &
    // passBit (skip whole subtrees with no dirty bit in their subtree).
    impl_->dirtyBits_ |= bits;
    auto * ancestor = impl_->parent_ptr;
    while(ancestor != nullptr){
        ancestor->impl_->descendantDirty_ |= bits;
        ancestor = ancestor->impl_->parent_ptr;
    }
}

uint8_t View::dirtyBits() const{
    return impl_->dirtyBits_;
}

uint8_t View::descendantDirty() const{
    return impl_->descendantDirty_;
}

void View::clearDirtyBits(){
    // Phase 4.7.3: clear BOTH masks. The propagated descendant mask
    // is invalidated when this node's own bits are cleared — a parent
    // that walked here only because a descendant was dirty needs its
    // own propagated mask cleared by the frame-end walker so the next
    // frame's gating starts fresh. The walker calls `clearDirtyBits`
    // on every visited node, so by the end of a frame's clear pass
    // both masks are zero across the tree.
    impl_->dirtyBits_       = 0;
    impl_->descendantDirty_ = 0;
}

bool View::isRootView(){
    return impl_->parent_ptr == nullptr;
}

std::uint64_t View::nodeId() const{
    // Phase 4.4: returned as a plain uint64_t so the public header stays
    // independent of the private AnimationScheduler header. Allocated
    // once at View construction; never reused.
    return impl_->nodeId_;
}

LayoutManager * View::layoutManager() const{
    // Phase 4.5: null = use the process-wide default. The manager
    // owner (Container, etc.) sets this explicitly via
    // setLayoutManager.
    if(impl_->layoutManager_ != nullptr){
        return impl_->layoutManager_;
    }
    return &AbsoluteLayout::instance();
}

void View::setLayoutManager(LayoutManager * manager){
    impl_->layoutManager_ = manager;
    markDirty(View::Layout);
}

OmegaCommon::ArrayRef<View *> View::subviews() const{
    return OmegaCommon::ArrayRef<View *>(impl_->subviews);
}

bool View::containsPoint(const Composition::Point2D &point) const{
    const auto &r = impl_->rect;
    return point.x >= r.pos.x && point.x < r.pos.x + r.w &&
           point.y >= r.pos.y && point.y < r.pos.y + r.h;
}


View::View(const Composition::Rect & rect,ViewPtr parent):
    impl_(std::make_unique<Impl>(
            *this,
            ViewInternal::sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f}),
            parent.get())){
    // Phase 3: View is purely virtual. No NativeItem, no per-View render
    // target, no pre-created visual resources. The window's render target
    // is propagated via setWindowRenderTarget() when the widget tree
    // attaches to an AppWindow.
    if(impl_->parent_ptr != nullptr) {
        parent->addSubView(this);
    }
}

bool View::hasDelegate(){
    return impl_->delegate != nullptr;
}

void View::setDelegate(ViewDelegate *_delegate){
    impl_->delegate = _delegate;
    impl_->delegate->view = this;
    setReciever(impl_->delegate);
}

void View::addSubView(View * view){
    if(view == nullptr){
        return;
    }
    for(auto *existing : impl_->subviews){
        if(existing == view){
            return;
        }
    }
    impl_->subviews.emplace_back(view);
    view->impl_->parent_ptr = this;
    // Phase 4.5: per-child registration on `ViewResizeCoordinator` is
    // gone. The parent's `LayoutManager` (default `AbsoluteLayout`)
    // discovers children through `node.subviews()` at arrange time;
    // no separate register/unregister step is needed any more.
    // Newly created subviews must inherit compositor wiring immediately.
    view->setFrontendRecurse(compositorProxy().getFrontendPtr());
    view->setSyncLaneRecurse(compositorProxy().getSyncLaneId());
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // inherit the parent's current dirty bits onto the new child.
    // Without this, a sub-UIView created via `Widget::makeSubView`
    // after `Widget::init()` already marked the parent's own view
    // dirty starts with `dirtyBits_ = 0`. The pre-order
    // Style / Layout / Paint walkers in `FrameBuilder::buildFrame`
    // gate descent on `(self|desc) & passBit`, so the new child
    // would be skipped entirely on the first frame — its sheet
    // cells never get written, its layout never resolves, and
    // Paint reads back the UA defaults until the next mutation
    // dirties it again. Inheriting the parent's bits makes the
    // child immediately participate in whatever passes the parent
    // is dirty for, removing the need for app code to call
    // `uiView->update()` after `makeSubView`. Pre-D8 every widget
    // that hosted a sub-UIView papered over this with an explicit
    // `update()` call — a rediscover-every-time bug class.
    if(impl_->dirtyBits_ != 0){
        view->markDirty(impl_->dirtyBits_);
    }
}

void View::removeSubView(View *view){
    auto it  = impl_->subviews.begin();
    while(it != impl_->subviews.end()){
        auto *v = *it;
        if(v == view){
            impl_->subviews.erase(it);
            // Phase 4.5: see addSubView — no coordinator unregister.
            view->impl_->parent_ptr = nullptr;
            return;
        }
        ++it;
    }
}

void View::resize(Composition::Rect newRect){
    auto sanitized = ViewInternal::sanitizeRect(newRect,impl_->rect);
    if(ViewInternal::sameRect(impl_->rect,sanitized)){
        return;
    }
    impl_->rect = sanitized;
    // Phase 4.8: the per-view `ownLayerTree` is gone. The window-level
    // tree's root layer is sized by the window, not by per-view
    // resize; the View's own rect change just emits the
    // `onLayoutResolved` signal (below) for `NativeViewHost` and any
    // other subscriber to pick up.
    // Phase 2.5: emit on the new rect *after* it's been committed and
    // the layer tree caught up, so subscribers (NativeViewHost et al)
    // observe a consistent post-resize state.
    onLayoutResolved.emit(impl_->rect);
}


// Phase 4.7.5: `startCompositionSession` / `endCompositionSession` are
// deleted. The window-level FrameBuilder owns the session via
// `ScopedFrame`; the compositor proxy is propagated at `addSubView`
// time (View.Core.cpp:115-133) so no per-view session-open dance is
// needed any more.

void View::enable() {
    impl_->enabled_ = true;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.4 (2026-06-03):
    // clear the Disabled pseudo-class bit so the cascade re-resolves
    // without the `:disabled` rules. PseudoClass::Disabled == 0x08.
    setPseudoClassBits(0x08U, false);
}

void View::disable() {
    impl_->enabled_ = false;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.4 (2026-06-03):
    // set the Disabled pseudo-class bit. PseudoClass::Disabled == 0x08.
    setPseudoClassBits(0x08U, true);
}

std::uint8_t View::pseudoClassBits() const {
    return impl_->pseudoClassBits_;
}

void View::setPseudoClassBits(std::uint8_t mask, bool on){
    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.4 (2026-06-03):
    // bit-set / bit-clear with change detection so the Style dirty
    // bit only fires when the cascade input actually changes. The
    // resolver re-runs from `UIView::resolveStyles()` which is gated
    // by `DirtyBit::Style` upstream; without the change check, a
    // hover-stable mouse-move would mark every frame as dirty.
    const auto before = impl_->pseudoClassBits_;
    auto after = before;
    if(on){
        after |= mask;
    }
    else {
        after = static_cast<std::uint8_t>(after & ~mask);
    }
    if(after == before){
        return;
    }
    impl_->pseudoClassBits_ = after;
    markDirty(View::Style);
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
// `:state(name)` custom-state surface. Mirrors the `setPseudoClassBits`
// change-detection pattern so a re-application of the same state does
// NOT dirty the cascade. The resolver's `selectorMatches` consults
// `hasState(name)` per name in the selector's `customStates` vector
// during cascade resolution.
//
// Layering note: `setState` / `clearState` ONLY record the bit. The
// View does not unilaterally request a frame — multiple views may
// flip state in the same idle batch, and the AppWindow owns the run-
// loop turn (one paint per batch, not one per mutation). Idle-context
// callers (menu callbacks, timers, deferred async results) finish
// their batch and then call `AppWindow::refresh()` exactly once to
// schedule the next paint. The native-window request coalesces, so
// over-calling `refresh()` is harmless; under-calling leaves the
// dirty bit parked until something else drives a frame.

void View::setState(const OmegaCommon::String & name){
    auto & set = impl_->customStates_;
    if(set.insert(name).second){
        markDirty(View::Style);
    }
}

void View::clearState(const OmegaCommon::String & name){
    auto & set = impl_->customStates_;
    if(set.erase(name) > 0){
        markDirty(View::Style);
    }
}

void View::setState(const OmegaCommon::String & name, bool on){
    if(on){
        setState(name);
    }
    else {
        clearState(name);
    }
}

bool View::hasState(const OmegaCommon::String & name) const {
    return impl_->customStates_.count(name) > 0;
}

void View::applyLayoutDelta(const LayoutDelta & delta,
                            const LayoutTransitionSpec & spec){
    // Phase 4.4 (Anim Tier B): the per-View layout tween. Pre-4.4 this
    // queued a `Composition::ViewAnimator::resizeTransition`; that path is
    // now dormant — the AnimationScheduler holds the four scalar tracks
    // (LayoutX/Y/Width/Height, all `layoutAffecting`) in its side table.
    //
    // 4.7 seam: until the Layout phase reads `scheduler.value<float>(
    // nodeId(), LayoutX/Y/Width/Height)` and writes the View's rect, the
    // rect stays at `from` for the animation's lifetime. There is no
    // caller of this method in the tree today; the side table is correct
    // and the read-back is the only missing link.
    if(!spec.enabled || spec.durationSec <= 0.f || delta.changedProperties.empty()){
        resize(delta.toRectPx);
        return;
    }

    auto * fb = AppWindow::activeFrameBuilder();
    auto * scheduler = (fb != nullptr) ? fb->animationScheduler() : nullptr;
    if(scheduler == nullptr){
        // No window scheduler reachable from this call site (no AppWindow
        // frame in flight). Commit directly — matches the pre-4.4
        // short-circuit on the `durationSec <= 0` path.
        resize(delta.toRectPx);
        return;
    }

    const auto & from = delta.fromRectPx;
    const auto & to   = delta.toRectPx;
    if(from.pos.x == to.pos.x && from.pos.y == to.pos.y &&
       from.w     == to.w     && from.h     == to.h){
        resize(delta.toRectPx);
        return;
    }

    Composition::TimingOptions timing {};
    timing.durationMs = static_cast<std::uint32_t>(spec.durationSec * 1000.f);
    if(timing.durationMs == 0){
        resize(delta.toRectPx);
        return;
    }
    auto curve = spec.curve;
    if(curve == nullptr){
        curve = Composition::AnimationCurve::Linear();
    }

    const auto node = nodeId();
    if(from.pos.x != to.pos.x){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutX,
                                        from.pos.x, to.pos.x, timing, curve);
    }
    if(from.pos.y != to.pos.y){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutY,
                                        from.pos.y, to.pos.y, timing, curve);
    }
    if(from.w != to.w){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutWidth,
                                        from.w, to.w, timing, curve);
    }
    if(from.h != to.h){
        scheduler->tweenProperty<float>(node, PropertyKey::LayoutHeight,
                                        from.h, to.h, timing, curve);
    }
}

void View::setWindowRenderTarget(SharedHandle<Composition::ViewRenderTarget> windowRT){
    impl_->renderTarget = std::move(windowRT);
    compositorProxy().setRenderTarget(
            std::static_pointer_cast<Composition::CompositionRenderTarget>(impl_->renderTarget));
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setWindowRenderTarget(impl_->renderTarget);
        }
    }
}

float View::getRenderScale() const {
    if(impl_ != nullptr && impl_->renderTarget != nullptr){
        return impl_->renderTarget->getRenderScale();
    }
    return 1.f;
}

Composition::Point2D View::offsetFromRoot() const{
    // Phase 4.7.5: parent-chain walk that sums each ancestor's
    // `rect.pos` plus the parent's `contentOffset()`. The
    // `contentOffset()` fold (Tier 3 Phase 3.6) carries
    // ScrollView's negative scroll offset down to descendants. Only
    // in-tree caller is `NativeViewHost::syncBounds`, which positions
    // an embedded native item against the window root from
    // `onLayoutResolved`; the production paint walk does not call
    // this — it reads `PaintContext.offset` instead.
    Composition::Point2D offset {0.f, 0.f};
    const View *v = this;
    while(v != nullptr){
        offset.x += v->impl_->rect.pos.x;
        offset.y += v->impl_->rect.pos.y;
        if(v->impl_->parent_ptr != nullptr){
            auto co = v->impl_->parent_ptr->contentOffset();
            offset.x += co.x;
            offset.y += co.y;
        }
        v = v->impl_->parent_ptr;
    }
    return offset;
}

// Phase 4.7.5: `computeWindowOffset` / `legacyComputeWindowOffset` /
// `scrollOffsetContribution` are deleted. The paint walker owns the
// absolute-position math via `PaintContext.offset`; `contentOffset()`
// (below) is what ScrollView overrides for the descent walk.

Composition::Point2D View::contentOffset() const{
    return {0.f, 0.f};
}

bool View::wantsLayer() const{
    return false;
}

void View::paint(Composition::PaintContext & pc){
    // Phase 4.7.0 / 4.7.1: `paint(pc)` is *per-node* — it emits this
    // view's own draw ops into `pc.displayList` and does NOT recurse.
    // Subtree traversal lives in `FrameBuilder::buildFrame`'s walker
    // (4.7.1), which calls `paint(pc)` once per node and manages
    // `pc.offset` across the descent. Base `View` is a pass-through
    // node and contributes nothing — the default body is a no-op.
    // Subclasses that emit ops (`UIView`, `SVGView`, `ScrollView` for
    // its PushClip) override without calling base; the walker visits
    // their subviews on its own.
    (void)pc;
}

void View::resolveStyles(){
    // Phase 4.7.2: base `View` has no style cache to populate —
    // default is a no-op. `UIView::resolveStyles` overrides to write
    // the per-property `styleTable_` (Tier D / D5, 2026-06-03 — pre-D5
    // this was the `resolvedViewStyle_` + `computedStyles_` aggregate
    // cache) that the Paint pass reads through `Impl::resolved<T>`.
}

void View::arrangeContent(){
    // Phase 4.7.2: base `View` has no intra-node elements — default
    // is a no-op. `UIView::arrange` overrides to resolve element rects
    // into `impl_->arranged_` from `UIViewLayoutV2`.
}

bool View::isEnabled() const{
    return impl_->enabled_;
}

View::~View(){
    // Phase 3: per-View PreCreatedVisualTreeData removed.
    // The window's visual tree is managed by AppWindow::Impl.
    std::cout << "View will destruct" << std::endl;
}

void View::setFrontendRecurse(Composition::Compositor *frontend){
    // Phase 4.8: per-view compositor observation is gone — the
    // window-level `windowLayerTree_` is observed at
    // `displayRootWindow` (AppWindow.cpp:151-155) instead, which is
    // the only tree the post-4.7 paint pipeline still uses. This
    // method now just propagates the frontend pointer down the View
    // subtree (so any newly-attached subview inherits its window's
    // compositor frontend).
    compositorProxy().setFrontendPtr(frontend);
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setFrontendRecurse(frontend);
        }
    }
}

void View::setSyncLaneRecurse(uint64_t syncLaneId){
    // Tier 1 (UIView-Render-Redesign-Plan §4): all Views in a window share
    // one sync lane. WidgetTreeHost owns the lane; this method propagates
    // it down the View subtree without re-allocation.
    compositorProxy().setSyncLaneId(syncLaneId);
    for(auto *subView : impl_->subviews){
        if(subView != nullptr){
            subView->setSyncLaneRecurse(syncLaneId);
        }
    }
}

ViewDelegate::ViewDelegate(){};

void ViewDelegate::setForwardDelegate(ViewDelegate *delegate){
    forwardDelegate = delegate;
}

ViewDelegate::~ViewDelegate(){};

void ViewDelegate::onRecieveEvent(Native::NativeEventPtr event){
    using Native::NativeEvent;
    switch (event->type) {
//        case NativeEvent::HasLoaded : {
//            viewHasLoaded(event);
//            break;
//        }
        case NativeEvent::CursorEnter : {
            onMouseEnter(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onMouseEnter(event);
            }
            break;
        }
        case NativeEvent::CursorExit : {
            onMouseExit(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onMouseExit(event);
            }
            break;
        }
        case NativeEvent::LMouseDown : {
            onLeftMouseDown(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onLeftMouseDown(event);
            }
            break;
        }
        case NativeEvent::LMouseUp : {
            onLeftMouseUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onLeftMouseUp(event);
            }
            break;
        }
        case NativeEvent::RMouseDown : {
            onRightMouseDown(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onRightMouseDown(event);
            }
            break;
        }
        case NativeEvent::RMouseUp : {
            onRightMouseUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onRightMouseUp(event);
            }
            break;
        };
        case NativeEvent::KeyDown : {
            onKeyDown(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onKeyDown(event);
            }
            break;
        };
        case NativeEvent::KeyUp : {
            onKeyUp(event);
            if(forwardDelegate != nullptr){
                forwardDelegate->onKeyUp(event);
            }
            break;
        }
        case Native::NativeEvent::ViewResize : {
            auto params = (Native::ViewResize *)event->params;
            if(params != nullptr){
                auto sanitized = ViewInternal::sanitizeRect(params->rect,view->getRect());
                view->getRect() = sanitized;
            }
            break;
        }

        default:
            break;
    }
}

}
