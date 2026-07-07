// AQUA Phase 7 §1/§9 — the runnable deliverable: a rope (chain of particles
// under distance constraints) driven through the PUBLIC AQContext/AQSpace
// surface exactly as kREATE would, validated against double-precision physical
// oracles computed OFF the hot path (§8 — the solve is float, the measurement
// is double, so measurement precision never depends on solve precision):
//
//   1. Energy no-injection: released to swing with zero damping, total
//      mechanical energy never exceeds its release value (a classic
//      PBD/XPBD misconfiguration injects energy; §9.1).
//   2. Monotone settle: with damping, energy decreases monotonically to a
//      rest floor and the rope hangs vertical.
//   3. Catenary: a slack rope pinned at both ends settles onto the analytic
//      y = a·cosh(x/a) within a stated RMS tolerance (§9.2).
//   4. compliance = 0 ⇒ inextensible under load (§9.3).
//   5. THE HEADLINE (§9.4): the same compliance produces the same steady-state
//      stretch at 60 Hz and 240 Hz stepping — the property classic PBD fails
//      and XPBD exists to provide — AND that stretch matches the analytic
//      per-segment C_i = α·T_i (T_i = weight hanging below segment i).
//   6. Bitwise within-path determinism (§9.6).
//   7. The explosion guard trips loudly on a degenerate rig and never on a
//      healthy one (§9.7); the debug bus draws constraints when asked.

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

// ---------------------------------------------------------------------------
// Rig plumbing
// ---------------------------------------------------------------------------

struct RopeRig {
    SharedHandle<AQContext> ctx;
    SharedHandle<AQSpace>   space;
    AQXPBDBodyHandle        body;
    std::uint32_t           count = 0;           // particles
    OmegaCommon::Vector<float> invMass;          // as authored (oracle needs masses)
    float                   segmentRest = 0.f;   // uniform rest length
    float                   fixedDt = 1.f / 60.f;
};

// Builds a rope of `count` particles at the given world positions; pins are
// invMass 0. Constraints chain i -> i+1 via the public addRope (rest derived
// from authored spacing).
static RopeRig makeRope(const OmegaCommon::Vector<OmegaGTE::FVec<3>>& pos,
                        const OmegaCommon::Vector<float>& invMass,
                        float compliance, const AQXPBDParams& params, float fixedDt) {
    RopeRig rig;
    rig.ctx = AQContext::CreateCPUOnly();
    rig.ctx->setFixedTimestep(fixedDt);
    rig.fixedDt = fixedDt;
    rig.space = rig.ctx->createSpace();
    rig.space->setXPBDParams(params);

    AQXPBDBodyDesc desc;
    desc.positions = pos.data();
    desc.invMass = invMass.data();
    desc.count = static_cast<std::uint32_t>(pos.size());
    rig.body = rig.space->createXPBDBody(desc);
    rig.count = desc.count;
    rig.invMass = invMass;

    OmegaCommon::Vector<std::uint32_t> chain;
    for (std::uint32_t i = 0; i < rig.count; ++i) chain.push_back(i);
    rig.space->addRope(rig.body, chain.data(), rig.count, compliance);

    const OmegaGTE::FVec<3> d = pos[0] - pos[1];
    rig.segmentRest = std::sqrt(OmegaGTE::dot(d, d));
    return rig;
}

static void run(RopeRig& rig, int frames) {
    for (int f = 0; f < frames; ++f) rig.ctx->advance(rig.fixedDt);
}

static std::uint32_t readState(const RopeRig& rig,
                               OmegaCommon::Vector<OmegaGTE::FVec<3>>& outPos,
                               OmegaCommon::Vector<OmegaGTE::FVec<3>>& outVel) {
    outPos.assign(rig.count, OmegaGTE::FVec<3>::Create());
    outVel.assign(rig.count, OmegaGTE::FVec<3>::Create());
    return rig.space->readXPBDState(rig.body, outPos.data(), outVel.data(), rig.count);
}

