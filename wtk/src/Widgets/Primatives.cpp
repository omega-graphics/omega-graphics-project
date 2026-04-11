#include "omegaWTK/Widgets/Primatives.h"
#include "omegaWTK/UI/CanvasView.h"

#include <algorithm>
#include <cmath>

namespace OmegaWTK {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a StyleSheet that applies a fill brush and optional stroke brush
/// to the element tagged "bg".
static StyleSheetPtr makeShapeStyleSheet(
        const SharedHandle<Composition::Brush> & fill,
        const SharedHandle<Composition::Brush> & stroke,
        float strokeWidth) {
    auto ss = StyleSheet::Create();
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
    auto & lv2 = viewAs<UIView>().layoutV2();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(rect());
    lv2.element(spec);

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
    viewAs<UIView>().update();
}

void Rectangle::onPaint(PaintReason reason) {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(rect());
    lv2.element(spec);

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
    viewAs<UIView>().update();
}

void Rectangle::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

void Rectangle::setProps(const RectangleProps & props) {
    props_ = props;
    invalidate();
}

// ---------------------------------------------------------------------------
// RoundedRectangle
// ---------------------------------------------------------------------------

RoundedRectangle::RoundedRectangle(Composition::Rect rect, const RoundedRectangleProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "roundedRectangle"))),
      props_(props) {}

void RoundedRectangle::onMount() {
    float maxR = std::max({props_.topLeft, props_.topRight,
                           props_.bottomLeft, props_.bottomRight});
    Composition::RoundedRect rr;
    rr.pos = rect().pos;
    rr.w = rect().w;
    rr.h = rect().h;
    rr.rad_x = maxR;
    rr.rad_y = maxR;

    auto & lv2 = viewAs<UIView>().layoutV2();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::RoundedRect(rr);
    lv2.element(spec);

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
    viewAs<UIView>().update();
}

void RoundedRectangle::onPaint(PaintReason reason) {
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

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
    viewAs<UIView>().update();
}

void RoundedRectangle::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

void RoundedRectangle::setProps(const RoundedRectangleProps & props) {
    props_ = props;
    invalidate();
}

// ---------------------------------------------------------------------------
// Ellipse
// ---------------------------------------------------------------------------

Ellipse::Ellipse(Composition::Rect rect, const EllipseProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "ellipse"))),
      props_(props) {}

void Ellipse::onMount() {
    Composition::Ellipse e{
        rect().pos.x + rect().w * 0.5f,
        rect().pos.y + rect().h * 0.5f,
        rect().w * 0.5f,
        rect().h * 0.5f};

    auto & lv2 = viewAs<UIView>().layoutV2();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Ellipse(e);
    lv2.element(spec);

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
    viewAs<UIView>().update();
}

void Ellipse::onPaint(PaintReason reason) {
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

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
    viewAs<UIView>().update();
}

void Ellipse::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

void Ellipse::setProps(const EllipseProps & props) {
    props_ = props;
    invalidate();
}

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

Path::Path(Composition::Rect rect, const PathProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "path"))),
      props_(props) {}

void Path::onMount() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Path(props_.path, props_.strokeWidth, props_.closePath);
    lv2.element(spec);

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke,
                            static_cast<float>(props_.strokeWidth)));
    viewAs<UIView>().update();
}

void Path::onPaint(PaintReason reason) {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Path(props_.path, props_.strokeWidth, props_.closePath);
    lv2.element(spec);

    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke,
                            static_cast<float>(props_.strokeWidth)));
    viewAs<UIView>().update();
}

void Path::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

void Path::setProps(const PathProps & props) {
    props_ = props;
    invalidate();
}

// ---------------------------------------------------------------------------
// Separator
// ---------------------------------------------------------------------------

Separator::Separator(Composition::Rect rect, const SeparatorProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "separator"))),
      props_(props) {}

void Separator::onMount() {
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
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(line);
    lv2.element(spec);

    auto ss = StyleSheet::Create();
    if (props_.brush) {
        ss->elementBrush("bg", props_.brush);
    }
    viewAs<UIView>().setStyleSheet(ss);
    viewAs<UIView>().update();
}

void Separator::onPaint(PaintReason reason) {
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

    auto ss = StyleSheet::Create();
    if (props_.brush) {
        ss->elementBrush("bg", props_.brush);
    }
    viewAs<UIView>().setStyleSheet(ss);
    viewAs<UIView>().update();
}

void Separator::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

void Separator::setProps(const SeparatorProps & props) {
    props_ = props;
    invalidate();
}

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------

