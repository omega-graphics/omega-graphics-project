#ifndef AQUA_SRC_AQQUERYMATH_H
#define AQUA_SRC_AQQUERYMATH_H

// Internal seam for Phase 4 queries (Phase-4 brief §6.L). The analytic ray/shape
// and ray/AABB math lives in AQQuery.cpp (pure, testable, no engine state); the
// public AQSpace::raycast / shapecast / overlap methods (AQSpace.cpp, which has
// friend access to the body SoA) iterate the candidate bodies — read straight
// from the per-body fat AABBs the broadphase already produced, valid until the
// next advance — and call this math per body.

#include <aqua/AQCollision.h>
#include <aqua/AQMath.h>
#include <omegaGTE/GTEMath.h>
#include <cstddef>

/// Slab test: does the ray `origin + t·dir`, t ∈ [0, maxT], enter the AABB?
/// On a hit, `tEnter` is the entry parameter (0 if the origin is inside).
bool AQrayAABB(const OmegaGTE::FVec<3> &mn, const OmegaGTE::FVec<3> &mx,
               const OmegaGTE::FVec<3> &origin, const OmegaGTE::FVec<3> &dir,
               float maxT, float &tEnter);

/// Ray vs a single shape worn by a body at `bodyXform`, the surface offset
/// outward by `inflate` (0 for a raycast; the cast shape's bounding radius for a
/// sphere-cast). Returns true and fills (tOut, posOut, normalOut) on a hit with
/// t ∈ [0, maxT]. `dir` need not be unit — t is the parameter, the hit point is
/// `origin + tOut·dir`. Sphere / box / plane are exact; capsule and convex hull
/// fall back to their world AABB (documented approximation — no deliverable
/// raycasts those, and the brute-force oracle uses this same function).
bool AQrayShape(const AQShape &shape, const AQTransform<float> &bodyXform,
                const OmegaGTE::FVec<3> &origin, const OmegaGTE::FVec<3> &dir,
                float maxT, float inflate,
                const OmegaGTE::FVec<3> *hullVerts, std::size_t hullCount,
                float &tOut, OmegaGTE::FVec<3> &posOut, OmegaGTE::FVec<3> &normalOut);

/// Bounding-sphere radius of a shape about its local origin — the inflate radius
/// a sphere-cast of this shape uses.
float AQshapeBoundingRadius(const AQShape &shape,
                            const OmegaGTE::FVec<3> *hullVerts, std::size_t hullCount);

#endif // AQUA_SRC_AQQUERYMATH_H