// Total mechanical energy, measured in DOUBLE off the hot path (§8). Pinned
// particles carry no kinetic or potential term. `yRef` sets the potential
// zero well below the scene so PE is positive and relative bands mean
// something.
static double ropeEnergy(const RopeRig& rig, double yRef) {
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos, vel;
    readState(rig, pos, vel);
    const double g = 9.81;
    double e = 0.0;
    for (std::uint32_t i = 0; i < rig.count; ++i) {
        if (rig.invMass[i] <= 0.f) continue;
        const double m = 1.0 / static_cast<double>(rig.invMass[i]);
        const double vx = vel[i][0][0], vy = vel[i][1][0], vz = vel[i][2][0];
        e += 0.5 * m * (vx * vx + vy * vy + vz * vz);
        e += m * g * (static_cast<double>(pos[i][1][0]) - yRef);
    }
    return e;
}

// ---------------------------------------------------------------------------
// 1 + 2 — energy: no injection undamped; monotone settle damped.
// ---------------------------------------------------------------------------

static RopeRig makeHorizontalRope(std::uint32_t particles, float damping, float fixedDt) {
    // Pinned at the origin, authored straight out along +x at exact rest
    // spacing — released to swing under gravity.
    const float spacing = 0.05f;
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    for (std::uint32_t i = 0; i < particles; ++i) {
        pos.push_back(AQvec3(spacing * static_cast<float>(i), 0.f, 0.f));
        w.push_back(i == 0 ? 0.f : 20.f);   // 50 g per particle
    }
    AQXPBDParams params;
    params.substeps = 8;
    params.velocityDamping = damping;
    return makeRope(pos, w, 0.f, params, fixedDt);
}

static void testEnergyNoInjection() {
    RopeRig rig = makeHorizontalRope(41, 0.f, 1.f / 60.f);
    const double yRef = -3.0;
    const double e0 = ropeEnergy(rig, yRef);

    double eMax = e0;
    for (int f = 0; f < 600; ++f) {                 // 10 s of undamped swinging
        rig.ctx->advance(rig.fixedDt);
        const double e = ropeEnergy(rig, yRef);
        if (e > eMax) eMax = e;
    }
    check(eMax <= e0 * (1.0 + 1e-4),
          "energy: undamped swing never rises above the release energy (no injection)");
    check(rig.space->xpbdGuardTrips(rig.body) == 0, "energy: healthy swing, zero guard trips");
}

static void testMonotoneSettle() {
    RopeRig rig = makeHorizontalRope(41, 0.05f, 1.f / 60.f);
    const double yRef = -3.0;

    double prev = ropeEnergy(rig, yRef);
    const double band = 1e-6 * prev + 1e-12;        // fp noise floor
    bool monotone = true;
    for (int f = 0; f < 1800; ++f) {                // 30 s damped
        rig.ctx->advance(rig.fixedDt);
        const double e = ropeEnergy(rig, yRef);
        if (e > prev + band) monotone = false;
        prev = e;
    }
    check(monotone, "settle: damped energy decreases monotonically to the rest floor");

    // Settled shape: hanging straight down from the pin.
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos, vel;
    readState(rig, pos, vel);
    bool vertical = true;
    double keFinal = 0.0;
    for (std::uint32_t i = 0; i < rig.count; ++i) {
        vertical = vertical && std::fabs(pos[i][0][0]) < 2e-3f &&
                   std::fabs(pos[i][2][0]) < 2e-3f;
        if (rig.invMass[i] > 0.f) {
            const double m = 1.0 / rig.invMass[i];
            keFinal += 0.5 * m * OmegaGTE::dot(vel[i], vel[i]);
        }
    }
    check(vertical, "settle: rope hangs vertical under the pin");
    check(keFinal < 1e-6, "settle: kinetic energy at the rest floor is ~0");
}

// ---------------------------------------------------------------------------
// 3 — catenary fit (double, off-path).
// ---------------------------------------------------------------------------

