#include "Widget.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Animation.h"
#include "View.h"
#include <chrono>
#include <cstddef>
#include <cstdint>

#ifndef OMEGAWTK_UI_UIVIEW_H
#define OMEGAWTK_UI_UIVIEW_H

namespace OmegaWTK {

typedef OmegaCommon::String UIElementTag;
typedef UIElementTag UIViewTag;

struct StyleSheet;
OMEGACOMMON_SHARED_CLASS(StyleSheet);

/**
 * @brief A generic shape descriptor for UIView elements.
 * @note Slice A supports Rect and RoundedRect rendering.
 */
struct OMEGAWTK_EXPORT Shape {
    enum class Type : uint8_t {
        Rect,
        RoundedRect,
        Ellipse,
        Path
    };

    Type type = Type::Rect;
    Core::Rect rect {};
    Core::RoundedRect roundedRect {};
    OmegaGTE::GEllipsoid ellipse {};
    Core::Optional<OmegaGTE::GVectorPath2D> path {};
    unsigned pathStrokeWidth = 1;
    bool closePath = false;

    static Shape Scalar(int width,int height);
    static Shape Rect(const Core::Rect & rect);
    static Shape RoundedRect(const Core::RoundedRect & rect);
    static Shape Ellipse(const OmegaGTE::GEllipsoid & ellipse);
    static Shape Ellipse(const Core::Ellipse & ellipse);
    static Shape Path(const OmegaGTE::GVectorPath2D & path,unsigned strokeWidth = 1,bool closePath = false);
};

enum ElementAnimationKey : int {
    ElementAnimationKeyColorRed,
    ElementAnimationKeyColorGreen,
    ElementAnimationKeyColorBlue,
    ElementAnimationKeyColorAlpha,
    ElementAnimationKeyWidth,
    ElementAnimationKeyHeight,
    ElementAnimationKeyPathNodeX,
    ElementAnimationKeyPathNodeY,
};

struct OMEGAWTK_EXPORT StyleSheet {
    struct Entry {
        enum class Kind : uint8_t {
            BackgroundColor,
            BorderEnabled,
            BorderColor,
            BorderWidth,
            DropShadowEffect,
            GaussianBlurEffect,
            DirectionalBlurEffect,
            ElementBrush,
            ElementBrushAnimation,
            ElementAnimation,
            ElementPathAnimation,
            TextFont,
            TextColor,
            TextAlignment,
            TextWrapping,
            TextLineLimit
        };

        Kind kind = Kind::BackgroundColor;
        UIViewTag viewTag {};
        UIElementTag elementTag {};
        Core::Optional<Composition::Color> color {};
        Core::Optional<bool> boolValue {};
        Core::Optional<float> floatValue {};
        SharedHandle<Composition::Brush> brush = nullptr;
        SharedHandle<Composition::Font> font = nullptr;
        ElementAnimationKey animationKey = ElementAnimationKeyColorAlpha;
        SharedHandle<Composition::AnimationCurve> curve = nullptr;
        Core::Optional<Composition::TextLayoutDescriptor::Alignment> textAlignment {};
        Core::Optional<Composition::TextLayoutDescriptor::Wrapping> textWrapping {};
        Core::Optional<unsigned> uintValue {};
        Core::Optional<Composition::LayerEffect::DropShadowParams> dropShadowValue {};
        Core::Optional<Composition::CanvasEffect::GaussianBlurParams> gaussianBlurValue {};
        Core::Optional<Composition::CanvasEffect::DirectionalBlurParams> directionalBlurValue {};
        int nodeIndex = -1;
        bool transition = false;
        float duration = 0.f;
    };

    OmegaCommon::Vector<Entry> entries;

    static StyleSheetPtr Create();
    StyleSheetPtr copy();

    StyleSheetPtr backgroundColor(UIViewTag tag,
                                  const Composition::Color & color,
                                  bool transition = false,
                                  float duration = 0.f);

    StyleSheetPtr border(UIViewTag tag,bool use);

