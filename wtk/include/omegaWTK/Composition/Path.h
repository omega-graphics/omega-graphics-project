#include "omegaWTK/Core/Core.h"
#include "Geometry.h"
#include "GTEForward.h"

#ifndef OMEGAWTK_COMPOSITION_PATH_H
#define OMEGAWTK_COMPOSITION_PATH_H

namespace OmegaWTK::Composition {

struct Brush;

/// One drawable sub-path produced by `Path::decomposeForDraw`: a single
/// segment's tessellation-ready vector path plus the resolved stroke /
/// fill brushes and flags. Mirrors the shape the compositor backend's
/// VectorPath rasterizer consumes (the former `VisualCommand::pathParams`)
/// so the Path -> segments+brushes decomposition has exactly one home,
/// shared by `Canvas::drawPath` (Tier 3 transitional) and the backend
/// `DrawOp::VectorPath` arm (Tier 4). See UIView-Render-Redesign-Plan
/// §4.0 resolution.
struct PathDrawSegment {
    Core::SharedPtr<OmegaGTE::GVectorPath2D> path;
    Core::SharedPtr<Brush> strokeBrush;
    Core::SharedPtr<Brush> fillBrush;
    float strokeWidth = 0.f;
    bool contour = false;
    bool fill = false;
};

class OMEGAWTK_EXPORT  Path {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Construct from a raw GTE vector path (Canvas-only).
    explicit Path(OmegaGTE::GVectorPath2D & path,float stroke = 1.f);

public:
    /// A New Segment is Created
    void goTo(Point2D pt);

    /// Set the stroke width in pixels. Width 0 means "no stroke" (callers
    /// that pass the path to `drawPath(Path&)` get a fill-only draw).
    void setStroke(float newStroke);

    void setPathBrush(Core::SharedPtr<Brush> & brush);

    void setArcPrecision(unsigned newPrecision);

    /// Adds a line to the current segment
    void addLine(Point2D dest_pt);

    /// Adds an arc to the current segment
    void addArc(Rect bounds,float startAngle,float endAngle);

    /// Close current path.
    void close();

    /// Translate every point of this path by `delta`, in place. Used by the
    /// paint walk to lift a view-local path into absolute window space
    /// (UIView-Render-Redesign absolute-coords decision 2026-05-29) — the
    /// counterpart to `rect.pos += offset` for rect/ellipse ops, since a
    /// path carries its geometry as points rather than a positioned rect.
    void translate(Point2D delta);

    /// Decompose this path into per-segment drawable records. The fill
    /// brush is the path's stored `pathBrush`; the stroke is the
    /// caller-supplied `(strokeBrush, strokeWidth)`. Reproduces the
    /// former `Canvas::drawPath(Path&, Border)` decomposition as a
    /// single shared helper (UIView-Render-Redesign-Plan §4.0
    /// resolution) — callers unpack a `Border` into the two stroke
    /// arguments so this header need not depend on `Canvas.h`. Returns
    /// empty when there is neither a fill nor a stroke.
    OmegaCommon::Vector<PathDrawSegment> decomposeForDraw(
        Core::SharedPtr<Brush> strokeBrush, float strokeWidth) const;

    /// Extract control points from the first segment (for animation).
    OmegaCommon::Vector<Point2D> getControlPoints() const;

    /// Build a linear path through the given control points.
    static Path fromControlPoints(const OmegaCommon::Vector<Point2D> & points,float stroke = 1.f);

    explicit Path(Point2D start,float initialStroke = 1.f);
    Path(Path && other) noexcept;
    Path & operator=(Path && other) noexcept;
    Path(const Path & other);
    Path & operator=(const Path & other);
    ~Path();
};
Core::SharedPtr<Path> RectFrame(Composition::Rect rect,float width);
Core::SharedPtr<Path> RoundedRectFrame(Composition::RoundedRect rect,float width);
Core::SharedPtr<Path> EllipseFrame(Composition::Ellipse ellipse,float width);

};

#endif