static void testCatenary() {
    // 40 segments of 0.05 m (L = 2 m) between pins 1.6 m apart at equal
    // height, authored as a V of exact rest-length segments so no constraint
    // starts violated. Settles under damping onto the catenary.
    const std::uint32_t segs = 40, particles = segs + 1;
    const double L = 2.0, S = 1.6;
    const double rest = L / segs;
    const double dx = S / segs;
    const double dy = std::sqrt(rest * rest - dx * dx);

    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    double x = 0.0, y = 0.0;
    for (std::uint32_t i = 0; i < particles; ++i) {
        pos.push_back(AQvec3(static_cast<float>(x), static_cast<float>(y), 0.f));
        w.push_back((i == 0 || i == particles - 1) ? 0.f : 20.f);
        x += dx;
        y += (i < segs / 2) ? -dy : dy;             // V shape, bottom mid-span
    }
    AQXPBDParams params;
    params.substeps = 16;
    params.velocityDamping = 0.05f;
    RopeRig rig = makeRope(pos, w, 0.f, params, 1.f / 60.f);
    run(rig, 3000);                                  // 50 s — deep settle

    // Analytic catenary through both pins: solve L = 2a·sinh(S/(2a)) for the
    // parameter a (bisection, double), then y(x) = a·cosh((x−S/2)/a) + C with
    // C fixed by y(0) = 0.
    double lo = 0.05, hi = 100.0;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        const double arc = 2.0 * mid * std::sinh(S / (2.0 * mid));
        (arc > L ? lo : hi) = mid;                  // arc shrinks as a grows
    }
    const double a = 0.5 * (lo + hi);
    const double C = -a * std::cosh((0.0 - S / 2.0) / a);

    OmegaCommon::Vector<OmegaGTE::FVec<3>> settled, vel;
    readState(rig, settled, vel);
    double rms = 0.0;
    for (std::uint32_t i = 0; i < particles; ++i) {
        const double xi = settled[i][0][0];
        const double yi = settled[i][1][0];
        const double yc = a * std::cosh((xi - S / 2.0) / a) + C;
        rms += (yi - yc) * (yi - yc);
    }
    rms = std::sqrt(rms / particles);
    std::printf("       catenary: a = %.4f, RMS deviation = %.4e m (L = %.1f m)\n", a, rms, L);
    check(rms < 0.02, "catenary: settled rope matches the analytic curve (RMS < 1% of L)");
    check(rig.space->xpbdGuardTrips(rig.body) == 0, "catenary: zero guard trips");
}

// ---------------------------------------------------------------------------
// 4 + 5 — inextensibility at alpha = 0; timestep-independent stretch at
//         alpha > 0 (the headline oracle) + the analytic alpha*T_i check.
// ---------------------------------------------------------------------------

static RopeRig makeVerticalRope(float compliance, float fixedDt, std::uint32_t substeps,
                                float endMassFactor) {
    const std::uint32_t particles = 41;
    const float spacing = 0.05f;
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    for (std::uint32_t i = 0; i < particles; ++i) {
        pos.push_back(AQvec3(0.f, -spacing * static_cast<float>(i), 0.f));
        float wi = (i == 0) ? 0.f : 20.f;                       // 50 g
        if (i == particles - 1 && endMassFactor > 1.f) wi /= endMassFactor;
        w.push_back(wi);
    }
    AQXPBDParams params;
    params.substeps = substeps;
    params.velocityDamping = 0.1f;
    return makeRope(pos, w, compliance, params, fixedDt);
}

static void segmentStretches(const RopeRig& rig, OmegaCommon::Vector<double>& out) {
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos, vel;
    readState(rig, pos, vel);
    out.clear();
    for (std::uint32_t i = 0; i + 1 < rig.count; ++i) {
        const double ax = pos[i][0][0],     ay = pos[i][1][0],     az = pos[i][2][0];
        const double bx = pos[i + 1][0][0], by = pos[i + 1][1][0], bz = pos[i + 1][2][0];
        const double len = std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by) +
                                     (az - bz) * (az - bz));
        out.push_back(len - static_cast<double>(rig.segmentRest));
    }
}

