#include "omegaWTK/Widgets/Primatives.h"

#include <algorithm>
#include <cmath>

namespace OmegaWTK {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a Style that applies a fill brush and optional stroke brush
/// to the element tagged "bg".
static StylePtr makeShapeStyle(
        const SharedHandle<Composition::Brush> & fill,
        const SharedHandle<Composition::Brush> & stroke,
        float strokeWidth) {
    auto ss = Style::Create();
    if (fill) {
        ss->elementBrush("bg", fill);
    }
    if (stroke && strokeWidth > 0.f) {
        ss->border("bg", true);
        ss->borderColor("bg", stroke->color);
        ss->borderWidth("bg", strokeWidth);
    }
    return ss;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Rectangle
// ---------------------------------------------------------------------------

Rectangle::Rectangle(Composition::Rect rect, const RectangleProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "rectangle"))),
      props_(props) {}

void Rectangle::onMount() {
    rebuildContent();
}

void Rectangle::rebuildContent() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(rect());
    lv2.element(spec);

    viewAs<UIView>().setStyle(
        makeShapeStyle(props_.fill, props_.stroke, props_.strokeWidth));
}

void Rectangle::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void Rectangle::setProps(const RectangleProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// RoundedRectangle
// ---------------------------------------------------------------------------

RoundedRectangle::RoundedRectangle(Composition::Rect rect, const RoundedRectangleProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "roundedRectangle"))),
      props_(props) {}

void RoundedRectangle::onMount() {
    rebuildContent();
}

void RoundedRectangle::rebuildContent() {
    float maxR = std::max({props_.topLeft, props_.topRight,
                           props_.bottomLeft, props_.bottomRight});
    Composition::RoundedRect rr;
    rr.pos = rect().pos;
    rr.w = rect().w;
    rr.h = rect().h;
    rr.rad_x = maxR;
    rr.rad_y = maxR;

    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::RoundedRect(rr);
    lv2.element(spec);

    viewAs<UIView>().setStyle(
        makeShapeStyle(props_.fill, props_.stroke, props_.strokeWidth));
}

void RoundedRectangle::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void RoundedRectangle::setProps(const RoundedRectangleProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// Ellipse
// ---------------------------------------------------------------------------

Ellipse::Ellipse(Composition::Rect rect, const EllipseProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "ellipse"))),
      props_(props) {}

void Ellipse::onMount() {
    rebuildContent();
}

void Ellipse::rebuildContent() {
    Composition::Ellipse e{
        rect().pos.x + rect().w * 0.5f,
        rect().pos.y + rect().h * 0.5f,
        rect().w * 0.5f,
        rect().h * 0.5f};

    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Ellipse(e);
    lv2.element(spec);

    viewAs<UIView>().setStyle(
        makeShapeStyle(props_.fill, props_.stroke, props_.strokeWidth));
}

void Ellipse::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void Ellipse::setProps(const EllipseProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

Path::Path(Composition::Rect rect, const PathProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "path"))),
      props_(props) {}

void Path::onMount() {
    rebuildContent();
}

void Path::rebuildContent() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Path(props_.path, props_.strokeWidth, props_.closePath);
    lv2.element(spec);

    viewAs<UIView>().setStyle(
        makeShapeStyle(props_.fill, props_.stroke,
                            static_cast<float>(props_.strokeWidth)));
}

void Path::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void Path::setProps(const PathProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// Separator
// ---------------------------------------------------------------------------

Separator::Separator(Composition::Rect rect, const SeparatorProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "separator"))),
      props_(props) {}

void Separator::onMount() {
    rebuildContent();
}

void Separator::rebuildContent() {
    Composition::Rect r = rect();
    Composition::Rect line;
    if (props_.orientation == Orientation::Horizontal) {
        float center = r.pos.y + r.h * 0.5f - props_.thickness * 0.5f;
        line = {Composition::Point2D{r.pos.x + props_.inset, center},
                r.w - props_.inset * 2.f, props_.thickness};
    } else {
        float center = r.pos.x + r.w * 0.5f - props_.thickness * 0.5f;
        line = {Composition::Point2D{center, r.pos.y + props_.inset},
                props_.thickness, r.h - props_.inset * 2.f};
    }

    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(line);
    lv2.element(spec);

    auto ss = Style::Create();
    if (props_.brush) {
        ss->elementBrush("bg", props_.brush);
    }
    viewAs<UIView>().setStyle(ss);
}

