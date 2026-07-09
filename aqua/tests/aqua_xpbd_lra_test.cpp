// AQUA Phase 7h #1 §13.7 — long-range attachments (Kim/Chentanez/Müller 2012).
// A long INEXTENSIBLE pinned chain that a 1-iteration XPBD solve leaves visibly
// over-stretched (the ~(N²/2)·g·h² residual the 7e rope data quantified) is
// pulled back toward its geodesic reach — inextensible-to-the-pin — in a single
// iteration once LRA is enabled. Driven through the PUBLIC AQContext/AQSpace API.
//
//   1. Convergence: at substeps×iterations=1×1 the chain over-stretches; with
//      LRA on, the pin→end distance holds at the geodesic sum of rest lengths.
//   2. No-op safety: enabling LRA on a rope with NO pinned particle changes
//      nothing and never crashes (no attachment target).
//   3. Determinism: LRA on, two runs are bitwise-identical.

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

struct Rig {
    SharedHandle<AQContext> ctx;
    SharedHandle<AQSpace>   space;
    AQXPBDBodyHandle        body;
    std::uint32_t           count = 0;
    float                   fixedDt = 1.f / 60.f;
};

// A vertical chain: particle 0 pinned at the origin (unless `pinTop` is false),
// the rest hanging straight down at `spacing`. Inextensible (compliance 0),
// deliberately under-iterated (1 substep × 1 iteration) so the LRA effect shows.
static Rig makeVerticalRope(std::uint32_t n, float spacing, bool pinTop, bool lra) {
    Rig rig;
    rig.ctx = AQContext::CreateCPUOnly();
    rig.ctx->setFixedTimestep(rig.fixedDt);
    rig.space = rig.ctx->createSpace();

    AQXPBDParams params;
    params.substeps = 1;              // deliberately starved — the LRA regime
    params.iterations = 1;
    params.velocityDamping = 0.1f;    // settle to a steady state we can measure
    rig.space->setXPBDParams(params);

    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float>             w;
    for (std::uint32_t i = 0; i < n; ++i) {
        pos.push_back(AQvec3(0.f, -spacing * static_cast<float>(i), 0.f));
        w.push_back((pinTop && i == 0) ? 0.f : 1.f);
    }
    AQXPBDBodyDesc desc;
    desc.positions = pos.data();
    desc.invMass   = w.data();
    desc.count     = n;
    rig.body  = rig.space->createXPBDBody(desc);
    rig.count = n;

    OmegaCommon::Vector<std::uint32_t> chain;
    for (std::uint32_t i = 0; i < n; ++i) chain.push_back(i);
    rig.space->addRope(rig.body, chain.data(), n, 0.f);   // inextensible
    if (lra) rig.space->setXPBDLongRangeAttachment(rig.body, true);
    return rig;
}

static float pinToEnd(const Rig& rig) {
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos(rig.count, OmegaGTE::FVec<3>::Create());
    OmegaCommon::Vector<OmegaGTE::FVec<3>> vel(rig.count, OmegaGTE::FVec<3>::Create());
    rig.space->readXPBDState(rig.body, pos.data(), vel.data(), rig.count);
    const OmegaGTE::FVec<3> d = pos[rig.count - 1] - pos[0];
    return std::sqrt(OmegaGTE::dot(d, d));
}

static void testConvergence() {
    const std::uint32_t n = 40;
    const float spacing = 0.1f;
    const float geodesic = spacing * static_cast<float>(n - 1);   // ideal inextensible reach

    Rig without = makeVerticalRope(n, spacing, /*pinTop=*/true, /*lra=*/false);
    Rig with    = makeVerticalRope(n, spacing, /*pinTop=*/true, /*lra=*/true);
    for (int f = 0; f < 20 * 60; ++f) { without.ctx->advance(without.fixedDt); with.ctx->advance(with.fixedDt); }

    const float dWithout = pinToEnd(without);
    const float dWith    = pinToEnd(with);
    std::printf("       geodesic=%.4f  without-LRA=%.4f  with-LRA=%.4f\n",
                geodesic, dWithout, dWith);

    // The starved solve over-stretches the chain past its geodesic reach...
    check(dWithout > geodesic * 1.02f,
          "starved 1x1 solve leaves the inextensible chain over-stretched");
    // ...and LRA pulls it back to (at most) the geodesic sum of rest lengths.
    check(dWith <= geodesic * 1.005f,
          "LRA holds the chain at its geodesic reach (inextensible-to-the-pin)");
    check(dWith < dWithout,
          "LRA strictly reduces the residual stretch");
}

static void testNoPinNoOp() {
    // A free (unpinned) rope: LRA has no attachment target ⇒ it must be a no-op,
    // not a crash. Compare the free-fall end-distance with and without the flag.
    Rig lraOn  = makeVerticalRope(10, 0.1f, /*pinTop=*/false, /*lra=*/true);
    Rig lraOff = makeVerticalRope(10, 0.1f, /*pinTop=*/false, /*lra=*/false);
    for (int f = 0; f < 60; ++f) { lraOn.ctx->advance(lraOn.fixedDt); lraOff.ctx->advance(lraOff.fixedDt); }
    check(std::abs(pinToEnd(lraOn) - pinToEnd(lraOff)) < 1e-4f,
          "no-op: LRA on a rope with no pins changes nothing");
}

static void testDeterminism() {
    auto run = [](float& out) {
        Rig r = makeVerticalRope(30, 0.1f, true, true);
        for (int f = 0; f < 5 * 60; ++f) r.ctx->advance(r.fixedDt);
        OmegaCommon::Vector<OmegaGTE::FVec<3>> pos(r.count, OmegaGTE::FVec<3>::Create());
        OmegaCommon::Vector<OmegaGTE::FVec<3>> vel(r.count, OmegaGTE::FVec<3>::Create());
        r.space->readXPBDState(r.body, pos.data(), vel.data(), r.count);
        out = pos[r.count - 1][1][0];
    };
    float a = 0.f, b = 0.f;
    run(a); run(b);
    check(a == b, "determinism: LRA runs are bitwise-identical");
}

int main() {
    std::printf("=== AQUA Phase 7h #1 — long-range attachments ===\n");
    testConvergence();
    testNoPinNoOp();
    testDeterminism();
    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
