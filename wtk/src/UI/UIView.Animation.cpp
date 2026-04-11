#include "UIViewImpl.h"

namespace OmegaWTK {

namespace {

constexpr const char *kUIViewRootEffectTag = "__UIViewRootEffectTarget__";

float clamp01(float value){
    return std::clamp(value,0.f,1.f);
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
    if(!std::isfinite(from) || !std::isfinite(to)){
        return;
    }

    auto tagIt = elementAnimations.find(tag);
    if(durationSec <= 0.f || std::fabs(to - from) <= 0.0001f){
        if(tagIt != elementAnimations.end()){
            auto existing = tagIt->second.find(key);
            if(existing != tagIt->second.end() && existing->second.compositionHandle.valid()){
                existing->second.compositionHandle.cancel();
            }
            tagIt->second.erase(key);
            if(tagIt->second.empty()){
                elementAnimations.erase(tagIt);
            }
        }
        return;
    }

    auto & propertyMap = elementAnimations[tag];
    auto & state = propertyMap[key];
    if(state.active && std::fabs(state.to - to) <= 0.0001f){
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float startValue = state.active ? state.value : from;
    state.active = true;
    state.from = startValue;
    state.to = to;
    state.value = startValue;
    state.lastProgress = 0.f;
    state.durationSec = std::max(0.001f,durationSec);
    state.startTime = now;
    state.curve = curve != nullptr ? curve : Composition::AnimationCurve::Linear();
    if(state.compositionHandle.valid()){
        state.compositionHandle.cancel();
    }
    state.compositionHandle = beginCompositionClock(tag,state.durationSec,state.curve);
    state.compositionClock = state.compositionHandle.valid();
    if(tag != kUIViewRootEffectTag){
        markElementDirty(tag,false,false,true,false,false);
    }
    styleDirty = true;
}

bool UIView::Impl::advanceAnimations(){
    auto now = std::chrono::steady_clock::now();
    bool changed = false;
    OmegaCommon::Vector<UIElementTag> removePropertyTags {};

    const auto laneDiagnostics = owner.compositorProxy().getSyncLaneDiagnostics();
    const bool hasLaneDiagnostics = laneDiagnostics.syncLaneId != 0;
    bool staleSkipMode = false;
    if(hasLaneDiagnostics){
        const bool droppedCountIncreased = hasObservedLaneDiagnostics &&
                                           laneDiagnostics.droppedPacketCount > lastObservedDroppedPacketCount;
        (void)droppedCountIncreased;
        lastObservedDroppedPacketCount = laneDiagnostics.droppedPacketCount;
        hasObservedLaneDiagnostics = true;
    }
    else {
        hasObservedLaneDiagnostics = false;
        lastObservedDroppedPacketCount = 0;
    }

    std::uint64_t staleStepsSkippedThisTick = 0;
    std::uint64_t monotonicProgressClampsThisTick = 0;
    std::uint64_t activeTrackCount = 0;
    std::uint64_t completedTrackCountThisTick = 0;
    std::uint64_t cancelledTrackCountThisTick = 0;
    std::uint64_t failedTrackCountThisTick = 0;

    auto resolveProgress = [&](PropertyAnimationState & state) -> float {
        float elapsedSec = std::chrono::duration<float>(now - state.startTime).count();
        if(!std::isfinite(elapsedSec) || elapsedSec < 0.f){
            elapsedSec = 0.f;
        }
        float wallClockT = state.durationSec <= 0.f ? 1.f : clamp01(elapsedSec / state.durationSec);

        if(!state.compositionClock || !state.compositionHandle.valid()){
            float monotonicT = clamp01(wallClockT);
            if(monotonicT + 0.0001f < state.lastProgress){
                monotonicT = state.lastProgress;
                monotonicProgressClampsThisTick += 1;
            }
            state.lastProgress = monotonicT;
            return monotonicT;
        }

        auto handleState = state.compositionHandle.state();
        if(handleState == Composition::AnimationState::Cancelled ||
           handleState == Composition::AnimationState::Failed){
            state.compositionClock = false;
            if(handleState == Composition::AnimationState::Cancelled){
                cancelledTrackCountThisTick += 1;
            }
            else {
                failedTrackCountThisTick += 1;
            }
            state.compositionHandle = {};
            float monotonicT = clamp01(wallClockT);
            if(monotonicT + 0.0001f < state.lastProgress){
                monotonicT = state.lastProgress;
                monotonicProgressClampsThisTick += 1;
            }
            state.lastProgress = monotonicT;
            return monotonicT;
        }

        float compositionT = clamp01(state.compositionHandle.progress());
        if(handleState == Composition::AnimationState::Completed){
            compositionT = 1.f;
        }

        float resolvedT = wallClockT;
        if(staleSkipMode &&
           handleState != Composition::AnimationState::Completed){
            if(wallClockT > compositionT + 0.0001f){
                staleStepsSkippedThisTick += 1;
            }
            resolvedT = compositionT;
        }
        else {
            resolvedT = std::max(wallClockT,compositionT);
        }

        resolvedT = clamp01(resolvedT);
        if(resolvedT + 0.0001f < state.lastProgress){
            resolvedT = state.lastProgress;
            monotonicProgressClampsThisTick += 1;
        }
        state.lastProgress = resolvedT;
        return resolvedT;
    };

    for(auto & tagEntry : elementAnimations){
        OmegaCommon::Vector<int> removeKeys {};
        for(auto & propertyEntry : tagEntry.second){
            auto & state = propertyEntry.second;
            if(!state.active){
                removeKeys.push_back(propertyEntry.first);
                continue;
            }
            activeTrackCount += 1;

            float t = resolveProgress(state);
            float sampled = state.curve != nullptr ? clamp01(state.curve->sample(t)) : t;
            float nextValue = state.from + ((state.to - state.from) * sampled);
            if(!std::isfinite(nextValue)){
                nextValue = state.to;
            }

            if(std::fabs(nextValue - state.value) > 0.0001f){
                state.value = nextValue;
                if(tagEntry.first != kUIViewRootEffectTag){
                    markElementDirty(tagEntry.first,false,false,true,false,false);
                }
                styleDirty = true;
                changed = true;
            }

            if(t >= 1.f){
                state.value = state.to;
                state.active = false;
                state.compositionClock = false;
                state.lastProgress = 1.f;
                state.compositionHandle = {};
                completedTrackCountThisTick += 1;
                if(tagEntry.first != kUIViewRootEffectTag){
                    markElementDirty(tagEntry.first,false,false,true,false,false);
                }
                styleDirty = true;
                changed = true;
                removeKeys.push_back(propertyEntry.first);
            }
        }

        for(const auto key : removeKeys){
            tagEntry.second.erase(key);
        }
        if(tagEntry.second.empty()){
            removePropertyTags.push_back(tagEntry.first);
        }
    }

    for(const auto & tagToRemove : removePropertyTags){
        elementAnimations.erase(tagToRemove);
    }

    OmegaCommon::Vector<UIElementTag> removePathTags {};
    for(auto & tagEntry : pathNodeAnimations){
        auto & nodeAnimations = tagEntry.second;
        OmegaCommon::Vector<PathNodeAnimationState> nextNodeAnimations {};
        nextNodeAnimations.reserve(nodeAnimations.size());
        bool tagChanged = false;

        for(auto nodeAnimation : nodeAnimations){
            auto advanceProperty = [&](PropertyAnimationState & propertyState) -> bool {
                if(!propertyState.active){
                    return false;
                }
                activeTrackCount += 1;

                float t = resolveProgress(propertyState);
                float sampled = propertyState.curve != nullptr ? clamp01(propertyState.curve->sample(t)) : t;
                float nextValue = propertyState.from + ((propertyState.to - propertyState.from) * sampled);
                if(!std::isfinite(nextValue)){
                    nextValue = propertyState.to;
                }
                bool propertyChanged = false;
                if(std::fabs(nextValue - propertyState.value) > 0.0001f){
                    propertyState.value = nextValue;
                    propertyChanged = true;
                }
                if(t >= 1.f){
                    propertyState.value = propertyState.to;
                    propertyState.active = false;
                    propertyState.compositionClock = false;
                    propertyState.lastProgress = 1.f;
                    propertyState.compositionHandle = {};
                    completedTrackCountThisTick += 1;
                    propertyChanged = true;
                }
                return propertyChanged;
            };

            bool xChanged = advanceProperty(nodeAnimation.x);
            bool yChanged = advanceProperty(nodeAnimation.y);
            bool nodeActive = nodeAnimation.x.active || nodeAnimation.y.active;
            if(nodeActive){
                nextNodeAnimations.push_back(nodeAnimation);
            }
            if(xChanged || yChanged){
                tagChanged = true;
            }
        }

        nodeAnimations = std::move(nextNodeAnimations);
        if(nodeAnimations.empty()){
            removePathTags.push_back(tagEntry.first);
        }
        if(tagChanged){
            markElementDirty(tagEntry.first,false,false,true,false,false);
            changed = true;
        }
    }

    for(const auto & tagToRemove : removePathTags){
        pathNodeAnimations.erase(tagToRemove);
    }

    lastAnimationDiagnostics.syncLaneId = laneDiagnostics.syncLaneId;
    lastAnimationDiagnostics.tickCount += 1;
    lastAnimationDiagnostics.staleStepsSkipped += staleStepsSkippedThisTick;
    lastAnimationDiagnostics.monotonicProgressClamps += monotonicProgressClampsThisTick;
    lastAnimationDiagnostics.activeTrackCount = activeTrackCount;
    lastAnimationDiagnostics.completedTrackCount += completedTrackCountThisTick;
    lastAnimationDiagnostics.cancelledTrackCount += cancelledTrackCountThisTick;
    lastAnimationDiagnostics.failedTrackCount += failedTrackCountThisTick;
    lastAnimationDiagnostics.queuedPacketCount = laneDiagnostics.queuedPacketCount;
    lastAnimationDiagnostics.submittedPacketCount = laneDiagnostics.submittedPacketCount;
    lastAnimationDiagnostics.presentedPacketCount = laneDiagnostics.presentedPacketCount;
    lastAnimationDiagnostics.droppedPacketCount = laneDiagnostics.droppedPacketCount;
    lastAnimationDiagnostics.failedPacketCount = laneDiagnostics.failedPacketCount;
    lastAnimationDiagnostics.lastSubmittedPacketId = laneDiagnostics.lastSubmittedPacketId;
    lastAnimationDiagnostics.lastPresentedPacketId = laneDiagnostics.lastPresentedPacketId;
    lastAnimationDiagnostics.inFlight = laneDiagnostics.inFlight;
    lastAnimationDiagnostics.staleSkipMode = staleSkipMode;

    return changed;
}

Core::Optional<float> UIView::Impl::animatedValue(const UIElementTag &tag,int key) const{
    auto tagIt = elementAnimations.find(tag);
    if(tagIt == elementAnimations.end()){
        return {};
    }
    auto propertyIt = tagIt->second.find(key);
    if(propertyIt == tagIt->second.end()){
        return {};
    }
    return propertyIt->second.value;
}

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

Shape UIView::Impl::applyAnimatedShape(const UIElementTag &tag,const Shape &inputShape) const{
    Shape output = inputShape;
    if(auto value = animatedValue(tag,ElementAnimationKeyWidth); value){
        (void)applyShapeDimension(output,ElementAnimationKeyWidth,*value);
    }
    if(auto value = animatedValue(tag,ElementAnimationKeyHeight); value){
        (void)applyShapeDimension(output,ElementAnimationKeyHeight,*value);
    }

    auto pathAnimIt = pathNodeAnimations.find(tag);
    if(pathAnimIt != pathNodeAnimations.end() &&
       output.type == Shape::Type::Path &&
       output.path){
        auto points = output.path->getControlPoints();
        for(const auto & nodeAnimation : pathAnimIt->second){
            if(nodeAnimation.nodeIndex < 0){
                continue;
            }
            auto nodeIndex = static_cast<std::size_t>(nodeAnimation.nodeIndex);
            if(nodeIndex >= points.size()){
                continue;
            }
            points[nodeIndex].x = nodeAnimation.x.value;
            points[nodeIndex].y = nodeAnimation.y.value;
        }
        if(!points.empty()){
            output.path = std::make_shared<Composition::Path>(
                Composition::Path::fromControlPoints(points, output.pathStrokeWidth));
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

    Composition::FontDescriptor descriptor("Helvetica",18,Composition::FontDescriptor::Regular);
    fallbackTextFont = fontEngine->CreateFont(descriptor);
    if(fallbackTextFont == nullptr){
        Composition::FontDescriptor secondaryDescriptor("Arial",18,Composition::FontDescriptor::Regular);
        fallbackTextFont = fontEngine->CreateFont(secondaryDescriptor);
    }
    return fallbackTextFont;
}

}
