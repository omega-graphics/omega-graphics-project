#ifndef AQUA_AQDEBUG_H
#define AQUA_AQDEBUG_H

// AQUA's neutral, drainable debug-primitive surface (Phase-1.1 §6.5). AQUA owns
// no renderer — it emits structured `AQDebugLine` records into a per-space
// buffer that the consumer (kREATE, a test, a tools UI) drains and replays. The
// boundary `Physics-Roadmap.md` §6 specifies; the kREATE debug-draw plan §7
// consumes; Phase 2 extends the same bus with AABBs and overlapping pairs.
//
// Header-only and AQ-prefixed (no namespace, per AGENTS.md). The line carries
// only the borrowed `OmegaGTE::FVec<3>` and an RGBA float quartet, so it stays
// trivially copyable / GPU-uploadable and never depends on any backend type.

#include <omegaGTE/GTEMath.h>
#include <cstdint>

/// A single debug-draw line segment from world `a` to world `b`, RGBA in [0,1].
/// Plain value type — drainable buffers are `std::vector<AQDebugLine>`. The FVec
/// members are default-initialized to zero via GTE's `Create()` factory because
/// `OmegaGTE::Matrix` keeps its default constructor private (the same Phase 1
/// idiom AQBodyState uses).
struct AQDebugLine {
    OmegaGTE::FVec<3> a = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> b = OmegaGTE::FVec<3>::Create();
    float             rgba[4] = {1.f, 1.f, 1.f, 1.f};
};

/// What an AQSpace emits each step. Bitfield; combine with bitwise OR.
/// `AQDebugNone` (the default) disables emission entirely — the per-space
/// buffer stays empty and `drainDebugLines` returns nothing, so the surface
/// is zero-cost when off.
enum AQDebugFlags : std::uint32_t {
    AQDebugNone        = 0,
    AQDebugBodyAxes    = 1U << 0,   ///< RGB principal axes at the COM (3 lines/body)
    AQDebugVelocity    = 1U << 1,   ///< Linear velocity vector
    AQDebugAngularVel  = 1U << 2,   ///< Angular velocity vector (world frame)
    AQDebugMomentum    = 1U << 3,   ///< World angular-momentum L vector
};

#endif // AQUA_AQDEBUG_H
