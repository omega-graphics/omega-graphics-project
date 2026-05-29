#include "omegaWTK/UI/SVGView.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/Composition/Animation.h"
#include "omegaWTK/Composition/Path.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/CanvasEffect.h"
#include "omegaWTK/Composition/DisplayList.h"

#include "FrameBuilder.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace OmegaWTK {

namespace {

// ---------------------------------------------------------------------------
// SVG color parser  (hex #RGB / #RRGGBB and the 17 CSS named colors)
// ---------------------------------------------------------------------------

bool tryParseHexColor(const OmegaCommon::String & s, Composition::Color & out) {
    if (s.empty() || s[0] != '#')
        return false;
    unsigned long rgb = 0;
    if (s.size() == 4) {
        rgb = std::strtoul(s.c_str() + 1, nullptr, 16);
        unsigned r = (rgb >> 8) & 0xF;
        unsigned g = (rgb >> 4) & 0xF;
        unsigned b = rgb & 0xF;
        out = Composition::Color::create8Bit(
            static_cast<std::uint8_t>((r << 4) | r),
            static_cast<std::uint8_t>((g << 4) | g),
            static_cast<std::uint8_t>((b << 4) | b),
            0xFF);
        return true;
    }
    if (s.size() == 7) {
        rgb = std::strtoul(s.c_str() + 1, nullptr, 16);
        out = Composition::Color::create8Bit(static_cast<std::uint32_t>(rgb));
        return true;
    }
    return false;
}

Composition::Color parseColor(const OmegaCommon::String & s) {
    if (s.empty() || s == "none")
        return Composition::Color::Transparent;

    Composition::Color c {};
    if (tryParseHexColor(s, c))
        return c;

    if (s == "black")   return Composition::Color::create8Bit(Composition::Color::Black8);
    if (s == "white")   return Composition::Color::create8Bit(Composition::Color::White8);
    if (s == "red")     return Composition::Color::create8Bit(Composition::Color::Red8);
    if (s == "green")   return Composition::Color::create8Bit(Composition::Color::Green8);
    if (s == "blue")    return Composition::Color::create8Bit(Composition::Color::Blue8);
    if (s == "yellow")  return Composition::Color::create8Bit(Composition::Color::Yellow8);
    if (s == "orange")  return Composition::Color::create8Bit(Composition::Color::Orange8);
    if (s == "purple")  return Composition::Color::create8Bit(Composition::Color::Purple8);

    return Composition::Color::create8Bit(Composition::Color::Black8);
}

float parseFloatAttr(Core::XMLDocument::Tag & tag, const char * name, float fallback = 0.f) {
    auto val = tag.attribute(name);
    if (val.size() == 0)
        return fallback;
    return std::strtof(OmegaCommon::String(val).c_str(), nullptr);
}

// Phase 7: OmegaWTK pos.y is top-edge (Y-down) — same as SVG — so no
// flip is needed.

// ---------------------------------------------------------------------------
// SVG path `d` tokenizer & parser  (M/m L/l H/h V/v Z/z, C/c as polyline)
// ---------------------------------------------------------------------------

struct PathTokenizer {
    const char * p;
    const char * end;

    explicit PathTokenizer(const OmegaCommon::String & d)
        : p(d.c_str()), end(d.c_str() + d.size()) {}

    void skipWhitespaceAndCommas() {
        while (p < end && (std::isspace(static_cast<unsigned char>(*p)) || *p == ','))
            ++p;
    }

    bool hasMore() const { return p < end; }

    bool peekCommand(char & cmd) {
        skipWhitespaceAndCommas();
        if (p >= end) return false;
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            cmd = *p++;
            return true;
        }
        return false;
    }

    bool readNumber(float & out) {
        skipWhitespaceAndCommas();
        if (p >= end) return false;
        if (!std::isdigit(static_cast<unsigned char>(*p)) &&
            *p != '-' && *p != '+' && *p != '.')
            return false;
        char * after = nullptr;
        out = std::strtof(p, &after);
        if (after == p) return false;
        p = after;
        return true;
    }

