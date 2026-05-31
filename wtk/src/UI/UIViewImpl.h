#ifndef OMEGAWTK_UI_UIVIEWIMPL_H
#define OMEGAWTK_UI_UIVIEWIMPL_H

#include "omegaWTK/UI/UIView.h"
#include "AnimationScheduler.h"   // Phase 4.4: NodeId, PropertyKey, scheduler API.

#include <algorithm>
#include <chrono>
#include <cmath>

namespace OmegaWTK {

namespace UIViewInternal {

struct ResolvedViewStyle {
    Core::Optional<Composition::Color> backgroundColor {};
    bool useBorder = false;
    Core::Optional<Composition::Color> borderColor {};
    float borderWidth = 0.f;
};

struct ResolvedTextStyle {
    SharedHandle<Composition::Font> font = nullptr;
    Composition::Color color = Composition::Color::create8Bit(Composition::Color::Black8);
    Composition::TextLayoutDescriptor layout {
        Composition::TextLayoutDescriptor::LeftUpper,
        Composition::TextLayoutDescriptor::None
    };
    unsigned lineLimit = 0;
};

struct ResolvedEffectTransition {
    bool transition = false;
    float duration = 0.f;
    SharedHandle<Composition::AnimationCurve> curve = nullptr;
};

struct ResolvedEffectStyle {
    Core::Optional<Composition::LayerEffect::DropShadowParams> dropShadow {};
    ResolvedEffectTransition dropShadowTransition {};
    Core::Optional<Composition::CanvasEffect::GaussianBlurParams> gaussianBlur {};
    ResolvedEffectTransition gaussianBlurTransition {};
    Core::Optional<Composition::CanvasEffect::DirectionalBlurParams> directionalBlur {};
    ResolvedEffectTransition directionalBlurTransition {};
};

// Tier B / B2: the resolved style for one UIView element — the output
// of the Style phase, consumed during Paint. It is fully resolved
// (visual + text; layout lives on `UIElementLayoutSpec::layout`). The
// effect-presence `Optional`s inside `ResolvedEffectStyle` are resolved
// values ("resolved to no effect"), not unresolved-property markers.
// `brush` is always a concrete resolved brush (white default) for shape
// elements. Block 3's `StyleResolver` will produce this; Tier D re-keys
// the cache from `UIElementTag` to `(NodeId,PropertyKey)`.
struct ComputedStyle {
    SharedHandle<Composition::Brush> brush = nullptr;
    ResolvedEffectStyle effects {};
    ResolvedTextStyle text {};
};

struct StyleScope {
    bool touchesRoot = false;
    bool touchesAllElements = false;
    OmegaCommon::Vector<UIElementTag> elementTags {};
};

// Tier B / B3: one element's resolved layout — the output of the Layout
// phase (arrange()), consumed by the Paint phase (paint()). `spec` points
// into `currentLayoutV2_.elements()` and is valid only within a single
// update() (the layout vector is not mutated between arrange and paint).
struct ArrangedElement {
    UIElementTag tag {};
    const UIElementLayoutSpec * spec = nullptr;
    Composition::Rect resolvedRectDp {};
    Composition::Rect resolvedRectPx {};
    int zIndex = 0;
    std::size_t insertionOrder = 0;
};

ResolvedViewStyle resolveViewStyle(const StylePtr & style,const UIViewTag & viewTag);
SharedHandle<Composition::Brush> resolveElementBrush(const StylePtr & style,
                                                     const UIViewTag & viewTag,
                                                     const UIElementTag & elementTag);
ResolvedTextStyle resolveTextStyle(const StylePtr & style,
                                   const UIViewTag & viewTag,
                                   const UIElementTag & elementTag);
ResolvedEffectStyle resolveElementEffectStyle(const StylePtr & style,
                                              const UIViewTag & viewTag,
                                              const UIElementTag & elementTag);
bool containsTag(const OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag);
void addUniqueTag(OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag);
StyleScope collectStyleScope(const StylePtr & style,const UIViewTag & viewTag);
Composition::Rect localBoundsFromView(UIView * view);

}

struct UIView::Impl {
    struct ElementDirtyState {
        bool layoutDirty = true;
        bool styleDirty = true;
        bool contentDirty = true;
        bool orderDirty = true;
        bool visibilityDirty = true;
    };

    struct PropertyAnimationState {
        bool active = false;
        float from = 0.f;
        float to = 0.f;
        float value = 0.f;
        float lastProgress = 0.f;
        float durationSec = 0.f;
        std::chrono::steady_clock::time_point startTime {};
        SharedHandle<Composition::AnimationCurve> curve = nullptr;
        Composition::AnimationHandle compositionHandle {};
        bool compositionClock = false;
    };

    struct PathNodeAnimationState {
        int nodeIndex = -1;
        PropertyAnimationState x;
        PropertyAnimationState y;
    };

    enum : int {
        EffectAnimationKeyShadowOffsetX = 1000,
        EffectAnimationKeyShadowOffsetY,
        EffectAnimationKeyShadowRadius,
        EffectAnimationKeyShadowBlur,
        EffectAnimationKeyShadowOpacity,
        EffectAnimationKeyShadowColorR,
        EffectAnimationKeyShadowColorG,
        EffectAnimationKeyShadowColorB,
        EffectAnimationKeyShadowColorA,
        EffectAnimationKeyGaussianRadius,
        EffectAnimationKeyDirectionalRadius,
        EffectAnimationKeyDirectionalAngle
    };

    explicit Impl(UIView & ownerRef,UIViewTag tagValue);

