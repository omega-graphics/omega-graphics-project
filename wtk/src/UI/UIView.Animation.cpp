#include "UIViewImpl.h"
#include "AnimationScheduler.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/AppWindow.h"

namespace OmegaWTK {

namespace {

constexpr const char *kUIViewRootEffectTag = "__UIViewRootEffectTarget__";

// Phase 4.4 (Anim Tier C): map a UIView per-element `int` animation key
// (the `ElementAnimationKey` enum and the `EffectAnimationKey*` constants
// on `UIView::Impl`) into a `PropertyKey` slot on the AnimationScheduler.
// Every UIView-internal channel rides the `UserDefined` half of the
// scheduler's key space, so the legacy "lerp every R/G/B/A independently
// per element" semantics carry through unchanged. Built-in scheduler
// keys (LayoutX/Y/Width/Height, PathNodeX/Y) keep their meanings and are
// not routed through this helper.
PropertyKey elementKeyToProperty(int key){
    return static_cast<PropertyKey>(
        static_cast<std::uint16_t>(PropertyKey::UserDefined) +
        static_cast<std::uint16_t>(key));
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
// path-node axis ints (ElementAnimationKeyPathNodeX/Y) → the built-in
// `PathNodeX/Y` PropertyKey slots on the scheduler. Built-ins, NOT
// the UserDefined range, so the (NodeId, PropertyKey, subIndex)
// table cell aligns with the cross-property surface the scheduler
// already publishes.
PropertyKey pathNodeAxisToProperty(int axisKey){
    return (axisKey == ElementAnimationKeyPathNodeY)
        ? PropertyKey::PathNodeY
        : PropertyKey::PathNodeX;
}

// Pack `(axisKey, nodeIndex)` into a single 64-bit slot for the
// path-node to-match short-circuit map. `axisKey` is small (0–7) but
// nodeIndex is unbounded and may legitimately be int-negative for an
// "unset" sentinel — clamp the negative case to 0 so it lands in the
// dead zone (no animation registers when nodeIndex < 0 anyway, per
// the runtime guard in startOrUpdatePathNodeAnimation).
std::uint64_t packPathNodeKey(int axisKey,int nodeIndex){
    const auto axisBits = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(axisKey));
    const auto nodeBits = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(nodeIndex < 0 ? 0 : nodeIndex));
    return (axisBits << 32) | nodeBits;
}

AnimationScheduler * activeScheduler(){
    auto * fb = AppWindow::activeFrameBuilder();
    return (fb != nullptr) ? fb->animationScheduler() : nullptr;
}

// Phase 4.8: `clamp01` + `applyShapeDimension` helpers were only used
// by `applyAnimatedColor` / `applyAnimatedShape` — both deleted above
// (orphans, never read by `UIView::paint`). The helpers go with them.

}

void UIView::Impl::markAllElementsDirty(){
    for(auto & entry : elementDirtyState){
        entry.second.layoutDirty = true;
        entry.second.styleDirty = true;
        entry.second.contentDirty = true;
        entry.second.orderDirty = true;
        entry.second.visibilityDirty = true;
    }
}

void UIView::Impl::markElementDirty(const UIElementTag &tag,
                                    bool layout,
                                    bool style,
                                    bool content,
                                    bool order,
                                    bool visibility){
    auto & dirty = elementDirtyState[tag];
    dirty.layoutDirty = dirty.layoutDirty || layout;
    dirty.styleDirty = dirty.styleDirty || style;
    dirty.contentDirty = dirty.contentDirty || content;
    dirty.orderDirty = dirty.orderDirty || order;
    dirty.visibilityDirty = dirty.visibilityDirty || visibility;
}

bool UIView::Impl::isElementDirty(const UIElementTag &tag) const{
    auto it = elementDirtyState.find(tag);
    if(it == elementDirtyState.end()){
        return true;
    }
    const auto & dirty = it->second;
    return dirty.layoutDirty ||
           dirty.styleDirty ||
           dirty.contentDirty ||
           dirty.orderDirty ||
           dirty.visibilityDirty;
}

