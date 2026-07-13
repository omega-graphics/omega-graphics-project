#ifndef AQUA_AQRIGIDBODY_H
#define AQUA_AQRIGIDBODY_H

// The single-body public surface — descriptor, type enum, and the pimpl handle
// that lives inside an AQSpace. Split out of AQSpace.h so consumers that only
// touch bodies (gameplay code threading impulses, debug overlays reading
// momentum) don't drag in the AQSpace ownership + step-loop surface they
// don't use, and so the kREATE debug-draw plan §7 adapter — which consumes
// AQRigidBody read-only — can include just this header.

#include "AQBase.h"
#include "AQCollision.h"
#include "AQJoint.h"   // AQActivationState, AQCCDMode (Phase 4)
#include "AQMath.h"
#include <omegaGTE/GTEMath.h>
#include <memory>

/// How a body participates in the simulation.
enum class AQBodyType {
    Static,    ///< Never moves; treated as infinite mass (ground, level geometry).
    Dynamic,   ///< Integrated each step; affected by gravity and forces.
    Kinematic, ///< Phase 4 — user-driven pose, infinite mass, pushes dynamics one-way.
};

/// Parameters for creating an AQRigidBody.
struct AQUA_EXPORT AQBodyDesc {
    AQBodyType type = AQBodyType::Dynamic;

    // --- pose & motion ---
    OmegaGTE::FVec<3>     position        = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FQuaternion orientation     = OmegaGTE::FQuaternion::Identity();
    OmegaGTE::FVec<3>     linearVelocity  = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3>     angularVelocity = OmegaGTE::FVec<3>::Create();  ///< world frame

    // --- mass properties ---
    float mass = 1.f;  ///< Ignored for Static bodies.
    /// Diagonal principal moments of inertia, body frame. Zero ⇒ derive from the
    /// body's collision shape (Phase 2); until shapes exist, fill via the
    /// AQMath.h helpers (e.g. AQinertiaSolidBox).
    OmegaGTE::FVec<3> inertiaPrincipalMoments = OmegaGTE::FVec<3>::Create();
    /// Optional FULL 3×3 inertia tensor (body frame). If any entry is non-zero,
    /// `addBody` diagonalizes it (`AQdiagonalizeInertia`) and folds the
    /// principal-axis rotation into the body orientation, PhysX/Chaos-style.
    /// When zero (the default), `inertiaPrincipalMoments` is used unchanged —
    /// the Phase 1 fast path. (Phase-1.1 §6.2.)
    AQMat3F           inertiaTensor = AQMat3F::Create();
    /// Reserved center-of-mass offset (Phase-1.1 §6.2, §11.7). The field enters
    /// the model now so Phase 2 shape local-transforms add geometry rather than
    /// refactor body state; the offset has no effect until Phase 2 wires it.
    OmegaGTE::FVec<3> centerOfMass = OmegaGTE::FVec<3>::Create();

    // --- robustness controls (Phase 1.1 §6.4) ---
    float linearDamping   = 0.f;   ///< Exponential per-sub-step: v ← v / (1 + c·dt).
    float angularDamping  = 0.f;   ///< Same shape on body-frame angular velocity.
    float gravityScale    = 1.f;   ///< Per-body multiplier on the space gravity.
    float maxAngularSpeed = 0.f;   ///< 0 ⇒ unlimited; opt-in safety clamp.

    // --- material (consumed in Phase 3; reserved now to avoid descriptor churn) ---
    float restitution = 0.f;
    float friction    = 0.5f;

    // --- Phase 2 additions (collision shapes + broadphase) ---
    /// Optional collision geometry. Obtain a handle from
    /// `AQSpace::createSphereShape`/etc.; an invalid handle (the default)
    /// means the body has no shape and is not seen by the broadphase. When
    /// the body is dynamic, the shape is valid, and both `inertiaTensor` and
    /// `inertiaPrincipalMoments` are zero, `addBody` derives the diagonal
    /// moments from the shape (closes the Phase 1 hook in `AQRigidBody.h`).
    AQShapeHandle     shape;
    /// Collision-filter layer/mask. Defaults: layer 1, mask = all — every
    /// body collides with every other body. Phase 2 brief §11.5 lean.
    AQCollisionFilter filter;

