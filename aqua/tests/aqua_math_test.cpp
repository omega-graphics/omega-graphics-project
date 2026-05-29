// AQUA Phase 1 — math core + integrator validation (Phase-1-Dynamics-Math-Core.md
// §6/§9). Pure CPU and header-only: it includes AQMath.h / AQIntegrator.h and the
// borrowed GTE math header, links nothing heavy, and so runs on any host
// independent of the GPU backend. It is the seed of the §6 CPU/GPU parity
// harness, brought online a phase early.
//
// Two parts:
//   1. Unit checks on the new owned math (skew, exp/log, rotate, inertia,
//      diagonalize, worldInvInertia, transform).
//   2. The headline dynamics: a torque-free asymmetric body that flips
//      (Dzhanibekov) while conserving angular momentum and bounding energy; a
//      torque-free symmetric top whose body-frame precession matches the
//      analytic rate; and float-vs-double parity from the SAME integrator code.

#include <aqua/AQMath.h>
#include <aqua/AQIntegrator.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

template<class Ty>
Ty vlen(const AQVec3<Ty>& v) { return std::sqrt(OmegaGTE::dot(v, v)); }

template<class Ty>
Ty vdist(const AQVec3<Ty>& a, const AQVec3<Ty>& b) { return vlen<Ty>(a - b); }

// World angular momentum L = R · (Ib ⊙ ω_body), and rotational energy
// E = ½ ω_body · (Ib ⊙ ω_body), from a body state (moments = 1/invInertia).
template<class Ty>
AQVec3<Ty> worldL(const AQBodyState<Ty>& b) {
    const AQVec3<Ty> Lb = AQvec3<Ty>(b.angularVelBody[0][0] / b.invInertiaBody[0][0],
                                     b.angularVelBody[1][0] / b.invInertiaBody[1][0],
                                     b.angularVelBody[2][0] / b.invInertiaBody[2][0]);
    return AQrotate(b.orientation, Lb);
}

template<class Ty>
Ty rotEnergy(const AQBodyState<Ty>& b) {
    Ty e = Ty(0);
    for (int i = 0; i < 3; ++i)
        e += b.angularVelBody[i][0] * b.angularVelBody[i][0] / b.invInertiaBody[i][0];
    return Ty(0.5) * e;
}

// ----------------------------------------------------------------------------
// Part 1 — math unit checks
// ----------------------------------------------------------------------------

