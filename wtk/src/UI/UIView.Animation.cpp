#include "UIViewImpl.h"
#include "AnimationScheduler.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/AppWindow.h"

namespace OmegaWTK {

namespace {

constexpr const char *kUIViewRootEffectTag = "__UIViewRootEffectTarget__";

float clamp01(float value){
    return std::clamp(value,0.f,1.f);
}

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

AnimationScheduler * activeScheduler(){
    auto * fb = AppWindow::activeFrameBuilder();
    return (fb != nullptr) ? fb->animationScheduler() : nullptr;
}

bool applyShapeDimension(Shape & shape,ElementAnimationKey key,float value){
    const float safeValue = std::max(1.f,value);
    switch(shape.type){
        case Shape::Type::Rect:
            if(key == ElementAnimationKeyWidth){
                shape.rect.w = safeValue;
            }
            else {
                shape.rect.h = safeValue;
            }
            return true;
        case Shape::Type::RoundedRect:
            if(key == ElementAnimationKeyWidth){
                shape.roundedRect.w = safeValue;
                shape.roundedRect.rad_x = std::min(shape.roundedRect.rad_x,shape.roundedRect.w * 0.5f);
            }
            else {
                shape.roundedRect.h = safeValue;
                shape.roundedRect.rad_y = std::min(shape.roundedRect.rad_y,shape.roundedRect.h * 0.5f);
            }
            return true;
        case Shape::Type::Ellipse:
            if(key == ElementAnimationKeyWidth){
                shape.ellipse.rad_x = safeValue * 0.5f;
            }
            else {
                shape.ellipse.rad_y = safeValue * 0.5f;
            }
            return true;
        default:
            return false;
    }
}

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

SharedHandle<Composition::ViewAnimator> UIView::Impl::ensureAnimationViewAnimator(){
    if(animationViewAnimator != nullptr){
        return animationViewAnimator;
    }
    animationViewAnimator = SharedHandle<Composition::ViewAnimator>(
            new Composition::ViewAnimator(owner.compositorProxy()));
    return animationViewAnimator;
}

SharedHandle<Composition::LayerAnimator> UIView::Impl::ensureAnimationLayerAnimator(const UIElementTag & tag){
    auto existing = animationLayerAnimators.find(tag);
    if(existing != animationLayerAnimators.end() && existing->second != nullptr){
        return existing->second;
    }

    auto viewAnimator = ensureAnimationViewAnimator();
    if(viewAnimator == nullptr){
        return nullptr;
    }

    SharedHandle<Composition::Layer> layer = nullptr;
    if(owner.getLayerTree() != nullptr){
        layer = owner.getLayerTree()->getRootLayer();
    }
    if(layer == nullptr){
        return nullptr;
    }

    auto layerAnimator = viewAnimator->layerAnimator(*layer);
    animationLayerAnimators[tag] = layerAnimator;
    return layerAnimator;
}

Composition::AnimationHandle UIView::Impl::beginCompositionClock(const UIElementTag & tag,
                                                                 float durationSec,
                                                                 SharedHandle<Composition::AnimationCurve> curve){
    if(durationSec <= 0.f){
        return {};
    }

    auto layerAnimator = ensureAnimationLayerAnimator(tag);
    if(layerAnimator == nullptr){
        return {};
    }

    Composition::LayerEffect::TransformationParams identity {};
    identity.translate = {0.f,0.f,0.f};
    identity.rotate = {0.f,0.f,0.f};
    identity.scale = {1.f,1.f,1.f};

    Composition::KeyframeValue<Composition::LayerEffect::TransformationParams> startKey {};
    startKey.offset = 0.f;
    startKey.value = identity;
    startKey.easingToNext = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    Composition::KeyframeValue<Composition::LayerEffect::TransformationParams> endKey {};
    endKey.offset = 1.f;
    endKey.value = identity;

    Composition::LayerClip clip {};
    clip.transform = Composition::KeyframeTrack<Composition::LayerEffect::TransformationParams>::From({
            startKey,
            endKey
    });

    Composition::TimingOptions timing {};
    timing.durationMs = static_cast<std::uint32_t>(std::max(1.f,durationSec * 1000.f));
    timing.frameRateHint = static_cast<std::uint16_t>(std::max(1,framesPerSec));
    timing.clockMode = Composition::ClockMode::Hybrid;
    timing.preferResizeSafeBudget = true;
    timing.maxCatchupSteps = 1;

    const auto laneId = owner.compositorProxy().getSyncLaneId();
    if(laneId != 0){
        return layerAnimator->animateOnLane(clip,timing,laneId);
    }
    return layerAnimator->animate(clip,timing);
}

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

bool UIView::Impl::advanceAnimations(){
    // Phase 4.4 (Anim Tier C): the per-view tween pump is gone. The
    // AnimationScheduler — ticked once per outermost frame from
    // `FrameBuilder::beginFrame` — is the sole driver of every animated
    // value reachable from `applyAnimated*` / `animatedValue`. This
    // method is kept as a private symbol so the 4.8 sweep can delete it
    // alongside the other dormant animation surfaces in one pass.
    return false;
}

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

// ORPHAN (post-4.4 dead-code sweep — Phase I): no caller in the tree.
// `UIView::paint` resolves the brush from `ComputedStyle` directly and
// never threads it through this helper, so animating ColorR/G/B/A
// produces no visible effect on the current paint path. Kept declared
// only because it shares a translation unit with the live shadow-channel
// reader; Phase I deletes it (header + .cpp) alongside the matching
// `ElementAnimationKeyColor*` enum entries on `UIView.h`.
Composition::Color UIView::Impl::applyAnimatedColor(const UIElementTag &tag,
                                                    const Composition::Color &baseColor) const{
    Composition::Color output = baseColor;
    if(auto value = animatedValue(tag,ElementAnimationKeyColorRed); value){
        output.r = clamp01(*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyColorGreen); value){
        output.g = clamp01(*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyColorBlue); value){
        output.b = clamp01(*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyColorAlpha); value){
        output.a = clamp01(*value);
    }
    return output;
}

// ORPHAN (post-4.4 dead-code sweep — Phase I): no caller in the tree.
// `UIView::paint` reads `spec.shape` directly and never threads it
// through this helper, so animating ElementAnimationKeyWidth/Height —
// and the path-node branch below — produces no visible effect on the
// current paint path. Phase I deletes this with `applyAnimatedColor`
// and the matching `ElementAnimationKeyWidth/Height/PathNodeX/Y` enum
// entries on `UIView.h`.
Shape UIView::Impl::applyAnimatedShape(const UIElementTag &tag,const Shape &inputShape) const{
    Shape output = inputShape;
    if(auto value = animatedValue(tag,ElementAnimationKeyWidth); value){
        (void)applyShapeDimension(output,ElementAnimationKeyWidth,*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyHeight); value){
        (void)applyShapeDimension(output,ElementAnimationKeyHeight,*value);
    }

    // Phase 4.4 (Anim Tier C): path-node tweens live on the
    // AnimationScheduler under `PathNodeX/Y` with `subIndex = nodeIndex`
    // (Animation-Scheduler-Plan Tier C). The legacy `pathNodeAnimations`
    // map is unused. Probe every control point — there is no side index
    // of "which nodes have animations" yet, but the cost is one
    // unordered_map lookup per (point, axis), which is cheap and stays
    // bounded by the path's complexity.
    if(output.type == Shape::Type::Path && output.path){
        auto * scheduler = activeScheduler();
        auto node = tryElementNodeId(tag);
        if(scheduler != nullptr && node){
            auto points = output.path->getControlPoints();
            bool changed = false;
            for(std::size_t i = 0; i < points.size(); ++i){
                const auto subIndex = static_cast<std::uint32_t>(i);
                if(auto x = scheduler->value<float>(*node, PropertyKey::PathNodeX, subIndex); x){
                    points[i].x = *x;
                    changed = true;
                }
                if(auto y = scheduler->value<float>(*node, PropertyKey::PathNodeY, subIndex); y){
                    points[i].y = *y;
                    changed = true;
                }
            }
            if(changed && !points.empty()){
                output.path = std::make_shared<Composition::Path>(
                    Composition::Path::fromControlPoints(points, output.pathStrokeWidth));
            }
        }
    }
    return output;
}

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