    StyleSheetPtr borderColor(UIViewTag tag,
                              const Composition::Color & color,
                              bool transition = false,
                              float duration = 0.f);

    StyleSheetPtr borderWidth(UIViewTag tag,
                              float width,
                              bool transition = false,
                              float duration = 0.f);

    StyleSheetPtr dropShadow(UIViewTag tag,
                             const Composition::LayerEffect::DropShadowParams & params,
                             bool transition = false,
                             float duration = 0.f);

    StyleSheetPtr gaussianBlur(UIViewTag tag,
                               float radius,
                               bool transition = false,
                               float duration = 0.f);

    StyleSheetPtr directionalBlur(UIViewTag tag,
                                  float radius,
                                  float angle,
                                  bool transition = false,
                                  float duration = 0.f);

    StyleSheetPtr elementDropShadow(UIElementTag elementTag,
                                    const Composition::LayerEffect::DropShadowParams & params,
                                    bool transition = false,
                                    float duration = 0.f);

    StyleSheetPtr elementGaussianBlur(UIElementTag elementTag,
                                      float radius,
                                      bool transition = false,
                                      float duration = 0.f);

    StyleSheetPtr elementDirectionalBlur(UIElementTag elementTag,
                                         float radius,
                                         float angle,
                                         bool transition = false,
                                         float duration = 0.f);

    StyleSheetPtr elementBrush(UIElementTag elementTag,
                               SharedHandle<Composition::Brush> brush,
                               bool transition = false,
                               float duration = 0.f);

    StyleSheetPtr elementBrushAnimation(SharedHandle<Composition::Brush> brush,
                                        ElementAnimationKey key,
                                        SharedHandle<Composition::AnimationCurve> curve,
                                        float duration);

    StyleSheetPtr elementAnimation(UIElementTag elementTag,
                                   ElementAnimationKey key,
                                   SharedHandle<Composition::AnimationCurve> curve,
                                   float duration);

    StyleSheetPtr elementPathAnimation(UIElementTag elementTag,
                                       SharedHandle<Composition::AnimationCurve> curve,
                                       int nodeIndex,
                                       float duration);

    StyleSheetPtr textFont(UIElementTag elementTag,
                           SharedHandle<Composition::Font> font);

    StyleSheetPtr textColor(UIElementTag elementTag,
                            const Composition::Color & color,
                            bool transition = false,
                            float duration = 0.f);

    StyleSheetPtr textAlignment(UIElementTag elementTag,
                                Composition::TextLayoutDescriptor::Alignment alignment);

    StyleSheetPtr textWrapping(UIElementTag elementTag,
                               Composition::TextLayoutDescriptor::Wrapping wrapping);

    StyleSheetPtr textLineLimit(UIElementTag elementTag,unsigned lineLimit);

    StyleSheet();
    ~StyleSheet() = default;
};

class UIViewLayout;
typedef SharedHandle<UIViewLayout> UIViewLayoutPtr;

class UIViewLayout {
public:
    struct Element {
        enum class Type : uint8_t {
            Text,
            Shape
        };

        Type type = Type::Text;
        UIElementTag tag;
        Core::Optional<OmegaCommon::UString> str;
        Core::Optional<Shape> shape;
        Core::Optional<Core::Rect> textRect;
        Core::Optional<UIElementTag> textStyleTag;
    };

private:
    OmegaCommon::Vector<Element> _content;
public:
    void text(UIElementTag tag,OmegaCommon::UString content);
    void text(UIElementTag tag,OmegaCommon::UString content,const Core::Rect & rect);
    void text(UIElementTag tag,OmegaCommon::UString content,const Core::Rect & rect,UIElementTag styleTag);
    void shape(UIElementTag tag,const Shape & shape);
    bool remove(UIElementTag tag);
    void clear();
    const OmegaCommon::Vector<Element> & elements() const;
};

class UIRenderer {
protected:
    struct RenderTargetBundle {
        SharedHandle<Composition::Layer> layer;
        SharedHandle<Composition::Canvas> canvas;
    };