void testMath() {
    std::printf("\n== math unit checks ==\n");
    const double eps = 1e-12;

    // skew(a)·b == cross(a,b)
    {
        auto a = AQvec3<double>(0.3, -1.2, 2.0);
        auto b = AQvec3<double>(1.5, 0.7, -0.4);
        auto viaSkew = AQskew(a) * b;
        auto viaCross = OmegaGTE::cross(a, b);
        check(vdist(viaSkew, viaCross) < eps, "skew(a)*b == cross(a,b)");
    }

    // exp/log round-trip and unit-ness
    {
        auto half = AQvec3<double>(0.21, -0.05, 0.13);  // ½·φ
        auto q = AQquatExp(half);
        check(std::abs(q.length() - 1.0) < 1e-12, "quatExp is unit");
        auto half2 = AQquatLog(q);
        check(vdist(half, half2) < 1e-12, "quatLog ∘ quatExp == id");
        // small-angle branch is finite and unit at (near) zero rotation
        auto qz = AQquatExp(AQvec3<double>(1e-9, 0.0, -1e-9));
        check(std::isfinite(qz.w) && std::abs(qz.length() - 1.0) < 1e-9,
              "quatExp finite & unit near zero rotation");
    }

    // rotate a free vector by 90° about +Z maps +X -> +Y
    {
        auto q = AQquatExp(AQvec3<double>(0.0, 0.0, OmegaGTE::Pi<double> / 4.0)); // ½·(π/2)
        auto r = AQrotate(q, AQvec3<double>(1.0, 0.0, 0.0));
        check(vdist(r, AQvec3<double>(0.0, 1.0, 0.0)) < 1e-12, "rotate +X by 90°Z == +Y");
        // rotate preserves length
        auto v = AQvec3<double>(0.4, -1.1, 2.3);
        check(std::abs(vlen(AQrotate(q, v)) - vlen(v)) < 1e-12, "rotate preserves length");
    }

    // solid box inertia: cube of half-extent h, mass m -> (2/3) m h² each axis
    {
        const double m = 2.0, h = 0.5;
        auto I = AQinertiaSolidBox(m, h, h, h);
        const double want = 2.0 / 3.0 * m * h * h;
        check(std::abs(I[0][0] - want) < eps && std::abs(I[1][0] - want) < eps &&
              std::abs(I[2][0] - want) < eps, "inertiaSolidBox cube == (2/3)mh²");
    }
    // solid sphere inertia: (2/5) m r²
    {
        auto I = AQinertiaSolidSphere(3.0, 0.5);
        check(std::abs(I[0][0] - 2.0 / 5.0 * 3.0 * 0.25) < eps, "inertiaSolidSphere == (2/5)mr²");
    }

    // diagonalize: feed a tensor built as Rᵀ diag(d) R, recover d (as a set)
    {
        auto d = AQvec3<double>(1.0, 2.0, 3.0);
        auto qR = AQquatExp(AQvec3<double>(0.3, -0.2, 0.5)).normalized();
        // Build I = R diag(d) Rᵀ via the same outer-product route as worldInvInertia.
        AQVec3<double> invd = AQvec3<double>(d[0][0], d[1][0], d[2][0]); // reuse as "moments"
        auto I = AQworldInvInertia(qR, invd); // == R diag(d) Rᵀ
        auto moments = AQVec3<double>::Create();
        auto axis = OmegaGTE::Quaternion<double>::Identity();
        AQdiagonalizeInertia(I, moments, axis);
        // eigenvalues recovered (order-independent): sort both
        double got[3] = {moments[0][0], moments[1][0], moments[2][0]};
        double exp3[3] = {1.0, 2.0, 3.0};
        std::sort(got, got + 3); std::sort(exp3, exp3 + 3);
        check(std::abs(got[0]-exp3[0]) < 1e-9 && std::abs(got[1]-exp3[1]) < 1e-9 &&
              std::abs(got[2]-exp3[2]) < 1e-9, "diagonalizeInertia recovers eigenvalues");
    }

    // worldInvInertia at identity == diag(invMoments)
    {
        auto inv = AQvec3<double>(0.5, 0.25, 0.125);
        auto M = AQworldInvInertia(OmegaGTE::Quaternion<double>::Identity(), inv);
        check(std::abs(M[0][0]-0.5) < eps && std::abs(M[1][1]-0.25) < eps &&
              std::abs(M[2][2]-0.125) < eps && std::abs(M[0][1]) < eps,
              "worldInvInertia(identity) == diag");
    }

    // transform: inverse undoes, and point transform == rotate+translate
    {
        AQTransform<double> t;
        t.q = AQquatExp(AQvec3<double>(0.1, 0.2, -0.3)).normalized();
        t.p = AQvec3<double>(1.0, -2.0, 0.5);
        auto pt = AQvec3<double>(0.3, 0.4, 0.5);
        auto round = t.inverse().transformPoint(t.transformPoint(pt));
        check(vdist(round, pt) < 1e-12, "Transform.inverse ∘ Transform == id");
        auto manual = t.p + AQrotate(t.q, pt);
        check(vdist(t.transformPoint(pt), manual) < 1e-12, "transformPoint == rotate+translate");
    }
}

// ----------------------------------------------------------------------------
// Part 2 — dynamics validation, driving the same AQStepBody used in production
// ----------------------------------------------------------------------------

// Drift metrics for a torque-free asymmetric-body run of `secs` at sub-step `dt`.
struct DriftRun { double lDrift, eDrift, qDrift, minWy, maxWy; };

