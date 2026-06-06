// AQUA Phase 3 — narrowphase branch table (Phase-3 brief §6.A, §11.6 #1).
//
// Per-(typeA, typeB) specialized contact functions plus a GJK/EPA fallback
// (in AQGJK.cpp) for pairs involving a convex hull. The dispatcher
// canonicalizes the input order by `AQShapeType` enum value (lower first) so
// each specialized function only has to implement one of {sphere/box,
// box/sphere} — and then flips the contact normal back to the caller's
// "from A to B" convention on the way out.
//
// Contact normal convention: `manifold.normalWorld` points FROM A TO B in
// the caller-supplied (a, b) order; equivalently, the non-penetration impulse
// on B is `+λ_n · normalWorld` and the impulse on A is `-λ_n · normalWorld`
// (Phase-3 brief §6.D — matches `AQRigidBody::applyImpulseAtPoint`).
//
// Plane shapes are half-spaces (unbounded support), so every pair involving
// a plane is handled here — GJK cannot consume an unbounded shape. Hull
// pairs with bounded partners route to GJK/EPA via the AQgjkEpa* entry
// points (declared at the bottom of this file, defined in AQGJK.cpp).

#include <aqua/AQCollision.h>
#include <aqua/AQContact.h>
#include <aqua/AQMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

// ----------------------------------------------------------------------------
// Local helpers — shape world pose, OBB extraction, segment-segment closest.
// ----------------------------------------------------------------------------

namespace {

inline AQTransform<float> shapeLocal(const AQShape &s) {
    AQTransform<float> t;
    t.p = AQvec3(s.lpx, s.lpy, s.lpz);
    t.q = FQuaternion{s.lqx, s.lqy, s.lqz, s.lqw};
    return t;
}

inline AQTransform<float> shapeWorld(const AQShape &s,
                                     const AQTransform<float> &body) {
    return body * shapeLocal(s);
}

// OmegaGTE's `Matrix` has a private default constructor (factory-only via
// `Create()`), so the OBB's `FVec<3>` members must be member-initialized
// using `AQvec3(0,0,0)` to avoid the implicit default ctor that the C-style
// array `ax[3]` would otherwise want to call.
struct OBB {
    FVec<3> c     = AQvec3(0.f, 0.f, 0.f);
    FVec<3> ax[3] = { AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f) };
    float   h[3]  = { 0.f, 0.f, 0.f };
};

inline OBB makeOBB(const AQShape &shape, const AQTransform<float> &body) {
    const AQTransform<float> xf = shapeWorld(shape, body);
    OBB o;
    o.c    = xf.p;
    o.ax[0] = AQrotate(xf.q, AQvec3(1.f, 0.f, 0.f));
    o.ax[1] = AQrotate(xf.q, AQvec3(0.f, 1.f, 0.f));
    o.ax[2] = AQrotate(xf.q, AQvec3(0.f, 0.f, 1.f));
    o.h[0] = shape.box.hx;
    o.h[1] = shape.box.hy;
    o.h[2] = shape.box.hz;
    return o;
}

// Plane equation in world coordinates: dot(nW, x) = dW. Derived from the
// plane's local (n_local · x_local = offset) by composing the shape's world
// pose with the body transform — a rigid transform preserves dot products,
// so the world-frame equation has the same offset shifted by `dot(nW, xfP.p)`.
struct PlaneWorld {
    FVec<3> n = AQvec3(0.f, 1.f, 0.f);
    float   d = 0.f;
};

inline PlaneWorld makePlaneWorld(const AQShape &plane,
                                 const AQTransform<float> &body) {
    const AQTransform<float> xf = shapeWorld(plane, body);
    PlaneWorld out;
    out.n = AQrotate(xf.q, AQvec3(plane.plane.nx, plane.plane.ny, plane.plane.nz));
    out.d = plane.plane.offset + OmegaGTE::dot(out.n, xf.p);
    return out;
}

