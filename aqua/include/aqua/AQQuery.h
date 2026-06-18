#ifndef AQUA_AQQUERY_H
#define AQUA_AQQUERY_H

// AQUA Phase 4 — gameplay-query result types and the trigger-event value type.
// AQUA-owned, AQ-prefixed (no namespace, per AGENTS.md). All types are
// trivially-copyable / standard-layout. Queries (raycast / shapecast / overlap)
// walk the same per-step broadphase grid the simulation builds (§5, §6.L); the
// grid scratch is promoted to "valid until the next advance" so these reads need
// no parallel acceleration structure.

#include "AQBase.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>

/// A single raycast or shapecast hit. Results are returned in fraction-sorted
/// order (smallest `fraction` first); the order is deterministic across runs
/// because the sort is on `(fraction, bodyIndex)` (§8). `position` and `normal`
/// are world-frame; for a shapecast `position` is the witness point on the hit
/// shape at the time of impact.
struct AQRaycastHit {
    std::uint32_t     bodyIndex = 0;
    float             fraction  = 0.f;   ///< t in [0, maxT] along the ray / sweep
    OmegaGTE::FVec<3> position  = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> normal    = OmegaGTE::FVec<3>::Create();
};

/// Filter for queries — same layer/mask shape as `AQCollisionFilter`, paired
/// with a body's filter via the same symmetric rule (`AQfilterAccepts`).
struct AQQueryFilter {
    std::uint32_t layer = 1u;
    std::uint32_t mask  = ~0u;
};

/// Trigger-overlap lifecycle kind. `Enter` the sub-step an overlap begins,
/// `Stay` while it persists, `Exit` the sub-step it ends.
enum class AQTriggerEventKind : std::uint8_t {
    Enter,
    Stay,
    Exit,
};

/// A trigger event drained via `AQSpace::triggerEvents()` and cleared each
/// `advance`. Body indices `(a, b)` satisfy `a < b` (the deterministic ordering
/// the broadphase already guarantees, §9). At least one of `a`/`b` is a body
/// whose descriptor set `isTrigger = true`.
struct AQTriggerEvent {
    std::uint32_t      a    = 0;
    std::uint32_t      b    = 0;
    AQTriggerEventKind kind = AQTriggerEventKind::Enter;
};

#endif // AQUA_AQQUERY_H