    UIView & owner;
    int framesPerSec = 60;
    UIViewTag tag;
    UIViewLayout currentLayout;
    StylePtr currentStyle;
    bool layoutDirty = true;
    bool styleDirty = true;
    bool firstFrameCoherentSubmit = true;
    bool styleDirtyGlobal = false;
    bool styleChangeRequiresCoherentFrame = false;
    SharedHandle<Composition::ViewAnimator> animationViewAnimator = nullptr;
    OmegaCommon::Map<UIElementTag,SharedHandle<Composition::LayerAnimator>> animationLayerAnimators;
    OmegaCommon::Map<UIElementTag,ElementDirtyState> elementDirtyState;
    OmegaCommon::Vector<UIElementTag> activeTagOrder;
    // Phase 4.4: legacy per-tag tween bookkeeping. UNUSED — nothing writes
    // these any more (Anim Tier C routed startOrUpdateAnimation / pathNode
    // updates onto the AnimationScheduler). Fields are retained so the 4.8
    // deletion can be a clean sweep across all the dormant animation
    // surfaces (the four ViewAnimator/LayerAnimator/elementAnimations/
    // pathNodeAnimations members; the ensure*/beginCompositionClock
    // helpers; advanceAnimations + lastAnimationDiagnostics).
    OmegaCommon::Map<UIElementTag,OmegaCommon::Map<int,PropertyAnimationState>> elementAnimations;
    OmegaCommon::Map<UIElementTag,OmegaCommon::Vector<PathNodeAnimationState>> pathNodeAnimations;
    // Phase 4.4 (Anim Tier C): stable NodeId per element tag, allocated on
    // first animation reference (registration or read). The
    // AnimationScheduler keys its side table on
    // `(elementNodeId, PropertyKey, subIndex)`; `applyAnimated*` /
    // `animatedValue` look up the NodeId before probing the scheduler.
    OmegaCommon::Map<UIElementTag,NodeId> elementNodeIds_;
    // Phase 4.4 (Anim Tier C): preserves the legacy "same target = no
    // restart" short-circuit on `startOrUpdateAnimation`. The scheduler
    // itself replaces on every re-registration (Anim §6 Q3); this map
    // tracks the last requested `.to` per `(tag, key)` so a repeated call
    // with the same target is a no-op rather than a fresh tween.
    OmegaCommon::Map<UIElementTag,OmegaCommon::Map<int,float>> animationTargets_;
    OmegaCommon::Map<UIElementTag,Composition::Color> lastResolvedElementColor;
    OmegaCommon::Map<UIElementTag,EffectState> lastResolvedEffects;
    OmegaCommon::Map<UIElementTag,Shape> previousShapeByTag;
    SharedHandle<Composition::Font> fallbackTextFont = nullptr;
    UIViewLayoutV2 currentLayoutV2_;
    OmegaCommon::Map<UIElementTag,Composition::Rect> lastResolvedV2Rects_;

    // Tier B / B2: resolved-style cache (the Style phase's output).
    // `resolveStyles()` rebuilds these each frame for now; B3 will gate
    // the rebuild on DirtyBit::Style. Paint reads them, never writes.
    UIViewInternal::ResolvedViewStyle resolvedViewStyle_ {};
    OmegaCommon::Map<UIElementTag,UIViewInternal::ComputedStyle> computedStyles_;

    // Tier B / B3: arranged layout (the Layout phase's output). `arrange()`
    // writes both; `paint()` reads them. Rebuilt every frame for now.
    OmegaCommon::Vector<UIViewInternal::ArrangedElement> arranged_;
    Composition::Rect arrangedLocalBounds_ {};
    LayoutDiagnosticSink * diagnosticSink_ = nullptr;
    UpdateDiagnostics lastUpdateDiagnostics {};
    AnimationDiagnostics lastAnimationDiagnostics {};
    std::uint64_t lastObservedDroppedPacketCount = 0;
    bool hasObservedLaneDiagnostics = false;

    void markAllElementsDirty();
    void markElementDirty(const UIElementTag & tag,
                          bool layout,
                          bool style,
                          bool content,
                          bool order,
                          bool visibility);
    bool isElementDirty(const UIElementTag & tag) const;
    void clearElementDirty(const UIElementTag & tag);
    SharedHandle<Composition::ViewAnimator> ensureAnimationViewAnimator();
    SharedHandle<Composition::LayerAnimator> ensureAnimationLayerAnimator(const UIElementTag & tag);
    Composition::AnimationHandle beginCompositionClock(const UIElementTag & tag,
                                                       float durationSec,
                                                       SharedHandle<Composition::AnimationCurve> curve);
    void startOrUpdateAnimation(const UIElementTag & tag,
                                int key,
                                float from,
                                float to,
                                float durationSec,
                                SharedHandle<Composition::AnimationCurve> curve);
    bool advanceAnimations();
    Core::Optional<float> animatedValue(const UIElementTag & tag,int key) const;
    // Phase 4.4: lazy NodeId allocation for one of this UIView's element
    // tags. `ensure*` writes the map (used by registration paths);
    // `try*` is read-only (used by `animatedValue` so an unknown tag
    // returns nullopt instead of growing the map).
    NodeId ensureElementNodeId(const UIElementTag & tag);
    Core::Optional<NodeId> tryElementNodeId(const UIElementTag & tag) const;
    Composition::Color applyAnimatedColor(const UIElementTag & tag,const Composition::Color & baseColor) const;
    Shape applyAnimatedShape(const UIElementTag & tag,const Shape & inputShape) const;
    SharedHandle<Composition::Font> resolveFallbackTextFont();
    void convertLegacyLayoutToV2();

    // Tier B / B2: read the cached resolved style for an element.
    // Returns a default (empty) style for tags not in the current
    // layout — a defensive fall-through; every live element is
    // populated by resolveStyles() before Paint reads it.
    const UIViewInternal::ComputedStyle & computedStyleFor(const UIElementTag & tag) const;
};

}

#endif
