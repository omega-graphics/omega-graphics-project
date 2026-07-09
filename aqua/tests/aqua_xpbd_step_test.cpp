// AQUA Phase 7 §13 7c — the XPBD projection math + the slice loop.
//
// INTERNAL test: drives the scalar-generic math (src/AQXPBDMath.h) against
// hand-computed closed forms, then AQXPBDBody::advance for the loop semantics
// (predict/derive bookkeeping, pinned lanes, λ reset, guard trips, bitwise
// determinism). Everything here is the float fast-path except the closed-form
// checks, which are exact enough to hand-verify; the double oracle and the
// physical deliverable oracles (energy/catenary/stretch) are the rope test's
// job (7e).

#include "AQSpaceImpl.h"
#include "AQXPBDMath.h"

#include <cmath>
#include <cstdio>
#include <string>

// Phase 7g gave AQXPBDBody::advance a collider parameter for the two-way contact
// coupling; these unit tests drive the solver with no colliders.
static const OmegaCommon::Vector<AQXPBDCollider> kNoColliders;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

static bool feq(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

static void testProjectRigidClosedForm() {
    // Two unit-mass particles 2 apart, rest 1, α = 0:
    //   C = 1, n = +x, Δλ = -C/(wa+wb) = -0.5, both endpoints move 0.5 together.
    auto xa = AQvec3(2.f, 0.f, 0.f);
    auto xb = AQvec3(0.f, 0.f, 0.f);
    float lambda = 0.f;
    const bool tripped = AQxpbdProjectDistance<float>(xa, xb, 1.f, 1.f, 1.f, 0.f,
                                                      0.01f, 10.f, lambda);
    check(!tripped, "rigid pair: no guard trip on a sane correction");
    check(feq(lambda, -0.5f), "rigid pair: lambda = -C/(wa+wb) = -0.5");
    check(feq(xa[0][0], 1.5f) && feq(xb[0][0], 0.5f),
          "rigid pair: both endpoints move half the violation (symmetric masses)");
    const float len = xa[0][0] - xb[0][0];
    check(feq(len, 1.f), "rigid pair: alpha = 0 satisfies C exactly in one projection");
}

static void testProjectComplianceClosedForm() {
    // Same rig, α = 0.02, h = 0.1 ⇒ α̃ = α/h² = 2:
    //   Δλ = (-C - α̃λ)/(wa+wb+α̃) = -1/4, endpoints each move 0.25.
    auto xa = AQvec3(2.f, 0.f, 0.f);
    auto xb = AQvec3(0.f, 0.f, 0.f);
    float lambda = 0.f;
    AQxpbdProjectDistance<float>(xa, xb, 1.f, 1.f, 1.f, 0.02f, 0.1f, 10.f, lambda);
    check(feq(lambda, -0.25f), "compliant pair: dLambda = (-C - a~*lambda)/(wsum + a~) = -0.25");
    check(feq(xa[0][0], 1.75f) && feq(xb[0][0], 0.25f),
          "compliant pair: correction scaled by the compliance term");

    // Second projection in the same slice ACCUMULATES λ: C = 0.5,
    // Δλ = (-0.5 - 2·(-0.25))/3 = 0 — the accumulated multiplier has already
    // paid for exactly the compliant steady state at this h.
    AQxpbdProjectDistance<float>(xa, xb, 1.f, 1.f, 1.f, 0.02f, 0.1f, 10.f, lambda);
    check(feq(lambda, -0.25f) && feq(xa[0][0], 1.75f),
          "compliant pair: within-slice iteration converges (lambda accumulation)");
}

static void testPinnedAndDegenerate() {
    // Pinned a (wa = 0): only b moves, by the full violation.
    auto xa = AQvec3(2.f, 0.f, 0.f);
    auto xb = AQvec3(0.f, 0.f, 0.f);
    float lambda = 0.f;
    AQxpbdProjectDistance<float>(xa, xb, 0.f, 1.f, 1.f, 0.f, 0.01f, 10.f, lambda);
    check(feq(xa[0][0], 2.f) && feq(xb[0][0], 1.f),
          "pinned endpoint: immobile; the free endpoint absorbs the whole correction");

    // Both pinned: nothing happens, no divide.
    lambda = 0.f;
    auto pa = AQvec3(2.f, 0.f, 0.f);
    auto pb = AQvec3(0.f, 0.f, 0.f);
    const bool t2 = AQxpbdProjectDistance<float>(pa, pb, 0.f, 0.f, 1.f, 0.f, 0.01f, 10.f, lambda);
    check(!t2 && feq(pa[0][0], 2.f) && feq(pb[0][0], 0.f) && lambda == 0.f,
          "both pinned: projection is a no-op");

    // Coincident particles: zero-length gradient — skip, never NaN.
    auto ca = AQvec3(1.f, 1.f, 1.f);
    auto cb = AQvec3(1.f, 1.f, 1.f);
    lambda = 0.f;
    const bool t3 = AQxpbdProjectDistance<float>(ca, cb, 1.f, 1.f, 1.f, 0.f, 0.01f, 10.f, lambda);
    check(!t3 && std::isfinite(ca[0][0]) && feq(ca[0][0], 1.f),
          "coincident particles: NaN-guarded skip (no divide by zero-length gradient)");
}

static void testGuardClamp() {
    // Violation 9 with maxMove 0.1: unclamped Δλ = -4.5 would move each
    // endpoint 4.5; the clamp scales Δλ so the larger endpoint moves 0.1.
    auto xa = AQvec3(10.f, 0.f, 0.f);
    auto xb = AQvec3(0.f, 0.f, 0.f);
    float lambda = 0.f;
    const bool tripped = AQxpbdProjectDistance<float>(xa, xb, 1.f, 1.f, 1.f, 0.f,
                                                      0.01f, 0.1f, lambda);
    check(tripped, "guard: oversized correction reports a trip");
    check(feq(xa[0][0], 9.9f) && feq(xb[0][0], 0.1f) && feq(lambda, -0.1f),
          "guard: correction AND lambda clamped consistently to maxMove");
}

static AQXPBDBody makePendulum(float restLength) {
    AQXPBDBody body;
    OmegaCommon::Vector<FVec<3>> pos;
    pos.push_back(AQvec3(0.f, 0.f, 0.f));                 // pinned anchor
    pos.push_back(AQvec3(restLength, 0.f, 0.f));          // bob, horizontal
    const float w[2] = {0.f, 1.f};
    body.addParticles(pos.data(), w, 2);
    body.addDistance(0, 1, restLength, 0.f);
    return body;
}

static void testFreeFallClosedForm() {
    // No constraints: n slices of symplectic Euler must compose to the exact
    // closed form v = g·t, y = -g·h²·Σk (per slice) — the predict/derive pair
    // must be a clean integrator when projection has nothing to do.
    AQXPBDBody body;
    auto p0 = AQvec3(0.f, 0.f, 0.f);
    const float w = 1.f;
    body.addParticles(&p0, &w, 1);

    AQXPBDParams params;                 // substeps 4, iterations 1
    const auto gravity = AQvec3(0.f, -10.f, 0.f);
    const float dt = 1.f / 60.f;
    for (int step = 0; step < 60; ++step) body.advance(dt, params, gravity, kNoColliders);

    // After exactly 1 s of slices: v = -10; y = -10·h²·(N(N+1)/2), N = 240
    // slices of h = dt/4. Tolerance note: XPBD DERIVES v = (x − x_prev)/h, so
    // each slice re-extracts the velocity through a subtraction at position
    // scale (error ≈ ulp(|x|)/h ≈ 1e-4 here) rather than accumulating it —
    // a ~2e-3 random walk over 240 slices is inherent to the position-level
    // formulation, not an integrator defect (the position itself is tighter).
    const float h = dt / 4.f;
    const float N = 240.f;
    const float yExpect = -10.f * h * h * (N * (N + 1.f) / 2.f);
    check(feq(body.velocities[0][1][0], -10.f, 5e-3f),
          "free fall: velocity matches g*t through predict/derive across slices");
    check(feq(body.positions[0][1][0], yExpect, 5e-3f),
          "free fall: position matches the symplectic closed form");
}

static void testPendulumHoldsLength() {
    AQXPBDBody body = makePendulum(1.f);
    AQXPBDParams params;
    params.substeps = 8;
    const auto gravity = AQvec3(0.f, -9.81f, 0.f);
    const float dt = 1.f / 60.f;

    float maxViolation = 0.f;
    for (int step = 0; step < 600; ++step) {              // 10 s of swinging
        body.advance(dt, params, gravity, kNoColliders);
        const FVec<3> d = body.positions[1] - body.positions[0];
        maxViolation = std::max(maxViolation,
                                std::fabs(std::sqrt(OmegaGTE::dot(d, d)) - 1.f));
    }
    check(maxViolation < 1e-4f,
          "pendulum: alpha = 0 keeps the rod length rigid through a 10 s swing");
    check(feq(body.positions[0][0][0], 0.f) && feq(body.positions[0][1][0], 0.f),
          "pendulum: the pinned anchor never moves");
    check(body.guardTrips == 0, "pendulum: a healthy swing never trips the guard");
    check(!body.anyNonFinite(), "pendulum: state stays finite");
}

static void testBodyGuardTripsLoudly() {
    // A degenerate rig: the bob authored 100x beyond rest with a tiny guard
    // threshold — the first projection wants a correction far above maxMove.
    AQXPBDBody body = makePendulum(1.f);
    body.positions[1] = AQvec3(100.f, 0.f, 0.f);
    AQXPBDParams params;
    params.explosionThreshold = 0.01f;
    const auto gravity = AQvec3(0.f, -9.81f, 0.f);

    body.advance(1.f / 60.f, params, gravity, kNoColliders);
    check(body.guardTrips > 0, "guard: degenerate rig trips (loud, counted)");
    check(!body.trippedThisFrame.empty(), "guard: tripped constraint recorded for the debug bus");
    check(!body.anyNonFinite(), "guard: clamped solve stays finite (no silent NaN)");
}

static void testAdvanceDeterminism() {
    auto run = [](AQXPBDBody& body) {
        AQXPBDParams params;
        params.substeps = 6;
        const auto gravity = AQvec3(0.f, -9.81f, 0.f);
        for (int step = 0; step < 300; ++step) body.advance(1.f / 60.f, params, gravity, kNoColliders);
    };
    AQXPBDBody a = makePendulum(0.7f);
    AQXPBDBody b = makePendulum(0.7f);
    run(a);
    run(b);
    bool same = true;
    for (int c = 0; c < 3; ++c) {
        same = same && (a.positions[1][c][0] == b.positions[1][c][0]);
        same = same && (a.velocities[1][c][0] == b.velocities[1][c][0]);
    }
    check(same, "determinism: identical rigs advance bitwise identically within a path");
}

int main() {
    testProjectRigidClosedForm();
    testProjectComplianceClosedForm();
    testPinnedAndDegenerate();
    testGuardClamp();
    testFreeFallClosedForm();
    testPendulumHoldsLength();
    testBodyGuardTripsLoudly();
    testAdvanceDeterminism();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
