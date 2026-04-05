#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Animation.h"
#include "omegaWTK/Composition/Canvas.h"

#include <cstdint>
#ifndef OMEGAWTK_UI_LAYOUT_H
#define OMEGAWTK_UI_LAYOUT_H

namespace OmegaWTK {

// ---------------------------------------------------------------------------
// 2.1 Units and lengths
// ---------------------------------------------------------------------------

enum class LayoutUnit : std::uint8_t {
    Auto,
    Px,
    Dp,
    Percent,
    Fr,
    Intrinsic
};

struct OMEGAWTK_EXPORT LayoutLength {
    LayoutUnit unit = LayoutUnit::Auto;
    float value = 0.f;

    static LayoutLength Auto();
    static LayoutLength Px(float v);
    static LayoutLength Dp(float v);
    static LayoutLength Percent(float v);
    static LayoutLength Fr(float v);
    static LayoutLength Intrinsic();

    bool isAuto() const;
    bool isIntrinsic() const;
    bool isFixed() const;
};

struct OMEGAWTK_EXPORT LayoutEdges {
    LayoutLength left;
    LayoutLength top;
    LayoutLength right;
    LayoutLength bottom;

    static LayoutEdges Zero();
    static LayoutEdges All(LayoutLength value);
    static LayoutEdges Symmetric(LayoutLength horizontal,LayoutLength vertical);
};

struct OMEGAWTK_EXPORT LayoutClamp {
    LayoutLength minWidth  = LayoutLength::Dp(1.f);
    LayoutLength minHeight = LayoutLength::Dp(1.f);
    LayoutLength maxWidth  = LayoutLength::Auto();
    LayoutLength maxHeight = LayoutLength::Auto();
};

// ---------------------------------------------------------------------------
// 2.2 Display and alignment
// ---------------------------------------------------------------------------

enum class LayoutDisplay : std::uint8_t {
    Stack,
    Flex,
    Grid,
    Overlay,
    Custom
};

enum class LayoutPositionMode : std::uint8_t {
    Flow,
    Absolute
};

enum class LayoutAlign : std::uint8_t {
    Start,
    Center,
    End,
    Stretch,
    Baseline
};

enum class FlexDirection : std::uint8_t {
    Row,
    RowReverse,
    Column,
    ColumnReverse
};

enum class FlexWrap : std::uint8_t {
    NoWrap,
    Wrap,
    WrapReverse
};

// ---------------------------------------------------------------------------
// 2.3 Layout style (per-node)
// ---------------------------------------------------------------------------

struct OMEGAWTK_EXPORT LayoutStyle {
    LayoutDisplay display       = LayoutDisplay::Stack;
    LayoutPositionMode position = LayoutPositionMode::Flow;

    LayoutLength width  = LayoutLength::Auto();
    LayoutLength height = LayoutLength::Auto();
    LayoutClamp clamp {};

    LayoutEdges margin  = LayoutEdges::Zero();
    LayoutEdges padding = LayoutEdges::Zero();

    LayoutLength insetLeft   = LayoutLength::Auto();
    LayoutLength insetTop    = LayoutLength::Auto();
    LayoutLength insetRight  = LayoutLength::Auto();
    LayoutLength insetBottom = LayoutLength::Auto();

    LayoutLength gap = LayoutLength::Dp(0.f);
    LayoutAlign alignSelfMain  = LayoutAlign::Start;
    LayoutAlign alignSelfCross = LayoutAlign::Start;

    float flexGrow   = 0.f;
    float flexShrink = 1.f;
    Core::Optional<float> aspectRatio {};

    FlexDirection flexDirection = FlexDirection::Column;
    FlexWrap flexWrap = FlexWrap::NoWrap;
    LayoutAlign justifyContent = LayoutAlign::Start;
    LayoutAlign alignItems     = LayoutAlign::Start;
};

// ---------------------------------------------------------------------------
// 2.4 Layout context and measure result
// ---------------------------------------------------------------------------

struct OMEGAWTK_EXPORT LayoutContext {
    Core::Rect availableRectPx {};
    float dpiScale = 1.f;
    std::uint64_t resizeSessionId = 0;
    bool liveResize = false;