DriftRun runDzhanibekov(double dt, double secs) {
    AQBodyState<double> b;
    // Principal moments I1<I2<I3; spin about the INTERMEDIATE axis (y) with a
    // tiny perturbation so the instability has a seed.
    b.invInertiaBody = AQvec3<double>(1.0 / 1.0, 1.0 / 2.0, 1.0 / 3.0);
    b.angularVelBody  = AQvec3<double>(0.02, 8.0, 0.0);   // mostly intermediate-axis

    const auto zeroG = AQvec3<double>(0.0, 0.0, 0.0);
    const int steps = static_cast<int>(secs / dt);

    const auto L0 = worldL(b);
    const double L0n = vlen(L0);
    const double E0 = rotEnergy(b);

    DriftRun r{0.0, 0.0, 0.0, b.angularVelBody[1][0], b.angularVelBody[1][0]};
    for (int i = 0; i < steps; ++i) {
        AQStepBody(b, zeroG, dt);
        r.lDrift = std::max(r.lDrift, vdist(worldL(b), L0) / L0n);
        r.eDrift = std::max(r.eDrift, std::abs(rotEnergy(b) - E0) / E0);
        r.qDrift = std::max(r.qDrift, std::abs(b.orientation.length() - 1.0));
        const double wy = b.angularVelBody[1][0];
        r.minWy = std::min(r.minWy, wy);
        r.maxWy = std::max(r.maxWy, wy);
    }
    return r;
}

// Torque-free asymmetric body: the Dzhanibekov / tennis-racket flip. The
// body-frame symplectic Lie + implicit-gyroscopic scheme is *stable* (it does
// not blow up the way the explicit gyroscopic term does) and its angular-
// momentum / energy drift is BOUNDED and SMALL at production sub-steps and
// CONVERGES at first order under refinement. (Note: unlike the momentum form,
// this scheme does not conserve ‖L‖ to machine precision — see §11.1 and the
// findings reported with this phase.)
void testDzhanibekov() {
    std::printf("\n== torque-free asymmetric: Dzhanibekov + bounded/convergent drift ==\n");

    const auto r = runDzhanibekov(1.0 / 2000.0, 20.0);
    std::printf("  @dt=1/2000, 20s:  ‖L‖ drift = %.3e   energy drift = %.3e   ‖q‖-1 = %.3e\n",
                r.lDrift, r.eDrift, r.qDrift);
    std::printf("  ω_y range = [%.3f, %.3f]\n", r.minWy, r.maxWy);

    check(r.lDrift < 2e-2, "angular momentum drift bounded & small (< 2% at dt=1/2000)");
    check(r.eDrift < 4e-2, "energy drift bounded & small (< 4% at dt=1/2000)");
    check(r.qDrift < 1e-9, "quaternion stays unit (‖q‖-1 < 1e-9)");
    // The flip: ω_y must reverse sign (swing from ~+8 to ~−8).
    check(r.minWy < -4.0 && r.maxWy > 4.0, "tennis-racket flip: ω_y reverses sign");

    // First-order convergence: refining dt by 4× must cut drift by ~4× (ratio in
    // [3,5]). This is the honest, dt-independent statement of "conserving".
    const auto coarse = runDzhanibekov(1.0 / 2000.0, 10.0);
    const auto fine   = runDzhanibekov(1.0 / 8000.0, 10.0);
    const double lRatio = coarse.lDrift / fine.lDrift;
    const double eRatio = coarse.eDrift / fine.eDrift;
    std::printf("  refinement 4×: ‖L‖ drift ratio = %.2f   energy drift ratio = %.2f (expect ~4)\n",
                lRatio, eRatio);
    check(lRatio > 3.0 && lRatio < 5.0, "‖L‖ drift converges at first order (4× refine ⇒ ~4× less)");
    check(eRatio > 3.0 && eRatio < 5.0, "energy drift converges at first order");
}

