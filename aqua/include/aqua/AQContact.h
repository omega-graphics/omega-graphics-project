#ifndef AQUA_AQCONTACT_H
#define AQUA_AQCONTACT_H

// AQUA Phase 3 — contact manifold + constraint row PODs and the per-space
// material-combine policy. AQUA-owned, AQ-prefixed (no namespace, per
// AGENTS.md). Every public surface type here is trivially-copyable / standard-
// layout so the row buffer uploads to a GPU constraint-solver kernel with no
// repacking — the Phase 5 SoA port reads `AQConstraintRow` records coalesced,
// one thread per row (Phase-3 brief §7, §8). All vector members default to a
// zero vector via OmegaGTE's `Create()` factory because `OmegaGTE::Matrix`
// keeps its default constructor private (the Phase-1 idiom AQBodyState /
// AQDebugLine / AQAABB use).
//
// Reading order: AQContactPoint is the smallest grain (one point on a shared
// normal); AQContactManifold groups 1..4 of them under that shared normal +
// combined materials; AQConstraintRow is what the PGS sweep actually reads
// (one normal row + two friction rows per contact point, per §6.D / §8).

#include "AQBase.h"
#include "AQCollision.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>

/// A single contact point. Up to 4 of these share an AQContactManifold and the
/// manifold's normal. `featureKey` identifies which features of the two shapes
/// produced this contact (e.g. for box/box, a packed (face_A, face_B,
/// edge_id) triple; for box/plane, the box corner's sign bits); the
/// persistence cache keys on (sortedPairIndex, featureKey) so the warm-started
/// impulse follows the same point across frames.
///
/// `accumNormal` / `accumFriction[]` are the warm-start carriers — the
/// solver restores them from the cache at the top of each sub-step and writes
/// the post-sweep values back at the bottom.
struct AQContactPoint {
    OmegaGTE::FVec<3> positionWorld = OmegaGTE::FVec<3>::Create();
    float             depth         = 0.f;
    std::uint32_t     featureKey    = 0;
    float             accumNormal       = 0.f;
    float             accumFriction[2]  = {0.f, 0.f};
};

/// A contact manifold between bodies `a` and `b` (always with `a < b` — same
/// invariant as `AQBroadphasePair`). All points share a single world-frame
/// normal pointing FROM A TO B; i.e., to resolve penetration the solver
/// applies `+λ_n · normalWorld` at B and `-λ_n · normalWorld` at A
/// (`AQRigidBody::applyImpulseAtPoint`). 1..4 points; the box/box face clip
/// produces up to 4 in the resting case, sphere/sphere always 1.
struct AQContactManifold {
    std::uint32_t     a = 0;
    std::uint32_t     b = 0;
    OmegaGTE::FVec<3> normalWorld          = OmegaGTE::FVec<3>::Create();
    std::uint32_t     pointCount           = 0;
    AQContactPoint    points[4];
    float             restitutionCombined  = 0.f;
    float             frictionCombined     = 0.f;
};

/// Which kind of constraint row a `AQConstraintRow` carries.
/// `ContactNormal` rows are lower-bounded at 0 (the solver never pulls bodies
/// together); `ContactFriction` rows are cone-clamped each iteration to
/// `±μ · λ_n` of the row their `peerRow` index points at. Phase 4 joints will
/// add their own kinds (revolute axis row, limit row, motor row) with their
/// own bound rules — the SoA layout stays the same.
enum class AQConstraintKind : std::uint32_t {
    ContactNormal,
    ContactFriction,
};

/// A single row consumed by the PGS sweep (Phase-3 brief §6.D, §7). Three
/// rows per contact point: one normal (λ ≥ 0) + two friction (|λ| ≤ μ·λ_n).
/// The fields are deliberately self-contained — `contactPoint`, `rA`, `rB`,
/// `direction`, and `effectiveMass` are precomputed once per sub-step so the
/// inner iteration loop touches only constant-size POD per row and the body
/// state, no per-row manifold lookup. This is the layout shape Phase 5's GPU
/// kernel reads coalesced (one thread per row).
struct AQConstraintRow {
    AQConstraintKind  kind          = AQConstraintKind::ContactNormal;
    std::uint32_t     bodyA         = 0;
    std::uint32_t     bodyB         = 0;
    OmegaGTE::FVec<3> contactPoint  = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> rA            = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> rB            = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> direction     = OmegaGTE::FVec<3>::Create();
    float             effectiveMass = 0.f;   ///< 1 / Keff, precomputed once
    float             bias          = 0.f;   ///< restitution bias on normal; 0 on friction
    float             accumImpulse  = 0.f;   ///< warm-started across the sweep
    std::uint32_t     peerRow       = 0;     ///< friction → its normal row index
    float             frictionCoeff = 0.f;   ///< μ on friction rows; 0 on normal
};

/// PhysX/Chaos-style material-combine rules. Picked per-AQSpace via
/// `AQSpace::setMaterialCombine`. `Average` is the default — the most
/// physically-defensible isotropic-material policy and the PhysX default.
/// `Max` is the gameplay "super bouncy ball" override (the ball's restitution
/// wins regardless of the floor); `Min` is the conservative "weakest link";
/// `Multiply` is the Chaos-style product.
enum class AQMaterialCombine : std::uint8_t {
    Average,
    Min,
    Max,
    Multiply,
};

#endif // AQUA_AQCONTACT_H