// Segment-segment closest points (Eberly 2001 / Lengyel formulation).
// Returns parameters s, t for the closest points c1 = p1+s*(q1-p1),
// c2 = p2+t*(q2-p2). Handles all degenerate cases — coincident segments
// pick s = t = 0, parallel segments fall back to endpoint clamping.
void segSegClosest(const FVec<3> &p1, const FVec<3> &q1,
                   const FVec<3> &p2, const FVec<3> &q2,
                   FVec<3> &c1, FVec<3> &c2) {
    const FVec<3> d1 = q1 - p1;
    const FVec<3> d2 = q2 - p2;
    const FVec<3> r  = p1 - p2;
    const float a = OmegaGTE::dot(d1, d1);
    const float e = OmegaGTE::dot(d2, d2);
    const float f = OmegaGTE::dot(d2, r);
    const float eps = 1e-8f;
    float s = 0.f, t = 0.f;
    if (a <= eps && e <= eps) {
        c1 = p1; c2 = p2; return;
    }
    if (a <= eps) {
        s = 0.f;
        t = std::clamp(f / e, 0.f, 1.f);
    } else {
        const float c = OmegaGTE::dot(d1, r);
        if (e <= eps) {
            t = 0.f;
            s = std::clamp(-c / a, 0.f, 1.f);
        } else {
            const float b = OmegaGTE::dot(d1, d2);
            const float denom = a * e - b * b;
            s = (denom != 0.f) ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f) {
                t = 0.f;
                s = std::clamp(-c / a, 0.f, 1.f);
            } else if (t > 1.f) {
                t = 1.f;
                s = std::clamp((b - c) / a, 0.f, 1.f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// SAT axis projection test. Returns the overlap along `axis` (positive when
// the OBBs interpenetrate by this much; negative when separated). Writes
// `signOut` = +1 when B is on the positive side of A along `axis`, -1
// otherwise — that sign is what flips the candidate contact normal so it
// always points from A toward B.
float satOverlap(const OBB &A, const OBB &B, const FVec<3> &axis, float &signOut) {
    const float axisLen2 = OmegaGTE::dot(axis, axis);
    if (axisLen2 < 1e-12f) {
        // Degenerate axis (cross-product of parallel edges) — never wins.
        signOut = 1.f;
        return std::numeric_limits<float>::max();
    }
    const float invLen = 1.f / std::sqrt(axisLen2);
    const FVec<3> axisN = axis * invLen;
    const float prA = std::abs(OmegaGTE::dot(axisN, A.ax[0]) * A.h[0]) +
                      std::abs(OmegaGTE::dot(axisN, A.ax[1]) * A.h[1]) +
                      std::abs(OmegaGTE::dot(axisN, A.ax[2]) * A.h[2]);
    const float prB = std::abs(OmegaGTE::dot(axisN, B.ax[0]) * B.h[0]) +
                      std::abs(OmegaGTE::dot(axisN, B.ax[1]) * B.h[1]) +
                      std::abs(OmegaGTE::dot(axisN, B.ax[2]) * B.h[2]);
    const float dist = OmegaGTE::dot(axisN, B.c - A.c);
    signOut = (dist >= 0.f) ? 1.f : -1.f;
    return (prA + prB) - std::abs(dist);
}

// Reduce a contact-point list (up to 8 candidates after Sutherland-Hodgman
// clipping) to at most 4 keepers, picking by depth. The §11.6 brief calls
// for "max spatial spread + max normal impulse" — depth alone is a robust
// first-cut that keeps the deepest contacts (which contribute most to the
// solver) and is symmetric across axis-aligned configurations. Manifold-
// spread heuristic is a follow-up if a stack proves jittery in profile.
void reduceContacts(AQContactPoint *pts, int &count, int maxKeep) {
    if (count <= maxKeep) return;
    for (int i = 0; i < maxKeep; ++i) {
        int best = i;
        for (int j = i + 1; j < count; ++j) {
            if (pts[j].depth > pts[best].depth) best = j;
        }
        std::swap(pts[i], pts[best]);
    }
    count = maxKeep;
}

} // namespace

// ----------------------------------------------------------------------------
// Specialized contact functions. Each takes the shape pair in canonical
// `AQShapeType` enum order (lower first) and fills `m.normalWorld`,
// `m.pointCount`, and `m.points[]`. The dispatcher (`AQnarrowphase`) negates
// the normal afterwards if the caller's body order was the reverse.
// ----------------------------------------------------------------------------

namespace {

bool nphSphereSphere(const AQShape &A, const AQShape &B,
                     const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                     AQContactManifold &m) {
    const FVec<3> cA = shapeWorld(A, xfA).p;
    const FVec<3> cB = shapeWorld(B, xfB).p;
    const float rA = A.sphere.radius;
    const float rB = B.sphere.radius;
    const FVec<3> d = cB - cA;
    const float d2 = OmegaGTE::dot(d, d);
    const float rSum = rA + rB;
    if (d2 >= rSum * rSum) return false;
    const float dist = std::sqrt(std::max(d2, 1e-12f));
    const FVec<3> n = (dist > 1e-6f) ? d * (1.f / dist) : AQvec3(1.f, 0.f, 0.f);
    m.normalWorld = n;
    m.pointCount = 1;
    // Midpoint between A's surface point (+rA·n) and B's surface point (-rB·n):
    //   ((cA + rA·n) + (cB - rB·n)) / 2 = (cA + cB)/2 + (rA - rB)/2 · n.
    m.points[0].positionWorld = (cA + cB) * 0.5f + n * ((rA - rB) * 0.5f);
    m.points[0].depth = rSum - dist;
    m.points[0].featureKey = 0;
    return true;
}

bool nphSphereBox(const AQShape &A, const AQShape &B,
                  const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                  AQContactManifold &m) {
    const FVec<3> cS = shapeWorld(A, xfA).p;
    const float r = A.sphere.radius;
    const OBB box = makeOBB(B, xfB);

    // Sphere center in box's local frame.
    const FVec<3> off = AQvec3(OmegaGTE::dot(cS - box.c, box.ax[0]),
                               OmegaGTE::dot(cS - box.c, box.ax[1]),
                               OmegaGTE::dot(cS - box.c, box.ax[2]));
    const float cx = std::clamp(off[0][0], -box.h[0], box.h[0]);
    const float cy = std::clamp(off[1][0], -box.h[1], box.h[1]);
    const float cz = std::clamp(off[2][0], -box.h[2], box.h[2]);
    const FVec<3> closestW = box.c + box.ax[0] * cx + box.ax[1] * cy + box.ax[2] * cz;
    const FVec<3> d = cS - closestW;
    const float d2 = OmegaGTE::dot(d, d);
    if (d2 > r * r) {
        // Sphere center outside box and farther than r — no contact unless
        // sphere is wholly inside the box (off-clamp identity check). If
        // sphere center is inside, `closestW == cS`, d2 == 0 and we fall
        // through to the contact path below.
        if (off[0][0] >= -box.h[0] && off[0][0] <= box.h[0] &&
            off[1][0] >= -box.h[1] && off[1][0] <= box.h[1] &&
            off[2][0] >= -box.h[2] && off[2][0] <= box.h[2]) {
            // (Shouldn't actually be reachable — interior means d2 == 0 < r*r.)
        } else {
            return false;
        }
    }
    const float dist = std::sqrt(std::max(d2, 1e-12f));
    FVec<3> nClosestToS = AQvec3(0.f,0.f,0.f);
    if (dist > 1e-6f) {
        nClosestToS = d * (1.f / dist);
    } else {
        // Sphere center inside the box: the contact normal is the closest box
        // face's outward normal. Pick the axis with the smallest |off| margin.
        const float dx = box.h[0] - std::abs(off[0][0]);
        const float dy = box.h[1] - std::abs(off[1][0]);
        const float dz = box.h[2] - std::abs(off[2][0]);
        if (dx < dy && dx < dz) nClosestToS = box.ax[0] * (off[0][0] >= 0.f ? 1.f : -1.f);
        else if (dy < dz)       nClosestToS = box.ax[1] * (off[1][0] >= 0.f ? 1.f : -1.f);
        else                    nClosestToS = box.ax[2] * (off[2][0] >= 0.f ? 1.f : -1.f);
    }
    // normalWorld points from A (sphere) to B (box): -nClosestToS (since
    // nClosestToS points from box outward toward sphere center).
    m.normalWorld = nClosestToS * -1.f;
    m.pointCount  = 1;
    m.points[0].positionWorld = closestW;
    m.points[0].depth         = (dist <= 1e-6f) ? r + std::min({box.h[0] - std::abs(off[0][0]),
                                                                 box.h[1] - std::abs(off[1][0]),
                                                                 box.h[2] - std::abs(off[2][0])})
                                                : (r - dist);
    m.points[0].featureKey    = 0;
    return true;
}

bool nphSphereCapsule(const AQShape &A, const AQShape &B,
                      const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                      AQContactManifold &m) {
    const FVec<3> cS = shapeWorld(A, xfA).p;
    const AQTransform<float> capXf = shapeWorld(B, xfB);
    const FVec<3> axisW = AQrotate(capXf.q, AQvec3(0.f, B.capsule.halfHeight, 0.f));
    const FVec<3> p0 = capXf.p - axisW;
    const FVec<3> p1 = capXf.p + axisW;
    const FVec<3> seg = p1 - p0;
    const float seg2 = OmegaGTE::dot(seg, seg);
    float t = (seg2 > 0.f) ? OmegaGTE::dot(cS - p0, seg) / seg2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const FVec<3> closest = p0 + seg * t;
    const FVec<3> d = cS - closest;
    const float d2 = OmegaGTE::dot(d, d);
    const float rS = A.sphere.radius;
    const float rC = B.capsule.radius;
    const float rSum = rS + rC;
    if (d2 >= rSum * rSum) return false;
    const float dist = std::sqrt(std::max(d2, 1e-12f));
    const FVec<3> nCapToSphere = (dist > 1e-6f) ? d * (1.f / dist)
                                                : AQvec3(0.f, 1.f, 0.f);
    // From A (sphere) to B (capsule): opposite direction.
    m.normalWorld = nCapToSphere * -1.f;
    m.pointCount  = 1;
    m.points[0].positionWorld = (cS + closest) * 0.5f + nCapToSphere * ((rC - rS) * 0.5f);
    m.points[0].depth = rSum - dist;
    m.points[0].featureKey = 0;
    return true;
}

bool nphSpherePlane(const AQShape &A, const AQShape &B,
                    const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                    AQContactManifold &m) {
    const PlaneWorld pl = makePlaneWorld(B, xfB);
    const FVec<3> cS = shapeWorld(A, xfA).p;
    const float r = A.sphere.radius;
    const float sd = OmegaGTE::dot(pl.n, cS) - pl.d;
    if (sd >= r) return false;
    // normalWorld from A=sphere to B=plane = -plane_outward_normal.
    m.normalWorld = pl.n * -1.f;
    m.pointCount  = 1;
    m.points[0].positionWorld = cS - pl.n * sd;     // foot of sphere center on plane
    m.points[0].depth         = r - sd;
    m.points[0].featureKey    = 0;
    return true;
}

bool nphBoxBox(const AQShape &A, const AQShape &B,
               const AQTransform<float> &xfA, const AQTransform<float> &xfB,
               AQContactManifold &m) {
    const OBB oa = makeOBB(A, xfA);
    const OBB ob = makeOBB(B, xfB);

    float bestOverlap = std::numeric_limits<float>::max();
    FVec<3> bestAxis  = AQvec3(0.f, 1.f, 0.f);

    auto tryAxis = [&](const FVec<3> &axis) -> bool {
        float sign = 1.f;
        const float ov = satOverlap(oa, ob, axis, sign);
        if (ov < 0.f) return false;        // separating axis — no contact
        if (ov < bestOverlap) {
            bestOverlap = ov;
            const float invLen = 1.f / std::sqrt(OmegaGTE::dot(axis, axis));
            bestAxis = axis * (sign * invLen);   // unit, points from A toward B
        }
        return true;
    };

    // 3 face axes of A, 3 face axes of B.
    if (!tryAxis(oa.ax[0]) || !tryAxis(oa.ax[1]) || !tryAxis(oa.ax[2])) return false;
    if (!tryAxis(ob.ax[0]) || !tryAxis(ob.ax[1]) || !tryAxis(ob.ax[2])) return false;
    // 9 cross-product axes (edge-edge). Degenerate (parallel) edges are
    // filtered out by satOverlap's near-zero axisLen2 check.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const FVec<3> cax = OmegaGTE::cross(oa.ax[i], ob.ax[j]);
            if (!tryAxis(cax)) return false;
        }

    // Decide reference (A) and incident (B) faces. Reference axis is A's face
    // most aligned with bestAxis; incident axis is B's face most ANTI-aligned
    // with bestAxis.
    int refAxisA = 0; float refDot = std::abs(OmegaGTE::dot(bestAxis, oa.ax[0]));
    for (int i = 1; i < 3; ++i) {
        const float d = std::abs(OmegaGTE::dot(bestAxis, oa.ax[i]));
        if (d > refDot) { refDot = d; refAxisA = i; }
    }
    const float refSign = (OmegaGTE::dot(bestAxis, oa.ax[refAxisA]) >= 0.f) ? 1.f : -1.f;
    const FVec<3> refNormal = oa.ax[refAxisA] * refSign;
    const FVec<3> refCenter = oa.c + refNormal * oa.h[refAxisA];
    const int refU = (refAxisA + 1) % 3, refV = (refAxisA + 2) % 3;
    const FVec<3> refUaxis = oa.ax[refU], refVaxis = oa.ax[refV];
    const float refUH = oa.h[refU], refVH = oa.h[refV];

    int incAxisB = 0; float incDot = std::abs(OmegaGTE::dot(bestAxis, ob.ax[0]));
    for (int i = 1; i < 3; ++i) {
        const float d = std::abs(OmegaGTE::dot(bestAxis, ob.ax[i]));
        if (d > incDot) { incDot = d; incAxisB = i; }
    }
    // Pick the incident face whose normal is ANTI-aligned with bestAxis (the
    // face of B closest to A along bestAxis).
    const float incSign = (OmegaGTE::dot(bestAxis, ob.ax[incAxisB]) >= 0.f) ? -1.f : 1.f;
    const FVec<3> incNormal = ob.ax[incAxisB] * incSign;
    const FVec<3> incCenter = ob.c + incNormal * ob.h[incAxisB];
    const int incU = (incAxisB + 1) % 3, incV = (incAxisB + 2) % 3;
    const FVec<3> incUaxis = ob.ax[incU], incVaxis = ob.ax[incV];
    const float incUH = ob.h[incU], incVH = ob.h[incV];
    const FVec<3> incP0 = incCenter + incUaxis * incUH + incVaxis * incVH;
    const FVec<3> incP1 = incCenter - incUaxis * incUH + incVaxis * incVH;
    const FVec<3> incP2 = incCenter - incUaxis * incUH - incVaxis * incVH;
    const FVec<3> incP3 = incCenter + incUaxis * incUH - incVaxis * incVH;

    // Sutherland-Hodgman clip the incident face against the 4 side planes of
    // the reference face. Side plane (+U): outward normal +refUaxis, offset
    // d = dot(refUaxis, refCenter) + refUH. "Inside" means dot(p, +refUaxis) ≤ d.
    // We hold the polygon in a `std::vector<FVec<3>>` because `FVec<3>` cannot
    // be default-constructed (factory-only via `Create()`), so a fixed
    // C-style array won't compile cleanly here.
    std::vector<FVec<3>> poly;
    poly.reserve(16);
    poly.push_back(incP0);
    poly.push_back(incP1);
    poly.push_back(incP2);
    poly.push_back(incP3);

    auto clipSide = [&poly](const FVec<3> &n, float d) {
        std::vector<FVec<3>> next;
        next.reserve(poly.size() + 2);
        const std::size_t N = poly.size();
        for (std::size_t i = 0; i < N; ++i) {
            const FVec<3> &v0 = poly[i];
            const FVec<3> &v1 = poly[(i + 1) % N];
            const float s0 = OmegaGTE::dot(v0, n) - d;
            const float s1 = OmegaGTE::dot(v1, n) - d;
            const bool in0 = (s0 <= 0.f);
            const bool in1 = (s1 <= 0.f);
            if (in0) next.push_back(v0);
            if (in0 != in1) {
                const float t = s0 / (s0 - s1);
                next.push_back(v0 + (v1 - v0) * t);
            }
        }
        poly.swap(next);
    };

    const float refUdC = OmegaGTE::dot(refUaxis, refCenter);
    const float refVdC = OmegaGTE::dot(refVaxis, refCenter);
    clipSide( refUaxis,  refUdC + refUH);
    clipSide(refUaxis * -1.f, -refUdC + refUH);
    clipSide( refVaxis,  refVdC + refVH);
    clipSide(refVaxis * -1.f, -refVdC + refVH);

    // Keep clipped points that lie on or below the reference face plane
    // (i.e., penetrating into A). The contact point is the projection of the
    // incident-face vertex onto the reference face — that puts the contact
    // exactly on A's surface, which is what the solver expects.
    const float refD = OmegaGTE::dot(refNormal, refCenter);
    AQContactPoint cand[16];
    int candN = 0;
    for (std::size_t i = 0; i < poly.size() && candN < 16; ++i) {
        const float sd = OmegaGTE::dot(refNormal, poly[i]) - refD;
        if (sd <= 0.f) {
            cand[candN].positionWorld = poly[i] - refNormal * sd;   // onto ref face
            cand[candN].depth         = -sd;
            // featureKey packs (refAxisA, refSign, incAxisB, incSign, vertex i)
            // for the persistence cache. The vertex index is the position
            // within the clipped polygon — stable for a given config across
            // sub-steps in the same frame.
            const std::uint32_t key = (static_cast<std::uint32_t>(refAxisA) << 24) |
                                      ((refSign > 0.f ? 1u : 0u) << 23) |
                                      (static_cast<std::uint32_t>(incAxisB) << 16) |
                                      ((incSign > 0.f ? 1u : 0u) << 15) |
                                      static_cast<std::uint32_t>(i);
            cand[candN].featureKey = key;
            candN++;
        }
    }

    if (candN == 0) {
        // No clipped points survived — fall back to the single deepest-axis
        // point on the incident face. Rare; the SAT minimum-overlap axis is a
        // robust contact normal even when face clipping degenerates. Each of
        // the four incident corners is a candidate.
        m.normalWorld = bestAxis;
        m.pointCount  = 1;
        FVec<3> deepest = incP0;
        float bestSd = OmegaGTE::dot(refNormal, incP0) - refD;
        const float sd1 = OmegaGTE::dot(refNormal, incP1) - refD;
        const float sd2 = OmegaGTE::dot(refNormal, incP2) - refD;
        const float sd3 = OmegaGTE::dot(refNormal, incP3) - refD;
        if (sd1 < bestSd) { bestSd = sd1; deepest = incP1; }
        if (sd2 < bestSd) { bestSd = sd2; deepest = incP2; }
        if (sd3 < bestSd) { bestSd = sd3; deepest = incP3; }
        m.points[0].positionWorld = deepest - refNormal * (bestSd * 0.5f);
        m.points[0].depth         = std::max(0.f, -bestSd);
        m.points[0].featureKey    = 0;
        return true;
    }

    reduceContacts(cand, candN, 4);
    m.normalWorld = bestAxis;
    m.pointCount  = static_cast<std::uint32_t>(candN);
    for (int i = 0; i < candN; ++i) m.points[i] = cand[i];
    return true;
}

bool nphBoxPlane(const AQShape &A, const AQShape &B,
                 const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                 AQContactManifold &m) {
    const OBB oa = makeOBB(A, xfA);
    const PlaneWorld pl = makePlaneWorld(B, xfB);
    AQContactPoint cand[8];
    int count = 0;
    for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2)
    for (int sz = -1; sz <= 1; sz += 2) {
        const FVec<3> corner = oa.c
            + oa.ax[0] * (static_cast<float>(sx) * oa.h[0])
            + oa.ax[1] * (static_cast<float>(sy) * oa.h[1])
            + oa.ax[2] * (static_cast<float>(sz) * oa.h[2]);
        const float sd = OmegaGTE::dot(pl.n, corner) - pl.d;
        if (sd < 0.f) {
            cand[count].positionWorld = corner - pl.n * sd;     // projection onto plane
            cand[count].depth         = -sd;
            // 3-bit corner signature is the feature key.
            const std::uint32_t k = ((sx > 0) ? 1u : 0u) | ((sy > 0) ? 2u : 0u) | ((sz > 0) ? 4u : 0u);
            cand[count].featureKey = k;
            count++;
        }
    }
    if (count == 0) return false;
    reduceContacts(cand, count, 4);
    m.normalWorld = pl.n * -1.f;
    m.pointCount  = static_cast<std::uint32_t>(count);
    for (int i = 0; i < count; ++i) m.points[i] = cand[i];
    return true;
}

bool nphCapsuleCapsule(const AQShape &A, const AQShape &B,
                       const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                       AQContactManifold &m) {
    const AQTransform<float> xa = shapeWorld(A, xfA);
    const AQTransform<float> xb = shapeWorld(B, xfB);
    const FVec<3> axisA = AQrotate(xa.q, AQvec3(0.f, A.capsule.halfHeight, 0.f));
    const FVec<3> axisB = AQrotate(xb.q, AQvec3(0.f, B.capsule.halfHeight, 0.f));
    const FVec<3> p0A = xa.p - axisA, p1A = xa.p + axisA;
    const FVec<3> p0B = xb.p - axisB, p1B = xb.p + axisB;
    FVec<3> c1 = AQvec3(0.f, 0.f, 0.f), c2 = AQvec3(0.f, 0.f, 0.f);
    segSegClosest(p0A, p1A, p0B, p1B, c1, c2);
    const FVec<3> d = c2 - c1;
    const float d2 = OmegaGTE::dot(d, d);
    const float rA = A.capsule.radius;
    const float rB = B.capsule.radius;
    const float rSum = rA + rB;
    if (d2 >= rSum * rSum) return false;
    const float dist = std::sqrt(std::max(d2, 1e-12f));
    const FVec<3> nAtoB = (dist > 1e-6f) ? d * (1.f / dist) : AQvec3(0.f, 1.f, 0.f);
    m.normalWorld = nAtoB;
    m.pointCount  = 1;
    m.points[0].positionWorld = (c1 + c2) * 0.5f + nAtoB * ((rA - rB) * 0.5f);
    m.points[0].depth = rSum - dist;
    m.points[0].featureKey = 0;
    return true;
}

bool nphCapsulePlane(const AQShape &A, const AQShape &B,
                     const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                     AQContactManifold &m) {
    const AQTransform<float> xa = shapeWorld(A, xfA);
    const PlaneWorld pl = makePlaneWorld(B, xfB);
    const FVec<3> axisA = AQrotate(xa.q, AQvec3(0.f, A.capsule.halfHeight, 0.f));
    const FVec<3> p0 = xa.p - axisA;
    const FVec<3> p1 = xa.p + axisA;
    const float r = A.capsule.radius;
    const float sd0 = OmegaGTE::dot(pl.n, p0) - pl.d;
    const float sd1 = OmegaGTE::dot(pl.n, p1) - pl.d;
    int count = 0;
    if (sd0 < r) {
        m.points[count].positionWorld = p0 - pl.n * sd0;
        m.points[count].depth         = r - sd0;
        m.points[count].featureKey    = 0;
        count++;
    }
    if (sd1 < r) {
        m.points[count].positionWorld = p1 - pl.n * sd1;
        m.points[count].depth         = r - sd1;
        m.points[count].featureKey    = 1;
        count++;
    }
    if (count == 0) return false;
    m.normalWorld = pl.n * -1.f;
    m.pointCount = static_cast<std::uint32_t>(count);
    return true;
}

// Hull / plane — per-vertex penetration test. Cheap and exact: each hull
// vertex has a signed distance from the plane, the negative-side ones are
// candidate contacts. Up to 4 deepest survive. Plane shapes are half-spaces
// so GJK is not an option (no bounded support); this specialized form fills
// the gap. The hull's local pose is composed with the body transform.
bool nphHullPlane(const AQShape &A, const AQShape &B,
                  const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                  const FVec<3> *hullVerts, std::size_t hullVertCount,
                  AQContactManifold &m) {
    if (hullVerts == nullptr || A.hull.vertexCount == 0) return false;
    if (A.hull.firstVertex + A.hull.vertexCount > hullVertCount) return false;
    const AQTransform<float> xa = shapeWorld(A, xfA);
    const PlaneWorld pl = makePlaneWorld(B, xfB);
    const FVec<3> *base = hullVerts + A.hull.firstVertex;
    AQContactPoint cand[16];
    int count = 0;
    for (std::uint32_t i = 0; i < A.hull.vertexCount && count < 16; ++i) {
        const FVec<3> w = xa.transformPoint(base[i]);
        const float sd = OmegaGTE::dot(pl.n, w) - pl.d;
        if (sd < 0.f) {
            cand[count].positionWorld = w - pl.n * sd;
            cand[count].depth         = -sd;
            cand[count].featureKey    = i;
            count++;
        }
    }
    if (count == 0) return false;
    reduceContacts(cand, count, 4);
    m.normalWorld = pl.n * -1.f;
    m.pointCount  = static_cast<std::uint32_t>(count);
    for (int i = 0; i < count; ++i) m.points[i] = cand[i];
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
// GJK/EPA entry points — defined in AQGJK.cpp. Used for any pair where both
// shapes are bounded convex and we don't have a specialized routine (box/
// capsule, anything with a hull and a bounded partner). Returns true on
// contact; writes manifold normal pointing from A to B, with a single
// contact point at the EPA witness pair midpoint.
bool AQgjkEpaContact(const AQShape &shapeA, const AQShape &shapeB,
                     const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                     const FVec<3> *hullVerts, std::size_t hullVertCount,
                     AQContactManifold &out);

// ----------------------------------------------------------------------------
// Dispatcher. Canonicalizes the input by AQShapeType enum order (lower
// first) so each specialized function sees a single (typeA, typeB) order.
// If the caller's (A, B) order is the reverse of canonical, the dispatcher
// flips `out.normalWorld` so it always points from A to B in caller order.
// ----------------------------------------------------------------------------

bool AQnarrowphase(const AQShape &shapeA, const AQShape &shapeB,
                   const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                   const FVec<3> *hullVerts, std::size_t hullVertCount,
                   AQContactManifold &out) {
    AQShapeType tA = shapeA.type;
    AQShapeType tB = shapeB.type;
    bool swapped = false;
    const AQShape *a = &shapeA;
    const AQShape *b = &shapeB;
    const AQTransform<float> *xa = &xfA;
    const AQTransform<float> *xb = &xfB;
    if (static_cast<int>(tA) > static_cast<int>(tB)) {
        swapped = true;
        std::swap(tA, tB);
        a = &shapeB; b = &shapeA;
        xa = &xfB;   xb = &xfA;
    }

    bool hit = false;
    if (tA == AQShapeType::Sphere) {
        switch (tB) {
        case AQShapeType::Sphere:     hit = nphSphereSphere (*a, *b, *xa, *xb, out); break;
        case AQShapeType::Box:        hit = nphSphereBox    (*a, *b, *xa, *xb, out); break;
        case AQShapeType::Capsule:    hit = nphSphereCapsule(*a, *b, *xa, *xb, out); break;
        case AQShapeType::Plane:      hit = nphSpherePlane  (*a, *b, *xa, *xb, out); break;
        case AQShapeType::ConvexHull: hit = AQgjkEpaContact (*a, *b, *xa, *xb,
                                                              hullVerts, hullVertCount, out); break;
        default: break;
        }
    } else if (tA == AQShapeType::Box) {
        switch (tB) {
        case AQShapeType::Box:        hit = nphBoxBox  (*a, *b, *xa, *xb, out); break;
        case AQShapeType::Plane:      hit = nphBoxPlane(*a, *b, *xa, *xb, out); break;
        case AQShapeType::Capsule:    hit = AQgjkEpaContact(*a, *b, *xa, *xb,
                                                            hullVerts, hullVertCount, out); break;
        case AQShapeType::ConvexHull: hit = AQgjkEpaContact(*a, *b, *xa, *xb,
                                                            hullVerts, hullVertCount, out); break;
        default: break;
        }
    } else if (tA == AQShapeType::Capsule) {
        switch (tB) {
        case AQShapeType::Capsule:    hit = nphCapsuleCapsule(*a, *b, *xa, *xb, out); break;
        case AQShapeType::Plane:      hit = nphCapsulePlane  (*a, *b, *xa, *xb, out); break;
        case AQShapeType::ConvexHull: hit = AQgjkEpaContact  (*a, *b, *xa, *xb,
                                                              hullVerts, hullVertCount, out); break;
        default: break;
        }
    } else if (tA == AQShapeType::Plane) {
        switch (tB) {
        case AQShapeType::ConvexHull: hit = nphHullPlane(*b, *a, *xb, *xa,
                                                          hullVerts, hullVertCount, out);
            // We called the hull-plane specialized with hull first; its
            // normal points from hull (its "A") to plane (its "B"). In our
            // canonical order plane < hull so we need the normal to point
            // FROM PLANE TO HULL — opposite. Flip here, then the swapped
            // case below flips again if the caller's order requires it.
            if (hit) out.normalWorld = out.normalWorld * -1.f;
            break;
        default: break;   // plane/plane excluded; broadphase doesn't emit
        }
    } else if (tA == AQShapeType::ConvexHull) {
        // Both hulls — pure GJK/EPA.
        hit = AQgjkEpaContact(*a, *b, *xa, *xb, hullVerts, hullVertCount, out);
    }

    if (hit && swapped) {
        out.normalWorld = out.normalWorld * -1.f;
    }
    return hit;
}
