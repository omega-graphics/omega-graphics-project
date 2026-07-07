#ifndef AQUA_SRC_AQPARTICLEMATH_H
#define AQUA_SRC_AQPARTICLEMATH_H

// AQUA Phase 6 §13.3 6c — the scalar-generic particle math: deterministic
// emission sampling (Reeves attributes), force-field evaluation, and semi-
// implicit integration. Everything here is templated on the scalar `Ty` so the
// production float fast-path AND the double reference oracle (6e) call the SAME
// code at different precision — a divergence then localizes to precision, never
// to an algorithm mismatch (§8).
//
// TWO determinism rules bite here (§9):
//   * The emission COUNT is computed with a `double` carry on ALL paths
//     (AQemitCount is not templated) so the count — and therefore the slot
//     assignment — is bit-identical cross-path. The census must match exactly;
//     only trajectories get a tolerance band.
//   * The per-particle RNG is seeded from an integer (emitter seed mixed with a
//     path-independent emission ORDINAL), so the same logical particle always
//     draws the same stream; the integer stream is identical on every path and
//     only the final [0,1) map rounds differently (within the trajectory band).
//
// Fields contribute ACCELERATION (mass-independent), which is how gravity and
// drag — the fountain's fields — physically behave and keeps the oracle simple;
// `invMass` is retained on the pool for later (two-way / SPH). Mass-scaled
// (force-space) fields are an additive future variant, not a pipeline change.

#include <aqua/AQParticles.h>
#include <aqua/AQMath.h>       // AQVec3<Ty>, AQvec3, OmegaGTE::dot/cross
#include <cstdint>
#include <cmath>
#include <algorithm>

// --- Deterministic RNG (SplitMix64) ----------------------------------------
// Integer stream is identical on any path; nextUnit<Ty>() is the only Ty-typed
// step (float rounds, double is exact — a trajectory-band difference, never a
// census one).
struct AQParticleRng {
    std::uint64_t state;
    explicit AQParticleRng(std::uint64_t seed) : state(seed) {}
    std::uint64_t nextU64() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    template<class Ty> Ty nextUnit() {                     // [0,1)
        return static_cast<Ty>(nextU64() >> 11) * (Ty(1) / Ty(9007199254740992.0)); // /2^53
    }
    template<class Ty> Ty nextRange(Ty lo, Ty hi) { return lo + (hi - lo) * nextUnit<Ty>(); }
    template<class Ty> Ty nextSym(Ty half)         { return nextRange(-half, half); }
};

// Mix an emitter seed with a per-particle ordinal into an RNG seed. The ordinal
// is a path-independent integer (emission count is computed in double on all
// paths), so the same logical particle reproduces its attributes everywhere.
inline std::uint64_t AQmixSeed(std::uint64_t seed, std::uint64_t ordinal) {
    std::uint64_t z = seed ^ (ordinal * 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 33)) * 0xFF51AFD7ED558CCDull;
    z = (z ^ (z >> 33)) * 0xC4CEB9FE1A85EC53ull;
    return z ^ (z >> 33);
}

// Emission count for one emitter over dt, with fractional carry. ALWAYS double —
// both the float fast-path and the double oracle call this — so the count and
// the slot assignment are bit-identical cross-path (§9 census-exact). Advances
// `carry`, kept in [0,1).
inline std::uint32_t AQemitCount(float rate, float dt, double& carry) {
    carry += static_cast<double>(rate) * static_cast<double>(dt);
    if (carry < 0.0) carry = 0.0;
    const double whole = std::floor(carry);
    carry -= whole;
    return static_cast<std::uint32_t>(whole);
}

// Integer death schedule (§14.2 / 6f): the number of age() calls a sampled
// lifetime survives, computed in DOUBLE at emission so every path — the float
// production step, the double oracle, and the GPU age kernel (which only
// decrements the integer) — derives the identical schedule without device
// fp64. ceil keeps the former accumulated-threshold semantics ("dies on the
// first sub-step where accumulated age >= lifetime"); a non-positive lifetime
// dies on the first age() call; an absurd one saturates rather than
// overflowing the uint32 cast.
inline std::uint32_t AQdeathCountdown(double sampledLifetime, float substepDt) {
    const double slices = std::ceil(sampledLifetime / static_cast<double>(substepDt));
    if (!(slices > 1.0))    return 1u;             // <= 0, NaN, or sub-one-step lifetime
    if (slices >= 4.0e9)    return 0xFFFFFFFFu;    // saturate, never overflow
    return static_cast<std::uint32_t>(slices);
}

