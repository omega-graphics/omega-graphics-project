#ifndef OMEGAWTK_UI_UIVIEWIMPL_H
#define OMEGAWTK_UI_UIVIEWIMPL_H

#include "omegaWTK/UI/UIView.h"

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

struct StyleScope {
    bool touchesRoot = false;
    bool touchesAllElements = false;
    OmegaCommon::Vector<UIElementTag> elementTags {};
};

ResolvedViewStyle resolveViewStyle(const StyleSheetPtr & style,const UIViewTag & viewTag);
SharedHandle<Composition::Brush> resolveElementBrush(const StyleSheetPtr & style,
                                                     const UIViewTag & viewTag,
                                                     const UIElementTag & elementTag);
ResolvedTextStyle resolveTextStyle(const StyleSheetPtr & style,
                                   const UIViewTag & viewTag,
                                   const UIElementTag & elementTag);
ResolvedEffectStyle resolveElementEffectStyle(const StyleSheetPtr & style,
                                              const UIViewTag & viewTag,
                                              const UIElementTag & elementTag);
bool containsTag(const OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag);
void addUniqueTag(OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag);
StyleScope collectStyleScope(const StyleSheetPtr & style,const UIViewTag & viewTag);
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
    StyleSheetPtr currentStyle;
    bool layoutDirty = true;
    bool styleDirty = true;
    bool firstFrameCoherentSubmit = true;
    bool styleDirtyGlobal = false;
    bool styleChangeRequiresCoherentFrame = false;
    SharedHandle<Composition::Canvas> rootCanvas;
    SharedHandle<Composition::ViewAnimator> animationViewAnimator = nullptr;
    OmegaCommon::Map<UIElementTag,SharedHandle<Composition::LayerAnimator>> animationLayerAnimators;
    OmegaCommon::Map<UIElementTag,ElementDirtyState> elementDirtyState;
    OmegaCommon::Vector<UIElementTag> activeTagOrder;
    OmegaCommon::Map<UIElementTag,OmegaCommon::Map<int,PropertyAnimationState>> elementAnimations;
    OmegaCommon::Map<UIElementTag,OmegaCommon::Vector<PathNodeAnimationState>> pathNodeAnimations;
    OmegaCommon::Map<UIElementTag,Composition::Color> lastResolvedElementColor;
    OmegaCommon::Map<UIElementTag,EffectState> lastResolvedEffects;
    OmegaCommon::Map<UIElementTag,Shape> previousShapeByTag;
    SharedHandle<Composition::Font> fallbackTextFont = nullptr;
    UIViewLayoutV2 currentLayoutV2_;
    OmegaCommon::Map<UIElementTag,Composition::Rect> lastResolvedV2Rects_;
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
    Composition::Color applyAnimatedColor(const UIElementTag & tag,const Composition::Color & baseColor) const;
    Shape applyAnimatedShape(const UIElementTag & tag,const Shape & inputShape) const;
    SharedHandle<Composition::Font> resolveFallbackTextFont();
    void convertLegacyLayoutToV2();
};

}

#endif
