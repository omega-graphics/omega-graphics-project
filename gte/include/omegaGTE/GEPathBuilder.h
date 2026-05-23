#include "GTEBase.h"
#include "GTEMath.h"
#include <vector>
#include <cmath>

#ifndef OMEGAGTE_GEPATHBUILDER_H
#define OMEGAGTE_GEPATHBUILDER_H

_NAMESPACE_BEGIN_

/**
 @brief Builds a 2D vector path from mixed linear, Bezier, and arc segments.

 Curve segments are flattened to a polyline at @ref build() time using a
 configurable tolerance (the maximum allowed deviation, in path units, between
 a curve and its linear approximation). The result is a plain GVectorPath2D, so
 the rest of the triangulation pipeline is unaffected.

 The builder produces a single connected subpath. moveTo() sets the starting
 point and is expected as the first positioning call; if called again after
 points exist it behaves like lineTo() so no geometry is silently dropped.
*/
class GPathBuilder2D {
    std::vector<GPoint2D> _points;
    GPoint2D _start{0.f,0.f};
    bool _hasStart = false;
    float _tolerance;

    void _appendPoint(const GPoint2D & p){
        if(_points.empty()){
            _start = p;
            _hasStart = true;
        }
        _points.push_back(p);
    }
    static GPoint2D _mid(const GPoint2D & a, const GPoint2D & b){
        return GPoint2D{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    }
    // Perpendicular distance from c to the line through a and b.
    static float _distToLine(const GPoint2D & c, const GPoint2D & a, const GPoint2D & b){
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx*dx + dy*dy);
        if(len < 1e-9f){
            const float ex = c.x - a.x, ey = c.y - a.y;
            return std::sqrt(ex*ex + ey*ey);
        }
        return std::fabs((c.x - a.x) * dy - (c.y - a.y) * dx) / len;
    }
    void _flattenQuad(const GPoint2D & p0, const GPoint2D & c, const GPoint2D & p1, int depth){
        if(_distToLine(c, p0, p1) <= _tolerance || depth >= 18){
            _appendPoint(p1);
            return;
        }
        const GPoint2D p01 = _mid(p0, c);
        const GPoint2D p12 = _mid(c, p1);
        const GPoint2D m = _mid(p01, p12);
        _flattenQuad(p0, p01, m, depth + 1);
        _flattenQuad(m, p12, p1, depth + 1);
    }
    void _flattenCubic(const GPoint2D & p0, const GPoint2D & c1, const GPoint2D & c2, const GPoint2D & p3, int depth){
        const float d = std::max(_distToLine(c1, p0, p3), _distToLine(c2, p0, p3));
        if(d <= _tolerance || depth >= 18){
            _appendPoint(p3);
            return;
        }
        const GPoint2D p01 = _mid(p0, c1);
        const GPoint2D p12 = _mid(c1, c2);
        const GPoint2D p23 = _mid(c2, p3);
        const GPoint2D p012 = _mid(p01, p12);
        const GPoint2D p123 = _mid(p12, p23);
        const GPoint2D m = _mid(p012, p123);
        _flattenCubic(p0, p01, p012, m, depth + 1);
        _flattenCubic(m, p123, p23, p3, depth + 1);
    }
    static float _vecAngle(float ux, float uy, float vx, float vy){
        const float dot = ux*vx + uy*vy;
        const float len = std::sqrt((ux*ux + uy*uy) * (vx*vx + vy*vy));
        float c = (len > 1e-12f) ? (dot / len) : 1.f;
        if(c < -1.f) c = -1.f;
        if(c > 1.f) c = 1.f;
        float ang = std::acos(c);
        if(ux*vy - uy*vx < 0.f) ang = -ang;
        return ang;
    }
public:
    /// @param tolerance Max curve-flattening error in path units (default 0.25).
    explicit GPathBuilder2D(float tolerance = 0.25f) : _tolerance(tolerance > 1e-4f ? tolerance : 1e-4f) {}

    // Sets the start point. A GVectorPath2D holds one connected subpath, so a
    // moveTo after points exist just connects (behaves like lineTo).
    void moveTo(float x, float y){ _appendPoint(GPoint2D{x, y}); }
    void lineTo(float x, float y){ _appendPoint(GPoint2D{x, y}); }