Label::Label(Composition::Rect rect, const LabelProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "label"))),
      props_(props) {}

void Label::onMount() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    UIElementLayoutSpec spec;
    spec.tag = "label";
    spec.text = props_.text;
    spec.textRect = rect();
    lv2.element(spec);

    auto ss = StyleSheet::Create();
    if (props_.font) {
        ss->textFont("label", props_.font);
    }
    ss->textColor("label", props_.textColor);
    ss->textAlignment("label", props_.alignment);
    ss->textWrapping("label", props_.wrapping);
    if (props_.lineLimit > 0) {
        ss->textLineLimit("label", props_.lineLimit);
    }
    viewAs<UIView>().setStyleSheet(ss);
    viewAs<UIView>().update();
}

void Label::onPaint(PaintReason reason) {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "label";
    spec.text = props_.text;
    spec.textRect = rect();
    lv2.element(spec);

    auto ss = StyleSheet::Create();
    if (props_.font) {
        ss->textFont("label", props_.font);
    }
    ss->textColor("label", props_.textColor);
    ss->textAlignment("label", props_.alignment);
    ss->textWrapping("label", props_.wrapping);
    if (props_.lineLimit > 0) {
        ss->textLineLimit("label", props_.lineLimit);
    }
    viewAs<UIView>().setStyleSheet(ss);
    viewAs<UIView>().update();
}

void Label::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

MeasureResult Label::measureSelf(const LayoutContext & ctx) {
    // Heuristic estimate until Phase 2A adds Font::measureText.
    if (!props_.font || props_.text.empty()) {
        return {rect().w / ctx.dpiScale, rect().h / ctx.dpiScale};
    }
    float fontSize = static_cast<float>(props_.font->desc.size);
    float charCount = static_cast<float>(props_.text.size());
    float estWidth = charCount * fontSize * 0.6f;
    float estHeight = fontSize * 1.2f;

    float availWidth = ctx.availableRectPx.w;
    if (props_.wrapping != Composition::TextLayoutDescriptor::None &&
        availWidth > 0.f && estWidth > availWidth) {
        float lines = std::ceil(estWidth / availWidth);
        if (props_.lineLimit > 0) {
            lines = std::min(lines, static_cast<float>(props_.lineLimit));
        }
        estWidth = availWidth;
        estHeight = lines * fontSize * 1.2f;
    }
    return {estWidth / ctx.dpiScale, estHeight / ctx.dpiScale};
}

void Label::setText(const OmegaCommon::UString & text) {
    props_.text = text;
    invalidate();
}

void Label::setProps(const LabelProps & props) {
    props_ = props;
    invalidate();
}

// ---------------------------------------------------------------------------
// Icon
// ---------------------------------------------------------------------------

Icon::Icon(Composition::Rect rect, const IconProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "icon"))),
      props_(props) {}

void Icon::onMount() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    UIElementLayoutSpec spec;
    spec.tag = "icon";
    // Convert narrow token string to UString for the text element
    spec.text = OmegaCommon::UString(props_.token.begin(), props_.token.end());
    spec.textRect = rect();
    lv2.element(spec);

    auto ss = StyleSheet::Create();
    ss->textColor("icon", props_.tintColor);
    viewAs<UIView>().setStyleSheet(ss);
    viewAs<UIView>().update();
}

void Icon::onPaint(PaintReason reason) {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "icon";
    spec.text = OmegaCommon::UString(props_.token.begin(), props_.token.end());
    spec.textRect = rect();
    lv2.element(spec);

    auto ss = StyleSheet::Create();
    ss->textColor("icon", props_.tintColor);
    viewAs<UIView>().setStyleSheet(ss);
    viewAs<UIView>().update();
}

void Icon::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    invalidate(PaintReason::Resize);
}

void Icon::setProps(const IconProps & props) {
    props_ = props;
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
    : Widget(rect), props_(props) {}

void Image::onPaint(PaintReason reason) {
    auto & cv = viewAs<CanvasView>();
    cv.clear(Composition::Color::Transparent);
    if (!props_.source) return;

    float imgW = static_cast<float>(props_.source->header.width);
    float imgH = static_cast<float>(props_.source->header.height);
    Composition::Rect dest = computeFitRect(rect(), imgW, imgH, props_.fitMode);
    cv.drawImage(props_.source, dest);
}

void Image::resize(Composition::Rect & newRect) {
    view->resize(newRect);
    invalidate(PaintReason::Resize);
}

void Image::setProps(const ImageProps & props) {
    props_ = props;
    invalidate();
}

}