void UIView::Impl::clearElementDirty(const UIElementTag &tag){
    auto it = elementDirtyState.find(tag);
    if(it == elementDirtyState.end()){
        return;
    }
    it->second.layoutDirty = false;
    it->second.styleDirty = false;
    it->second.contentDirty = false;
    it->second.orderDirty = false;
    it->second.visibilityDirty = false;
}

// Phase 4.8: `ensureAnimationViewAnimator` / `ensureAnimationLayerAnimator`
// / `beginCompositionClock` deleted. They spawned `ViewAnimator` /
// `LayerAnimator` instances against the per-view `LayerTree` — both
// classes and the per-view tree are gone now. The live animation pipe
// runs `startOrUpdateAnimation` → `AnimationScheduler` directly.

void UIView::Impl::startOrUpdateAnimation(const UIElementTag &tag,
                                          int key,
                                          float from,
                                          float to,
                                          float durationSec,
                                          SharedHandle<Composition::AnimationCurve> curve){
    // Phase 4.4 (Anim Tier C): per-element scalar tween. Routes through
    // the per-window AnimationScheduler — the legacy per-tag
    // `elementAnimations` / `pathNodeAnimations` machinery, the
    // `ViewAnimator` / `LayerAnimator` pair, and the per-tween
    // `beginCompositionClock` clock are all dormant after this phase
    // (4.8 deletes them). The `(tag, key)` short-circuit ("same target
    // → no restart") is preserved via the side `animationTargets_`
    // bookkeeping — the scheduler replaces on every re-registration
    // (Anim §6 Q3), so we have to suppress repeat calls ourselves.
    if(!std::isfinite(from) || !std::isfinite(to)){
        return;
    }

    auto * scheduler = activeScheduler();
    if(scheduler == nullptr){
        // No window scheduler reachable. Drop on the floor — pre-4.4
        // this would still have queued LayerAnimator work the now-dead
        // composition path would never run; the new path bails the
        // same way `View::applyLayoutDelta` does in the same situation.
        return;
    }

    const auto node = ensureElementNodeId(tag);
    const auto propKey = elementKeyToProperty(key);

    if(durationSec <= 0.f || std::fabs(to - from) <= 0.0001f){
        // Cancel-equivalent: a zero-duration tween completes inside the
        // next tick (see `AnimationScheduler::tick`: `durNs == 0` →
        // `finished = true`), which erases the side-table cell. Reads
        // then fall through to the resolved style. Local target tracking
        // is dropped so the next real call won't short-circuit.
        auto tagIt = animationTargets_.find(tag);
        if(tagIt != animationTargets_.end()){
            tagIt->second.erase(key);
            if(tagIt->second.empty()){
                animationTargets_.erase(tagIt);
            }
        }
        Composition::TimingOptions cancelTiming {};
        cancelTiming.durationMs = 0;
        scheduler->tweenProperty<float>(node, propKey, to, to, cancelTiming,
                                        Composition::AnimationCurve::Linear());
        return;
    }

    auto & propertyMap = animationTargets_[tag];
    auto existing = propertyMap.find(key);
    if(existing != propertyMap.end() && std::fabs(existing->second - to) <= 0.0001f){
        return;
    }
    propertyMap[key] = to;

    auto effectiveCurve = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    Composition::TimingOptions timing {};
    timing.durationMs = static_cast<std::uint32_t>(std::max(0.001f, durationSec) * 1000.f);

    scheduler->tweenProperty<float>(node, propKey, from, to, timing, effectiveCurve);

    if(tag != kUIViewRootEffectTag){
        markElementDirty(tag,false,false,true,false,false);
    }
    styleDirty = true;
}

