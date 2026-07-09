// AQUA Phase 7h #2 §13.7 — far-from-origin hardening (the solver2d XPBD precision
// failure). Simulated far from the world origin, the derive step's (x−x_prev)/h
// and the projection differences (xa−xb) are computed from large, nearly-equal
// float values → catastrophic cancellation → the rope never settles (velocities
// jitter at the quantization floor) and its shape is coarsened to the float ulp.
// With `originRelative` on, the body solves on small offsets from a re-basing
// origin, recovering full precision. Driven through the PUBLIC AQContext/AQSpace.
//
//   1. Settling: at world ~1e5 the flag-OFF rope's residual speed is stuck at the
//      float-noise floor; the flag-ON rope settles as cleanly as at the origin.
//   2. Correctness: the flag-ON far rope's LOCAL shape matches the near-origin
//      solve within a tight tolerance.
//   3. The flag defaults OFF ⇒ a near-origin rope is unaffected (origin stays 0).

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
    OmegaGTE::FVec<3>       pin = OmegaGTE::FVec<3>::Create();
    float                   fixedDt = 1.f / 60.f;
};

// Horizontal rope pinned at particle 0, authored straight out along +x at
// `origin + i·spacing`, released to swing down and settle under damping.
static Rig makeRope(const OmegaGTE::FVec<3>& worldOrigin, bool originRelative) {
    Rig rig;
    rig.ctx = AQContext::CreateCPUOnly();
    rig.ctx->setFixedTimestep(rig.fixedDt);
    rig.space = rig.ctx->createSpace();

    AQXPBDParams params;
    params.substeps = 8;
    params.velocityDamping = 0.05f;
    params.originRelative = originRelative;
    rig.space->setXPBDParams(params);

    const std::uint32_t n = 20;
    const float spacing = 0.1f;
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float>             w;
    for (std::uint32_t i = 0; i < n; ++i) {
        pos.push_back(worldOrigin + AQvec3(spacing * static_cast<float>(i), 0.f, 0.f));
        w.push_back(i == 0 ? 0.f : 1.f);
    }
    AQXPBDBodyDesc desc;
    desc.positions = pos.data();
    desc.invMass   = w.data();
    desc.count     = n;
    rig.body  = rig.space->createXPBDBody(desc);
    rig.count = n;
    rig.pin   = worldOrigin;

    OmegaCommon::Vector<std::uint32_t> chain;
    for (std::uint32_t i = 0; i < n; ++i) chain.push_back(i);
    rig.space->addRope(rig.body, chain.data(), n, 0.f);
    return rig;
}

static void readState(const Rig& rig, OmegaCommon::Vector<OmegaGTE::FVec<3>>& pos,
                      OmegaCommon::Vector<OmegaGTE::FVec<3>>& vel) {
    pos.assign(rig.count, OmegaGTE::FVec<3>::Create());
    vel.assign(rig.count, OmegaGTE::FVec<3>::Create());
    rig.space->readXPBDState(rig.body, pos.data(), vel.data(), rig.count);
}

// The free end's LOCAL height below the pin, in double, off the hot path. A
// hanging rope droops (large negative); a rope FROZEN by float cancellation at a
// huge world coordinate — where a slice's gravity step falls below the ulp and
// rounds to nothing — stays at its authored horizontal (≈ 0).
static double endDropBelowPin(const Rig& rig) {
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos, vel;
    readState(rig, pos, vel);
    return static_cast<double>(pos[rig.count - 1][1][0]) - static_cast<double>(rig.pin[1][0]);
}

static void run(Rig& rig, int frames) { for (int f = 0; f < frames; ++f) rig.ctx->advance(rig.fixedDt); }

int main() {
    std::printf("=== AQUA Phase 7h #2 — far-from-origin hardening ===\n");

    const OmegaGTE::FVec<3> near = AQvec3(0.f, 0.f, 0.f);
    const OmegaGTE::FVec<3> far  = AQvec3(1.0e5f, 0.f, 0.f);

    Rig nearOff = makeRope(near, /*originRelative=*/false);
    Rig farOff  = makeRope(far,  /*originRelative=*/false);
    Rig farOn   = makeRope(far,  /*originRelative=*/true);
    run(nearOff, 20 * 60);
    run(farOff,  20 * 60);
    run(farOn,   20 * 60);

    const double dNear   = endDropBelowPin(nearOff);
    const double dFarOff = endDropBelowPin(farOff);
    const double dFarOn  = endDropBelowPin(farOn);
    std::printf("       free-end drop below pin:  near(off)=%.4f  far(off)=%.4f  far(on)=%.4f\n",
                dNear, dFarOff, dFarOn);

    // Near the origin the default (flag-off) rope hangs — the free end droops.
    check(dNear < -1.0, "near origin: rope hangs under gravity (float precision ample)");
    // Far away, the flag-off rope barely falls: at world ~1e5 a slice's g·h² step
    // is near the float ulp and mostly rounds away, so the rope droops far less
    // than it should (here ~0.46 m vs the correct ~1.9 m — the precision failure).
    check(dFarOff > -1.0, "far + flag OFF: rope barely falls (float cancellation starves the solve)");
    // With origin-relative on, the solve runs on small offsets and the far rope
    // hangs just like at the origin.
    check(dFarOn < -1.0, "far + flag ON: rope hangs again (precision recovered)");
    check(std::abs(dFarOn - dNear) < 0.05, "far + flag ON: same drop as the near-origin solve");

    // Correctness: the flag-ON far rope's LOCAL shape (positions relative to its
    // pin) matches the near-origin solve within a tight tolerance.
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pn, vn, pf, vf;
    readState(nearOff, pn, vn);
    readState(farOn,   pf, vf);
    float rms = 0.f;
    for (std::uint32_t i = 0; i < nearOff.count; ++i) {
        const OmegaGTE::FVec<3> ln = pn[i] - nearOff.pin;   // local (pin-relative)
        const OmegaGTE::FVec<3> lf = pf[i] - farOn.pin;
        const OmegaGTE::FVec<3> e = ln - lf;
        rms += OmegaGTE::dot(e, e);
    }
    rms = std::sqrt(rms / static_cast<float>(nearOff.count));
    std::printf("       local-shape RMS(far-on vs near) = %.6f m\n", rms);
    check(rms < 5e-3f, "far + flag ON: local shape matches the near-origin solve");

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