    bool isNextANumber() {
        skipWhitespaceAndCommas();
        if (p >= end) return false;
        return std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.';
    }
};

Composition::Path parseSVGPathData(const OmegaCommon::String & d) {
    PathTokenizer tok(d);

    float curX = 0.f, curY = 0.f;
    float startX = 0.f, startY = 0.f;
    bool started = false;
    Composition::Path path(Composition::Point2D{0.f, 0.f});

    auto ensureStarted = [&](float x, float y) {
        if (!started) {
            path.goTo(Composition::Point2D{x, y});
            startX = x;
            startY = y;
            curX = x;
            curY = y;
            started = true;
        }
    };

    char cmd = 'M';
    while (tok.hasMore()) {
        char nextCmd = 0;
        if (tok.peekCommand(nextCmd)) {
            cmd = nextCmd;
        } else if (!tok.isNextANumber()) {
            break;
        }

        float x, y;
        switch (cmd) {
        case 'M':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            if (started) path.goTo(Composition::Point2D{x, y});
            ensureStarted(x, y);
            curX = x; curY = y;
            startX = x; startY = y;
            cmd = 'L';
            break;
        case 'm':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            x += curX; y += curY;
            if (started) path.goTo(Composition::Point2D{x, y});
            ensureStarted(x, y);
            curX = x; curY = y;
            startX = x; startY = y;
            cmd = 'l';
            break;
        case 'L':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            ensureStarted(x, y);
            path.addLine(Composition::Point2D{x, y});
            curX = x; curY = y;
            break;
        case 'l':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            x += curX; y += curY;
            ensureStarted(x, y);
            path.addLine(Composition::Point2D{x, y});
            curX = x; curY = y;
            break;
        case 'H':
            if (!tok.readNumber(x)) goto done;
            ensureStarted(x, curY);
            path.addLine(Composition::Point2D{x, curY});
            curX = x;
            break;
        case 'h':
            if (!tok.readNumber(x)) goto done;
            x += curX;
            ensureStarted(x, curY);
            path.addLine(Composition::Point2D{x, curY});
            curX = x;
            break;
        case 'V':
            if (!tok.readNumber(y)) goto done;
            ensureStarted(curX, y);
            path.addLine(Composition::Point2D{curX, y});
            curY = y;
            break;
        case 'v':
            if (!tok.readNumber(y)) goto done;
            y += curY;
            ensureStarted(curX, y);
            path.addLine(Composition::Point2D{curX, y});
            curY = y;
            break;
        case 'C': {
            float x1, y1, x2, y2, x3, y3;
            if (!tok.readNumber(x1) || !tok.readNumber(y1) ||
                !tok.readNumber(x2) || !tok.readNumber(y2) ||
                !tok.readNumber(x3) || !tok.readNumber(y3)) goto done;
            ensureStarted(curX, curY);
            path.addLine(Composition::Point2D{x3, y3});
            curX = x3; curY = y3;
            break;
        }
        case 'c': {
            float dx1, dy1, dx2, dy2, dx3, dy3;
            if (!tok.readNumber(dx1) || !tok.readNumber(dy1) ||
                !tok.readNumber(dx2) || !tok.readNumber(dy2) ||
                !tok.readNumber(dx3) || !tok.readNumber(dy3)) goto done;
            ensureStarted(curX, curY);
            float ex = curX + dx3, ey = curY + dy3;
            path.addLine(Composition::Point2D{ex, ey});
            curX = ex; curY = ey;
            break;
        }
        case 'Z':
        case 'z':
            path.close();
            curX = startX; curY = startY;
            break;
        default:
            goto done;
        }
    }
done:
    return path;
}

// ---------------------------------------------------------------------------
// Parse SVG `points` attribute  (polyline / polygon)
// ---------------------------------------------------------------------------

