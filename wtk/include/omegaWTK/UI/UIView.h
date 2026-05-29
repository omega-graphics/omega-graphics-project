#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Animation.h"
#include "View.h"
#include "Layout.h"
#include <cstddef>
#include <cstdint>

#ifndef OMEGAWTK_UI_UIVIEW_H
#define OMEGAWTK_UI_UIVIEW_H

namespace OmegaWTK {

namespace Composition { struct PaintContext; }

typedef OmegaCommon::String UIElementTag;
typedef UIElementTag UIViewTag;

struct Style;
OMEGACOMMON_SHARED_CLASS(Style);

// Tier B / B1: `StyleSheet` was renamed to `Style` (a per-view,
// element-tag-keyed inline visual+text surface; layout authoring moved
// off it onto `UIElementLayoutSpec::layout`). These aliases keep
// out-of-tree callers compiling for one deprecation cycle. All in-tree
// callers use the new names.
[[deprecated("StyleSheet was renamed to Style")]]
typedef Style StyleSheet;
[[deprecated("StyleSheetPtr was renamed to StylePtr")]]
typedef StylePtr StyleSheetPtr;

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
    Composition::Rect rect {};
    Composition::RoundedRect roundedRect {};
    Composition::Ellipse ellipse {};
    Core::SharedPtr<Composition::Path> path {};
    unsigned pathStrokeWidth = 1;
    bool closePath = false;

    static Shape Scalar(int width,int height);
    static Shape Rect(const Composition::Rect & rect);
    static Shape RoundedRect(const Composition::RoundedRect & rect);
    static Shape Ellipse(const OmegaGTE::GEllipsoid & ellipse);
    static Shape Ellipse(const Composition::Ellipse & ellipse);
    static Shape Path(Composition::Path path,unsigned strokeWidth = 1,bool closePath = false);
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

struct OMEGAWTK_EXPORT Style {
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

    static StylePtr Create();
    StylePtr copy();

    StylePtr backgroundColor(UIViewTag tag,
                                  const Composition::Color & color,
                                  bool transition = false,
                                  float duration = 0.f);

    StylePtr border(UIViewTag tag,bool use);

    StylePtr borderColor(UIViewTag tag,
                              const Composition::Color & color,
                              bool transition = false,
                              float duration = 0.f);

    StylePtr borderWidth(UIViewTag tag,
                              float width,
                              bool transition = false,
                              float duration = 0.f);

    StylePtr dropShadow(UIViewTag tag,
                             const Composition::LayerEffect::DropShadowParams & params,
                             bool transition = false,
                             float duration = 0.f);

    StylePtr gaussianBlur(UIViewTag tag,
                               float radius,
                               bool transition = false,
                               float duration = 0.f);

    StylePtr directionalBlur(UIViewTag tag,
                                  float radius,
                                  float angle,
                                  bool transition = false,
                                  float duration = 0.f);

    StylePtr elementDropShadow(UIElementTag elementTag,
                                    const Composition::LayerEffect::DropShadowParams & params,
                                    bool transition = false,
                                    float duration = 0.f);

    StylePtr elementGaussianBlur(UIElementTag elementTag,
                                      float radius,
                                      bool transition = false,
                                      float duration = 0.f);

    StylePtr elementDirectionalBlur(UIElementTag elementTag,
                                         float radius,
                                         float angle,
                                         bool transition = false,
                                         float duration = 0.f);

    StylePtr elementBrush(UIElementTag elementTag,
                               SharedHandle<Composition::Brush> brush,
                               bool transition = false,
                               float duration = 0.f);

    StylePtr elementBrushAnimation(SharedHandle<Composition::Brush> brush,
                                        ElementAnimationKey key,
                                        SharedHandle<Composition::AnimationCurve> curve,
                                        float duration);

    StylePtr elementAnimation(UIElementTag elementTag,
                                   ElementAnimationKey key,
                                   SharedHandle<Composition::AnimationCurve> curve,
                                   float duration);

    StylePtr elementPathAnimation(UIElementTag elementTag,
                                       SharedHandle<Composition::AnimationCurve> curve,
                                       int nodeIndex,
                                       float duration);

    StylePtr textFont(UIElementTag elementTag,
                           SharedHandle<Composition::Font> font);

    StylePtr textColor(UIElementTag elementTag,
                            const Composition::Color & color,
                            bool transition = false,
                            float duration = 0.f);

    StylePtr textAlignment(UIElementTag elementTag,
                                Composition::TextLayoutDescriptor::Alignment alignment);

    StylePtr textWrapping(UIElementTag elementTag,
                               Composition::TextLayoutDescriptor::Wrapping wrapping);

    StylePtr textLineLimit(UIElementTag elementTag,unsigned lineLimit);

