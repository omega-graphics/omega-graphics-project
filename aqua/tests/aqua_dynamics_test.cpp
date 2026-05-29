// AQUA Phase 1 — integration test through the PUBLIC API (AQContext / AQSpace /
// AQRigidBody). The math test (aqua_math_test.cpp) validates the integrator
// directly; this one proves the public surface — descriptors, the force/impulse
// API, and the fixed-sub-step accumulator in AQContext — wires through to that
// same integrator.
//
// It links the AQUA library (which has no link-time GTE dependency — the math is
// header-only templates), and drives a torque-free asymmetric body so the
// tennis-racket flip and angular-momentum conservation are observable through
// the public getters.

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQMath.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

float vlen(const OmegaGTE::FVec<3>& v) { return std::sqrt(OmegaGTE::dot(v, v)); }

// World angular momentum from the public getters: ω is reported world-frame, so
// L = R · (Ib ⊙ (Rᵀ ω)).
OmegaGTE::FVec<3> worldL(const AQRigidBody& body) {
    const auto q = body.orientation();
    const auto wB = AQrotate(q.conjugate(), body.angularVelocity());
    const auto I = body.inertiaPrincipalMoments();
    const auto Lb = AQvec3(I[0][0] * wB[0][0], I[1][0] * wB[1][0], I[2][0] * wB[2][0]);
    return AQrotate(q, Lb);
}

} // namespace

int main() {
    std::printf("== public-API dynamics: AQContext/AQSpace/AQRigidBody ==\n");

    // AQContext holds an OmegaGTE command queue but never touches it during CPU
    // stepping, so a null handle is sufficient for this headless dynamics test.
    auto ctx = AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue>());
    ctx->setFixedTimestep(1.f / 2000.f);

    auto space = ctx->createSpace();
    space->setGravity(AQvec3(0.f, 0.f, 0.f));            // torque-free: no gravity

    // --- impulse API check: off-center impulse induces the expected spin. ---
    {
        AQBodyDesc d;
        d.mass = 1.f;
        d.inertiaPrincipalMoments = AQinertiaSolidBox(1.f, 0.5f, 1.0f, 1.5f);
        d.position = AQvec3(0.f, 0.f, 0.f);
        auto body = space->addBody(d);

        // Impulse along +Y applied at +X offset -> torque about +Z (r × J).
        body->applyImpulseAtPoint(AQvec3(0.f, 1.f, 0.f), AQvec3(1.f, 0.f, 0.f));
        const auto w = body->angularVelocity();
        check(vlen(body->velocity()) > 0.f, "off-center impulse adds linear velocity");
        check(w[2][0] > 0.f && std::abs(w[0][0]) < 1e-5f && std::abs(w[1][0]) < 1e-5f,
              "off-center impulse spins body about +Z only");
        space->removeBody(body);
    }

    // --- headline: torque-free asymmetric body flips while conserving L. ---
    AQBodyDesc desc;
    desc.mass = 1.f;
    // I1<I2<I3 via box half-extents; spin about the intermediate (y) axis.
    desc.inertiaPrincipalMoments = AQvec3(1.f, 2.f, 3.f);
    desc.angularVelocity = AQvec3(0.02f, 8.f, 0.f);      // world == body at identity q
    auto body = space->addBody(desc);

    // The tennis-racket flip reverses the BODY-frame ω_y; the public getter
    // reports world frame, so rotate it back by Rᵀ to observe the flip.
    auto bodyOmegaY = [](const AQRigidBody& b) {
        return AQrotate(b.orientation().conjugate(), b.angularVelocity())[1][0];
    };

    const auto L0 = worldL(*body);
    const float L0n = vlen(L0);
    float maxLdrift = 0.f, maxQdrift = 0.f;
    float minWy = bodyOmegaY(*body), maxWy = minWy;

    // 20 s of real time fed through the fixed-step accumulator.
    const float frame = 1.f / 60.f;
    for (int i = 0; i < 20 * 60; ++i) {
        ctx->advance(frame);
        const auto Lw = worldL(*body);
        maxLdrift = std::max(maxLdrift, vlen(Lw - L0) / L0n);
        maxQdrift = std::max(maxQdrift, std::abs(body->orientation().length() - 1.f));
        const float wy = bodyOmegaY(*body);
        minWy = std::min(minWy, wy);
        maxWy = std::max(maxWy, wy);
    }

    std::printf("  elapsed sim time = %.3f s   ‖L‖ drift max = %.3e   ‖q‖-1 max = %.3e\n",
                ctx->elapsed(), maxLdrift, maxQdrift);
    std::printf("  ω_y range = [%.3f, %.3f]\n", minWy, maxWy);

    check(std::abs(ctx->elapsed() - 20.0) < 0.1, "accumulator advanced ~20 s of sim time");
    // Bounded & small at this sub-step (the scheme is O(dt) in conservation, not
    // exact — see the math test's convergence check and the §11.1 finding).
    check(maxLdrift < 2e-2f, "angular momentum drift bounded through public API");
    check(maxQdrift < 1e-4f, "orientation stays unit through public API");
    check(minWy < -4.f && maxWy > 4.f, "tennis-racket flip observed via public getters");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