OmegaCommon::Vector<Composition::Point2D> parsePointsList(const OmegaCommon::String & pts) {
    OmegaCommon::Vector<Composition::Point2D> result;
    const char * p = pts.c_str();
    const char * end = p + pts.size();
    auto skip = [&]() {
        while (p < end && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
    };
    while (p < end) {
        skip();
        char * after = nullptr;
        float x = std::strtof(p, &after);
        if (after == p) break;
        p = after;
        skip();
        float y = std::strtof(p, &after);
        if (after == p) break;
        p = after;
        result.push_back(Composition::Point2D{x, y});
    }
    return result;
}

// ---------------------------------------------------------------------------
// Style attribute decoding — produced once per shape at parse time and
// folded directly into the DrawOp.
// ---------------------------------------------------------------------------

struct ResolvedStyle {
    Composition::Color fillColor   = Composition::Color::create8Bit(Composition::Color::Black8);
    float              fillOpacity = 1.f;
    Composition::Color strokeColor {};
    float              strokeWidth = 0.f;
    float              strokeOpacity = 1.f;

    bool hasFill()   const { return fillOpacity   > 0.f; }
    bool hasStroke() const { return strokeWidth   > 0.f && strokeOpacity > 0.f; }

    Core::SharedPtr<Composition::Brush> fillBrush() const {
        auto c = fillColor; c.a = fillOpacity;
        return Composition::ColorBrush(c);
    }
    Core::SharedPtr<Composition::Brush> strokeBrush() const {
        auto c = strokeColor; c.a = strokeOpacity;
        return Composition::ColorBrush(c);
    }
    Core::Optional<Composition::Border> border() const {
        if (!hasStroke()) return std::nullopt;
        auto brush = strokeBrush();
        return Composition::Border{brush, static_cast<unsigned>(strokeWidth)};
    }
};

ResolvedStyle parseStyleAttrs(Core::XMLDocument::Tag & tag) {
    ResolvedStyle s {};
    auto fill = tag.attribute("fill");
    if (fill.size() != 0) {
        OmegaCommon::String fs(fill);
        if (fs == "none")
            s.fillOpacity = 0.f;
        else
            s.fillColor = parseColor(fs);
    }
    auto fo = tag.attribute("fill-opacity");
    if (fo.size() != 0)
        s.fillOpacity = std::strtof(OmegaCommon::String(fo).c_str(), nullptr);

    auto stroke = tag.attribute("stroke");
    if (stroke.size() != 0) {
        OmegaCommon::String ss(stroke);
        if (ss == "none")
            s.strokeWidth = 0.f;
        else
            s.strokeColor = parseColor(ss);
    }
    auto sw = tag.attribute("stroke-width");
    if (sw.size() != 0)
        s.strokeWidth = std::strtof(OmegaCommon::String(sw).c_str(), nullptr);
    auto so = tag.attribute("stroke-opacity");
    if (so.size() != 0)
        s.strokeOpacity = std::strtof(OmegaCommon::String(so).c_str(), nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Helpers: pack a Path with the fill brush + stroke width and wrap it
// in a `DrawOp::VectorPath`. Mirrors the `Canvas::drawPath` shape: the
// Path carries the fill via `pathBrush`; the Border carries the stroke.
// ---------------------------------------------------------------------------

void emitPath(Composition::DisplayList & list,
              Composition::Path && rawPath,
              const ResolvedStyle & s,
              bool closePath) {
    if (closePath) rawPath.close();
    auto path = std::make_shared<Composition::Path>(std::move(rawPath));

    // SDF/triangulator pipeline reads stroke width off the Path itself
    // for vector ops; setting it here keeps the Canvas::drawPath
    // contract intact during replay.
    if (s.hasStroke()) path->setStroke(s.strokeWidth);
    if (s.hasFill()) {
        auto fill = s.fillBrush();
        path->setPathBrush(fill);
    }
    if (!s.hasFill() && !s.hasStroke()) return;
    list.append(Composition::DrawOp{std::move(path), s.border()});
}

// Stroke-only shapes (e.g. `<rect>` with `fill="none"`) need a
// transparent fill brush so the single-pass SDF draw emits just the
// stroke band. Matches the prior fill-then-frame substitution.
Core::SharedPtr<Composition::Brush> transparentFill() {
    return Composition::ColorBrush(Composition::Color{0.f, 0.f, 0.f, 0.f});
}

// ---------------------------------------------------------------------------
// Recursive element walker — appends one DrawOp per SVG shape.
// ---------------------------------------------------------------------------

void walkElement(Core::XMLDocument::Tag & tag, Composition::DisplayList & list) {
    auto name = tag.name();
    if (!tag.isElement())
        return;

    if (name == "rect") {
        float x  = parseFloatAttr(tag, "x");
        float y  = parseFloatAttr(tag, "y");
        float w  = parseFloatAttr(tag, "width");
        float h  = parseFloatAttr(tag, "height");
        float rx = parseFloatAttr(tag, "rx");
        float ry = parseFloatAttr(tag, "ry");
        auto s = parseStyleAttrs(tag);
        if (!s.hasFill() && !s.hasStroke()) return;
        auto brush = s.hasFill() ? s.fillBrush() : transparentFill();
        if (rx > 0.f || ry > 0.f) {
            if (rx == 0.f) rx = ry;
            if (ry == 0.f) ry = rx;
            Composition::RoundedRect rr{Composition::Point2D{x, y}, w, h, rx, ry};
            list.append(Composition::DrawOp{rr, std::move(brush), s.border()});
        } else {
            Composition::Rect r{Composition::Point2D{x, y}, w, h};
            list.append(Composition::DrawOp{r, std::move(brush), s.border()});
        }
    }
    else if (name == "circle" || name == "ellipse") {
        float cx = parseFloatAttr(tag, "cx");
        float cy = parseFloatAttr(tag, "cy");
        float rx, ry;
        if (name == "circle") {
            rx = ry = parseFloatAttr(tag, "r");
        } else {
            rx = parseFloatAttr(tag, "rx");
            ry = parseFloatAttr(tag, "ry");
        }
        auto s = parseStyleAttrs(tag);
        if (!s.hasFill() && !s.hasStroke()) return;
        auto brush = s.hasFill() ? s.fillBrush() : transparentFill();
        Composition::Ellipse e{cx, cy, rx, ry};
        list.append(Composition::DrawOp{e, std::move(brush), s.border()});
    }
    else if (name == "line") {
        float x1 = parseFloatAttr(tag, "x1");
        float y1 = parseFloatAttr(tag, "y1");
        float x2 = parseFloatAttr(tag, "x2");
        float y2 = parseFloatAttr(tag, "y2");
        auto s = parseStyleAttrs(tag);
        // `<line>` is stroke-only by SVG semantics; force the fill off
        // and ensure a default stroke width so the line is visible.
        s.fillOpacity = 0.f;
        if (s.strokeWidth == 0.f) s.strokeWidth = 1.f;
        if (!s.hasStroke()) return;
        Composition::Path p(Composition::Point2D{x1, y1});
        p.addLine(Composition::Point2D{x2, y2});
        emitPath(list, std::move(p), s, /*closePath=*/false);
    }
    else if (name == "polyline" || name == "polygon") {
        auto pts = parsePointsList(OmegaCommon::String(tag.attribute("points")));
        if (pts.size() < 2) return;
        auto s = parseStyleAttrs(tag);
        const bool closed = (name == "polygon");
        // `<polyline>` has no implicit fill — only `<polygon>` does.
        if (!closed) s.fillOpacity = 0.f;
        if (!s.hasFill() && !s.hasStroke()) return;
        Composition::Path p(pts[0]);
        for (std::size_t i = 1; i < pts.size(); ++i)
            p.addLine(pts[i]);
        emitPath(list, std::move(p), s, closed);
    }
    else if (name == "path") {
        auto d = tag.attribute("d");
        if (d.size() == 0) return;
        auto s = parseStyleAttrs(tag);
        if (!s.hasFill() && !s.hasStroke()) return;
        auto p = parseSVGPathData(OmegaCommon::String(d));
        emitPath(list, std::move(p), s, /*closePath=*/false);
    }
    else if (name == "g" || name == "svg") {
        auto children = tag.children();
        for (auto & child : children)
            walkElement(child, list);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SVGView implementation
// ---------------------------------------------------------------------------

SVGView::SVGView(const Composition::Rect & rect, ViewPtr parent)
    : View(rect, parent),
      cachedOps_(std::make_unique<Composition::DisplayList>()) {
}

SVGView::~SVGView() = default;

void SVGView::setDelegate(SVGViewDelegate * delegate) {
    delegate_ = delegate;
}

void SVGView::setRenderOptions(const SVGViewRenderOptions & options) {
    options_ = options;
    needsRebuild_ = true;
}

const SVGViewRenderOptions & SVGView::renderOptions() const {
    return options_;
}

void SVGView::rebuildDisplayList() {
    cachedOps_->clear();
    if (!sourceDoc_.has_value())
        return;
    auto root = sourceDoc_->root();
    auto children = root.children();
    for (auto & child : children)
        walkElement(child, *cachedOps_);
    needsRebuild_ = false;
}

bool SVGView::setSourceDocument(Core::XMLDocument doc) {
    sourceDoc_.emplace(std::move(doc));
    needsRebuild_ = true;
    if (delegate_)
        delegate_->onSVGLoaded();
    return true;
}

bool SVGView::setSourceString(const OmegaCommon::String & svgString) {
    try {
        sourceDoc_.emplace(Core::XMLDocument::parseFromString(svgString));
        needsRebuild_ = true;
        if (delegate_)
            delegate_->onSVGLoaded();
        return true;
    } catch (...) {
        if (delegate_)
            delegate_->onSVGParseError("Failed to parse SVG from string");
        return false;
    }
}

bool SVGView::setSourceStream(std::istream & stream) {
    try {
        sourceDoc_.emplace(Core::XMLDocument::parseFromStream(stream));
        needsRebuild_ = true;
        if (delegate_)
            delegate_->onSVGLoaded();
        return true;
    } catch (...) {
        if (delegate_)
            delegate_->onSVGParseError("Failed to parse SVG from stream");
        return false;
    }
}

// ---------------------------------------------------------------------------
// Rendering pipeline (Phase 2.3)
// ---------------------------------------------------------------------------

void SVGView::paint() {
    if (needsRebuild_)
        rebuildDisplayList();

    startCompositionSession();

    auto white = Composition::Color::create8Bit(Composition::Color::White8);

    // Tier 3 Phase 3.4: push this SVGView's absolute window offset
    // onto the FrameBuilder accumulator for the lifetime of paint().
    // Both branches resolve `View::computeWindowOffset` for this
    // view (the on-flag branch via submitView, the off-flag branch
    // via Canvas::nextFrame's `ownerView_->computeWindowOffset` at
    // sendFrame time); the wrapper returns `fb->currentOffset()`
    // whenever a FrameBuilder is active, so both paths need this
    // view on top of the stack for the stamped offset to be right.
    FrameBuilder::ScopedViewOffset offsetScope(
        AppWindow::activeFrameBuilder(), this);

    // Tier 3 Phase 3.8: window-scoped paint is the only route. Hand the
    // cached DisplayList to the FrameBuilder bracketing this paint pass.
    // The window canvas's frame.background is shared across all
    // submissions for the frame (other views may set it for their own
    // backgrounds), so the SVG white background is prepended as an
    // explicit Rect op in local coordinates.
    if (auto * fb = AppWindow::activeFrameBuilder(); fb != nullptr) {
        Composition::DisplayList list;
        const auto & viewRect = getRect();
        Composition::Rect localBg{
            Composition::Point2D{0.f, 0.f}, viewRect.w, viewRect.h};
        list.append(Composition::DrawOp{localBg, Composition::ColorBrush(white)});
        for (const auto & op : cachedOps_->ops())
            list.append(op);
        fb->submitView(this, std::move(list));
    }

    endCompositionSession();
}

void SVGView::resize(Composition::Rect newRect) {
    View::resize(newRect);
    needsRebuild_ = true;
    paint();
}

} // namespace OmegaWTK
