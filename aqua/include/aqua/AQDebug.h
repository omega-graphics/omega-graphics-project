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
/// Plain value type — drainable buffers are `OmegaCommon::Vector<AQDebugLine>`. The FVec
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
    AQDebugNone             = 0,
    AQDebugBodyAxes         = 1U << 0,   ///< RGB principal axes at the COM (3 lines/body)
    AQDebugVelocity         = 1U << 1,   ///< Linear velocity vector
    AQDebugAngularVel       = 1U << 2,   ///< Angular velocity vector (world frame)
    AQDebugMomentum         = 1U << 3,   ///< World angular-momentum L vector
    // --- Phase 2 additions (broadphase + collision shapes, see Phase-2 brief §9) ---
    AQDebugAABB             = 1U << 4,   ///< Per-body fattened world AABB (12 line segments)
    AQDebugBroadphasePair   = 1U << 5,   ///< One line per emitted candidate, COM(a)→COM(b)
    AQDebugBroadphaseGuard  = 1U << 6,   ///< Single red line when candidate/brute(n²) > 0.5
    // --- Phase 3 additions (narrowphase + contact solver, see Phase-3 brief §9) ---
    AQDebugContactPoint     = 1U << 7,   ///< Tiny cross at each contact point (3 line segments)
    AQDebugContactNormal    = 1U << 8,   ///< Contact normal scaled by penetration depth
    AQDebugContactImpulse   = 1U << 9,   ///< Accumulated normal-impulse vector (post-solve)
    // --- Phase 4 additions (joints, islands, queries, sleep, CCD; see Phase-4 brief §9) ---
    AQDebugJointAnchor      = 1U << 10,  ///< A short cross at each joint's two world anchor points
    AQDebugJointAxis        = 1U << 11,  ///< The hinge/slider axis as a colored segment at anchor A
    AQDebugIsland           = 1U << 12,  ///< Per-island: COM(member)→COM(root) spokes, color by sleep state
    AQDebugRaycastHit       = 1U << 13,  ///< Last query: origin→hit segment + a short normal tick
    AQDebugSleepingBody     = 1U << 14,  ///< A greyed marker cross at each sleeping body's COM
    AQDebugCCDSweep         = 1U << 15,  ///< One segment per opted-in body showing the swept extension
    // --- Phase 6 additions (particle systems, see Phase-6 brief §9/§13) ---
    AQDebugParticle         = 1U << 16,  ///< Per-particle velocity vector + live-set bounds; a live/free count line
    AQDebugForceField       = 1U << 17,  ///< Each field's influence region (vortex axis, point radius, wind direction)
    // --- Phase 7 additions (XPBD constraint core, see Phase-7 brief §9/§10) ---
    AQDebugConstraint       = 1U << 18,  ///< One line per active constraint, color-graded by strain
    AQDebugConstraintColor  = 1U << 19,  ///< Constraint lines tinted by graph-color batch id
};

#endif // AQUA_AQDEBUG_H