void UIView::animateElement(const UIElementTag & tag,
                            AnimationChannel channel,
                            float from,
                            float to,
                            float durationSec,
                            SharedHandle<Composition::AnimationCurve> curve){
    // Phase 4.4 public test/runtime entry — thin wrapper that forwards
    // to the private `Impl::startOrUpdateAnimation`. The enum values are
    // already aligned with the `EffectAnimationKey*` constants UIView's
    // paint code reads through `animatedValue`, so no remap is needed.
    impl_->startOrUpdateAnimation(tag, static_cast<int>(channel),
                                  from, to, durationSec, curve);
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
// per-axis path-node tween. Mirrors `startOrUpdateAnimation` but
// dispatches to `tweenPropertyAt` so the scheduler side-table cell
// is keyed by `(node, PathNodeX/Y, subIndex=nodeIndex)` instead of
// collapsing every node's X/Y onto a single cell. Maintains its own
// to-match short-circuit via `pathNodeAnimationTargets_`, packed by
// `(axisKey, nodeIndex)` so two different node indices on the same
// element animate independently.
void UIView::Impl::startOrUpdatePathNodeAnimation(const UIElementTag &tag,
                                                  int axisKey,
                                                  int nodeIndex,
                                                  float from,
                                                  float to,
                                                  float durationSec,
                                                  SharedHandle<Composition::AnimationCurve> curve){
    if(!std::isfinite(from) || !std::isfinite(to)){
        return;
    }
    if(nodeIndex < 0){
        // Sentinel "no node" — caller hasn't fixed a target node yet.
        // Drop on the floor (same shape as the scalar guard against
        // non-finite values).
        return;
    }

    auto * scheduler = activeScheduler();
    if(scheduler == nullptr){
        // No window scheduler reachable (matches the
        // `startOrUpdateAnimation` and `View::applyLayoutDelta` bail
        // paths in this same situation).
        return;
    }

    const auto node = ensureElementNodeId(tag);
    const auto propKey = pathNodeAxisToProperty(axisKey);
    const auto subIndex = static_cast<std::uint32_t>(nodeIndex);
    const auto packed = packPathNodeKey(axisKey, nodeIndex);

    if(durationSec <= 0.f || std::fabs(to - from) <= 0.0001f){
        // Cancel-equivalent: zero-duration tween completes inside the
        // next tick — `AnimationScheduler::tick` treats `durNs == 0`
        // as `finished = true`, which erases the side-table cell.
        // Reads then fall through to the resolved style. Local target
        // tracking is dropped so the next real call won't short-circuit.
        auto tagIt = pathNodeAnimationTargets_.find(tag);
        if(tagIt != pathNodeAnimationTargets_.end()){
            tagIt->second.erase(packed);
            if(tagIt->second.empty()){
                pathNodeAnimationTargets_.erase(tagIt);
            }
        }
        Composition::TimingOptions cancelTiming {};
        cancelTiming.durationMs = 0;
        scheduler->tweenPropertyAt<float>(node, propKey, subIndex,
                                          to, to, cancelTiming,
                                          Composition::AnimationCurve::Linear());
        return;
    }

    auto & propertyMap = pathNodeAnimationTargets_[tag];
    auto existing = propertyMap.find(packed);
    if(existing != propertyMap.end() && std::fabs(existing->second - to) <= 0.0001f){
        return;
    }
    propertyMap[packed] = to;

    auto effectiveCurve = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    Composition::TimingOptions timing {};
    timing.durationMs = static_cast<std::uint32_t>(std::max(0.001f, durationSec) * 1000.f);

    scheduler->tweenPropertyAt<float>(node, propKey, subIndex,
                                      from, to, timing, effectiveCurve);

    if(tag != kUIViewRootEffectTag){
        markElementDirty(tag,false,false,true,false,false);
    }
    styleDirty = true;
}

void UIView::animatePathNode(const UIElementTag & tag,
                             PathNodeAxis axis,
                             int nodeIndex,
                             float from,
                             float to,
                             float durationSec,
                             SharedHandle<Composition::AnimationCurve> curve){
    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // path-node public entry — thin wrapper that forwards to the
    // private `Impl::startOrUpdatePathNodeAnimation`. `axis` casts
    // to its `int` peer (the legacy `ElementAnimationKeyPathNodeX/Y`
    // constants, by construction of the enum class), so no remap is
    // needed.
    impl_->startOrUpdatePathNodeAnimation(tag, static_cast<int>(axis), nodeIndex,
                                          from, to, durationSec, curve);
}

NodeId UIView::Impl::ensureElementNodeId(const UIElementTag & tag){
    auto it = elementNodeIds_.find(tag);
    if(it != elementNodeIds_.end()){
        return it->second;
    }
    const auto id = allocateNodeId();
    elementNodeIds_[tag] = id;
    return id;
}

Core::Optional<NodeId> UIView::Impl::tryElementNodeId(const UIElementTag & tag) const{
    auto it = elementNodeIds_.find(tag);
    if(it == elementNodeIds_.end()){
        return {};
    }
    return it->second;
}

bool UIView::isAnimating(const AnimationScheduler & scheduler) const {
    // UIView-Render-Redesign-Plan §G.3.2 eligibility rule #1. A UIView's
    // animations register under several node ids: view-level ones (opacity /
    // transform) under the view's own node id, and every per-element one
    // (drop shadow, per-element style transitions, path-node tweens) under
    // its element node id (`elementNodeIds_`). The cache must treat the view
    // as animating if ANY of them is active — otherwise an element-only
    // animation (the canonical case: an animated drop shadow) leaves the
    // view eligible, it gets cached on its start frame, and every tween
    // frame blits that stale texture instead of re-rendering (the
    // "freezes on the start frame" bug).
    if(scheduler.hasAnyAnimationFor(nodeId())){
        return true;
    }
    for(const auto & entry : impl_->elementNodeIds_){
        if(scheduler.hasAnyAnimationFor(entry.second)){
            return true;
        }
    }
    return false;
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
// `advanceAnimations` deleted. Phase 4.4 already retired its body
// to a `return false;` stub, and no caller has reached it since —
// `UIView.Update.cpp:114` already documents the symbol as dormant.
// The deletion lands header + impl in the same sweep so an external
// re-introduction would have to declare AND define.

Core::Optional<float> UIView::Impl::animatedValue(const UIElementTag &tag,int key) const{
    // Phase 4.4 (Anim Tier C): read from the AnimationScheduler's side
    // table. Untagged elements (no NodeId ever allocated) and unbacked
    // (NodeId, PropertyKey) slots both fall through to `{}` — the
    // caller (`applyAnimatedColor` / `applyAnimatedShape`) then keeps
    // the resolved-style fallback value (the prior `ComputedStyle` /
    // legacy `ElementBase` value).
    auto * scheduler = activeScheduler();
    if(scheduler == nullptr){
        return {};
    }
    auto node = tryElementNodeId(tag);
    if(!node){
        return {};
    }
    return scheduler->value<float>(*node, elementKeyToProperty(key));
}

Core::Optional<float> UIView::Impl::animatedPathNodeValue(const UIElementTag &tag,
                                                          int axisKey,
                                                          int nodeIndex) const{
    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // path-node reader mirroring `animatedValue` for the
    // `(NodeId, PathNodeX/Y, subIndex=nodeIndex)` side-table cell.
    // Untagged element, negative node index, or no live tween — fall
    // through to `{}` and let Paint use the resolved-style fallback.
    if(nodeIndex < 0){
        return {};
    }
    auto * scheduler = activeScheduler();
    if(scheduler == nullptr){
        return {};
    }
    auto node = tryElementNodeId(tag);
    if(!node){
        return {};
    }
    return scheduler->value<float>(*node,
                                   pathNodeAxisToProperty(axisKey),
                                   static_cast<std::uint32_t>(nodeIndex));
}

// Phase 4.8: `applyAnimatedColor` / `applyAnimatedShape` deleted.
// Both were orphans pre-4.8 (no in-tree caller — `UIView::paint`
// resolves brush/shape directly from `ComputedStyle` /
// `UIElementLayoutSpec`, never threading them through these
// helpers), and both relied on the now-deleted dormant per-tag
// tween maps. The live animation path is `animatedValue()` →
// `AnimationScheduler` queried inline during `UIView::paint`.

SharedHandle<Composition::Font> UIView::Impl::resolveFallbackTextFont(){
    if(fallbackTextFont != nullptr){
        return fallbackTextFont;
    }

    auto fontEngine = Composition::FontEngine::inst();
    if(fontEngine == nullptr){
        return nullptr;
    }

    Composition::FontDescriptor descriptor("Arial",18,Composition::FontDescriptor::Regular);
    fallbackTextFont = fontEngine->CreateFont(descriptor);
    if(fallbackTextFont == nullptr){
        Composition::FontDescriptor secondaryDescriptor("Arial",18,Composition::FontDescriptor::Regular);
        fallbackTextFont = fontEngine->CreateFont(secondaryDescriptor);
    }
    return fallbackTextFont;
}

}