    void quadTo(float cx, float cy, float x, float y){
        if(_points.empty()) _appendPoint(GPoint2D{0.f, 0.f});
        const GPoint2D p0 = _points.back();
        _flattenQuad(p0, GPoint2D{cx, cy}, GPoint2D{x, y}, 0);
    }
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y){
        if(_points.empty()) _appendPoint(GPoint2D{0.f, 0.f});
        const GPoint2D p0 = _points.back();
        _flattenCubic(p0, GPoint2D{c1x, c1y}, GPoint2D{c2x, c2y}, GPoint2D{x, y}, 0);
    }
    /// SVG-style elliptical arc from the current point to (x,y).
    void arcTo(float rx, float ry, float rotation, bool largeArc, bool sweep, float x, float y){
        if(_points.empty()) _appendPoint(GPoint2D{0.f, 0.f});
        const GPoint2D p0 = _points.back();
        if(rx == 0.f || ry == 0.f){
            _appendPoint(GPoint2D{x, y});
            return;
        }
        rx = std::fabs(rx); ry = std::fabs(ry);
        const float cosP = std::cos(rotation), sinP = std::sin(rotation);
        // Step 1: midpoint-frame coordinates.
        const float dx2 = (p0.x - x) * 0.5f, dy2 = (p0.y - y) * 0.5f;
        const float x1p = cosP * dx2 + sinP * dy2;
        const float y1p = -sinP * dx2 + cosP * dy2;
        // Step 2: correct out-of-range radii.
        float rxs = rx*rx, rys = ry*ry;
        const float lambda = (x1p*x1p)/rxs + (y1p*y1p)/rys;
        if(lambda > 1.f){
            const float s = std::sqrt(lambda);
            rx *= s; ry *= s; rxs = rx*rx; rys = ry*ry;
        }
        // Step 3: center in the midpoint frame.
        float num = rxs*rys - rxs*(y1p*y1p) - rys*(x1p*x1p);
        const float den = rxs*(y1p*y1p) + rys*(x1p*x1p);
        float co = (den > 1e-12f) ? std::sqrt(std::max(0.f, num/den)) : 0.f;
        if(largeArc == sweep) co = -co;
        const float cxp = co * (rx * y1p / ry);
        const float cyp = co * -(ry * x1p / rx);
        // Step 4: center in user space.
        const float cx = cosP * cxp - sinP * cyp + (p0.x + x) * 0.5f;
        const float cy = sinP * cxp + cosP * cyp + (p0.y + y) * 0.5f;
        // Step 5: start angle and sweep.
        const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
        const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
        const float theta1 = _vecAngle(1.f, 0.f, ux, uy);
        float dTheta = _vecAngle(ux, uy, vx, vy);
        const float twoPi = 2.f * float(PI);
        if(!sweep && dTheta > 0.f) dTheta -= twoPi;
        else if(sweep && dTheta < 0.f) dTheta += twoPi;
        // Step 6: sample by tolerance-derived angular step.
        const float maxR = std::max(rx, ry);
        float dStep = (maxR > _tolerance) ? 2.f * std::acos(1.f - _tolerance / maxR) : float(PI);
        if(dStep < 1e-3f) dStep = 1e-3f;
        const int steps = std::max(1, (int)std::ceil(std::fabs(dTheta) / dStep));
        for(int i = 1; i <= steps; ++i){
            const float t = theta1 + dTheta * (float(i) / float(steps));
            const float ex = rx * std::cos(t), ey = ry * std::sin(t);
            _appendPoint(GPoint2D{cosP * ex - sinP * ey + cx, sinP * ex + cosP * ey + cy});
        }
    }
    /// Closes the path back to the starting point.
    void close(){
        if(_hasStart && !_points.empty()){
            const GPoint2D & l = _points.back();
            if(std::fabs(l.x - _start.x) > 1e-6f || std::fabs(l.y - _start.y) > 1e-6f){
                _points.push_back(_start);
            }
        }
    }
    /// Materializes the flattened polyline as a GVectorPath2D.
    GVectorPath2D build() const {
        if(_points.empty()){
            return GVectorPath2D(GPoint2D{0.f, 0.f});
        }
        GVectorPath2D path(_points.front());
        for(size_t i = 1; i < _points.size(); ++i){
            path.append(_points[i]);
        }
        return path;
    }
};