void Separator::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void Separator::setProps(const SeparatorProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------

Label::Label(Composition::Rect rect, const LabelProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "label"))),
      props_(props) {
    // Seed the theme so a followThemeForeground label picks the current
    // Light/Dark foreground on its first paint — onThemeSet only fires on
    // a later *change*. Mirrors Button's construction-time seed.
    theme_ = Native::queryCurrentTheme();
}

void Label::onThemeSet(Native::ThemeDesc & desc) {
    theme_ = desc;
    // Only theme-following labels care about the OS color; a label with a
    // fixed textColor is left untouched (and avoids a needless rebuild).
    if(props_.followThemeForeground){
        rebuildContent();
    }
}

void Label::onMount() {
    rebuildContent();
    // Resize-Clamping §1.7 / Text-Measurement-API-Plan: a wrapping Label's
    // height is content-driven (a function of its width). Install the View
    // `ContentMeasure` hook so the parent's `FlexLayout::measure` sizes this
    // widget to its wrapped text instead of its frozen construction-time
    // rect. The hook delegates to `UIView::measureText`, which runs the real
    // text-layout engine — the same shaping/wrapping the renderer uses — so
    // the reported height matches the glyphs that paint. Units are dp.
    //
    // Height-only: a vertical stack's content-driven dimension is height, so
    // we publish `outHeightDp` and pass the available width straight through
    // (the cross axis is owned by stretch). When `measureText` returns 0 (no
    // text, or no font/shaper resolved), we leave `outHeightDp` at the
    // caller-supplied fallback rather than collapsing the box to zero.
    viewAs<UIView>().setContentMeasure(
        [this](float availWidthDp, float /*availHeightDp*/,
               float & outWidthDp, float & outHeightDp) {
            const float measuredHeight =
                viewAs<UIView>().measureText("label", availWidthDp);
            if (measuredHeight > 0.f) {
                outHeightDp = measuredHeight;
            }
            outWidthDp = availWidthDp;
        });
}

void Label::rebuildContent() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "label";
    spec.text = props_.text;
    // Text-Measurement-API-Plan §5: deliberately DO NOT pin `spec.textRect`.
    // A Label's text fills its view, and the layout pass resizes the view (not
    // the Widget), so a rect baked here would freeze at the construction-time
    // size and clip wrapped text on resize. Leaving it unset makes paint lay
    // the text out against the live view bounds, matching what `measureText`
    // measured.
    lv2.element(spec);

    auto ss = Style::Create();
    if (props_.font) {
        ss->textFont("label", props_.font);
    }
    // Widget-Inline-Default-Strip-Plan Phase L (2026-07-01): author each
    // text cell inline ONLY when the app set it (or, for color, when the
    // theme-following override is active). Cells left unset here fall
    // through to the user-agent stylesheet's `label` defaults during the
    // cascade — the UA sheet is now the source of truth for a default
    // Label, not a shadowed safety net.
    //
    // Color has three cases: `followThemeForeground` wins (dynamic per OS
    // theme, so it cannot live in the static sheet and must be written
    // inline every rebuild); an explicitly authored `textColor` is honored;
    // otherwise the cell is left to the UA sheet (black).
    if (props_.followThemeForeground) {
        ss->textColor("label", theme_.colors.foreground);
    } else if (props_.textColor) {
        ss->textColor("label", *props_.textColor);
    }
    if (props_.alignment) {
        ss->textAlignment("label", *props_.alignment);
    }
    if (props_.wrapping) {
        ss->textWrapping("label", *props_.wrapping);
    }
    if (props_.lineLimit && *props_.lineLimit > 0) {
        ss->textLineLimit("label", *props_.lineLimit);
    }
    viewAs<UIView>().setStyle(ss);
}

void Label::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

