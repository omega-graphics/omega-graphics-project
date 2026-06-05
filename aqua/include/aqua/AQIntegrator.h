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
//
// Phase 1.1 extensions (see Phase-1.1-Rigid-Body-Completion.md §6.4): per-body
// gravity scale, adaptive gyroscopic Newton iteration gated on ‖ω‖·dt, linear
// and angular exponential damping, an opt-in max-angular-speed clamp, and a
// debug-build NaN/inf guard. Reserved COM offset for Phase 2 (geometry adds
// the case where COM ≠ pose origin).

#include "AQMath.h"
#ifndef NDEBUG
#include <cassert>
#endif

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

    // --- Phase 1.1 additions (defaults reproduce Phase 1 behaviour) ---
    Ty                       linearDamping   = Ty(0);                  // v ← v / (1 + c·dt)
    Ty                       angularDamping  = Ty(0);                  // ω ← ω / (1 + c·dt)
    Ty                       gravityScale    = Ty(1);                  // per-body gravity multiplier
    Ty                       maxAngularSpeed = Ty(0);                  // 0 ⇒ unlimited
    AQVec3<Ty>               comOffset       = AQVec3<Ty>::Create();   // reserved (Phase 2)
};

// Adaptive-iteration policy (§4, §11.1). When the per-step rotation ‖ω‖·dt is
// large, a single Newton iteration of the implicit-gyroscopic solve leaves a
// noticeable residual (Phase 1's O(dt) secular finding); a small fixed cap
// gated on ‖ω‖·dt cuts it sharply without going to a variable-length loop
// (deterministic, GPU-uniform). Threshold below kAQAdaptiveAngle keeps the
// Phase 1 single-iteration path verbatim; above it iterates up to kAQAdaptiveCap.
template<class Ty> inline constexpr Ty kAQAdaptiveAngle() { return Ty(1e-2); }
inline constexpr int kAQAdaptiveCap = 4;

