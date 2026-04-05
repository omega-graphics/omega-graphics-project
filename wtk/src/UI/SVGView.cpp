#include "omegaWTK/UI/SVGView.h"
#include "omegaWTK/Composition/Animation.h"
#include "omegaWTK/Composition/Path.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/Canvas.h"

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

/// Flip a Y coordinate from SVG top-down to OmegaWTK bottom-up.
float flipY(float y, float svgHeight) { return svgHeight - y; }

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

Composition::Path parseSVGPathData(const OmegaCommon::String & d, float svgH) {
    PathTokenizer tok(d);

    // curX/curY track the current point in SVG (Y-down) space.
    // All absolute Y values are flipped when emitted to the Path.
    // Relative deltas invert only their Y component.
    float curX = 0.f, curY = 0.f;
    float startX = 0.f, startY = 0.f;
    bool started = false;
    Composition::Path path(OmegaGTE::GPoint2D{0.f, 0.f});

    auto emit = [&](float ax, float ay) -> OmegaGTE::GPoint2D {
        return {ax, flipY(ay, svgH)};
    };

    auto ensureStarted = [&](float x, float y) {
        if (!started) {
            path.goTo(emit(x, y));
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
            if (started) path.goTo(emit(x, y));
            ensureStarted(x, y);
            curX = x; curY = y;
            startX = x; startY = y;
            cmd = 'L';
            break;
        case 'm':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            x += curX; y += curY;
            if (started) path.goTo(emit(x, y));
            ensureStarted(x, y);
            curX = x; curY = y;
            startX = x; startY = y;
            cmd = 'l';
            break;
        case 'L':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            ensureStarted(x, y);
            path.addLine(emit(x, y));
            curX = x; curY = y;
            break;
        case 'l':
            if (!tok.readNumber(x) || !tok.readNumber(y)) goto done;
            x += curX; y += curY;
            ensureStarted(x, y);
            path.addLine(emit(x, y));
            curX = x; curY = y;
            break;
        case 'H':
            if (!tok.readNumber(x)) goto done;
            ensureStarted(x, curY);
            path.addLine(emit(x, curY));
            curX = x;
            break;
        case 'h':
            if (!tok.readNumber(x)) goto done;
            x += curX;
            ensureStarted(x, curY);
            path.addLine(emit(x, curY));
            curX = x;
            break;
        case 'V':
            if (!tok.readNumber(y)) goto done;
            ensureStarted(curX, y);
            path.addLine(emit(curX, y));
            curY = y;
            break;
        case 'v':
            if (!tok.readNumber(y)) goto done;
            y += curY;
            ensureStarted(curX, y);
            path.addLine(emit(curX, y));
            curY = y;
            break;
        case 'C': {
            float x1, y1, x2, y2, x3, y3;
            if (!tok.readNumber(x1) || !tok.readNumber(y1) ||
                !tok.readNumber(x2) || !tok.readNumber(y2) ||
                !tok.readNumber(x3) || !tok.readNumber(y3)) goto done;
            ensureStarted(curX, curY);
            path.addLine(emit(x3, y3));
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
            path.addLine(emit(ex, ey));
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

OmegaCommon::Vector<OmegaGTE::GPoint2D> parsePointsList(const OmegaCommon::String & pts, float svgH) {
    OmegaCommon::Vector<OmegaGTE::GPoint2D> result;
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
        result.push_back(OmegaGTE::GPoint2D{x, flipY(y, svgH)});
    }
    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SVGDrawOp  -- the display-list entry
// ---------------------------------------------------------------------------

struct SVGDrawOp {
    enum class Type : int {
        Rect,
        RoundedRect,
        Ellipse,
        Path,
        Line
    };

    Type type;

    Core::Rect rectGeom {};
    Core::RoundedRect roundedRectGeom {};
    Core::Ellipse ellipseGeom {};
    Core::Optional<Composition::Path> pathGeom;

    Composition::Color fillColor {};
    float fillOpacity = 1.f;
    Composition::Color strokeColor {};
    float strokeWidth = 0.f;
    float strokeOpacity = 1.f;
};

struct SVGDrawOpList {
    OmegaCommon::Vector<SVGDrawOp> ops;
};

// ---------------------------------------------------------------------------
// Style attribute helpers
// ---------------------------------------------------------------------------

namespace {

void parseStyleAttrs(Core::XMLDocument::Tag & tag, SVGDrawOp & op) {
    auto fill = tag.attribute("fill");
    if (fill.size() != 0) {
        OmegaCommon::String fs(fill);
        if (fs == "none")
            op.fillOpacity = 0.f;
        else
            op.fillColor = parseColor(fs);
    } else {
        op.fillColor = Composition::Color::create8Bit(Composition::Color::Black8);
    }

    auto fo = tag.attribute("fill-opacity");
    if (fo.size() != 0)
        op.fillOpacity = std::strtof(OmegaCommon::String(fo).c_str(), nullptr);

    auto stroke = tag.attribute("stroke");
    if (stroke.size() != 0) {
        OmegaCommon::String ss(stroke);
        if (ss == "none")
            op.strokeWidth = 0.f;
        else
            op.strokeColor = parseColor(ss);
    }

    auto sw = tag.attribute("stroke-width");
    if (sw.size() != 0)
        op.strokeWidth = std::strtof(OmegaCommon::String(sw).c_str(), nullptr);

    auto so = tag.attribute("stroke-opacity");
    if (so.size() != 0)
        op.strokeOpacity = std::strtof(OmegaCommon::String(so).c_str(), nullptr);
}

// ---------------------------------------------------------------------------
// Recursive element walker
// ---------------------------------------------------------------------------

void walkElement(Core::XMLDocument::Tag & tag, OmegaCommon::Vector<SVGDrawOp> & ops, float svgH) {
    auto name = tag.name();
    if (!tag.isElement())
        return;

    if (name == "rect") {
        SVGDrawOp op {};
        float x  = parseFloatAttr(tag, "x");
        float y  = parseFloatAttr(tag, "y");
        float w  = parseFloatAttr(tag, "width");
        float h  = parseFloatAttr(tag, "height");
        float rx = parseFloatAttr(tag, "rx");
        float ry = parseFloatAttr(tag, "ry");
        // SVG y is top edge; OmegaWTK pos.y is bottom edge (Y-up).
        float fy = flipY(y + h, svgH);
        if (rx > 0.f || ry > 0.f) {
            if (rx == 0.f) rx = ry;
            if (ry == 0.f) ry = rx;
            op.type = SVGDrawOp::Type::RoundedRect;
            op.roundedRectGeom = Core::RoundedRect{Core::Position{x, fy}, w, h, rx, ry};
        } else {
            op.type = SVGDrawOp::Type::Rect;
            op.rectGeom = Core::Rect{Core::Position{x, fy}, w, h};
        }
        parseStyleAttrs(tag, op);
        ops.push_back(std::move(op));
    }
    else if (name == "circle") {
        SVGDrawOp op {};
        float cx = parseFloatAttr(tag, "cx");
        float cy = parseFloatAttr(tag, "cy");
        float r  = parseFloatAttr(tag, "r");
        op.type = SVGDrawOp::Type::Ellipse;
        op.ellipseGeom = Core::Ellipse(cx, flipY(cy, svgH), r, r);
        parseStyleAttrs(tag, op);
        ops.push_back(std::move(op));
    }
    else if (name == "ellipse") {
        SVGDrawOp op {};
        float cx = parseFloatAttr(tag, "cx");
        float cy = parseFloatAttr(tag, "cy");
        float rx = parseFloatAttr(tag, "rx");
        float ry = parseFloatAttr(tag, "ry");
        op.type = SVGDrawOp::Type::Ellipse;
        op.ellipseGeom = Core::Ellipse(cx, flipY(cy, svgH), rx, ry);
        parseStyleAttrs(tag, op);
        ops.push_back(std::move(op));
    }
    else if (name == "line") {
        SVGDrawOp op {};
        float x1 = parseFloatAttr(tag, "x1");
        float y1 = parseFloatAttr(tag, "y1");
        float x2 = parseFloatAttr(tag, "x2");
        float y2 = parseFloatAttr(tag, "y2");
        op.type = SVGDrawOp::Type::Path;
        Composition::Path p(OmegaGTE::GPoint2D{x1, flipY(y1, svgH)});
        p.addLine(OmegaGTE::GPoint2D{x2, flipY(y2, svgH)});
        op.pathGeom.emplace(std::move(p));
        parseStyleAttrs(tag, op);
        if (op.strokeWidth == 0.f)
            op.strokeWidth = 1.f;
        ops.push_back(std::move(op));
    }
    else if (name == "polyline" || name == "polygon") {
        auto pts = parsePointsList(OmegaCommon::String(tag.attribute("points")), svgH);
        if (pts.size() >= 2) {
            SVGDrawOp op {};
            op.type = SVGDrawOp::Type::Path;
            Composition::Path p(pts[0]);
            for (size_t i = 1; i < pts.size(); ++i)
                p.addLine(pts[i]);
            if (name == "polygon")
                p.close();
            op.pathGeom.emplace(std::move(p));
            parseStyleAttrs(tag, op);
            ops.push_back(std::move(op));
        }
    }
    else if (name == "path") {
        auto d = tag.attribute("d");
        if (d.size() != 0) {
            SVGDrawOp op {};
            op.type = SVGDrawOp::Type::Path;
            op.pathGeom.emplace(parseSVGPathData(OmegaCommon::String(d), svgH));
            parseStyleAttrs(tag, op);
            ops.push_back(std::move(op));
        }
    }
    else if (name == "g" || name == "svg") {
        auto children = tag.children();
        for (auto & child : children)
            walkElement(child, ops, svgH);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SVGView implementation
// ---------------------------------------------------------------------------

SVGView::SVGView(const Core::Rect & rect, ViewPtr parent)
    : View(rect, parent),
      drawOps_(std::make_unique<SVGDrawOpList>()) {
    svgCanvas = makeCanvas(getLayerTree()->getRootLayer());
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
    drawOps_->ops.clear();
    if (!sourceDoc_.has_value())
        return;
    auto root = sourceDoc_->root();
    // Read SVG document height for Y-axis flip (SVG Y-down → OmegaWTK Y-up).
    // Fall back to the view's own height if the attribute is missing.
    float svgH = parseFloatAttr(root, "height", getRect().h);
    auto children = root.children();
    for (auto & child : children)
        walkElement(child, drawOps_->ops, svgH);
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
// Rendering pipeline
// ---------------------------------------------------------------------------

void SVGView::renderNow() {
    if (needsRebuild_)
        rebuildDisplayList();

    startCompositionSession();

    // White background (SVG default, matching browser behaviour).
    {
        auto & bg = svgCanvas->getCurrentFrame()->background;
        auto white = Composition::Color::create8Bit(Composition::Color::White8);
        bg.r = white.r; bg.g = white.g; bg.b = white.b; bg.a = white.a;
    }

    for (auto & op : drawOps_->ops) {
        bool hasFill   = op.fillOpacity > 0.f;
        bool hasStroke = op.strokeWidth > 0.f && op.strokeOpacity > 0.f;

        auto makeFillBrush = [&]() -> Core::SharedPtr<Composition::Brush> {
            Composition::Color c = op.fillColor;
            c.a = op.fillOpacity;
            return Composition::ColorBrush(c);
        };

        switch (op.type) {
        case SVGDrawOp::Type::Rect: {
            if (hasFill) {
                auto brush = makeFillBrush();
                svgCanvas->drawRect(op.rectGeom, brush);
            }
            if (hasStroke) {
                Composition::Color sc = op.strokeColor;
                sc.a = op.strokeOpacity;
                auto strokeBrush = Composition::ColorBrush(sc);
                Core::RoundedRect rr{op.rectGeom.pos, op.rectGeom.w, op.rectGeom.h, 0.f, 0.f};
                auto frame = Composition::RoundedRectFrame(rr, static_cast<unsigned>(op.strokeWidth));
                frame->setPathBrush(strokeBrush);
                svgCanvas->drawPath(*frame);
            }
            break;
        }
        case SVGDrawOp::Type::RoundedRect: {
            if (hasFill) {
                auto brush = makeFillBrush();
                svgCanvas->drawRoundedRect(op.roundedRectGeom, brush);
            }
            if (hasStroke) {
                Composition::Color sc = op.strokeColor;
                sc.a = op.strokeOpacity;
                auto strokeBrush = Composition::ColorBrush(sc);
                auto frame = Composition::RoundedRectFrame(op.roundedRectGeom, static_cast<unsigned>(op.strokeWidth));
                frame->setPathBrush(strokeBrush);
                svgCanvas->drawPath(*frame);
            }
            break;
        }
        case SVGDrawOp::Type::Ellipse: {
            if (hasFill) {
                auto brush = makeFillBrush();
                svgCanvas->drawEllipse(op.ellipseGeom, brush);
            }
            if (hasStroke) {
                Composition::Color sc = op.strokeColor;
                sc.a = op.strokeOpacity;
                auto strokeBrush = Composition::ColorBrush(sc);
                auto frame = Composition::EllipseFrame(op.ellipseGeom, static_cast<unsigned>(op.strokeWidth));
                frame->setPathBrush(strokeBrush);
                svgCanvas->drawPath(*frame);
            }
            break;
        }
        case SVGDrawOp::Type::Path: {
            if (op.pathGeom.has_value()) {
                auto & p = op.pathGeom.value();
                if (hasFill) {
                    auto brush = makeFillBrush();
                    p.setPathBrush(brush);
                    p.setStroke(0);
                    svgCanvas->drawPath(p);
                }
                if (hasStroke) {
                    Composition::Color sc = op.strokeColor;
                    sc.a = op.strokeOpacity;
                    auto strokeBrush = Composition::ColorBrush(sc);
                    p.setPathBrush(strokeBrush);
                    p.setStroke(static_cast<unsigned>(op.strokeWidth));
                    svgCanvas->drawPath(p);
                }
            }
            break;
        }
        default:
            break;
        }
    }

    svgCanvas->sendFrame();
    endCompositionSession();
}

void SVGView::resize(Core::Rect newRect) {
    View::resize(newRect);
    needsRebuild_ = true;
    renderNow();
}

} // namespace OmegaWTK
