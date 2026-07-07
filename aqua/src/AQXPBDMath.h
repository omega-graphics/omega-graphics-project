#ifndef AQUA_SRC_AQXPBDMATH_H
#define AQUA_SRC_AQXPBDMATH_H

// AQUA Phase 7 — the scalar-generic XPBD projection math (brief §6, corrected
// per §13). Everything here is templated on the scalar `Ty` so the production
// float fast-path AND the double reference oracle (the rope test) call the SAME
// code at different precision — a divergence then localizes to precision, never
// to an algorithm mismatch. This is the Phase 6 `AQParticleMath.h` discipline
// pointed at constraint projection.
//
// The three passes of one XPBD slice of size h (Macklin 2016 + Macklin 2019
// small steps — the engine sub-step is subdivided into n slices, each running
// ONE projection sweep):
//
//   1. AQxpbdPredict       — unconstrained symplectic step (gravity only in
//                            Phase 7; fields/coupling are later phases).
//   2. AQxpbdProjectDistance — one constraint's position correction with the
//                            compliance term α̃ = α/h² and the accumulated
//                            Lagrange multiplier λ (reset each slice).
//   3. AQxpbdDeriveVelocity — v = (x − x_prev)/h, then optional damping.
//
// Guards live IN the projection (not around it) so every caller — float path,
// double oracle, and the eventual OmegaSL transcription — inherits them:
//   * zero-length gradient (coincident particles) → skip, never divide;
//   * per-slice position delta clamped to `maxMove` with a loud trip report
//     (the caller counts trips and names the constraint — brief §6 "guards for
//     the 3am engineer"; a silent default-return would hide a diverging solve
//     until the whole rope is at infinity).

#include <aqua/AQMath.h>       // AQVec3<Ty>, AQvec3, OmegaGTE::dot
#include <cmath>

// --- 1. predict: unconstrained integrate --------------------------------------
// Pinned particles (invMass == 0) are skipped EXPLICITLY by the caller — their
// x_prev must still be refreshed to x so a later unpin never derives a bogus
// velocity from stale history; this helper is only for the dynamic lanes.
template<class Ty>
inline void AQxpbdPredict(AQVec3<Ty>& x, AQVec3<Ty>& xPrev, AQVec3<Ty>& v,
                          const AQVec3<Ty>& gravity, Ty h) {
    xPrev = x;
    v = v + gravity * h;
    x = x + v * h;
}

// --- 2. project: one distance constraint --------------------------------------
// XPBD position correction (Macklin 2016, Eq. 17/18) for C(x) = |xa−xb| − L:
//
//   α̃      = α / h²
//   Δλ     = (−C − α̃·λ) / (wa + wb + α̃)
//   λ     += Δλ
//   xa    += wa · Δλ · n        n = (xa − xb)/|xa − xb|
//   xb    −= wb · Δλ · n
//
// α = 0 collapses α̃ to 0 and the update to the classic inextensible PBD
// projection (compliance = 0 ⇒ rigid — the §1 oracle). Because h (the slice,
// not the frame) enters α̃ and λ accumulates within a slice, a given α produces
// a timestep-independent steady-state stretch (the headline §9.4 oracle).
//
// `maxMove` clamps the LARGER endpoint's per-projection move: Δλ is scaled so
// max(wa,wb)·|Δλ| ≤ maxMove, keeping λ and the applied correction consistent.
// Returns true when that clamp engaged (an explosion-guard trip the caller
// must report loudly). Degenerate gradient (|xa−xb| < ε) returns false with no
// motion — a NaN here would silently poison the whole SoA by next slice.
template<class Ty>
inline bool AQxpbdProjectDistance(AQVec3<Ty>& xa, AQVec3<Ty>& xb,
                                  Ty wa, Ty wb,
                                  Ty restLength, Ty compliance,
                                  Ty h, Ty maxMove, Ty& lambda) {
    const Ty wsum = wa + wb;
    if (wsum <= Ty(0)) return false;               // both pinned — nothing to move

    const AQVec3<Ty> d = xa - xb;
    const Ty len2 = OmegaGTE::dot(d, d);
    if (len2 < Ty(1e-24)) return false;            // coincident: zero-length gradient
    const Ty len = std::sqrt(len2);

    const AQVec3<Ty> n = d * (Ty(1) / len);
    const Ty C          = len - restLength;
    const Ty alphaTilde = compliance / (h * h);

    Ty dLambda = (-C - alphaTilde * lambda) / (wsum + alphaTilde);

    // Explosion guard: bound the larger endpoint's move this projection.
    bool tripped = false;
    const Ty wMax = (wa > wb) ? wa : wb;
    const Ty move = std::abs(dLambda) * wMax;
    if (move > maxMove) {
        dLambda *= maxMove / move;
        tripped = true;
    }

    lambda += dLambda;
    xa = xa + n * (wa * dLambda);
    xb = xb - n * (wb * dLambda);
    return tripped;
}

// --- 3. derive: velocity from the net position change -------------------------
// Standard XPBD velocity update; `damping` in [0,1) is the optional scalar
// post-derive damping (§11 #5) — 0 leaves the solve undamped.
template<class Ty>
inline void AQxpbdDeriveVelocity(const AQVec3<Ty>& x, const AQVec3<Ty>& xPrev,
                                 AQVec3<Ty>& v, Ty h, Ty damping) {
    v = (x - xPrev) * (Ty(1) / h);
    if (damping > Ty(0)) v = v * (Ty(1) - damping);
}

#endif // AQUA_SRC_AQXPBDMATH_H
