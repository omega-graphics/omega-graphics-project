#ifndef AQUA_AQXPBD_H
#define AQUA_AQXPBD_H

// AQUA Phase 7 — XPBD constraint-projection core: the public POD types
// (Phase-7 brief §7, corrected per the §13.1 substrate audit). AQUA-owned,
// AQ-prefixed (no namespace, per AGENTS.md). Every type here is trivially-
// copyable / standard-layout so a constraint array or parameter block uploads
// to a GPU buffer with NO repacking — the same discipline AQShape, AQDebugLine,
// and AQParticles follow. No virtuals anywhere near the kernel path: the
// constraint interface is per-type TYPED ARRAYS (coalesced reads) plus the
// `AQConstraintType` tag, never a subclass hierarchy. Phases 8–10 extend it by
// adding an enum value and their own typed array — the solver core does not
// change.
//
// Corrections to the brief's §7 draft (mirroring Phase 6 §13.1):
//   * There is no `AQVec3f` — vector members are `OmegaGTE::FVec<3>`, the
//     public-API convention the rest of AQUA uses.
//   * `AQXPBDParams` carries NO `fixedDt` and NO `gravity`. The engine has one
//     clock — `AQContext`'s fixed sub-step — and one gravity — the space's.
//     XPBD subdivides the engine sub-step into `substeps` slices of
//     h = fixedDt / substeps (Macklin 2019 small steps); duplicating either
//     here would fork the single source of truth Phase 6 §14.1 established.
//   * The generic tagged `AQConstraint` record was dropped as dead weight: the
//     LOGICAL interface is the `AQConstraintType` taxonomy, the PHYSICAL
//     storage is one typed array per type, and nothing would ever instantiate
//     the union-of-all-types record. Kernels switch on the batch's type, not
//     on per-record tags.

#include "AQBase.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>
#include <type_traits>

// --- Constraint taxonomy -----------------------------------------------------
// Distance ships in Phase 7; the reserved values are the downstream phases'
// slots (Phase-7 brief §7). Appending here + adding a typed array is ALL a new
// constraint type needs — the projection loop, coloring, and batching are
// shared.
enum AQConstraintType : std::uint32_t {
    AQConstraintDistance = 0,   ///< Phase 7 (this phase)
    AQConstraintBending  = 1,   ///< Phase 8 (cloth)
    AQConstraintVolume   = 2,   ///< Phase 9 (deformable solids)
    AQConstraintDensity  = 3,   ///< Phase 10 (PBF fluids)
    AQConstraintTypeCount
};

// --- Distance constraint (typed array record) --------------------------------
// One record per constraint, stored sorted by (color, authoring index) so a
// color batch is a contiguous, coalesced-read range. `lambda` is the XPBD
// Lagrange-multiplier accumulator — solver state reset at the top of every
// sub-step, carried here (not in a side array) so the GPU path uploads ONE
// buffer per type. `compliance` is XPBD's α = 1/k: 0 ⇒ rigid/inextensible;
// > 0 ⇒ a physical, timestep-independent softness (the §1 oracle property).
struct AQDistanceConstraint {
    std::uint32_t a = 0;            ///< particle index into the owning body
    std::uint32_t b = 0;            ///< particle index into the owning body
    float         restLength = 0.f; ///< C(x) = |xa - xb| - restLength
    float         compliance = 0.f; ///< α = 1/k; 0 ⇒ hard/inextensible
    float         lambda     = 0.f; ///< accumulated multiplier (per sub-step)
    std::uint32_t color      = 0;   ///< graph-color / batch id
};

// --- Coloring / batching metadata --------------------------------------------
// One record per color: a contiguous range of the color-sorted constraint
// array. Colors are projected SERIALLY; constraints within a color share no
// particle, so they project in parallel with no write conflicts — on the GPU
// this is a correctness precondition, not an optimization (Phase-7 brief §2.3).
struct AQConstraintBatch {
    AQConstraintType type            = AQConstraintDistance;
    std::uint32_t    color           = 0;
    std::uint32_t    firstConstraint = 0; ///< offset into the color-sorted array
    std::uint32_t    constraintCount = 0; ///< constraints in this color batch
};

