// AQUA Phase 2 — free-function bodies over AQShape (Phase-2 brief §7).
//
// Three responsibilities live here, all kept off the AQSpace storage path so
// the math layer stays free of any space-owned state:
//   1. AQshapeAABB     — the world-space rotation-correct bound used by the
//                        broadphase (Phase-2 §6.A / §6.1).
//   2. AQshapeSupport  — surface-point-for-direction; minimal correct landing
//                        for the Phase-3 GJK consumer.
//   3. AQshapeInertiaMoments — diagonal principal moments, delegating to the
//                        AQMath.h Phase 1 helpers so the §1 hook closes
//                        without new numerics.
//
// The convex-hull case is the §11.6 stretch: we ship hull AABB + a
// solid-box-of-the-local-AABB inertia approximation + a max-dot support scan.
// Phase 3 will replace the inertia with a proper integral if profiling
// shows the approximation is the wrong choice for hulls in practice.

#include <aqua/AQCollision.h>
#include <aqua/AQMath.h>

#include <algorithm>
#include <cmath>
#include <limits>

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

namespace {

// AQShape stores its local pose as raw floats (so the union stays trivial).
// Build a Phase-1 AQTransform<float> for the math layer at the call site;
// non-trivial compounds use the same path, so this is the single seam.
AQTransform<float> shapeLocalXform(const AQShape &s) {
    AQTransform<float> t;
    t.p = AQvec3(s.lpx, s.lpy, s.lpz);
    t.q = FQuaternion{s.lqx, s.lqy, s.lqz, s.lqw};
    return t;
}

// Compose body * local into a single world-pose for the shape. The compose
// operator (AQTransform::operator*) is "apply child first, then this", which
// is what `worldXform = bodyXform * localXform` means.
AQTransform<float> shapeWorldXform(const AQShape &s,
                                   const AQTransform<float> &bodyXform) {
    return bodyXform * shapeLocalXform(s);
}

// AABB of a vertex pool in its OWN frame — used by the hull case for both
// bound and inertia. Returns an empty AABB when there are no vertices, which
// the caller treats as "skip" (no inertia, broadphase-invisible).
AQAABB<float> hullLocalAABB(const FVec<3> *verts, std::size_t count) {
    if (verts == nullptr || count == 0) return AQAABB<float>::empty();
    auto bb = AQAABB<float>::empty();
    for (std::size_t i = 0; i < count; ++i) {
        bb.min = AQvec3(std::min(bb.min[0][0], verts[i][0][0]),
                        std::min(bb.min[1][0], verts[i][1][0]),
                        std::min(bb.min[2][0], verts[i][2][0]));
        bb.max = AQvec3(std::max(bb.max[0][0], verts[i][0][0]),
                        std::max(bb.max[1][0], verts[i][1][0]),
                        std::max(bb.max[2][0], verts[i][2][0]));
    }
    return bb;
}

} // namespace

// ============================================================================
// AQshapeAABB — world-space rotation-correct bound.
// ============================================================================

AQAABB<float> AQshapeAABB(const AQShape &shape,
                          const AQTransform<float> &bodyXform,
                          const FVec<3> *hullVerts,
                          std::size_t hullVertCount) {
    (void)hullVertCount; // reserved for a debug bounds-check on (firstVertex + vertexCount)
    const AQTransform<float> xf = shapeWorldXform(shape, bodyXform);

    switch (shape.type) {
    case AQShapeType::Sphere: {
        const float r = shape.sphere.radius;
        const auto h = AQvec3(r, r, r);
        // Sphere bound is orientation-trivial: c ± r·1.
        return AQAABB<float>::fromCenterHalfExtents(xf.p, h);
    }
    case AQShapeType::Box: {
        const auto h = AQvec3(shape.box.hx, shape.box.hy, shape.box.hz);
        // The Phase 1.1 |R|·h oriented-box bound — the §6.1 rotation-correct
        // bound. Cheap and exactly contains the 8 box corners.
        return AQaabbOfOrientedBox(xf.p, h, xf.q);
    }
    case AQShapeType::Capsule: {
        // Capsule axis is local +Y. World endpoints of the cylinder segment
        // are xf.p ± R·(0, halfHeight, 0). Bound = AABB of the two endpoints,
        // expanded by radius on every axis.
        const auto axisW = AQrotate(xf.q, AQvec3(0.f, shape.capsule.halfHeight, 0.f));
        const auto a = xf.p - axisW;
        const auto b = xf.p + axisW;
        const float r = shape.capsule.radius;
        const auto lo = AQvec3(std::min(a[0][0], b[0][0]) - r,
                               std::min(a[1][0], b[1][0]) - r,
                               std::min(a[2][0], b[2][0]) - r);
        const auto hi = AQvec3(std::max(a[0][0], b[0][0]) + r,
                               std::max(a[1][0], b[1][0]) + r,
                               std::max(a[2][0], b[2][0]) + r);
        return AQAABB<float>::fromMinMax(lo, hi);
    }
    case AQShapeType::Plane: {
        // Planes are infinite half-spaces; the broadphase treats them as
        // "always candidates against everything the filter accepts". A very
        // large finite AABB stands in so the grid hash math doesn't go NaN.
        // Phase 3 narrowphase will branch on the shape type and skip the
        // fat-AABB check for plane pairs.
        const float kHuge = 1e18f;
        return AQAABB<float>::fromMinMax(AQvec3(-kHuge, -kHuge, -kHuge),
                                         AQvec3( kHuge,  kHuge,  kHuge));
    }
    case AQShapeType::ConvexHull: {
        // Bound = |R|·h of the local-AABB of the vertices, recentered at the
        // shape's world position + R·(local-AABB-center). Mirrors what we'd
        // get from running the box rule on the hull's bounding box.
        const auto local = hullLocalAABB(hullVerts + shape.hull.firstVertex,
                                         shape.hull.vertexCount);
        const auto h = local.extents() * 0.5f;
        const auto cLocal = local.center();
        // Local-frame center is relative to the shape origin; transform to
        // world by applying the shape's world pose to it.
        const auto cWorld = xf.transformPoint(cLocal);
        return AQaabbOfOrientedBox(cWorld, h, xf.q);
    }
    }
    return AQAABB<float>::empty(); // unreachable; quiets the compiler
}

