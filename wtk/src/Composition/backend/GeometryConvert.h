#ifndef OMEGAWTK_COMPOSITION_GEOMETRYCONVERT_H
#define OMEGAWTK_COMPOSITION_GEOMETRYCONVERT_H

#include "omegaWTK/Composition/Geometry.h"
#include <OmegaGTE.h>

namespace OmegaWTK::Composition {

    // Point2D <-> GPoint2D

    inline OmegaGTE::GPoint2D toGTE(const Point2D& p) {
        return {p.x, p.y};
    }

    inline Point2D fromGTE(const OmegaGTE::GPoint2D& p) {
        return {p.x, p.y};
    }

    // Rect <-> GRect

    inline OmegaGTE::GRect toGTE(const Rect& r) {
        return {toGTE(r.pos), r.w, r.h};
    }

    inline Rect fromGTE(const OmegaGTE::GRect& r) {
        return {fromGTE(r.pos), r.w, r.h};
    }

    // RoundedRect <-> GRoundedRect

    inline OmegaGTE::GRoundedRect toGTE(const RoundedRect& r) {
        return {toGTE(r.pos), r.w, r.h, r.rad_x, r.rad_y};
    }

    inline RoundedRect fromGTE(const OmegaGTE::GRoundedRect& r) {
        return {fromGTE(r.pos), r.w, r.h, r.rad_x, r.rad_y};
    }

    // Ellipse <-> GEllipsoid (2D projection)

    inline OmegaGTE::GEllipsoid toGTE(const Ellipse& e) {
        return {e.x, e.y, 0.f, e.rad_x, e.rad_y, 0.f};
    }

    inline Ellipse fromGTE(const OmegaGTE::GEllipsoid& e) {
        return {e.x, e.y, e.rad_x, e.rad_y};
    }

    // Matrix4x4 <-> FMatrix<4,4>

    inline OmegaGTE::FMatrix<4,4> toGTEMatrix(const Matrix4x4& mat) {
        auto m = OmegaGTE::FMatrix<4,4>::Create();
        for(unsigned c = 0; c < 4; c++)
            for(unsigned r = 0; r < 4; r++)
                m[c][r] = mat.m[c * 4 + r];
        return m;
    }

    inline Matrix4x4 fromGTEMatrix(const OmegaGTE::FMatrix<4,4>& m) {
        Matrix4x4 mat;
        for(unsigned c = 0; c < 4; c++)
            for(unsigned r = 0; r < 4; r++)
                mat.m[c * 4 + r] = m[c][r];
        return mat;
    }

}

#endif
