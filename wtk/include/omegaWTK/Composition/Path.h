#include "omegaWTK/Core/Core.h"
#include "Geometry.h"
#include "GTEForward.h"

#ifndef OMEGAWTK_COMPOSITION_PATH_H
#define OMEGAWTK_COMPOSITION_PATH_H

namespace OmegaWTK::Composition {

struct Brush;

class OMEGAWTK_EXPORT  Path {
    friend class Canvas;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Construct from a raw GTE vector path (Canvas-only).
    explicit Path(OmegaGTE::GVectorPath2D & path,unsigned stroke = 1);

public:
    /// A New Segment is Created
    void goTo(Point2D pt);

    void setStroke(unsigned newStroke);

    void setPathBrush(Core::SharedPtr<Brush> & brush);

    void setArcPrecision(unsigned newPrecision);

    /// Adds a line to the current segment
    void addLine(Point2D dest_pt);

    /// Adds an arc to the current segment
    void addArc(Rect bounds,float startAngle,float endAngle);

    /// Close current path.
    void close();

    /// Extract control points from the first segment (for animation).
    OmegaCommon::Vector<Point2D> getControlPoints() const;

    /// Build a linear path through the given control points.
    static Path fromControlPoints(const OmegaCommon::Vector<Point2D> & points,unsigned stroke = 1);

    explicit Path(Point2D start,unsigned initialStroke = 1);
    Path(Path && other) noexcept;
    Path & operator=(Path && other) noexcept;
    Path(const Path & other);
    Path & operator=(const Path & other);
    ~Path();
};
Core::SharedPtr<Path> RectFrame(Composition::Rect rect,unsigned width);
Core::SharedPtr<Path> RoundedRectFrame(Composition::RoundedRect rect,unsigned width);
Core::SharedPtr<Path> EllipseFrame(Composition::Ellipse ellipse,unsigned width);

};

#endif