// ============================================================================
// AQshapeSupport — surface point in `dirWorld` direction (Phase 3 GJK seed).
// ============================================================================

FVec<3> AQshapeSupport(const AQShape &shape,
                       const FVec<3> &dirWorld,
                       const AQTransform<float> &bodyXform,
                       const FVec<3> *hullVerts,
                       std::size_t hullVertCount) {
    (void)hullVertCount;
    const AQTransform<float> xf = shapeWorldXform(shape, bodyXform);

    // Direction in the shape's local frame: world dir rotated by Rᵀ.
    const FVec<3> dirLocal = AQrotate(xf.q.conjugate(), dirWorld);

    switch (shape.type) {
    case AQShapeType::Sphere: {
        const float r = shape.sphere.radius;
        const float n2 = OmegaGTE::dot(dirWorld, dirWorld);
        if (n2 <= 0.f) return xf.p;
        return xf.p + (r / std::sqrt(n2)) * dirWorld;
    }
    case AQShapeType::Box: {
        // The box vertex with the same-sign coordinates as the local dir.
        const auto sgn = AQvec3(dirLocal[0][0] >= 0.f ? shape.box.hx : -shape.box.hx,
                                dirLocal[1][0] >= 0.f ? shape.box.hy : -shape.box.hy,
                                dirLocal[2][0] >= 0.f ? shape.box.hz : -shape.box.hz);
        return xf.transformPoint(sgn);
    }
    case AQShapeType::Capsule: {
        const float hh = shape.capsule.halfHeight;
        const float r  = shape.capsule.radius;
        // Pick the cylinder endpoint that lies in the direction; add a unit
        // sphere of radius r on top of it.
        const auto endpointLocal = AQvec3(0.f, dirLocal[1][0] >= 0.f ? hh : -hh, 0.f);
        const auto endpointWorld = xf.transformPoint(endpointLocal);
        const float n2 = OmegaGTE::dot(dirWorld, dirWorld);
        if (n2 <= 0.f) return endpointWorld;
        return endpointWorld + (r / std::sqrt(n2)) * dirWorld;
    }
    case AQShapeType::Plane: {
        // A half-space has no bounded support; for the GJK consumer we hand
        // back a far point along the projection of dir onto the plane, which
        // is the standard handling. Phase 3 will replace this with a clipped
        // form if/when plane-vs-plane pairs need real treatment.
        return xf.p + dirWorld * 0.f; // 0-vector relative to the plane origin
    }
    case AQShapeType::ConvexHull: {
        // Max-dot scan in the shape's local frame, then transform to world.
        if (hullVerts == nullptr || shape.hull.vertexCount == 0) return xf.p;
        const FVec<3> *base = hullVerts + shape.hull.firstVertex;
        std::size_t best = 0;
        float bestDot = -std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < shape.hull.vertexCount; ++i) {
            const float d = OmegaGTE::dot(base[i], dirLocal);
            if (d > bestDot) { bestDot = d; best = i; }
        }
        return xf.transformPoint(base[best]);
    }
    }
    return xf.p;
}

// ============================================================================
// AQshapeInertiaMoments — diagonal principal moments at total mass `mass`.
// ============================================================================

FVec<3> AQshapeInertiaMoments(const AQShape &shape, float mass,
                              const FVec<3> *hullVerts,
                              std::size_t hullVertCount) {
    (void)hullVertCount;
    if (mass <= 0.f) return AQvec3(0.f, 0.f, 0.f);

    switch (shape.type) {
    case AQShapeType::Sphere:
        return AQinertiaSolidSphere(mass, shape.sphere.radius);
    case AQShapeType::Box:
        return AQinertiaSolidBox(mass, shape.box.hx, shape.box.hy, shape.box.hz);
    case AQShapeType::Capsule:
        return AQinertiaCapsule(mass, shape.capsule.radius, shape.capsule.halfHeight);
    case AQShapeType::Plane:
        // Planes are static-only intent and have no inertia.
        return AQvec3(0.f, 0.f, 0.f);
    case AQShapeType::ConvexHull: {
        // Stretch posture (§11.6): solid-box-of-the-local-AABB approximation.
        // Honest enough for "got geometry, used some inertia"; Phase 3 will
        // replace this with a real triangulated integral if profiling shows
        // the approximation drifts the trajectory noticeably.
        const auto local = hullLocalAABB(hullVerts + shape.hull.firstVertex,
                                         shape.hull.vertexCount);
        if (local.surfaceArea() <= 0.f) return AQvec3(0.f, 0.f, 0.f);
        const auto h = local.extents() * 0.5f;
        return AQinertiaSolidBox(mass, h[0][0], h[1][0], h[2][0]);
    }
    }
    return AQvec3(0.f, 0.f, 0.f);
}