    // --- Phase 4 additions (triggers, CCD, sleep) ---
    /// When true the body is a TRIGGER: it still generates broadphase candidate
    /// pairs and an overlap test runs, but the narrowphase emits Enter/Stay/Exit
    /// events (drained via `AQSpace::triggerEvents`) instead of constraint rows.
    /// No collision response. (Phase-4 brief §6.M.)
    bool      isTrigger = false;
    /// Opt-in continuous-collision mode for fast/thin bodies. `Off` (default)
    /// keeps the Phase 3 discrete path and the settling-stack performance bar.
    AQCCDMode ccdMode   = AQCCDMode::Off;
    /// Per-body sleep thresholds; 0 ⇒ use the space default
    /// (`AQSpace::setSleepThresholds`). Linear m/s, angular rad/s. An island
    /// sleeps when every member is below threshold for the configured frames.
    float     sleepLinearVelocity  = 0.f;
    float     sleepAngularVelocity = 0.f;
};

/// Handle to a body living inside an AQSpace. Owned by the AQSpace; obtained from
/// `AQSpace::addBody` and valid until removed or the AQSpace is destroyed.
class AQUA_EXPORT AQRigidBody {
public:
    ~AQRigidBody();

    // --- linear state ---
    OMEGA_NODISCARD OmegaGTE::FVec<3> position() const;
    void setPosition(const OmegaGTE::FVec<3> &p);

    OMEGA_NODISCARD OmegaGTE::FVec<3> velocity() const;
    void setVelocity(const OmegaGTE::FVec<3> &v);

    // --- angular state ---
    OMEGA_NODISCARD OmegaGTE::FQuaternion orientation() const;
    void setOrientation(const OmegaGTE::FQuaternion &q);
    OMEGA_NODISCARD OmegaGTE::FVec<3> angularVelocity() const;   ///< world frame
    void setAngularVelocity(const OmegaGTE::FVec<3> &w);         ///< world frame

    // --- mass properties ---
    OMEGA_NODISCARD float mass() const;                          ///< 0 ⇒ static
    OMEGA_NODISCARD OmegaGTE::FVec<3> inertiaPrincipalMoments() const;
    /// World-space inverse inertia tensor: R · diag(invMomentsBody) · Rᵀ. Phase 3
    /// contact solvers and the conserved-quantity accessors below share this.
    OMEGA_NODISCARD OmegaGTE::FMatrix<3,3> worldInverseInertia() const;

    // --- conserved-quantity accessors (Phase 1.1 §6.3) ---
    // Make tests + gameplay assert against the engine's own numbers rather than
    // re-deriving L / E by hand.
    OMEGA_NODISCARD OmegaGTE::FVec<3> linearMomentum()  const;   ///< m · v (world)
    OMEGA_NODISCARD OmegaGTE::FVec<3> angularMomentum() const;   ///< R · Ib · ω_b (world)
    OMEGA_NODISCARD float             kineticEnergy()   const;   ///< ½m‖v‖² + ½ω_b·Ib·ω_b

    // --- robustness controls (Phase 1.1 §6.4) ---
    void  setLinearDamping(float c);   OMEGA_NODISCARD float linearDamping()   const;
    void  setAngularDamping(float c);  OMEGA_NODISCARD float angularDamping()  const;
    void  setGravityScale(float s);    OMEGA_NODISCARD float gravityScale()    const;
    /// 0 ⇒ unlimited (default). Opt-in safety valve for fast spinners; changes
    /// the physics, so off by default — the adaptive gyroscopic iteration is
    /// the silent default stability path.
    void  setMaxAngularSpeed(float s); OMEGA_NODISCARD float maxAngularSpeed() const;

