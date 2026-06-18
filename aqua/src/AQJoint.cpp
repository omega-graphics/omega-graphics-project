// AQUA Phase 4 — per-type joint Jacobian-row construction (Phase-4 brief §6.C,
// §7). Pure math over the borrowed OmegaGTE types and the AQConstraintRow schema
// Phase 3 shipped; no AQSpace / AQRigidBody state. AQSpace.cpp calls
// AQbuildJointRows once per joint per sub-step, then sets bodyA/bodyB and the
// warm-started impulse on the returned rows and appends them to the shared row
// buffer the PGS sweep iterates. The five specialized types (Distance,
// BallSocket, Hinge, Slider, Fixed) build, respectively, 1 / 3 / 5 / 5 / 6 base
// rows, plus optional limit and motor rows on the hinge/slider axis.
//
// Conventions:
//   * Anchors are body-local, relative to the body origin (= COM while the
//     reserved comOffset is 0). World anchor = com + R·anchorLocal.
//   * A LINEAR row constrains the relative velocity of the shared anchor point
//     along `direction`; an ANGULAR row constrains the relative angular velocity
//     along `direction` (a pure torque). The PGS sweep branches on row.isAngular.
//   * Position error feeds a Catto/Baumgarte velocity bias (β·C/dt) rather than a
//     split-impulse pass — joints skip split-impulse (Phase-4 brief §6.I).
//   * Soft constraints (Catto 2011): a non-zero AQJointSoftness translates to a
//     CFM compliance + an ERP bias; the default (0,0) is a hard constraint whose
//     row reduces to the Phase 3 effective-mass formula.

#include "AQJointBuild.h"
#include <aqua/AQMath.h>
#include <cmath>

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;
using OmegaGTE::FMatrix;