    // Layout authoring (layoutWidth/Height/Size/Margin/Padding/Clamp/
    // Transition) moved off the style surface in Tier B / B1. Author
    // layout directly on `UIElementLayoutSpec::layout`.

    Style();
    ~Style() = default;
};

class UIViewLayout;
typedef SharedHandle<UIViewLayout> UIViewLayoutPtr;

class OMEGAWTK_EXPORT UIViewLayout {
public:
    struct Element {
        enum class Type : uint8_t {
            Text,
            Shape,
            Image
        };

        Type type = Type::Text;
        UIElementTag tag;
        Core::Optional<OmegaCommon::UString> str;
        Core::Optional<Shape> shape;
        Core::Optional<Composition::Rect> textRect;
        Core::Optional<UIElementTag> textStyleTag;
        Core::Optional<SharedHandle<OmegaCommon::Img::BitmapImage>> image;
        Core::Optional<Composition::Rect> imageRect;
    };

private:
    OmegaCommon::Vector<Element> _content;
public:
    void text(UIElementTag tag,OmegaCommon::UString content);
    void text(UIElementTag tag,OmegaCommon::UString content,const Composition::Rect & rect);
    void text(UIElementTag tag,OmegaCommon::UString content,const Composition::Rect & rect,UIElementTag styleTag);
    void shape(UIElementTag tag,const Shape & shape);
    void image(UIElementTag tag,const SharedHandle<OmegaCommon::Img::BitmapImage> & img,const Composition::Rect & rect);
    bool remove(UIElementTag tag);
    void clear();
    const OmegaCommon::Vector<Element> & elements() const;
};

struct OMEGAWTK_EXPORT UIElementLayoutSpec {
    UIElementTag tag {};
    LayoutStyle layout {};
    Core::Optional<Shape> shape {};
    Core::Optional<OmegaCommon::UString> text {};
    Core::Optional<Composition::Rect> textRect {};
    Core::Optional<UIElementTag> textStyleTag {};
    Core::Optional<SharedHandle<OmegaCommon::Img::BitmapImage>> image {};
    Core::Optional<Composition::Rect> imageRect {};
    int zIndex = 0;
};

class OMEGAWTK_EXPORT UIViewLayoutV2 {
    OmegaCommon::Vector<UIElementLayoutSpec> elements_;
public:
    UIViewLayoutV2 & element(const UIElementLayoutSpec & spec);
    bool remove(UIElementTag tag);
    void clear();
    const OmegaCommon::Vector<UIElementLayoutSpec> & elements() const;
    bool hasElement(UIElementTag tag) const;
};

class OMEGAWTK_EXPORT UIView : public View {
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
    struct Impl;
    Core::UniquePtr<Impl> impl_;
public:
    explicit UIView(const Composition::Rect & rect,ViewPtr parent,UIViewTag tag);
    ~UIView() override;
    UIViewLayout & layout();
    void setLayout(const UIViewLayout & layout);
    void setStyle(const StylePtr & style);
    StylePtr getStyle() const;

    // Deprecated B1 forwarders — `setStyleSheet`/`getStyleSheet` were
    // renamed to `setStyle`/`getStyle`. Kept for one cycle for
    // out-of-tree callers.
    [[deprecated("setStyleSheet was renamed to setStyle")]]
    void setStyleSheet(const StylePtr & style){ setStyle(style); }
    [[deprecated("getStyleSheet was renamed to getStyle")]]
    StylePtr getStyleSheet() const { return getStyle(); }

    const UpdateDiagnostics & getLastUpdateDiagnostics() const;
    const AnimationDiagnostics & getLastAnimationDiagnostics() const;

    UIViewLayoutV2 & layoutV2();
    void setLayoutV2(const UIViewLayoutV2 & layout);

    void setDiagnosticSink(LayoutDiagnosticSink * sink);

    void applyLayoutDelta(const UIElementTag & elementTag,
                          const LayoutDelta & delta,
                          const LayoutTransitionSpec & spec);

    void update();

private:
    // Tier B / B3: the per-phase methods that update() orchestrates in
    // order, flipping the window FrameBuilder's currentPhase_ around each.
    //  - tickAnimations(): Tick — drives the per-view animator pump.
    //  - resolveStyles():  Style — resolves `currentStyle` into the
    //    per-element `ComputedStyle` cache (+ view-level resolved style).
    //  - arrange():        Layout — resolves element rects + z-order into
    //    the arranged cache.
    //  - paint(PaintContext&): Paint — pure build of the DisplayList from
    //    arranged layout + ComputedStyle + animation values.
    // Each reads/writes Impl-side caches; B3 gates none of them yet (they
    // rebuild every frame), B5 wires the cross-phase assertions.
    void tickAnimations();
    void resolveStyles();
    void arrange();
    void paint(Composition::PaintContext & pc);
};

}

#endif // OMEGAWTK_UI_UIVIEW_H