// Torque-free symmetric top (I1=I2≠I3): the body-frame transverse angular
// velocity precesses about the symmetry axis at the analytic rate
// Ω = ω₃ (I₃ − I₁) / I₁.
void testSymmetricPrecession() {
    std::printf("\n== torque-free symmetric top: analytic precession rate ==\n");

    const double I1 = 1.0, I3 = 2.5;
    AQBodyState<double> b;
    b.invInertiaBody = AQvec3<double>(1.0 / I1, 1.0 / I1, 1.0 / I3);
    const double w3 = 5.0;
    b.angularVelBody = AQvec3<double>(0.6, 0.0, w3);      // transverse seed on x

    const double Omega = w3 * (I3 - I1) / I1;             // analytic precession rate
    const double dt = 1.0 / 4000.0;

    // Measure the period by tracking sign changes of ω_x (full period = 2 zero
    // crossings of the same sign slope). Run ~3 analytic periods.
    const double Tana = 2.0 * OmegaGTE::Pi<double> / std::abs(Omega);
    const int steps = static_cast<int>(3.0 * Tana / dt);

    double prevWx = b.angularVelBody[0][0];
    double firstUp = -1.0, lastUp = -1.0; int upCount = 0;
    const auto zeroG = AQvec3<double>(0.0, 0.0, 0.0);
    for (int i = 0; i < steps; ++i) {
        AQStepBody(b, zeroG, dt);
        const double wx = b.angularVelBody[0][0];
        if (prevWx < 0.0 && wx >= 0.0) {                  // rising zero crossing
            const double t = (i + 1) * dt;
            if (firstUp < 0.0) firstUp = t; else lastUp = t;
            ++upCount;
        }
        prevWx = wx;
    }
    check(upCount >= 2, "precession produced multiple periods");
    if (upCount >= 2) {
        const double Tmeas = (lastUp - firstUp) / (upCount - 1);
        const double Omeas = 2.0 * OmegaGTE::Pi<double> / Tmeas;
        std::printf("  Ω analytic = %.5f   Ω measured = %.5f   rel err = %.3e\n",
                    std::abs(Omega), Omeas, std::abs(Omeas - std::abs(Omega)) / std::abs(Omega));
        check(std::abs(Omeas - std::abs(Omega)) / std::abs(Omega) < 1e-3,
              "body-frame precession matches analytic rate (<0.1%)");
    }
}

// Same integrator code at float and double; the float path must track the
// double oracle within tolerance over the run (the §6 parity seed).
void testParity() {
    std::printf("\n== float vs double parity (same integrator code) ==\n");

    auto setup = [](auto& b, auto one) {
        using T = decltype(one);
        b.invInertiaBody = AQvec3<T>(T(1.0) / T(1.0), T(1.0) / T(2.0), T(1.0) / T(3.0));
        b.angularVelBody = AQvec3<T>(T(0.02), T(8.0), T(0.0));
    };
    AQBodyState<double> bd; setup(bd, 0.0);
    AQBodyState<float>  bf; setup(bf, 0.0f);

    const auto gd = AQvec3<double>(0.0, 0.0, 0.0);
    const auto gf = AQvec3<float>(0.f, 0.f, 0.f);
    const double dt = 1.0 / 2000.0;
    const int steps = 4000;                               // 2 s

    double maxDiv = 0.0;
    for (int i = 0; i < steps; ++i) {
        AQStepBody(bd, gd, dt);
        AQStepBody(bf, gf, static_cast<float>(dt));
        const double dq =
            std::abs(static_cast<double>(bf.orientation.x) - bd.orientation.x) +
            std::abs(static_cast<double>(bf.orientation.y) - bd.orientation.y) +
            std::abs(static_cast<double>(bf.orientation.z) - bd.orientation.z) +
            std::abs(static_cast<double>(bf.orientation.w) - bd.orientation.w);
        maxDiv = std::max(maxDiv, dq);
    }
    std::printf("  max |q_float - q_double| (L1) over 2 s = %.3e\n", maxDiv);
    // Float drift on a fast asymmetric spinner over 2 s — bounded, not bitwise.
    check(maxDiv < 1e-2, "float tracks double oracle within tolerance");
}

} // namespace

int main() {
    testMath();
    testDzhanibekov();
    testSymmetricPrecession();
    testParity();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
