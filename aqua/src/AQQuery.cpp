// AQUA Phase 4 — analytic ray/shape + ray/AABB query math (Phase-4 brief §6.L,
// §7). Pure functions over AQShape + AQTransform; no AQSpace / AQRigidBody state.
// AQSpace::raycast / shapecast / overlap (AQSpace.cpp) iterate the candidate
// bodies and call these per body, then sort the hits deterministically by
// (fraction, bodyIndex).
//
// Ray convention: the ray is origin + t·dir for t ∈ [0, maxT]; dir need not be
// unit, so t is the curve parameter and the hit point is origin + t·dir. A
// sphere-cast reuses the same code with the target surface inflated by the cast
// shape's bounding radius (the Minkowski-sum specialization for a sphere swept
// volume) — exact for spheres/planes, conservative-but-correct for the rest.

#include "AQQueryMath.h"
#include <cmath>
#include <algorithm>

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

namespace {

FQuaternion shapeLocalQ(const AQShape &s) {
    FQuaternion q; q.x = s.lqx; q.y = s.lqy; q.z = s.lqz; q.w = s.lqw;
    return q.normalized();
}
FVec<3> shapeLocalP(const AQShape &s) { return AQvec3(s.lpx, s.lpy, s.lpz); }

// World pose of the shape: body pose composed with the shape's local pose.
void shapeWorldPose(const AQShape &s, const AQTransform<float> &body,
                    FVec<3> &cOut, FQuaternion &qOut) {
    qOut = (body.q * shapeLocalQ(s)).normalized();
    cOut = body.p + AQrotate(body.q, shapeLocalP(s));
}

} // namespace