// --- Solve parameters ---------------------------------------------------------
// Per-space XPBD tuning (AQSpace::setXPBDParams). The solver runs `substeps`
// XPBD slices per ENGINE fixed sub-step, each slice one predict → project →
// derive-velocity pass of h = fixedDt / substeps (Macklin 2019: n sub-steps ×
// 1 iteration beats 1 step × n iterations at equal cost). `iterations` is the
// escape hatch the brief keeps configurable — the tuned path leaves it 1.
struct AQXPBDParams {
    std::uint32_t substeps        = 4;    ///< XPBD slices per engine sub-step
    std::uint32_t iterations      = 1;    ///< projection sweeps per slice (lean: 1)
    float         velocityDamping = 0.f;  ///< [0,1) post-derive damping (§11 #5)
    /// Loud-guard threshold on the per-slice position delta of any particle,
    /// in world units. A projection that moves a particle further than this in
    /// one slice is treated as a diverging solve: the delta is clamped, the
    /// constraint is flagged on the debug bus (AQDebugConstraint), and a
    /// diagnostic line names the constraint, its color, and both particles —
    /// never a silent NaN three frames later (§6 "guards for the 3am engineer").
    float         explosionThreshold = 10.f;
    /// Far-from-origin hardening (§13.7 7h #2). When false (the default) every
    /// body keeps its origin at 0 and solves in world coordinates — byte-for-
    /// byte identical to the pre-7h path. When true, each body stores particle
    /// positions as offsets from a per-body origin that re-bases toward the
    /// particle centroid, so the derive step's `(x − x_prev)/h` and the
    /// projection differences run on small offsets and keep full float precision
    /// even as the body travels far from the world origin (the solver2d XPBD
    /// precision failure). CPU-path only; GPU-batched bodies keep origin 0.
    bool          originRelative    = false;
};

// --- Handles ------------------------------------------------------------------
// Opaque ids resolved behind the pimpl (the AQParticleSystemHandle idiom): a
// stable id, never a pointer, so a caller can never dangle into freed state.
struct AQXPBDBodyHandle {
    std::uint64_t id = 0;
    OMEGA_NODISCARD bool valid() const { return id != 0; }
};

/// Identifies one authored constraint: the owning body + the constraint's
/// authoring index within that body's typed array for `type`. Authoring order
/// is stable — coloring re-sorts an internal mirror, never this index space.
struct AQConstraintHandle {
    std::uint64_t    body  = 0;                     ///< AQXPBDBodyHandle::id
    AQConstraintType type  = AQConstraintDistance;
    std::uint32_t    index = 0;                     ///< authoring index within the type
    OMEGA_NODISCARD bool valid() const { return body != 0; }
};

// --- Body description ----------------------------------------------------------
// An XPBD body is a PERSISTENT particle set + the constraints over it. It is
// deliberately NOT an emitter-driven AQParticleSystem: those particles expire
// and their slots are compacted/relocated, which would corrupt constraint
// indices. XPBD particles live for the body's lifetime at stable indices.
// `invMass[i] == 0` pins particle i (kinematic anchor — skipped by predict and
// projection, the rope's pinned end).
struct AQXPBDBodyDesc {
    const OmegaGTE::FVec<3> *positions = nullptr; ///< [count] initial world positions
    const float             *invMass   = nullptr; ///< [count] 1/mass; 0 ⇒ pinned
    std::uint32_t            count     = 0;
};

static_assert(std::is_trivially_copyable<AQDistanceConstraint>::value,
              "AQDistanceConstraint must stay GPU-uploadable (no repacking)");
static_assert(std::is_trivially_copyable<AQConstraintBatch>::value,
              "AQConstraintBatch must stay GPU-uploadable (no repacking)");
static_assert(std::is_trivially_copyable<AQXPBDParams>::value,
              "AQXPBDParams must stay GPU-uploadable (no repacking)");

#endif // AQUA_AQXPBD_H