namespace {

// Linear effective-mass denominator (Catto 2005):
//   1/mA + 1/mB + (rA×d)·invIA·(rA×d) + (rB×d)·invIB·(rB×d).
float kLinear(const AQJointBodyKin &A, const AQJointBodyKin &B,
              const FVec<3> &rA, const FVec<3> &rB, const FVec<3> &d) {
    const FVec<3> rAxd = OmegaGTE::cross(rA, d);
    const FVec<3> rBxd = OmegaGTE::cross(rB, d);
    return A.invMass + B.invMass
         + OmegaGTE::dot(rAxd, A.invI * rAxd)
         + OmegaGTE::dot(rBxd, B.invI * rBxd);
}

// Angular effective-mass denominator: d·invIA·d + d·invIB·d.
float kAngular(const AQJointBodyKin &A, const AQJointBodyKin &B, const FVec<3> &d) {
    return OmegaGTE::dot(d, A.invI * d) + OmegaGTE::dot(d, B.invI * d);
}

// Hard/soft translation. For a HARD constraint (frequency 0) the effective mass
// is 1/kRaw and the bias is the Baumgarte term βC/dt (β = 0.2, the conservative
// Catto value). For a SOFT constraint (Catto 2011) a spring (ω_n) + damper (ζ)
// give a CFM γ and an ERP β; the row stores the compliance (γ·dt²) so the PGS
// CFM term recovers γ, and the effective mass folds γ in.
void softParams(float kRaw, float C, float dt, const AQJointSoftness &s,
                float &effMass, float &bias, float &compliance) {
    if (s.frequency > 0.f && kRaw > 1e-12f && dt > 0.f) {
        const float m     = 1.f / kRaw;                  // effective mass
        const float k     = m * s.frequency * s.frequency;  // spring
        const float c     = 2.f * m * s.damping * s.frequency; // damper
        const float denom = c + dt * k;
        const float gamma = (denom > 1e-12f) ? (1.f / (dt * denom)) : 0.f;
        const float beta  = (denom > 1e-12f) ? (dt * k / denom)     : 0.f;
        effMass    = 1.f / (kRaw + gamma);
        bias       = (beta / dt) * C;
        compliance = gamma * dt * dt;
    } else {
        constexpr float betaHard = 0.2f;
        effMass    = (kRaw > 1e-12f) ? (1.f / kRaw) : 0.f;
        bias       = (dt > 0.f) ? (betaHard / dt) * C : 0.f;
        compliance = 0.f;
    }
}

void emitLinear(AQConstraintRow &r, AQConstraintKind kind, const FVec<3> &cp,
                const FVec<3> &rA, const FVec<3> &rB, const FVec<3> &dir,
                float effMass, float bias, float compliance, float motorMax = 0.f) {
    r = AQConstraintRow{};
    r.kind          = kind;
    r.isAngular     = false;
    r.contactPoint  = cp;
    r.rA            = rA;
    r.rB            = rB;
    r.direction     = dir;
    r.effectiveMass = effMass;
    r.bias          = bias;
    r.compliance    = compliance;
    r.frictionCoeff = motorMax;     // JointMotor: |λ| ≤ motorMax
    r.accumImpulse  = 0.f;
}

void emitAngular(AQConstraintRow &r, AQConstraintKind kind, const FVec<3> &dir,
                 float effMass, float bias, float compliance, float motorMax = 0.f) {
    r = AQConstraintRow{};
    r.kind          = kind;
    r.isAngular     = true;
    r.direction     = dir;
    r.effectiveMass = effMass;
    r.bias          = bias;
    r.compliance    = compliance;
    r.frictionCoeff = motorMax;
    r.accumImpulse  = 0.f;
}

// Two unit vectors spanning the plane perpendicular to `n` (n need not be unit).
void perpBasis(const FVec<3> &n, FVec<3> &t1, FVec<3> &t2) {
    FVec<3> nn = n;
    const float l = std::sqrt(OmegaGTE::dot(nn, nn));
    nn = (l > 1e-9f) ? nn * (1.f / l) : AQvec3(1.f, 0.f, 0.f);
    const FVec<3> alt = (std::abs(nn[0][0]) < 0.6f) ? AQvec3(1.f, 0.f, 0.f)
                      : (std::abs(nn[1][0]) < 0.6f) ? AQvec3(0.f, 1.f, 0.f)
                                                    : AQvec3(0.f, 0.f, 1.f);
    t1 = OmegaGTE::cross(nn, alt);
    const float l1 = std::sqrt(OmegaGTE::dot(t1, t1));
    t1 = (l1 > 1e-9f) ? t1 * (1.f / l1) : AQvec3(1.f, 0.f, 0.f);
    t2 = OmegaGTE::cross(nn, t1);
    const float l2 = std::sqrt(OmegaGTE::dot(t2, t2));
    t2 = (l2 > 1e-9f) ? t2 * (1.f / l2) : AQvec3(0.f, 1.f, 0.f);
}

// World-frame rotation-vector error for a FULL relative-orientation lock to the
// rest relative orientation: ≈ the small rotation taking the current relative
// orientation back to rest. Used by the 3 angular rows of Slider/Fixed.
FVec<3> angularErrorVec(const AQJointBodyKin &A, const AQJointBodyKin &B,
                        const AQJointRest &rest) {
    const FQuaternion qRel = (A.q.conjugate() * B.q).normalized();
    FQuaternion qErr = (qRel * rest.relOrient.conjugate()).normalized();   // in A's frame
    if (qErr.w < 0.f) { qErr.x = -qErr.x; qErr.y = -qErr.y; qErr.z = -qErr.z; qErr.w = -qErr.w; }
    const FVec<3> eA = AQvec3(2.f * qErr.x, 2.f * qErr.y, 2.f * qErr.z);    // ≈ rotation vector (A frame)
    return AQrotate(A.q, eA);                                               // to world
}

// Signed hinge angle about the body-A axis, relative to the rest pose.
float hingeAngle(const AQJointBodyKin &A, const AQJointBodyKin &B,
                 const AQJointRest &rest, const FVec<3> &axisLocalAUnit) {
    const FQuaternion qRel = (A.q.conjugate() * B.q).normalized();
    FQuaternion dq = (qRel * rest.relOrient.conjugate()).normalized();
    if (dq.w < 0.f) { dq.x = -dq.x; dq.y = -dq.y; dq.z = -dq.z; dq.w = -dq.w; }
    const float sinHalfProj = dq.x * axisLocalAUnit[0][0]
                            + dq.y * axisLocalAUnit[1][0]
                            + dq.z * axisLocalAUnit[2][0];
    return 2.f * std::atan2(sinHalfProj, dq.w);
}

// Limit + motor rows about a single axis (hinge: angular; slider: linear). For
// the linear (slider) case `linear == true` and (cp, rA, rB) are supplied; the
// rows then act at the anchor along `axisWorld`. Returns rows written.
int emitAxisLimitMotor(AQConstraintRow *out, const AQJointDesc &d,
                       const AQJointBodyKin &A, const AQJointBodyKin &B,
                       const FVec<3> &axisWorld, float coord, float dt,
                       bool linear, const FVec<3> &cp, const FVec<3> &rA, const FVec<3> &rB) {
    int n = 0;
    const AQJointAxisLimit &lim = d.limit;
    const float kRaw = linear ? kLinear(A, B, rA, rB, axisWorld) : kAngular(A, B, axisWorld);
    const float em   = (kRaw > 1e-12f) ? (1.f / kRaw) : 0.f;

    // Motor: drive the relative axis velocity toward motorTargetVelocity, the
    // corrective impulse clamped to ±motorMaxImpulse (the §7 F_max·dt the caller
    // passed). bias = -target so lambda = -(relV − target)·effMass.
    if (lim.motorEnabled && em > 0.f) {
        if (linear) emitLinear (out[n], AQConstraintKind::JointMotor, cp, rA, rB, axisWorld, em, -lim.motorTargetVelocity, 0.f, lim.motorMaxImpulse);
        else        emitAngular(out[n], AQConstraintKind::JointMotor,             axisWorld, em, -lim.motorTargetVelocity, 0.f, lim.motorMaxImpulse);
        ++n;
    }

    // Limit: a one-sided (λ ≥ 0) row at the violated end, mirroring a contact
    // normal. `coord` is the current angle (hinge) or signed offset (slider).
    // Past the upper limit the row pushes the coordinate down (direction = -axis,
    // bias = -β·overshoot/dt, a separating target velocity); past the lower limit
    // it pushes up (direction = +axis). Inside [min, max] no row is emitted —
    // the axis is free.
    if (lim.enabled && em > 0.f && dt > 0.f) {
        constexpr float betaLim = 0.2f;
        if (coord > lim.max) {
            const float bias = -(betaLim * (coord - lim.max)) / dt;
            if (linear) emitLinear (out[n], AQConstraintKind::JointLimit, cp, rA, rB, axisWorld * -1.f, em, bias, 0.f);
            else        emitAngular(out[n], AQConstraintKind::JointLimit,             axisWorld * -1.f, em, bias, 0.f);
            ++n;
        } else if (coord < lim.min) {
            const float bias = -(betaLim * (lim.min - coord)) / dt;
            if (linear) emitLinear (out[n], AQConstraintKind::JointLimit, cp, rA, rB, axisWorld, em, bias, 0.f);
            else        emitAngular(out[n], AQConstraintKind::JointLimit,             axisWorld, em, bias, 0.f);
            ++n;
        }
    }
    return n;
}

} // namespace

