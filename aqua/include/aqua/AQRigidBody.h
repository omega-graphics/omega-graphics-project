#ifndef AQUA_AQRIGIDBODY_H
#define AQUA_AQRIGIDBODY_H

// The single-body public surface — descriptor, type enum, and the pimpl handle
// that lives inside an AQSpace. Split out of AQSpace.h so consumers that only
// touch bodies (gameplay code threading impulses, debug overlays reading
// momentum) don't drag in the AQSpace ownership + step-loop surface they
// don't use, and so the kREATE debug-draw plan §7 adapter — which consumes
// AQRigidBody read-only — can include just this header.

#include "AQBase.h"
#include "AQMath.h"
#include <omegaGTE/GTEMath.h>
#include <memory>

/// How a body participates in the simulation.
enum class AQBodyType {
    Static,   ///< Never moves; treated as infinite mass (ground, level geometry).
    Dynamic,  ///< Integrated each step; affected by gravity and forces.
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
};

/// Handle to a body living inside an AQSpace. Owned by the AQSpace; obtained from
/// `AQSpace::addBody` and valid until removed or the AQSpace is destroyed.
class AQUA_EXPORT AQRigidBody {
public:
    ~AQRigidBody();

    // --- linear state ---
    AQUA_NODISCARD OmegaGTE::FVec<3> position() const;
    void setPosition(const OmegaGTE::FVec<3> &p);

    AQUA_NODISCARD OmegaGTE::FVec<3> velocity() const;
    void setVelocity(const OmegaGTE::FVec<3> &v);

    // --- angular state ---
    AQUA_NODISCARD OmegaGTE::FQuaternion orientation() const;
    void setOrientation(const OmegaGTE::FQuaternion &q);
    AQUA_NODISCARD OmegaGTE::FVec<3> angularVelocity() const;   ///< world frame
    void setAngularVelocity(const OmegaGTE::FVec<3> &w);         ///< world frame

    // --- mass properties ---
    AQUA_NODISCARD float mass() const;                          ///< 0 ⇒ static
    AQUA_NODISCARD OmegaGTE::FVec<3> inertiaPrincipalMoments() const;
    /// World-space inverse inertia tensor: R · diag(invMomentsBody) · Rᵀ. Phase 3
    /// contact solvers and the conserved-quantity accessors below share this.
    AQUA_NODISCARD OmegaGTE::FMatrix<3,3> worldInverseInertia() const;

    // --- conserved-quantity accessors (Phase 1.1 §6.3) ---
    // Make tests + gameplay assert against the engine's own numbers rather than
    // re-deriving L / E by hand.
    AQUA_NODISCARD OmegaGTE::FVec<3> linearMomentum()  const;   ///< m · v (world)
    AQUA_NODISCARD OmegaGTE::FVec<3> angularMomentum() const;   ///< R · Ib · ω_b (world)
    AQUA_NODISCARD float             kineticEnergy()   const;   ///< ½m‖v‖² + ½ω_b·Ib·ω_b

    // --- robustness controls (Phase 1.1 §6.4) ---
    void  setLinearDamping(float c);   AQUA_NODISCARD float linearDamping()   const;
    void  setAngularDamping(float c);  AQUA_NODISCARD float angularDamping()  const;
    void  setGravityScale(float s);    AQUA_NODISCARD float gravityScale()    const;
    /// 0 ⇒ unlimited (default). Opt-in safety valve for fast spinners; changes
    /// the physics, so off by default — the adaptive gyroscopic iteration is
    /// the silent default stability path.
    void  setMaxAngularSpeed(float s); AQUA_NODISCARD float maxAngularSpeed() const;

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

    AQUA_NODISCARD AQBodyType type() const;

private:
    AQRigidBody();
    friend class AQSpace;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AQUA_AQRIGIDBODY_H