// Advance one body by one fixed sub-step `dt` under world-frame `gravity`.
// Body-frame symplectic Lie integrator with implicit-gyroscopic angular update
// (Phase-1 doc §6). One body, no interaction — embarrassingly parallel; this is
// exactly the body of a future one-thread-per-body GPU kernel.
template<class Ty>
inline void AQStepBody(AQBodyState<Ty>& b, const AQVec3<Ty>& gravity, Ty dt) {
    if (b.invMass == Ty(0)) return;                       // static / immovable

    // 1. Linear: symplectic Euler, velocity first. Per-body gravityScale scales
    //    the world acceleration; accumulated forces are scaled by inverse mass.
    b.velocity += dt * (b.gravityScale * gravity + b.invMass * b.forceAccum);

    // 1a. Linear damping (exponential, unconditionally stable; §11.2).
    if (b.linearDamping > Ty(0)) {
        b.velocity = b.velocity * (Ty(1) / (Ty(1) + b.linearDamping * dt));
    }

    // 2. Angular: world torque -> body frame, then explicit kick by Ib⁻¹ (diag,
    //    so componentwise).
    const AQVec3<Ty> tauBody = AQrotate(b.orientation.conjugate(), b.torqueAccum);
    AQVec3<Ty>& w = b.angularVelBody;
    w[0][0] += dt * b.invInertiaBody[0][0] * tauBody[0][0];
    w[1][0] += dt * b.invInertiaBody[1][0] * tauBody[1][0];
    w[2][0] += dt * b.invInertiaBody[2][0] * tauBody[2][0];

    // 3. Implicit gyroscopic Newton iteration in the body frame. Stable where
    //    the explicit term ω×(Ib·ω) diverges. Needs the principal moments
    //    (Ib = 1 / Ib⁻¹); skip cleanly if any axis has infinite inertia.
    //
    //    Adaptive cap (§4, §11.1): below kAQAdaptiveAngle the single Phase 1
    //    iteration is exactly preserved; above it, iterate up to kAQAdaptiveCap
    //    times. The count is a pure function of ‖ω‖·dt (no per-thread variable
    //    loop length), keeping the (future) GPU path deterministic.
    //
    //    Multi-iteration Newton solves Ib·(ω' − ω₀) + dt·cross(ω', Ib·ω') = 0
    //    starting from ω₀ = w (post-kick). Iteration k linearizes around the
    //    current iterate but the residual includes the deviation from ω₀ — at
    //    k=0 that deviation is zero, so the residual reduces to dt·cross(w₀,
    //    Ib·w₀), recovering the Phase 1 single-iteration step exactly. Without
    //    the ω₀ term, subsequent iterations would drive `cross(ω', Ib·ω')` to
    //    zero, which is a different equation (it makes ω' an eigenvector of Ib)
    //    and produces large drift on asymmetric bodies — the Phase 1 form is
    //    what conserves L when fully converged.
    if (b.invInertiaBody[0][0] > Ty(0) &&
        b.invInertiaBody[1][0] > Ty(0) &&
        b.invInertiaBody[2][0] > Ty(0)) {
        const Ty ibx = Ty(1) / b.invInertiaBody[0][0];
        const Ty iby = Ty(1) / b.invInertiaBody[1][0];
        const Ty ibz = Ty(1) / b.invInertiaBody[2][0];

        auto Ib = AQMat3<Ty>::Create();
        Ib[0][0] = ibx; Ib[1][1] = iby; Ib[2][2] = ibz;

        const Ty omegaSq = w[0][0]*w[0][0] + w[1][0]*w[1][0] + w[2][0]*w[2][0];
        const Ty stepAngleSq = omegaSq * dt * dt;
        const Ty thr = kAQAdaptiveAngle<Ty>();
        const int iters = (stepAngleSq > thr * thr) ? kAQAdaptiveCap : 1;

        const AQVec3<Ty> w0 = w;        // post-kick starting point (Newton fixed)
        for (int k = 0; k < iters; ++k) {
            const AQVec3<Ty> Iw    = AQvec3<Ty>(ibx * w[0][0], iby * w[1][0], ibz * w[2][0]);
            const AQVec3<Ty> dw    = w - w0;
            const AQVec3<Ty> Idw   = AQvec3<Ty>(ibx * dw[0][0], iby * dw[1][0], ibz * dw[2][0]);
            const AQVec3<Ty> f     = Idw + dt * OmegaGTE::cross(w, Iw);    // full residual
            const AQMat3<Ty> J     = Ib + dt * (AQskew(w) * Ib - AQskew(Iw));
            w -= OmegaGTE::inverse(J) * f;                                 // GTE analytic 3x3 inverse
        }
    }

    // 3a. Angular damping (after Newton, before orientation update — the damped
    //     ω is what advances the frame).
    if (b.angularDamping > Ty(0)) {
        const Ty s = Ty(1) / (Ty(1) + b.angularDamping * dt);
        w[0][0] *= s; w[1][0] *= s; w[2][0] *= s;
    }

    // 3b. Opt-in max-angular-speed clamp (off when maxAngularSpeed == 0).
    //     Off by default because it changes the physics; the adaptive iteration
    //     is the default stability path, this is an explicit safety valve.
    if (b.maxAngularSpeed > Ty(0)) {
        const Ty wn2 = w[0][0]*w[0][0] + w[1][0]*w[1][0] + w[2][0]*w[2][0];
        const Ty cap2 = b.maxAngularSpeed * b.maxAngularSpeed;
        if (wn2 > cap2) {
            const Ty scale = b.maxAngularSpeed / std::sqrt(wn2);
            w[0][0] *= scale; w[1][0] *= scale; w[2][0] *= scale;
        }
    }

    // 4. Orientation: exponential-map update from body-frame ω (on-manifold).
    b.orientation = AQintegrate(b.orientation, b.angularVelBody, dt);

    // 5. Position: symplectic update with the new velocity.
    b.position += dt * b.velocity;

    // 6. Consume the per-sub-step accumulators.
    b.forceAccum  = AQVec3<Ty>::Create();
    b.torqueAccum = AQVec3<Ty>::Create();

    // 7. NaN/inf guard (debug builds only — compiled out under NDEBUG). A body
    //    that goes non-finite spreads silent NaN through any downstream system;
    //    catching it here is the loud-fail principle (Physics-Roadmap.md §6).
#ifndef NDEBUG
    assert(std::isfinite(b.position[0][0])    && std::isfinite(b.position[1][0])    && std::isfinite(b.position[2][0]));
    assert(std::isfinite(b.velocity[0][0])    && std::isfinite(b.velocity[1][0])    && std::isfinite(b.velocity[2][0]));
    assert(std::isfinite(w[0][0])             && std::isfinite(w[1][0])             && std::isfinite(w[2][0]));
    assert(std::isfinite(b.orientation.x) && std::isfinite(b.orientation.y) &&
           std::isfinite(b.orientation.z) && std::isfinite(b.orientation.w));
#endif
}

#endif // AQUA_AQINTEGRATOR_H
