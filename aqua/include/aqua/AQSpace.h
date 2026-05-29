#ifndef AQUA_AQSPACE_H
#define AQUA_AQSPACE_H

#include "AQBase.h"
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

/// The simulation space: holds bodies that AQContext advances each sub-step.
///
/// Spaces are created by `AQContext::createSpace` and owned by that context,
/// which also drives their stepping — see the timekeeping note on `AQContext`.
///
/// The simulation backend is intentionally hidden behind this pimpl, the same
/// way Omega kREATE hides OmegaGTE behind its own surface. Whether AQUA grows
/// its own solver or wraps a vendored physics SDK is an implementation decision
/// that must not change this public API.
class AQUA_EXPORT AQSpace {
public:
    ~AQSpace();

    void setGravity(const OmegaGTE::FVec<3> &g);
    AQUA_NODISCARD OmegaGTE::FVec<3> gravity() const;

    /// Adds a body and returns a handle owned by this space.
    std::shared_ptr<AQRigidBody> addBody(const AQBodyDesc &desc);

    /// Removes a body. Returns false if it was not in this space.
    bool removeBody(const std::shared_ptr<AQRigidBody> &body);

    /// Number of bodies currently in the space.
    AQUA_NODISCARD std::size_t bodyCount() const;

private:
    AQSpace();

    /// Advances this space by one fixed sub-step of `dt` seconds. Driven by
    /// AQContext::advance; deliberately not public so all timekeeping lives in
    /// the context rather than being duplicated per call site.
    void stepInternal(float dt);

    /// Re-arms the per-body debug fast-spin warning. Called by AQContext when
    /// the fixed sub-step changes; a no-op effect in release builds.
    void resetStepWarnings();

    friend class AQContext;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AQUA_AQSPACE_H