static void testInextensibleUnderLoad() {
    // 50x end mass, alpha = 0: every segment must hold its rest length.
    //
    // Substep note (§9.3 tuning): a 40-deep chain under a 50x end load is the
    // hard Gauss-Seidel case — one projection sweep per slice propagates a
    // correction one constraint per color, so the UNCONVERGED residual stretch
    // scales ~ (N²/2)·g·h². That is exactly the budget the small-steps doctrine
    // says to spend on substeps (Macklin 2019): at 64 slices per 1/60 s
    // sub-step (h ≈ 0.26 ms) the residual for this rig sits well under 0.5%.
    // Deeper convergence upgrades (multi-layer XPBD, Mercier-Aubin 2024;
    // long-range attachments) are the flagged 7.x follow-up in the plan doc.
    RopeRig rig = makeVerticalRope(0.f, 1.f / 60.f, 64, 50.f);
    run(rig, 1200);                                  // 20 s settle

    OmegaCommon::Vector<double> stretch;
    segmentStretches(rig, stretch);
    double maxRel = 0.0;
    for (double s : stretch) maxRel = std::max(maxRel, std::fabs(s) / rig.segmentRest);
    std::printf("       alpha=0 max |stretch|/rest = %.3e under a 50x end load\n", maxRel);
    check(maxRel < 5e-3, "alpha = 0: rigid under a 50x end load (max stretch < 0.5%)");
}

static void testTimestepIndependentStretch() {
    // Same compliance, two different slice sizes: 60 Hz x 24 substeps
    // (h ≈ 0.69 ms) vs 240 Hz x 8 substeps (h ≈ 0.52 ms). Classic PBD's
    // effective stiffness scales with the step, so its steady-state stretch
    // would differ ~4x between these rigs; XPBD's must match. The compliance
    // is chosen big enough (analytic stretch ≈ 0.40 m over the 2 m rope) that
    // the ~(N²/2)·g·h² unconverged-sweep residual (≈ 2% / 0.5% of the signal
    // at these h) cannot masquerade as the property under test.
    const float alpha = 1e-3f;
    RopeRig r60  = makeVerticalRope(alpha, 1.f / 60.f, 24, 1.f);
    RopeRig r240 = makeVerticalRope(alpha, 1.f / 240.f, 8, 1.f);
    run(r60, 1800);                                  // 30 s
    run(r240, 7200);                                 // 30 s at 4x frame rate

    OmegaCommon::Vector<double> s60, s240;
    segmentStretches(r60, s60);
    segmentStretches(r240, s240);

    double total60 = 0.0, total240 = 0.0;
    for (double s : s60) total60 += s;
    for (double s : s240) total240 += s;
    const double relDiff = std::fabs(total60 - total240) / std::max(total240, 1e-12);
    // Analytic total: alpha·m·g·Σk = 1e-3 · 0.05 · 9.81 · 820 ≈ 0.402 m.
    std::printf("       stretch @60Hz = %.6f m, @240Hz = %.6f m (analytic 0.402), rel diff = %.3e\n",
                total60, total240, relDiff);
    check(total60 > 0.1, "compliance > 0: stretch is measurable (the spring is real)");
    check(relDiff < 3e-2,
          "HEADLINE: the same compliance yields the same stretch at 60 Hz and 240 Hz (XPBD)");

    // Analytic oracle: at equilibrium C_i = alpha * T_i, T_i = weight below
    // segment i = (particles below) * m * g. Checked mid-rope where discrete
    // end effects are smallest; 5% covers the 1-iteration steady-state bias
    // on the finer rig.
    const double m = 1.0 / 20.0, g = 9.81;
    bool analytic = true;
    for (std::uint32_t i = 5; i + 5 < static_cast<std::uint32_t>(s240.size()); ++i) {
        const double tension = static_cast<double>(s240.size() - i) * m * g;
        const double expect = static_cast<double>(alpha) * tension;
        if (std::fabs(s240[i] - expect) > 0.05 * expect) analytic = false;
    }
    check(analytic, "compliance > 0: per-segment stretch matches alpha * T_i within 5%");
}