// --- small vector helpers (Ty-generic) -------------------------------------
template<class Ty> inline Ty AQvlen(const AQVec3<Ty>& v) { return std::sqrt(OmegaGTE::dot(v, v)); }

template<class Ty>
inline AQVec3<Ty> AQnormalizeOr(const AQVec3<Ty>& v, const AQVec3<Ty>& fallback) {
    const Ty l = AQvlen(v);
    return (l > Ty(1e-12)) ? (v * (Ty(1) / l)) : fallback;
}

// Component-cast an emitter/field FVec<3> (always float) into AQVec3<Ty>.
template<class Ty>
inline AQVec3<Ty> AQvecCast(const OmegaGTE::FVec<3>& v) {
    return AQvec3<Ty>(static_cast<Ty>(v[0][0]), static_cast<Ty>(v[1][0]), static_cast<Ty>(v[2][0]));
}

// Orthonormal tangents for a (unit-ish) direction — for cone perturbation.
template<class Ty>
inline void AQbasisFrom(const AQVec3<Ty>& n, AQVec3<Ty>& t, AQVec3<Ty>& b) {
    const AQVec3<Ty> up = (std::fabs(n[1][0]) < Ty(0.99)) ? AQvec3<Ty>(0, 1, 0) : AQvec3<Ty>(1, 0, 0);
    t = AQnormalizeOr(OmegaGTE::cross(up, n), AQvec3<Ty>(1, 0, 0));
    b = OmegaGTE::cross(n, t);
}

// Perturb `dir` to a random direction within a cone of the given half-angle.
// Draws exactly two units (u, v) — a fixed count so paths stay in lockstep.
template<class Ty>
inline AQVec3<Ty> AQperturbInCone(const AQVec3<Ty>& dir, Ty halfAngle, AQParticleRng& rng) {
    const Ty u = rng.template nextUnit<Ty>();
    const Ty v = rng.template nextUnit<Ty>();
    const Ty cosMax = std::cos(halfAngle);
    const Ty cosT = Ty(1) - u * (Ty(1) - cosMax);
    const Ty sinT = std::sqrt(std::max(Ty(0), Ty(1) - cosT * cosT));
    const Ty phi = Ty(2) * Ty(3.14159265358979323846) * v;
    AQVec3<Ty> t = AQvec3<Ty>(1, 0, 0), b = AQvec3<Ty>(0, 0, 1);
    AQbasisFrom(dir, t, b);
    return t * (std::cos(phi) * sinT) + b * (std::sin(phi) * sinT) + dir * cosT;
}

// --- Reeves emission attributes (Ty-generic) -------------------------------
// Spawn position inside the emitter shape, world space. Draw counts are fixed
// per shape kind (identical branch on both paths).
template<class Ty>
inline AQVec3<Ty> AQsampleEmitPosition(const AQEmitter& e, AQParticleRng& rng) {
    AQVec3<Ty> local = AQvec3<Ty>(0, 0, 0);
    switch (e.shapeKind) {
    case AQEmitPoint:
        break;                                               // 0 draws
    case AQEmitSphere: {                                     // 3 draws (uniform in ball)
        const Ty z   = Ty(1) - Ty(2) * rng.template nextUnit<Ty>();
        const Ty phi = Ty(2) * Ty(3.14159265358979323846) * rng.template nextUnit<Ty>();
        const Ty rr  = std::cbrt(rng.template nextUnit<Ty>()) * Ty(e.shape.sphere.radius);
        const Ty s   = std::sqrt(std::max(Ty(0), Ty(1) - z * z));
        local = AQvec3<Ty>(rr * s * std::cos(phi), rr * s * std::sin(phi), rr * z);
        break;
    }
    case AQEmitBox:                                          // 3 draws
        local = AQvec3<Ty>(rng.template nextSym<Ty>(Ty(e.shape.box.hx)),
                           rng.template nextSym<Ty>(Ty(e.shape.box.hy)),
                           rng.template nextSym<Ty>(Ty(e.shape.box.hz)));
        break;
    case AQEmitCone:                                         // 0 draws (apex point; spread is in velocity)
        break;
    case AQEmitDisc: {                                       // 2 draws (uniform in XZ disc)
        const Ty ang = Ty(2) * Ty(3.14159265358979323846) * rng.template nextUnit<Ty>();
        const Ty rr  = std::sqrt(rng.template nextUnit<Ty>()) * Ty(e.shape.disc.radius);
        local = AQvec3<Ty>(rr * std::cos(ang), Ty(0), rr * std::sin(ang));
        break;
    }
    }
    return AQvecCast<Ty>(e.origin) + local;
}

