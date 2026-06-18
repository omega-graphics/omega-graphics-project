#ifndef AQUA_SRC_AQJOINTBUILD_H
#define AQUA_SRC_AQJOINTBUILD_H

// Internal seam between AQSpace.cpp (owns the joint table + the body SoA, drives
// warm-start and persistence) and AQJoint.cpp (the pure per-type Jacobian-row
// math, Phase-4 brief §7). AQSpace gathers each joint's two bodies' kinematics
// into AQJointBodyKin, calls AQbuildJointRows, then fills in bodyA/bodyB and the
// warm-started accumImpulse on the returned rows. Keeping the math here (no
// AQSpace::Impl, no AQRigidBody access) means it stays unit-testable and the GPU
// port (Phase 5) can call it per joint with the same POD inputs.

#include <aqua/AQJoint.h>
#include <aqua/AQContact.h>
#include <omegaGTE/GTEMath.h>

/// World-frame kinematics of one body that a joint row builder needs.
/// (FVec / FMatrix members are Create()-initialized because OmegaGTE::Matrix's
/// default constructor is private — the same factory-only idiom the rest of
/// AQUA uses.)
struct AQJointBodyKin {
    OmegaGTE::FVec<3>      com     = OmegaGTE::FVec<3>::Create();      ///< world COM (= position while comOffset is 0)
    OmegaGTE::FQuaternion  q       = OmegaGTE::FQuaternion::Identity(); ///< orientation
    float                  invMass = 0.f;                              ///< 0 for static / kinematic (one-way)
    OmegaGTE::FMatrix<3,3> invI    = OmegaGTE::FMatrix<3,3>::Create();  ///< world inverse inertia (R·diag(Ib⁻¹)·Rᵀ)
};

/// Per-joint rest-pose state captured at creation; the angular locks and the
/// hinge/slider axis tracking are defined relative to it.
struct AQJointRest {
    OmegaGTE::FQuaternion relOrient  = OmegaGTE::FQuaternion::Identity(); ///< conj(qA0)·qB0
    OmegaGTE::FVec<3>     axisLocalB = OmegaGTE::FVec<3>::Create();       ///< hinge/slider axis in B-local at rest
};

/// Maximum rows any single joint can emit: 3 ball-socket + 3 angular + 1 limit
/// + 1 motor = 8. Callers size their scratch to this.
static constexpr int kAQMaxJointRows = 8;

/// Build the constraint rows for one joint into `out` (capacity >= kAQMaxJointRows).
/// Returns the number written. Fills kind / isAngular / direction / rA / rB /
/// contactPoint / effectiveMass / bias / frictionCoeff / compliance; the caller
/// sets bodyA / bodyB and the warm-started accumImpulse. Pure math — no engine state.
int AQbuildJointRows(const AQJointDesc &desc, const AQJointRest &rest,
                     const AQJointBodyKin &A, const AQJointBodyKin &B,
                     float dt, AQConstraintRow *out);

#endif // AQUA_SRC_AQJOINTBUILD_H