/**
 @brief Builds a 3D vector path from linear and Bezier segments.

 Mirrors GPathBuilder2D for 3D points. Bezier curves generalize directly to 3D;
 there is no arcTo() because an elliptical arc requires a 2D plane.
*/
class GPathBuilder3D {
    std::vector<GPoint3D> _points;
    GPoint3D _start{0.f,0.f,0.f};
    bool _hasStart = false;
    float _tolerance;

    void _appendPoint(const GPoint3D & p){
        if(_points.empty()){
            _start = p;
            _hasStart = true;
        }
        _points.push_back(p);
    }
    static GPoint3D _mid(const GPoint3D & a, const GPoint3D & b){
        return GPoint3D{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    }
    static float _distToLine(const GPoint3D & c, const GPoint3D & a, const GPoint3D & b){
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float len2 = dx*dx + dy*dy + dz*dz;
        const float ex = c.x - a.x, ey = c.y - a.y, ez = c.z - a.z;
        if(len2 < 1e-12f){
            return std::sqrt(ex*ex + ey*ey + ez*ez);
        }
        // |e x d| / |d|
        const float crx = ey*dz - ez*dy;
        const float cry = ez*dx - ex*dz;
        const float crz = ex*dy - ey*dx;
        return std::sqrt(crx*crx + cry*cry + crz*crz) / std::sqrt(len2);
    }
    void _flattenQuad(const GPoint3D & p0, const GPoint3D & c, const GPoint3D & p1, int depth){
        if(_distToLine(c, p0, p1) <= _tolerance || depth >= 18){
            _appendPoint(p1);
            return;
        }
        const GPoint3D p01 = _mid(p0, c);
        const GPoint3D p12 = _mid(c, p1);
        const GPoint3D m = _mid(p01, p12);
        _flattenQuad(p0, p01, m, depth + 1);
        _flattenQuad(m, p12, p1, depth + 1);
    }
    void _flattenCubic(const GPoint3D & p0, const GPoint3D & c1, const GPoint3D & c2, const GPoint3D & p3, int depth){
        const float d = std::max(_distToLine(c1, p0, p3), _distToLine(c2, p0, p3));
        if(d <= _tolerance || depth >= 18){
            _appendPoint(p3);
            return;
        }
        const GPoint3D p01 = _mid(p0, c1);
        const GPoint3D p12 = _mid(c1, c2);
        const GPoint3D p23 = _mid(c2, p3);
        const GPoint3D p012 = _mid(p01, p12);
        const GPoint3D p123 = _mid(p12, p23);
        const GPoint3D m = _mid(p012, p123);
        _flattenCubic(p0, p01, p012, m, depth + 1);
        _flattenCubic(m, p123, p23, p3, depth + 1);
    }
public:
    explicit GPathBuilder3D(float tolerance = 0.25f) : _tolerance(tolerance > 1e-4f ? tolerance : 1e-4f) {}

    void moveTo(float x, float y, float z){ _appendPoint(GPoint3D{x, y, z}); }
    void lineTo(float x, float y, float z){ _appendPoint(GPoint3D{x, y, z}); }
    void quadTo(float cx, float cy, float cz, float x, float y, float z){
        if(_points.empty()) _appendPoint(GPoint3D{0.f, 0.f, 0.f});
        const GPoint3D p0 = _points.back();
        _flattenQuad(p0, GPoint3D{cx, cy, cz}, GPoint3D{x, y, z}, 0);
    }
    void cubicTo(float c1x, float c1y, float c1z, float c2x, float c2y, float c2z, float x, float y, float z){
        if(_points.empty()) _appendPoint(GPoint3D{0.f, 0.f, 0.f});
        const GPoint3D p0 = _points.back();
        _flattenCubic(p0, GPoint3D{c1x, c1y, c1z}, GPoint3D{c2x, c2y, c2z}, GPoint3D{x, y, z}, 0);
    }
    void close(){
        if(_hasStart && !_points.empty()){
            const GPoint3D & l = _points.back();
            if(std::fabs(l.x - _start.x) > 1e-6f || std::fabs(l.y - _start.y) > 1e-6f || std::fabs(l.z - _start.z) > 1e-6f){
                _points.push_back(_start);
            }
        }
    }
    GVectorPath3D build() const {
        if(_points.empty()){
            return GVectorPath3D(GPoint3D{0.f, 0.f, 0.f});
        }
        GVectorPath3D path(_points.front());
        for(size_t i = 1; i < _points.size(); ++i){
            path.append(_points[i]);
        }
        return path;
    }
};

_NAMESPACE_END_

#endif