// Spawn velocity: base direction * (speed ± jitter), optionally spread within a
// cone. Draw order: speed jitter (1), then cone (2) when dirJitterRad > 0.
template<class Ty>
inline AQVec3<Ty> AQsampleEmitVelocity(const AQEmitter& e, AQParticleRng& rng) {
    const AQVec3<Ty> base = AQvecCast<Ty>(e.baseVelocity);
    const Ty speed = AQvlen(base);
    AQVec3<Ty> dir = (speed > Ty(1e-12)) ? (base * (Ty(1) / speed)) : AQvec3<Ty>(0, 1, 0);
    const Ty s = speed + rng.template nextSym<Ty>(Ty(e.speedJitter));
    if (e.dirJitterRad > 0.f) dir = AQperturbInCone(dir, Ty(e.dirJitterRad), rng);
    return dir * s;
}

template<class Ty>
inline Ty AQsampleLifetime(const AQEmitter& e, AQParticleRng& rng) {
    const Ty lt = Ty(e.lifetime) + rng.template nextSym<Ty>(Ty(e.lifetimeJitter));
    return (lt > Ty(0)) ? lt : Ty(0);
}

// --- Force-field evaluation (Ty-generic, returns ACCELERATION) --------------
template<class Ty>
inline AQVec3<Ty> AQevalField(const AQForceField& f,
                              const AQVec3<Ty>& pos, const AQVec3<Ty>& vel) {
    const AQVec3<Ty> zero = AQvec3<Ty>(0, 0, 0);
    if (!f.enabled) return zero;

    switch (f.kind) {
    case AQFieldGravity:
        return AQnormalizeOr(AQvecCast<Ty>(f.axis), AQvec3<Ty>(0, -1, 0)) * Ty(f.p.gravity.g);
    case AQFieldDrag:
        return vel * (-Ty(f.p.drag.k));
    case AQFieldWind:
        return AQnormalizeOr(AQvecCast<Ty>(f.axis), AQvec3<Ty>(1, 0, 0)) * Ty(f.p.wind.speed);
    case AQFieldVortex: {
        const AQVec3<Ty> axis = AQnormalizeOr(AQvecCast<Ty>(f.axis), AQvec3<Ty>(0, 1, 0));
        const AQVec3<Ty> r    = pos - AQvecCast<Ty>(f.position);
        const AQVec3<Ty> rPerp = r - axis * OmegaGTE::dot(r, axis);   // radial component ⟂ axis
        const Ty rr = AQvlen(rPerp);
        if (f.radiusOfInfluence > 0.f && rr > Ty(f.radiusOfInfluence)) return zero;
        const AQVec3<Ty> tang = AQnormalizeOr(OmegaGTE::cross(axis, rPerp), zero);
        const Ty fall = Ty(1) / std::pow(std::max(rr, Ty(1e-4)), Ty(f.p.vortex.falloff));
        return tang * (Ty(f.p.vortex.strength) * fall);
    }
    case AQFieldPoint: {
        const AQVec3<Ty> d = AQvecCast<Ty>(f.position) - pos;
        const Ty dist = AQvlen(d);
        if (f.radiusOfInfluence > 0.f && dist > Ty(f.radiusOfInfluence)) return zero;
        const AQVec3<Ty> dir = AQnormalizeOr(d, zero);
        const Ty fall = Ty(1) / std::pow(std::max(dist, Ty(1e-4)), Ty(f.p.point.falloff));
        return dir * (Ty(f.p.point.strength) * fall);   // +strength attracts, − repels
    }
    }
    return zero;
}

// --- Semi-implicit (symplectic) Euler --------------------------------------
// v ← v + a·dt ; x ← x + v·dt  (the new velocity advances position — §6 Pass C).
template<class Ty>
inline void AQintegrateSemiImplicit(AQVec3<Ty>& pos, AQVec3<Ty>& vel,
                                    const AQVec3<Ty>& accel, Ty dt) {
    vel = vel + accel * dt;
    pos = pos + vel * dt;
}

#endif // AQUA_SRC_AQPARTICLEMATH_H