// Text-Measurement-API-Plan §3 task 3: the heuristic `Label::measureSelf`
// (charCount × pointSize × 0.6 width, 1.2 line factor) was deleted. It had
// no call site — nothing in the tree invoked `measureSelf` on a Label — and
// its `props_.font`-gating bailed to a no-op for the exact font-less,
// fallback-Arial Labels this path needs to size. Real wrapped height now
// flows through the `ContentMeasure` hook installed in `onMount`, which
// delegates to `UIView::measureText` (the real text-layout engine). The base
// `Widget::measureSelf` remains for widgets that still want the default.

void Label::setText(const OmegaCommon::UString & text) {
    props_.text = text;
    rebuildContent();
    invalidate();
}

void Label::setProps(const LabelProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// Icon
// ---------------------------------------------------------------------------

Icon::Icon(Composition::Rect rect, const IconProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "icon"))),
      props_(props) {}

void Icon::onMount() {
    rebuildContent();
}

void Icon::rebuildContent() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "icon";
    // Convert narrow token string to UString for the text element
    spec.text = OmegaCommon::UString(props_.token.begin(), props_.token.end());
    spec.textRect = rect();
    lv2.element(spec);

    auto ss = Style::Create();
    // Widget-Inline-Default-Strip-Plan Phase L (2026-07-01): author the
    // tint inline only when the app set it; an unset tint falls through to
    // the UA sheet's `icon` text-color default (black). The Icon widget
    // never authored alignment/wrapping/lineLimit, so those already flow
    // from the UA sheet's `icon` rule (MiddleCenter / None / unlimited).
    if (props_.tintColor) {
        ss->textColor("icon", *props_.tintColor);
    }
    viewAs<UIView>().setStyle(ss);
}

void Icon::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void Icon::setProps(const IconProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

namespace {

/// Compute destination rect for an image given the widget bounds, native image
/// dimensions, and the requested fit mode.
static Composition::Rect computeFitRect(const Composition::Rect & bounds,
                                 float imgW, float imgH,
                                 ImageFitMode mode) {
    if (imgW <= 0.f || imgH <= 0.f) return bounds;

    switch (mode) {
    case ImageFitMode::Fill:
        return bounds;

    case ImageFitMode::Center: {
        float cx = bounds.pos.x + (bounds.w - imgW) * 0.5f;
        float cy = bounds.pos.y + (bounds.h - imgH) * 0.5f;
        return {Composition::Point2D{cx, cy}, imgW, imgH};
    }

    case ImageFitMode::Contain: {
        float scale = std::min(bounds.w / imgW, bounds.h / imgH);
        float dw = imgW * scale;
        float dh = imgH * scale;
        float cx = bounds.pos.x + (bounds.w - dw) * 0.5f;
        float cy = bounds.pos.y + (bounds.h - dh) * 0.5f;
        return {Composition::Point2D{cx, cy}, dw, dh};
    }

    case ImageFitMode::Cover:
    case ImageFitMode::Crop: {
        float scale = std::max(bounds.w / imgW, bounds.h / imgH);
        float dw = imgW * scale;
        float dh = imgH * scale;
        float cx = bounds.pos.x + (bounds.w - dw) * 0.5f;
        float cy = bounds.pos.y + (bounds.h - dh) * 0.5f;
        return {Composition::Point2D{cx, cy}, dw, dh};
    }
    }
    return bounds;
}

} // anonymous namespace

Image::Image(Composition::Rect rect, const ImageProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "image"))), props_(props) {}

void Image::onMount() {
    rebuildContent();
}

void Image::rebuildContent() {
    auto & uv = viewAs<UIView>();
    auto & lv2 = uv.layoutV2();
    lv2.clear();

    if (props_.source) {
        float imgW = static_cast<float>(props_.source->header.width);
        float imgH = static_cast<float>(props_.source->header.height);
        Composition::Rect dest = computeFitRect(rect(), imgW, imgH, props_.fitMode);
        UIElementLayoutSpec spec;
        spec.tag = "img";
        spec.image = props_.source;
        spec.imageRect = dest;
        lv2.element(spec);
    }
}

void Image::resize(Composition::Rect & newRect) {
    view->resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

void Image::setProps(const ImageProps & props) {
    props_ = props;
    rebuildContent();
    invalidate();
}

}