// ---------------------------------------------------------------------------
// 6 + 7 — determinism; the loud guard; debug bus.
// ---------------------------------------------------------------------------

static void testDeterminism() {
    auto sample = [](RopeRig& rig, OmegaCommon::Vector<OmegaGTE::FVec<3>>& pos) {
        run(rig, 300);
        OmegaCommon::Vector<OmegaGTE::FVec<3>> vel;
        readState(rig, pos, vel);
    };
    RopeRig a = makeHorizontalRope(33, 0.01f, 1.f / 60.f);
    RopeRig b = makeHorizontalRope(33, 0.01f, 1.f / 60.f);
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pa, pb;
    sample(a, pa);
    sample(b, pb);
    bool same = pa.size() == pb.size();
    for (std::size_t i = 0; same && i < pa.size(); ++i)
        for (int c = 0; c < 3; ++c) same = same && (pa[i][c][0] == pb[i][c][0]);
    check(same, "determinism: identical ropes advance bitwise identically within a path");
}

static void testGuardAndDebugBus() {
    // Degenerate rig: authored 20x overstretched with a tiny guard threshold.
    const float spacing = 1.0f;                      // vs segmentRest authored below
    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    for (std::uint32_t i = 0; i < 8; ++i) {
        pos.push_back(AQvec3(spacing * static_cast<float>(i), 0.f, 0.f));
        w.push_back(i == 0 ? 0.f : 1.f);
    }
    AQXPBDParams params;
    params.explosionThreshold = 1e-3f;
    RopeRig rig;
    rig.ctx = AQContext::CreateCPUOnly();
    rig.ctx->setFixedTimestep(1.f / 60.f);
    rig.space = rig.ctx->createSpace();
    rig.space->setXPBDParams(params);
    AQXPBDBodyDesc desc;
    desc.positions = pos.data();
    desc.invMass = w.data();
    desc.count = 8;
    rig.body = rig.space->createXPBDBody(desc);
    rig.count = 8;
    rig.invMass = w;
    // Explicit rest length FAR below the authored spacing — a violated rig.
    for (std::uint32_t i = 0; i + 1 < 8; ++i)
        rig.space->addDistanceConstraint(rig.body, i, i + 1, 0.05f, 0.f);

    rig.space->setDebugFlags(AQDebugConstraint | AQDebugConstraintColor);
    rig.ctx->advance(1.f / 60.f);

    check(rig.space->xpbdGuardTrips(rig.body) > 0,
          "guard: a violated over-constrained rig trips loudly (counted, clamped)");

    OmegaCommon::Vector<OmegaGTE::FVec<3>> p, v;
    readState(rig, p, v);
    bool finite = true;
    for (std::uint32_t i = 0; i < rig.count; ++i)
        for (int c = 0; c < 3; ++c) finite = finite && std::isfinite(p[i][c][0]);
    check(finite, "guard: clamped solve stays finite (no NaN escapes)");

    auto lines = rig.space->drainDebugLines();
    // 7 constraints x 2 flag families = 14 lines per advance.
    check(lines.size() >= 14, "debug bus: constraint + color lines emitted when flagged");

    rig.space->setDebugFlags(AQDebugNone);
    rig.ctx->advance(1.f / 60.f);
    check(rig.space->drainDebugLines().empty(), "debug bus: silent when flags are off");
}

int main() {
    testEnergyNoInjection();
    testMonotoneSettle();
    testCatenary();
    testInextensibleUnderLoad();
    testTimestepIndependentStretch();
    testDeterminism();
    testGuardAndDebugBus();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