    float dpToPx(float dp) const;
    Core::Rect availableRectDp() const;
};

struct OMEGAWTK_EXPORT MeasureResult {
    float measuredWidthDp  = 1.f;
    float measuredHeightDp = 1.f;
};

// ---------------------------------------------------------------------------
// 2.5 Layout node and behavior (abstract)
// ---------------------------------------------------------------------------

class LayoutNode;

class OMEGAWTK_EXPORT LayoutBehavior {
public:
    virtual ~LayoutBehavior() = default;
    virtual MeasureResult measure(LayoutNode & node,const LayoutContext & ctx) = 0;
    virtual void arrange(LayoutNode & node,const LayoutContext & ctx) = 0;
};

using LayoutBehaviorPtr = SharedHandle<LayoutBehavior>;

class ViewResizeCoordinator;

// ---------------------------------------------------------------------------
// Resolver free functions (A3)
// ---------------------------------------------------------------------------

OMEGAWTK_EXPORT float resolveLength(const LayoutLength & len,
                                    float availableDp,
                                    float dpiScale);

OMEGAWTK_EXPORT float clampValue(float value,
                                 const LayoutLength & min,
                                 const LayoutLength & max,
                                 float availableDp,
                                 float dpiScale);

OMEGAWTK_EXPORT Core::Rect resolveClampedRect(const LayoutStyle & style,
                                              const Core::Rect & availableDp,
                                              float dpiScale);

// ---------------------------------------------------------------------------
// 6.1 Layout transition types (D1)
// ---------------------------------------------------------------------------

enum class LayoutTransitionProperty : std::uint8_t {
    X,
    Y,
    Width,
    Height,
    Opacity,
    CornerRadius,
    Shadow,
    Blur
};

struct OMEGAWTK_EXPORT LayoutTransitionSpec {
    bool enabled = false;
    float durationSec = 0.f;
    SharedHandle<Composition::AnimationCurve> curve = nullptr;
    OmegaCommon::Vector<LayoutTransitionProperty> properties;
};

struct OMEGAWTK_EXPORT LayoutDelta {
    Core::Rect fromRectPx {};
    Core::Rect toRectPx {};
    OmegaCommon::Vector<LayoutTransitionProperty> changedProperties;
};

OMEGAWTK_EXPORT LayoutDelta computeLayoutDelta(const Core::Rect & fromPx,
                                               const Core::Rect & toPx);

// ---------------------------------------------------------------------------
// F1 — Diagnostic sink
// ---------------------------------------------------------------------------

struct OMEGAWTK_EXPORT LayoutDiagnosticEntry {
    enum class Pass : std::uint8_t {
        Measure,
        Arrange,
        Commit
    };

    OmegaCommon::String nodeId {};
    Core::Rect rectDp {};
    Core::Rect rectPx {};
    Pass pass = Pass::Measure;
};

class OMEGAWTK_EXPORT LayoutDiagnosticSink {
public:
    virtual ~LayoutDiagnosticSink() = default;
    virtual void record(const LayoutDiagnosticEntry & entry) = 0;
};

class OMEGAWTK_EXPORT VectorDiagnosticSink : public LayoutDiagnosticSink {
    OmegaCommon::Vector<LayoutDiagnosticEntry> entries_;
public:
    void record(const LayoutDiagnosticEntry & entry) override;
    const OmegaCommon::Vector<LayoutDiagnosticEntry> & entries() const;
    void clear();
};

// ---------------------------------------------------------------------------
// Style rule helpers
// ---------------------------------------------------------------------------

struct StyleRule {
    enum class Property : std::uint8_t {
        LayoutWidth,
        LayoutHeight,
        LayoutMarginLeft,
        LayoutMarginTop,
        LayoutMarginRight,
        LayoutMarginBottom,
        LayoutPaddingLeft,
        LayoutPaddingTop,
        LayoutPaddingRight,
        LayoutPaddingBottom,
        LayoutClampMinWidth,
        LayoutClampMinHeight,
        LayoutClampMaxWidth,
        LayoutClampMaxHeight,
        LayoutTransition,
        BackgroundColor,
        BorderEnabled,
        BorderColor,
        BorderWidth,
        DropShadow,
        GaussianBlur,
        DirectionalBlur,
        ElementBrush,
        TextFont,
        TextColor,
        TextAlignment,
        TextWrapping,
        TextLineLimit
    };

    OmegaCommon::String selectorTag {};
    int specificity = 0;
    std::size_t sourceOrder = 0;
    Property property = Property::LayoutWidth;

    Core::Optional<LayoutLength> lengthValue {};
    Core::Optional<LayoutEdges> edgesValue {};
    Core::Optional<LayoutClamp> layoutClampValue {};
    Core::Optional<LayoutTransitionSpec> transitionValue {};
    Core::Optional<Composition::Color> colorValue {};
    Core::Optional<bool> boolValue {};
    Core::Optional<float> floatValue {};
    SharedHandle<Composition::Brush> brushValue = nullptr;
    SharedHandle<Composition::Font> fontValue = nullptr;
    Core::Optional<Composition::TextLayoutDescriptor::Alignment> textAlignmentValue {};
    Core::Optional<Composition::TextLayoutDescriptor::Wrapping> textWrappingValue {};
    Core::Optional<unsigned> uintValue {};
    Core::Optional<Composition::LayerEffect::DropShadowParams> dropShadowValue {};
    Core::Optional<Composition::CanvasEffect::GaussianBlurParams> gaussianBlurValue {};
    Core::Optional<Composition::CanvasEffect::DirectionalBlurParams> directionalBlurValue {};

    bool beats(const StyleRule & other) const;
};

struct StyleSheet;

OMEGAWTK_EXPORT OmegaCommon::Vector<StyleRule> convertEntriesToRules(
    const struct StyleSheet & sheet,
    const OmegaCommon::String & viewTag);

OMEGAWTK_EXPORT void mergeLayoutRulesIntoStyle(
    LayoutStyle & style,
    const OmegaCommon::Vector<StyleRule> & rules,
    const OmegaCommon::String & elementTag);

OMEGAWTK_EXPORT Core::Optional<LayoutTransitionSpec> resolveLayoutTransition(
    const OmegaCommon::Vector<StyleRule> & rules,
    const OmegaCommon::String & elementTag);

} // namespace OmegaWTK

#endif // OMEGAWTK_UI_LAYOUT_H