int AQbuildJointRows(const AQJointDesc &d, const AQJointRest &rest,
                     const AQJointBodyKin &A, const AQJointBodyKin &B,
                     float dt, AQConstraintRow *out) {
    int n = 0;

    // Shared anchor geometry (the constraint point is the midpoint of the two
    // world anchors; the residual separation feeds the position bias).
    const FVec<3> aW   = A.com + AQrotate(A.q, d.anchorA);
    const FVec<3> bW   = B.com + AQrotate(B.q, d.anchorB);
    const FVec<3> cp   = (aW + bW) * 0.5f;
    const FVec<3> rA   = cp - A.com;
    const FVec<3> rB   = cp - B.com;
    const FVec<3> Cvec = bW - aW;                    // position error (B relative to A)

    const FVec<3> EX = AQvec3(1.f, 0.f, 0.f);
    const FVec<3> EY = AQvec3(0.f, 1.f, 0.f);
    const FVec<3> EZ = AQvec3(0.f, 0.f, 1.f);

    // 3 bilateral linear rows on the world axes — the point (ball-socket) lock.
    auto emitPoint3 = [&]() {
        const FVec<3> axes[3] = {EX, EY, EZ};
        for (const FVec<3> &ax : axes) {
            float em, bias, comp;
            softParams(kLinear(A, B, rA, rB, ax), OmegaGTE::dot(Cvec, ax), dt, d.softness, em, bias, comp);
            emitLinear(out[n], AQConstraintKind::JointAxis, cp, rA, rB, ax, em, bias, comp);
            ++n;
        }
    };

    // 3 bilateral angular rows on the world axes — the full orientation lock.
    auto emitAngular3 = [&]() {
        const FVec<3> theta = angularErrorVec(A, B, rest);
        const FVec<3> axes[3] = {EX, EY, EZ};
        for (const FVec<3> &ax : axes) {
            float em, bias, comp;
            softParams(kAngular(A, B, ax), OmegaGTE::dot(theta, ax), dt, d.softness, em, bias, comp);
            emitAngular(out[n], AQConstraintKind::JointAxis, ax, em, bias, comp);
            ++n;
        }
    };

    switch (d.type) {
    case AQJointType::Distance: {
        float len = std::sqrt(OmegaGTE::dot(Cvec, Cvec));
        FVec<3> dir = (len > 1e-9f) ? Cvec * (1.f / len) : EX;
        float em, bias, comp;
        softParams(kLinear(A, B, rA, rB, dir), len - d.distanceTarget, dt, d.softness, em, bias, comp);
        emitLinear(out[n], AQConstraintKind::JointAxis, cp, rA, rB, dir, em, bias, comp);
        ++n;
        break;
    }
    case AQJointType::BallSocket:
        emitPoint3();
        break;
    case AQJointType::Hinge: {
        emitPoint3();
        // 2 angular rows perpendicular to the hinge axis keep the axis aligned.
        const FVec<3> axisA = AQrotate(A.q, d.axisLocalA);
        const FVec<3> axisB = AQrotate(B.q, rest.axisLocalB);
        const FVec<3> mis   = OmegaGTE::cross(axisA, axisB);   // alignment error
        FVec<3> t1 = AQvec3(0.f, 0.f, 0.f), t2 = AQvec3(0.f, 0.f, 0.f);
        perpBasis(axisA, t1, t2);
        const FVec<3> ts[2] = {t1, t2};
        for (const FVec<3> &t : ts) {
            float em, bias, comp;
            softParams(kAngular(A, B, t), OmegaGTE::dot(mis, t), dt, d.softness, em, bias, comp);
            emitAngular(out[n], AQConstraintKind::JointAxis, t, em, bias, comp);
            ++n;
        }
        // Limit + motor about the hinge axis (angular).
        FVec<3> axisAUnit = d.axisLocalA;
        const float al = std::sqrt(OmegaGTE::dot(axisAUnit, axisAUnit));
        axisAUnit = (al > 1e-9f) ? axisAUnit * (1.f / al) : AQvec3(0.f, 1.f, 0.f);
        const FVec<3> axisAUnitW = AQrotate(A.q, axisAUnit);
        const float theta = hingeAngle(A, B, rest, axisAUnit);
        n += emitAxisLimitMotor(out + n, d, A, B, axisAUnitW, theta, dt, false, cp, rA, rB);
        break;
    }
    case AQJointType::Slider: {
        // Motion only along the slide axis: 2 perpendicular linear rows + a full
        // angular lock (a prismatic joint permits no rotation).
        const FVec<3> axisA = AQrotate(A.q, d.axisLocalA);
        FVec<3> t1 = AQvec3(0.f, 0.f, 0.f), t2 = AQvec3(0.f, 0.f, 0.f);
        perpBasis(axisA, t1, t2);
        const FVec<3> ts[2] = {t1, t2};
        for (const FVec<3> &t : ts) {
            float em, bias, comp;
            softParams(kLinear(A, B, rA, rB, t), OmegaGTE::dot(Cvec, t), dt, d.softness, em, bias, comp);
            emitLinear(out[n], AQConstraintKind::JointAxis, cp, rA, rB, t, em, bias, comp);
            ++n;
        }
        emitAngular3();
        // Limit + motor along the slide axis (linear). The slide coordinate is
        // the anchor offset projected on the (unit) axis.
        FVec<3> axisAUnit = axisA;
        const float al = std::sqrt(OmegaGTE::dot(axisAUnit, axisAUnit));
        axisAUnit = (al > 1e-9f) ? axisAUnit * (1.f / al) : AQvec3(0.f, 1.f, 0.f);
        const float coord = OmegaGTE::dot(Cvec, axisAUnit);
        n += emitAxisLimitMotor(out + n, d, A, B, axisAUnit, coord, dt, true, cp, rA, rB);
        break;
    }
    case AQJointType::Fixed:
        emitPoint3();
        emitAngular3();
        break;
    }
    return n;
}
