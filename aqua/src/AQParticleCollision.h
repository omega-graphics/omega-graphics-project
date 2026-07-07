#ifndef AQUA_SRC_AQPARTICLECOLLISION_H
#define AQUA_SRC_AQPARTICLECOLLISION_H

// AQUA Phase 6 §13.3 6e — scalar-generic point-vs-shape signed distance.
//
// The 6a `AQshapeSignedDistance` (public, float) is the SINGLE source of truth
// for the closed-form collision query, but the double reference oracle needs the
// SAME math at double precision so a divergence localizes to precision, never to
// a second implementation (§8). This header is that one implementation, templated
// on `Ty`; the public float entry point (AQCollision.cpp) delegates to
// AQshapeSignedDistanceGeneric<float>, and the oracle uses <double>. Transcribed
// verbatim from the 6a float body — the 6a test pins the float instantiation.

#include <aqua/AQCollision.h>   // AQShape, AQShapeType
#include <aqua/AQMath.h>        // AQVec3<Ty>, AQTransform<Ty>, AQvec3, OmegaGTE::dot
#include <cmath>
#include <algorithm>
#include <limits>

// Templated sample — like the public AQShapeSample but generic on the scalar.
template<class Ty>
struct AQShapeSampleT {
    Ty          distance;
    AQVec3<Ty>  normal = AQvec3<Ty>(Ty(0), Ty(1), Ty(0));
};

// Shape local pose (stored as raw floats on AQShape) cast into an AQTransform<Ty>.
template<class Ty>
inline AQTransform<Ty> AQshapeLocalXformT(const AQShape& s) {
    AQTransform<Ty> t;
    t.p = AQvec3<Ty>(Ty(s.lpx), Ty(s.lpy), Ty(s.lpz));
    t.q = OmegaGTE::Quaternion<Ty>{Ty(s.lqx), Ty(s.lqy), Ty(s.lqz), Ty(s.lqw)};
    return t;
}

template<class Ty>
inline AQVec3<Ty> AQsafeDirLocalT(const AQVec3<Ty>& v, Ty len) {
    if (len > Ty(1e-12)) return v * (Ty(1) / len);
    return AQvec3<Ty>(Ty(0), Ty(1), Ty(0));
}

// Signed distance + outward world normal of `pointWorld` against `shape` worn by
// a body at `bodyXform`. Exact for sphere/box/capsule/plane; hull → +inf (Phase
// 6 does not collide hulls). See AQCollision.h::AQshapeSignedDistance for the
// contract.
template<class Ty>
inline AQShapeSampleT<Ty> AQshapeSignedDistanceGeneric(const AQShape& shape,
                                                       const AQVec3<Ty>& pointWorld,
                                                       const AQTransform<Ty>& bodyXform) {
    const AQTransform<Ty> xf = bodyXform * AQshapeLocalXformT<Ty>(shape);
    const AQVec3<Ty> pl = xf.inverse().transformPoint(pointWorld);

    AQShapeSampleT<Ty> out;
    out.distance = std::numeric_limits<Ty>::infinity();
    out.normal   = AQvec3<Ty>(Ty(0), Ty(1), Ty(0));

    switch (shape.type) {
    case AQShapeType::Sphere: {
        const Ty r   = Ty(shape.sphere.radius);
        const Ty len = std::sqrt(OmegaGTE::dot(pl, pl));
        out.distance = len - r;
        out.normal   = xf.transformVector(AQsafeDirLocalT<Ty>(pl, len));
        break;
    }
    case AQShapeType::Plane: {
        AQVec3<Ty> nLocal = AQvec3<Ty>(Ty(shape.plane.nx), Ty(shape.plane.ny), Ty(shape.plane.nz));
        const Ty nlen = std::sqrt(OmegaGTE::dot(nLocal, nLocal));
        Ty offset = Ty(shape.plane.offset);
        if (nlen > Ty(1e-12)) { nLocal = nLocal * (Ty(1) / nlen); offset /= nlen; }
        out.distance = OmegaGTE::dot(nLocal, pl) - offset;
        out.normal   = xf.transformVector(nLocal);
        break;
    }
    case AQShapeType::Box: {
        const Ty qx = std::fabs(pl[0][0]) - Ty(shape.box.hx);
        const Ty qy = std::fabs(pl[1][0]) - Ty(shape.box.hy);
        const Ty qz = std::fabs(pl[2][0]) - Ty(shape.box.hz);
        const Ty mx = std::max(qx, Ty(0)), my = std::max(qy, Ty(0)), mz = std::max(qz, Ty(0));
        const Ty outside = std::sqrt(mx*mx + my*my + mz*mz);
        const Ty inside  = std::min(std::max(qx, std::max(qy, qz)), Ty(0));
        out.distance = outside + inside;
        if (outside > Ty(1e-12)) {
            const AQVec3<Ty> g = AQvec3<Ty>(std::copysign(mx, pl[0][0]),
                                            std::copysign(my, pl[1][0]),
                                            std::copysign(mz, pl[2][0]));
            out.normal = xf.transformVector(AQsafeDirLocalT<Ty>(g, std::sqrt(OmegaGTE::dot(g, g))));
        } else {
            AQVec3<Ty> nLocal = AQvec3<Ty>(Ty(0), Ty(1), Ty(0));
            if (qx >= qy && qx >= qz)      nLocal = AQvec3<Ty>(std::copysign(Ty(1), pl[0][0]), Ty(0), Ty(0));
            else if (qy >= qz)             nLocal = AQvec3<Ty>(Ty(0), std::copysign(Ty(1), pl[1][0]), Ty(0));
            else                           nLocal = AQvec3<Ty>(Ty(0), Ty(0), std::copysign(Ty(1), pl[2][0]));
            out.normal = xf.transformVector(nLocal);
        }
        break;
    }
    case AQShapeType::Capsule: {
        const Ty hh = Ty(shape.capsule.halfHeight);
        const Ty cy = std::max(-hh, std::min(pl[1][0], hh));
        const AQVec3<Ty> delta = pl - AQvec3<Ty>(Ty(0), cy, Ty(0));
        const Ty len = std::sqrt(OmegaGTE::dot(delta, delta));
        out.distance = len - Ty(shape.capsule.radius);
        out.normal   = xf.transformVector(AQsafeDirLocalT<Ty>(delta, len));
        break;
    }
    case AQShapeType::ConvexHull:
        break;   // unsupported in Phase 6 — distance stays +inf (no contact)
    }
    return out;
}

#endif // AQUA_SRC_AQPARTICLECOLLISION_H