bool AQrayAABB(const FVec<3> &mn, const FVec<3> &mx,
               const FVec<3> &origin, const FVec<3> &dir, float maxT, float &tEnter) {
    float tmin = 0.f, tmax = maxT;
    for (int k = 0; k < 3; ++k) {
        const float o = origin[k][0], d = dir[k][0];
        const float lo = mn[k][0], hi = mx[k][0];
        if (std::abs(d) < 1e-9f) {
            if (o < lo || o > hi) return false;            // parallel & outside the slab
        } else {
            const float inv = 1.f / d;
            float t1 = (lo - o) * inv, t2 = (hi - o) * inv;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    tEnter = tmin;
    return true;
}

float AQshapeBoundingRadius(const AQShape &shape,
                            const FVec<3> *hullVerts, std::size_t hullCount) {
    switch (shape.type) {
    case AQShapeType::Sphere:  return shape.sphere.radius;
    case AQShapeType::Box: {
        const float hx = shape.box.hx, hy = shape.box.hy, hz = shape.box.hz;
        return std::sqrt(hx*hx + hy*hy + hz*hz);
    }
    case AQShapeType::Capsule: return shape.capsule.halfHeight + shape.capsule.radius;
    case AQShapeType::Plane:   return 0.f;
    case AQShapeType::ConvexHull: {
        float r2 = 0.f;
        for (std::uint32_t i = 0; i < shape.hull.vertexCount; ++i) {
            const std::size_t idx = shape.hull.firstVertex + i;
            if (idx >= hullCount) break;
            r2 = std::max(r2, OmegaGTE::dot(hullVerts[idx], hullVerts[idx]));
        }
        return std::sqrt(r2);
    }
    }
    return 0.f;
}

bool AQrayShape(const AQShape &shape, const AQTransform<float> &bodyXform,
                const FVec<3> &origin, const FVec<3> &dir, float maxT, float inflate,
                const FVec<3> *hullVerts, std::size_t hullCount,
                float &tOut, FVec<3> &posOut, FVec<3> &normalOut) {
    switch (shape.type) {
    case AQShapeType::Sphere: {
        FVec<3> c = AQvec3(0.f, 0.f, 0.f); FQuaternion q; shapeWorldPose(shape, bodyXform, c, q);
        const float r = shape.sphere.radius + inflate;
        const FVec<3> oc = origin - c;
        const float a = OmegaGTE::dot(dir, dir);
        if (a < 1e-12f) return false;
        const float b = OmegaGTE::dot(oc, dir);
        const float cc = OmegaGTE::dot(oc, oc) - r * r;
        const float disc = b * b - a * cc;
        if (disc < 0.f) return false;
        const float sq = std::sqrt(disc);
        float t = (-b - sq) / a;
        if (t < 0.f) t = (-b + sq) / a;          // origin inside: take the far root
        if (t < 0.f || t > maxT) return false;
        tOut = t;
        posOut = origin + dir * t;
        const FVec<3> n = posOut - c;
        const float nl = std::sqrt(OmegaGTE::dot(n, n));
        normalOut = (nl > 1e-9f) ? n * (1.f / nl) : AQvec3(0.f, 1.f, 0.f);
        return true;
    }
    case AQShapeType::Plane: {
        // Half-space n·x = offset, in world. Inflate moves the surface toward the
        // ray's side by `inflate` (a sphere stops one radius short).
        FQuaternion q = (bodyXform.q * shapeLocalQ(shape)).normalized();
        FVec<3> n = AQrotate(q, AQvec3(shape.plane.nx, shape.plane.ny, shape.plane.nz));
        float offset = shape.plane.offset + OmegaGTE::dot(n, bodyXform.p);  // shift by body translation
        const float denom = OmegaGTE::dot(n, dir);
        if (std::abs(denom) < 1e-9f) return false;                         // parallel
        const float side = OmegaGTE::dot(n, origin) - offset;
        offset += (side >= 0.f ? inflate : -inflate);
        const float t = (offset - OmegaGTE::dot(n, origin)) / denom;
        if (t < 0.f || t > maxT) return false;
        tOut = t;
        posOut = origin + dir * t;
        normalOut = (side >= 0.f) ? n : n * -1.f;                          // face the ray
        return true;
    }
    case AQShapeType::Box: {
        FVec<3> c = AQvec3(0.f, 0.f, 0.f); FQuaternion q; shapeWorldPose(shape, bodyXform, c, q);
        const FQuaternion qc = q.conjugate();
        const FVec<3> lo = AQrotate(qc, origin - c);     // ray into box-local frame
        const FVec<3> ld = AQrotate(qc, dir);
        const FVec<3> h  = AQvec3(shape.box.hx + inflate, shape.box.hy + inflate, shape.box.hz + inflate);
        const FVec<3> mn = h * -1.f;
        float tEnter;
        if (!AQrayAABB(mn, h, lo, ld, maxT, tEnter)) return false;
        tOut = tEnter;
        const FVec<3> lp = lo + ld * tEnter;             // local hit point
        // Local normal = the axis with the largest |lp|/h ratio (the face hit).
        FVec<3> ln = AQvec3(0.f, 0.f, 0.f);
        const float ax = std::abs(lp[0][0]) / h[0][0];
        const float ay = std::abs(lp[1][0]) / h[1][0];
        const float az = std::abs(lp[2][0]) / h[2][0];
        if (ax >= ay && ax >= az)      ln = AQvec3(lp[0][0] >= 0.f ? 1.f : -1.f, 0.f, 0.f);
        else if (ay >= az)             ln = AQvec3(0.f, lp[1][0] >= 0.f ? 1.f : -1.f, 0.f);
        else                           ln = AQvec3(0.f, 0.f, lp[2][0] >= 0.f ? 1.f : -1.f);
        posOut = origin + dir * tEnter;
        normalOut = AQrotate(q, ln);
        return true;
    }
    case AQShapeType::Capsule:
    case AQShapeType::ConvexHull:
    default: {
        // Conservative fallback: ray vs the shape's world AABB, inflated. No
        // deliverable raycasts these; the brute-force oracle uses the same path.
        AQTransform<float> xf; xf.p = bodyXform.p; xf.q = bodyXform.q;
        AQAABB<float> bb = AQshapeAABB(shape, xf, hullVerts, hullCount);
        const FVec<3> inf = AQvec3(inflate, inflate, inflate);
        float tEnter;
        if (!AQrayAABB(bb.min - inf, bb.max + inf, origin, dir, maxT, tEnter)) return false;
        tOut = tEnter;
        posOut = origin + dir * tEnter;
        const FVec<3> nd = dir;
        const float nl = std::sqrt(OmegaGTE::dot(nd, nd));
        normalOut = (nl > 1e-9f) ? nd * (-1.f / nl) : AQvec3(0.f, 1.f, 0.f);
        return true;
    }
    }
}
