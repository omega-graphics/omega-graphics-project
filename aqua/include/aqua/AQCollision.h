#ifndef AQUA_AQCOLLISION_H
#define AQUA_AQCOLLISION_H

// AQUA Phase 2 — collision shapes, broadphase pair, and filter types.
// AQUA-owned, AQ-prefixed (no namespace, per AGENTS.md). All types here are
// trivially-copyable / standard-layout so they upload to a GPU buffer with no
// repacking — the Phase 5 sort-based-grid kernel reads `AQShape` records and
// `AQBroadphasePair` outputs as raw arrays. Math (AABB, |R|·h oriented-box
// bound) lives in AQMath.h and is *consumed* here (Phase 1.1 §6.1 shipped it).
//
// Convention parity with the rest of AQUA: vectors are the borrowed
// `OmegaGTE::FVec<3>` at the public-API surface; AABB math stays Ty-generic on
// `AQAABB<Ty>` so the broadphase oracle in tests can run at double precision.

#include "AQBase.h"
#include "AQMath.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>
#include <cstddef>

/// Tag for the analytic primitive an AQShape carries. The Phase 2 brief §11.6
/// lean is sphere / box / capsule / plane first; convex hull is the stretch.
enum class AQShapeType : std::uint32_t {
    Sphere,
    Box,
    Capsule,
    Plane,
    ConvexHull,
};

/// Collision shape — a tagged POD union. Primitive parameters are raw floats
/// (not GTE `FVec`) so the type stays trivially copyable: `OmegaGTE::Matrix`
/// has a private default ctor and so cannot be a union member. The local pose
/// (offset from the body COM and orientation of the shape relative to the
/// body frame) is stored as raw position + quaternion components for the same
/// reason — it converts to/from `AQTransform<float>` at call sites in
/// `AQCollision.cpp`. The convex-hull case stores `(firstVertex, vertexCount)`
/// into a vertex pool owned by the AQSpace (§8 — shapes pooled and shared).
struct AQShape {
    AQShapeType type;

    // Local pose: shape pose relative to the body's COM frame. Phase 2 ships
    // this defaulted to identity; non-trivial compounds remain a future use.
    float lpx = 0.f, lpy = 0.f, lpz = 0.f;
    float lqx = 0.f, lqy = 0.f, lqz = 0.f, lqw = 1.f;

    union {
        struct { float radius; }                          sphere;
        struct { float hx, hy, hz; }                      box;       // half-extents
        struct { float radius, halfHeight; }              capsule;   // axis = local +Y
        struct { float nx, ny, nz, offset; }              plane;     // n·x = offset half-space
        struct { std::uint32_t firstVertex, vertexCount; } hull;
    };

    AQShape() : type(AQShapeType::Sphere), sphere{0.f} {}
};

/// Opaque, backend-free handle into an AQSpace's shape table. The `generation`
/// counter (incremented on remove) keeps a stale handle from quietly aliasing
/// a recycled slot. A zero generation means "no shape" — `valid()` returns
/// false and the body skips collision processing.
struct AQShapeHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;
    AQUA_NODISCARD bool valid() const { return generation != 0; }
};

/// 32-bit layer membership + 32-bit collision mask. Two bodies generate a
/// candidate pair iff `(a.layer & b.mask) && (b.layer & a.mask)` — the
/// symmetric rule the Phase 2 brief §11.5 lean specifies. Defaults to
/// "everyone in layer 1, collides with everyone".
struct AQCollisionFilter {
    std::uint32_t layer = 1u;
    std::uint32_t mask  = ~0u;
};

inline bool AQfilterAccepts(const AQCollisionFilter &a, const AQCollisionFilter &b) {
    return ((a.layer & b.mask) != 0u) && ((b.layer & a.mask) != 0u);
}

/// A broadphase candidate pair (indices into the AQSpace's body-SoA arrays).
/// Invariant: `a < b` (the deterministic ordering of the §5 / §8 decision).
struct AQBroadphasePair {
    std::uint32_t a;
    std::uint32_t b;
};

inline bool operator<(const AQBroadphasePair &lhs, const AQBroadphasePair &rhs) {
    return (lhs.a != rhs.a) ? (lhs.a < rhs.a) : (lhs.b < rhs.b);
}
inline bool operator==(const AQBroadphasePair &lhs, const AQBroadphasePair &rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b;
}

// ============================================================================
// Free functions over AQShape.
// ============================================================================

/// World-space axis-aligned bound of a shape worn by a body at `bodyXform`.
/// Composes the shape's local transform with the body transform and uses the
/// rotation-correct |R|·h bound (Phase 1.1 §6.1 `AQaabbOfOrientedBox`) where
/// applicable. The plane case returns a "very large" finite AABB — planes
/// participate in collision only via the filter, and the broadphase never
/// emits plane/plane pairs.
///
/// `hullVerts` and `hullVertCount` describe the AQSpace's vertex pool; passed
/// in so the math layer stays free of any space-owned storage. The pointer
/// is unused unless `shape.type == AQShapeType::ConvexHull`.
AQAABB<float> AQshapeAABB(const AQShape &shape,
                          const AQTransform<float> &bodyXform,
                          const OmegaGTE::FVec<3> *hullVerts = nullptr,
                          std::size_t hullVertCount = 0);

/// Support function for Phase 3 narrowphase (GJK). The world-space point on
/// the shape's surface that maximizes `dot(p, dirWorld)`. Minimal correct
/// implementations land in Phase 2 so the public surface is complete; the
/// GJK consumer ships in Phase 3.
OmegaGTE::FVec<3> AQshapeSupport(const AQShape &shape,
                                 const OmegaGTE::FVec<3> &dirWorld,
                                 const AQTransform<float> &bodyXform,
                                 const OmegaGTE::FVec<3> *hullVerts = nullptr,
                                 std::size_t hullVertCount = 0);

/// Diagonal principal moments of inertia for a shape of the given total mass.
/// Bridges to the Phase 1 helpers (`AQinertiaSolidBox / Sphere / Capsule`,
/// AQMath.h) so the Phase 2 brief §1 hook closes without new numerics. The
/// convex-hull case uses its local-AABB extents as a solid-box approximation
/// (the §11.6 stretch posture — full hull inertia ships with Phase 3 if
/// profiling warrants it). The plane case returns zero (planes are
/// static-only intent and have no inertia).
OmegaGTE::FVec<3> AQshapeInertiaMoments(const AQShape &shape, float mass,
                                        const OmegaGTE::FVec<3> *hullVerts = nullptr,
                                        std::size_t hullVertCount = 0);

#endif // AQUA_AQCOLLISION_H