    int framesPerSec = 60;
    UIView *view;
    OmegaCommon::Map<UIElementTag,RenderTargetBundle> renderTargetStore;
    RenderTargetBundle & buildLayerRenderTarget(UIElementTag tag);
public:
    explicit UIRenderer(UIView *view);
    void handleElement(UIElementTag tag);
    void handleTransition(UIElementTag tag,ElementAnimationKey k,float duration);
    void handleAnimation(UIElementTag tag,
                         ElementAnimationKey k,
                         float duration,
                         SharedHandle<Composition::AnimationCurve> & curve);
};

class OMEGAWTK_EXPORT UIView : public CanvasView, UIRenderer {
public:
    struct UpdateDiagnostics {
        std::size_t activeTagCount = 0;
        std::size_t dirtyTagCount = 0;
        std::size_t submittedTagCount = 0;
        std::uint64_t revision = 0;
    };

    struct AnimationDiagnostics {
        std::uint64_t syncLaneId = 0;
        std::uint64_t tickCount = 0;
        std::uint64_t staleStepsSkipped = 0;
        std::uint64_t monotonicProgressClamps = 0;
        std::uint64_t activeTrackCount = 0;
        std::uint64_t completedTrackCount = 0;
        std::uint64_t cancelledTrackCount = 0;
        std::uint64_t failedTrackCount = 0;
        std::uint64_t queuedPacketCount = 0;
        std::uint64_t submittedPacketCount = 0;
        std::uint64_t presentedPacketCount = 0;
        std::uint64_t droppedPacketCount = 0;
        std::uint64_t failedPacketCount = 0;
        std::uint64_t lastSubmittedPacketId = 0;
        std::uint64_t lastPresentedPacketId = 0;
        unsigned inFlight = 0;
        bool staleSkipMode = false;
        bool laneUnderPressure = false;
        bool resizeBudgetActive = false;
    };

    struct EffectState {
        Core::Optional<Composition::LayerEffect::DropShadowParams> dropShadow {};
        Core::Optional<Composition::CanvasEffect::GaussianBlurParams> gaussianBlur {};
        Core::Optional<Composition::CanvasEffect::DirectionalBlurParams> directionalBlur {};
    };

private:
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

    UIViewTag tag;
    UIViewLayout currentLayout;
    StyleSheetPtr currentStyle;
    bool layoutDirty = true;
    bool styleDirty = true;
    bool rootLayoutDirty = true;
    bool rootStyleDirty = true;
    bool rootContentDirty = true;
    bool rootOrderDirty = true;
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
    UpdateDiagnostics lastUpdateDiagnostics {};
    AnimationDiagnostics lastAnimationDiagnostics {};
    std::uint64_t lastObservedDroppedPacketCount = 0;
    bool hasObservedLaneDiagnostics = false;

    void markRootDirty();
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
    void prepareElementAnimations(const OmegaCommon::Vector<UIViewLayout::Element> & elements,
                                  bool layoutChanged,
                                  bool styleChanged);
    void prepareEffectAnimations(const OmegaCommon::Vector<UIViewLayout::Element> & elements,
                                 bool layoutChanged,
                                 bool styleChanged);
    SharedHandle<Composition::Font> resolveFallbackTextFont();
    void syncElementDirtyState(const OmegaCommon::Vector<UIViewLayout::Element> & elements,
                               bool layoutChanged,
                               bool styleChanged,
                               bool orderChanged);
public:
    explicit UIView(const Core::Rect & rect,Composition::LayerTree *layerTree,ViewPtr parent,UIViewTag tag);
    UIViewLayout & layout();
    void setLayout(const UIViewLayout & layout);
    void setStyleSheet(const StyleSheetPtr & style);
    StyleSheetPtr getStyleSheet() const;
    const UpdateDiagnostics & getLastUpdateDiagnostics() const;
    const AnimationDiagnostics & getLastAnimationDiagnostics() const;
    void update();
};

}

#endif // OMEGAWTK_UI_UIVIEW_H