    // --- material coefficients (Phase 3) ---
    /// Coefficient of restitution in [0, 1]. 0 ⇒ perfectly inelastic; 1 ⇒
    /// perfectly elastic. Combined per-pair via the AQSpace's restitution
    /// combine rule (see `AQSpace::setMaterialCombine`).
    OMEGA_NODISCARD float restitution() const;
    void setRestitution(float r);
    /// Coefficient of friction ≥ 0. Single μ on the isotropic Coulomb cone
    /// (the Phase 3 lean — anisotropic friction is the §11.6 deferred case).
    /// Combined per-pair via the AQSpace's friction combine rule.
    OMEGA_NODISCARD float friction() const;
    void setFriction(float mu);

    // --- collision geometry & filter (Phase 2) ---
    /// Current shape handle. Invalid handle ⇒ no shape (broadphase-invisible).
    OMEGA_NODISCARD AQShapeHandle shape() const;
    /// Sets the shape handle. Does NOT re-derive inertia automatically — the
    /// auto-derive runs in `addBody` only. Callers that want the moments
    /// recomputed should set them to zero and re-add the body, or set the
    /// principal moments explicitly via the descriptor.
    void setShape(const AQShapeHandle &s);

    /// World-space fattened AABB of the body's shape (per Phase 2 §6.A). Both
    /// components are zero if the body has no shape. Useful for debug overlays
    /// and gameplay queries.
    OMEGA_NODISCARD OmegaGTE::FVec<3> aabbMin() const;
    OMEGA_NODISCARD OmegaGTE::FVec<3> aabbMax() const;

    OMEGA_NODISCARD AQCollisionFilter collisionFilter() const;
    void setCollisionFilter(const AQCollisionFilter &f);

    // --- activation / sleep (Phase 4) ---
    /// Current activation state. The integrator and PGS sweep fast-path
    /// `Sleeping`; `Kinematic` is reported for kinematic bodies.
    OMEGA_NODISCARD AQActivationState activation() const;
    /// Force the body Active (and, if it is in an island, wakes the whole
    /// island on the next step). A no-op on kinematic bodies.
    void wakeUp();
    /// Force the body Sleeping (clears its velocities). A no-op on static /
    /// kinematic bodies. The island sleep logic may re-wake it if it is
    /// touched by an active body.
    void putToSleep();

    // --- triggers / CCD (Phase 4) ---
    OMEGA_NODISCARD bool isTrigger() const;
    OMEGA_NODISCARD AQCCDMode ccdMode() const;
    void setCCDMode(AQCCDMode m);

    // --- kinematic control (Phase 4) ---
    /// Set the kinematic target pose. Only meaningful for `Kinematic` bodies:
    /// at the next sub-step the body teleports to `(p, q)` and its implicit
    /// velocity ((target − previous)/dt) is used for one-way collision response
    /// and to drive joints. `setPosition`/`setOrientation` still work but do
    /// NOT compute the implicit velocity.
    void setKinematicTarget(const OmegaGTE::FVec<3>     &p,
                            const OmegaGTE::FQuaternion &q);

    // --- force / torque / impulse API ---
    // Forces/torques accumulate in world space and are consumed at the start of
    // each sub-step. Impulses apply instantaneously to velocity.
    void applyForce(const OmegaGTE::FVec<3> &force);
    void applyForceAtPoint(const OmegaGTE::FVec<3> &force,
                           const OmegaGTE::FVec<3> &worldPoint);   // + torque (r × F)
    void applyTorque(const OmegaGTE::FVec<3> &torque);
    void applyImpulse(const OmegaGTE::FVec<3> &impulse);           // instantaneous Δp
    void applyImpulseAtPoint(const OmegaGTE::FVec<3> &impulse,
                             const OmegaGTE::FVec<3> &worldPoint);  // + angular impulse
    void applyAngularImpulse(const OmegaGTE::FVec<3> &angularImpulse);

    OMEGA_NODISCARD AQBodyType type() const;

private:
    AQRigidBody();
    friend class AQSpace;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AQUA_AQRIGIDBODY_H
