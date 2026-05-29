#ifndef AQUA_AQINTEGRATOR_H
#define AQUA_AQINTEGRATOR_H

// The Phase 1 integrator, written ONCE over the scalar `Ty` (Phase-1 doc §5/§6).
// AQSpace instantiates it at `float` for the production path; the parity test
// instantiates it at `double` as a high-precision reference oracle — same code,
// two precisions. This is the "opening" the float-committed incumbents (PhysX,
// Chaos) cannot hand us; it falls out of GTE's math being a single template.
//
// Header-only and AQ-prefixed (no namespace, per AGENTS.md). It depends only on
// AQMath.h, so it carries no link-time dependency and can be exercised by a
// pure-CPU unit test without building any GPU backend.

#include "AQMath.h"

// Per-body dynamics state (Phase-1 doc §6 / §10 hidden state). SoA-friendly at
// the buffer level later; AoS here for the CPU reference path. Angular velocity
// is stored in the BODY frame so the world inertia never enters the hot path.
template<class Ty>
struct AQBodyState {
    AQVec3<Ty>               position       = AQVec3<Ty>::Create();
    AQVec3<Ty>               velocity       = AQVec3<Ty>::Create();
    OmegaGTE::Quaternion<Ty> orientation    = OmegaGTE::Quaternion<Ty>::Identity();
    AQVec3<Ty>               angularVelBody = AQVec3<Ty>::Create();    // body frame

    Ty                       invMass        = Ty(1);                   // 0 ⇒ static
    AQVec3<Ty>               invInertiaBody = AQVec3<Ty>::Create();    // 1 / principal moments

    AQVec3<Ty>               forceAccum     = AQVec3<Ty>::Create();    // world frame
    AQVec3<Ty>               torqueAccum    = AQVec3<Ty>::Create();    // world frame
};

// Advance one body by one fixed sub-step `dt` under world-frame `gravity`.
// Body-frame symplectic Lie integrator with implicit-gyroscopic angular update
// (Phase-1 doc §6). One body, no interaction — embarrassingly parallel; this is
// exactly the body of a future one-thread-per-body GPU kernel.
template<class Ty>
inline void AQStepBody(AQBodyState<Ty>& b, const AQVec3<Ty>& gravity, Ty dt) {
    if (b.invMass == Ty(0)) return;                       // static / immovable

    // 1. Linear: symplectic Euler, velocity first. Gravity is an acceleration;
    //    accumulated forces are scaled by inverse mass.
    b.velocity += dt * (gravity + b.invMass * b.forceAccum);

    // 2. Angular: world torque -> body frame, then explicit kick by Ib⁻¹ (diag,
    //    so componentwise).
    const AQVec3<Ty> tauBody = AQrotate(b.orientation.conjugate(), b.torqueAccum);
    AQVec3<Ty>& w = b.angularVelBody;
    w[0][0] += dt * b.invInertiaBody[0][0] * tauBody[0][0];
    w[1][0] += dt * b.invInertiaBody[1][0] * tauBody[1][0];
    w[2][0] += dt * b.invInertiaBody[2][0] * tauBody[2][0];

    // 3. Implicit gyroscopic (one Newton iteration in the body frame). Stable
    //    where the explicit term ω×(Ib·ω) diverges. Needs the principal moments
    //    (Ib = 1 / Ib⁻¹); skip cleanly if any axis has infinite inertia.
    if (b.invInertiaBody[0][0] > Ty(0) &&
        b.invInertiaBody[1][0] > Ty(0) &&
        b.invInertiaBody[2][0] > Ty(0)) {
        const Ty ibx = Ty(1) / b.invInertiaBody[0][0];
        const Ty iby = Ty(1) / b.invInertiaBody[1][0];
        const Ty ibz = Ty(1) / b.invInertiaBody[2][0];

        auto Ib = AQMat3<Ty>::Create();
        Ib[0][0] = ibx; Ib[1][1] = iby; Ib[2][2] = ibz;

        const AQVec3<Ty> Iw = AQvec3<Ty>(ibx * w[0][0], iby * w[1][0], ibz * w[2][0]);
        const AQVec3<Ty> f  = dt * OmegaGTE::cross(w, Iw);          // gyroscopic residual
        const AQMat3<Ty> J  = Ib + dt * (AQskew(w) * Ib - AQskew(Iw));  // Jacobian
        w -= OmegaGTE::inverse(J) * f;                              // GTE analytic 3x3 inverse
    }

    // 4. Orientation: exponential-map update from body-frame ω (on-manifold).
    b.orientation = AQintegrate(b.orientation, b.angularVelBody, dt);

    // 5. Position: symplectic update with the new velocity.
    b.position += dt * b.velocity;

    // 6. Consume the per-sub-step accumulators.
    b.forceAccum  = AQVec3<Ty>::Create();
    b.torqueAccum = AQVec3<Ty>::Create();
}

#endif // AQUA_AQINTEGRATOR_H
