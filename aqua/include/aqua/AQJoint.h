#ifndef AQUA_AQJOINT_H
#define AQUA_AQJOINT_H

// AQUA Phase 4 — joint descriptors, the per-body CCD/activation enums, and the
// joint handle. AQUA-owned, AQ-prefixed (no namespace, per AGENTS.md). Every
// public type here is trivially-copyable / standard-layout so the joint
// descriptor table uploads to a GPU buffer with no repacking — the Phase 5 PGS
// joint kernel reads the SoA pack one thread per joint (Phase-4 brief §7, §8).
//
// This header is deliberately light: it pulls in only AQBase.h + GTEMath and
// declares value types, so the low-level integrator (AQIntegrator.h) can include
// it for AQActivationState without dragging in the contact / collision surface.
// The new AQConstraintKind cases joints add (JointAxis / JointLimit / JointMotor)
// live in the existing enum in AQContact.h — the row schema stays one type.

#include "AQBase.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>

/// Joint type discriminator. Five specialized types ship in Phase 4 (§5/§11.1);
/// a D6 superset is a §11.1 follow-up that builds the same rows.
enum class AQJointType : std::uint32_t {
    Distance,    ///< 1 row — keep ‖anchorA_world − anchorB_world‖ at the target length
    BallSocket,  ///< 3 rows — coincident world-frame anchors (spherical)
    Hinge,       ///< 5 rows — BallSocket + 2 axis rows (revolute)
    Slider,      ///< 5 rows — 2 perpendicular ball-socket rows + 3 angular rows (prismatic)
    Fixed,       ///< 6 rows — 3 ball-socket + 3 angular
};

/// CCD opt-in per body (§5, §11.5). `Off` is the default — it keeps the Phase 3
/// settling-stack performance bar; the other two are explicit opt-ins.
enum class AQCCDMode : std::uint8_t {
    Off,         ///< Discrete: the body can tunnel through thin shapes
    Speculative, ///< Fatten the broadphase AABB by ‖v‖·dt; cheap, catches the common case
    Continuous,  ///< Conservative-advancement TOI; expensive, exact for the swept volume
};

/// Activation state, stored on the body SoA (§8). The integrator and the PGS
/// sweep fast-path `Sleeping` (no velocity/position update; both-sleeping rows
/// are skipped). `Kinematic` bodies are user-driven pose with infinite mass —
/// they are not integrated, but their implicit velocity ((target − prev)/dt)
/// drives one-way collision response and bilateral joints (§11.7).
enum class AQActivationState : std::uint8_t {
    Active,
    Sleeping,
    Kinematic,
};

/// Per-joint soft-constraint parameters (Catto 2011, §6.D). `frequency` is the
/// angular natural frequency in rad/s; `damping` is the damping ratio
/// (1.0 ⇒ critically damped). The defaults (`0, 0`) produce a HARD constraint —
/// the row reduces to the Phase 3 formula byte-for-byte (compliance 0, no bias
/// rescale). The `(frequency, damping)` → `(compliance, bias)` translation
/// lives in the joint build path behind the pimpl, so users author behaviour.
struct AQJointSoftness {
    float frequency = 0.f;
    float damping   = 0.f;
};

/// Optional per-axis limit + motor for the joint types that carry one (Hinge:
/// angular about the hinge axis; Slider: linear along the slide axis). When
/// `enabled` is false the axis is free; when enabled it is bounded to
/// `[min, max]` (radians for rotational axes, metres for translational). A
/// motor on the same axis drives toward `motorTargetVelocity`, its corrective
/// impulse clamped each sub-step to `±motorMaxImpulse`.
struct AQJointAxisLimit {
    bool  enabled             = false;
    float min                 = 0.f;
    float max                 = 0.f;
    bool  motorEnabled        = false;
    float motorTargetVelocity = 0.f;   ///< rad/s (Hinge) or m/s (Slider)
    float motorMaxImpulse     = 0.f;   ///< per-sub-step clamp; F_max·dt at the call site
};

/// Parameters for a joint. Anchors are in each body's LOCAL frame; the joint
/// build path transforms them to world each sub-step. `axisLocalA` (Hinge /
/// Slider) is in body A's local frame. POD by construction so the descriptor
/// uploads as a row of a typed table (§8).
struct AQUA_EXPORT AQJointDesc {
    AQJointType       type           = AQJointType::BallSocket;
    std::uint32_t     bodyA          = 0;   ///< body index into the space's body SoA
    std::uint32_t     bodyB          = 0;
    OmegaGTE::FVec<3> anchorA        = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> anchorB        = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> axisLocalA     = OmegaGTE::FVec<3>::Create();  ///< Hinge/Slider axis (body-A local)
    float             distanceTarget = 0.f; ///< Distance joint resting length
    AQJointSoftness   softness;             ///< Hard by default
    AQJointAxisLimit  limit;                ///< Hinge: angular; Slider: linear; others ignored
};

/// Opaque joint handle returned by the `AQSpace::create*Joint` factories. Same
/// shape as AQShapeHandle — a small backend-free value (index + generation).
/// A zero generation means "no joint": `valid()` returns false.
struct AQJointHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;
    OMEGA_NODISCARD bool valid() const { return generation != 0; }
};

#endif // AQUA_AQJOINT_H
