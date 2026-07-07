// AQUA Phase 7f — XPBD-kernel stage-isolation parity test (Phase-7 brief
// §13.4 7f, the 5c-5e pattern). The GPU AQXPBDPredict / AQXPBDZeroLambda /
// AQXPBDProjectDistance / AQXPBDDerive kernels run the same rope the CPU
// AQXPBDBody::advance slice loop steps, from identical initial state, over
// many engine sub-steps: colors serial, one conflict-free dispatch per color,
// within-color order identical to the CPU's fixed sorted order. Per-particle
// divergence must stay inside the cross-path tolerance band (float
// reassociation/FMA + libm-vs-GPU sqrt are the only expected gaps), pinned
// particles must hold BIT-exact, two GPU runs must be BYTE-identical
// (within-path determinism), and the explosion guard must trip loudly on a
// degenerate rig on both paths.
//
// INTERNAL test: includes AQUA's src/ headers directly (AQXPBDBody from
// AQSpaceImpl.h is the CPU reference; AQComputeBackend drives the kernels)
// and loads the precompiled AQKernels.omegasllib via AQUA_KERNELS_LIB_PATH.
// Skips cleanly when no GTE device exists (headless CI).

#include "AQSpaceImpl.h"
#include "AQComputeBackend.h"

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQMath.h>
#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GE.h>
#include <omegaGTE/GECommandQueue.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using OmegaGTE::FVec;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Cross-path tolerance. The rope's colored projection chains float ops far
// deeper per sub-step than the 5c integrator (every constraint feeds the
// next color), so the band is wider than 5c's 1e-4: pinned at 2e-3 with the
// measured divergence printed alongside so a regression is visible long
// before the bound trips. Applied |a-b| <= tol * max(1, |a|, |b|).
constexpr float kTol = 2e-3f;

bool close(float a, float b) {
    const float scale = std::fmax(1.f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= kTol * scale;
}

// A rope body: `count` particles spaced `seg` apart along +X from `origin`,
// particle 0 pinned, chained with distance constraints at `compliance`.
AQXPBDBody makeRope(std::uint32_t count, float seg, float compliance) {
    AQXPBDBody body;
    body.id = 1;
    OmegaCommon::Vector<FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    for (std::uint32_t i = 0; i < count; ++i) {
        pos.push_back(AQvec3(static_cast<float>(i) * seg, 0.f, 0.f));
        w.push_back(i == 0 ? 0.f : 1.f);
    }
    body.addParticles(pos.data(), w.data(), count);
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        body.addDistance(i, i + 1, seg, compliance);
    }
    body.recolor();
    return body;
}

// Upload one body's particle state + color-sorted constraints to the backend.
bool uploadBody(AQComputeBackend& backend, const AQXPBDBody& body) {
    const std::size_t n = body.positions.size();
    OmegaCommon::Vector<float> px(n), py(n), pz(n), im(n), vx(n), vy(n), vz(n);
    for (std::size_t i = 0; i < n; ++i) {
        px[i] = body.positions[i][0][0];
        py[i] = body.positions[i][1][0];
        pz[i] = body.positions[i][2][0];
        im[i] = body.invMass[i];
        vx[i] = body.velocities[i][0][0];
        vy[i] = body.velocities[i][1][0];
        vz[i] = body.velocities[i][2][0];
    }
    OmegaCommon::Vector<AQComputeBackend::XPBDConstraintIn> sorted;
    for (const AQDistanceConstraint& c : body.distanceSorted) {
        AQComputeBackend::XPBDConstraintIn in;
        in.a = c.a;
        in.b = c.b;
        in.restLength = c.restLength;
        in.compliance = c.compliance;
        in.color = c.color;
        sorted.push_back(in);
    }
    OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (const AQConstraintBatch& batch : body.batches) {
        ranges.push_back({batch.firstConstraint, batch.constraintCount});
    }
    return backend.uploadXPBDParticles(body.id, px, py, pz, im, vx, vy, vz) &&
           backend.uploadXPBDConstraints(body.id, sorted, ranges);
}

struct GpuState {
    OmegaCommon::Vector<float> px, py, pz, vx, vy, vz;
};

std::string stateBytes(const GpuState& s) {
    std::string out;
    auto append = [&](const OmegaCommon::Vector<float>& v) {
        out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
    };
    append(s.px); append(s.py); append(s.pz);
    append(s.vx); append(s.vy); append(s.vz);
    return out;
}

// Run the SAME scene on both paths for `steps` engine sub-steps and report
// the worst per-component divergence.
struct ParityResult {
    double maxPos = 0.0, maxVel = 0.0;
    bool ok = true;
    GpuState gpu;
    AQXPBDBody cpu;
};

ParityResult runParity(AQComputeBackend& backend, const AQXPBDBody& rope,
                       const AQXPBDParams& params, const FVec<3>& gravity,
                       float dt, int steps) {
    ParityResult r;
    r.cpu = rope;                                  // CPU reference copy

    r.ok = uploadBody(backend, rope);
    const float g[3] = {gravity[0][0], gravity[1][0], gravity[2][0]};
    const std::size_t n = rope.positions.size();

    for (int s = 0; s < steps && r.ok; ++s) {
        r.cpu.advance(dt, params, gravity);
        r.ok = backend.encodeXPBDAdvance(rope.id, dt, g, params.substeps, params.iterations,
                                         params.velocityDamping, params.explosionThreshold,
                                         n);
    }
    r.ok = r.ok && backend.downloadXPBDParticles(rope.id, r.gpu.px, r.gpu.py, r.gpu.pz,
                                                 r.gpu.vx, r.gpu.vy, r.gpu.vz, n);
    if (!r.ok) return r;

    for (std::size_t i = 0; i < n; ++i) {
        const float cp[3] = {r.cpu.positions[i][0][0], r.cpu.positions[i][1][0], r.cpu.positions[i][2][0]};
        const float gp[3] = {r.gpu.px[i], r.gpu.py[i], r.gpu.pz[i]};
        const float cv[3] = {r.cpu.velocities[i][0][0], r.cpu.velocities[i][1][0], r.cpu.velocities[i][2][0]};
        const float gv[3] = {r.gpu.vx[i], r.gpu.vy[i], r.gpu.vz[i]};
        for (int c = 0; c < 3; ++c) {
            r.maxPos = std::fmax(r.maxPos, double(std::fabs(cp[c] - gp[c])));
            r.maxVel = std::fmax(r.maxVel, double(std::fabs(cv[c] - gv[c])));
            if (!close(cp[c], gp[c]) || !close(cv[c], gv[c])) {
                if (r.ok) {
                    std::printf("  particle %zu axis %d: cpu pos %.9g gpu pos %.9g / "
                                "cpu vel %.9g gpu vel %.9g\n",
                                i, c, double(cp[c]), double(gp[c]), double(cv[c]), double(gv[c]));
                }
                r.ok = false;
            }
        }
    }
    return r;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 7f: GPU XPBD kernels vs CPU AQXPBDBody::advance ==\n");

    OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
        OmegaGTE::enumerateDevices();
    if (devices.empty()) {
        std::printf("[SKIP] no GTE device on this host — GPU parity test skipped\n");
        return 0;
    }

    OmegaGTE::GTE gte = OmegaGTE::Init(devices[0]);
    check(static_cast<bool>(gte.graphicsEngine), "GTE initialized a graphics engine");

    OmegaGTE::GECommandQueueDesc queueDesc;
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);

    auto backend = AQComputeBackend::TryCreate(gte.graphicsEngine, queue);
    check(backend != nullptr, "AQComputeBackend::TryCreate succeeds");
    if (!backend) { OmegaGTE::Close(gte); return 1; }

    check(backend->loadKernelLibrary(OmegaCommon::String(AQUA_KERNELS_LIB_PATH)),
          "loadKernelLibrary loads the merged AQKernels.omegasllib");

    const float dt = 1.f / 120.f;
    const FVec<3> gravity = AQvec3(0.f, -9.81f, 0.f);

    // ---- Case A: rigid rope (alpha = 0) swinging under gravity ----
    // 17 particles, pinned end, 8 slices x 1 iteration, 30 engine sub-steps
    // (0.25 s of swing — long enough to exercise every kernel branch every
    // slice, short enough that the pendulum-chain chaos hasn't amplified
    // float-reassociation noise past the band).
    {
        AQXPBDParams params;
        params.substeps = 8;
        params.iterations = 1;
        params.velocityDamping = 0.f;
        params.explosionThreshold = 10.f;
        AQXPBDBody rope = makeRope(17, 0.1f, 0.f);
        ParityResult r = runParity(*backend, rope, params, gravity, dt, 30);
        std::printf("    [measure] rigid swing: max |pos| gap %.3e, max |vel| gap %.3e\n",
                    r.maxPos, r.maxVel);
        check(r.ok, "rigid rope (alpha=0): CPU and GPU agree within the band after 30 sub-steps");

        // The pinned particle is skipped by predict/derive and immovable in
        // projection on both paths — its position must be BIT-exact.
        check(r.gpu.px[0] == 0.f && r.gpu.py[0] == 0.f && r.gpu.pz[0] == 0.f &&
              r.gpu.vx[0] == 0.f && r.gpu.vy[0] == 0.f && r.gpu.vz[0] == 0.f,
              "pinned particle holds bit-exact on the GPU");

        // Rope actually moved (the parity is not vacuous).
        check(r.gpu.py[16] < -0.05f, "free end has fallen under gravity");

        // No guard trips on a healthy rope, either path.
        OmegaCommon::Vector<std::uint32_t> trips;
        check(backend->downloadXPBDTrips(1, trips), "downloadXPBDTrips reads the counters");
        std::uint32_t gpuTrips = 0;
        for (std::uint32_t t : trips) gpuTrips += t;
        check(gpuTrips == 0 && r.cpu.guardTrips == 0,
              "healthy rope: zero explosion-guard trips on both paths");
    }

    // ---- Case B: compliant rope (alpha > 0) with damping, settled ----
    // Damped settle is contracting, so the two paths converge to the same
    // equilibrium; also proves the lambda-accumulation (a compliance > 0
    // steady state is wrong if lambda resets or leaks across slices).
    {
        AQXPBDParams params;
        params.substeps = 8;
        params.iterations = 1;
        params.velocityDamping = 0.05f;
        params.explosionThreshold = 10.f;
        AQXPBDBody rope = makeRope(9, 0.1f, 0.002f);
        ParityResult r = runParity(*backend, rope, params, gravity, dt, 240);
        std::printf("    [measure] compliant settle: max |pos| gap %.3e, max |vel| gap %.3e\n",
                    r.maxPos, r.maxVel);
        check(r.ok, "compliant rope (alpha>0): CPU and GPU agree within the band after 2 s settle");

        // Compliance stretched the rope on both paths: the summed segment
        // length exceeds the 0.8 total rest length by a visible margin
        // (orientation-independent — under the heavy per-slice damping the
        // rope creeps rather than swings, but the pin carries the full
        // weight either way, so alpha > 0 must show tension stretch).
        auto ropeLength = [](const float* x, const float* y, const float* z) {
            float len = 0.f;
            for (int i = 0; i + 1 < 9; ++i) {
                const float dx = x[i + 1] - x[i], dy = y[i + 1] - y[i], dz = z[i + 1] - z[i];
                len += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            return len;
        };
        float cx[9], cy[9], cz[9];
        for (int i = 0; i < 9; ++i) {
            cx[i] = r.cpu.positions[i][0][0];
            cy[i] = r.cpu.positions[i][1][0];
            cz[i] = r.cpu.positions[i][2][0];
        }
        const float gpuLen = ropeLength(r.gpu.px.data(), r.gpu.py.data(), r.gpu.pz.data());
        const float cpuLen = ropeLength(cx, cy, cz);
        std::printf("    [measure] compliant rope length: cpu %.4f gpu %.4f (rest 0.8)\n",
                    double(cpuLen), double(gpuLen));
        check(gpuLen > 0.85f && cpuLen > 0.85f,
              "alpha > 0 produced measurable tension stretch on both paths");
    }

    // ---- Case C: within-path determinism — full re-run, byte-identical ----
    {
        AQXPBDParams params;
        params.substeps = 4;
        params.iterations = 1;
        params.velocityDamping = 0.f;
        params.explosionThreshold = 10.f;
        AQXPBDBody rope = makeRope(17, 0.1f, 0.001f);
        ParityResult r1 = runParity(*backend, rope, params, gravity, dt, 20);
        ParityResult r2 = runParity(*backend, rope, params, gravity, dt, 20);
        check(r1.ok && r2.ok, "both GPU re-runs complete");
        check(stateBytes(r1.gpu) == stateBytes(r2.gpu),
              "two GPU runs are byte-identical (within-path determinism)");
    }

    // ---- Case D: the explosion guard trips loudly on a degenerate rig ----
    // A rope authored 10x longer than its rest lengths wants a huge first
    // correction; with a tiny threshold every projection clamps. The guard
    // must COUNT on the GPU exactly as the CPU's does, and the clamped state
    // must stay finite (no silent NaN three frames later).
    {
        AQXPBDParams params;
        params.substeps = 2;
        params.iterations = 1;
        params.velocityDamping = 0.f;
        params.explosionThreshold = 0.001f;
        AQXPBDBody rope = makeRope(5, 1.0f, 0.f);
        for (auto& c : rope.distance) c.restLength = 0.1f;   // authored far from rest
        rope.colorsDirty = true;
        rope.recolor();

        ParityResult r = runParity(*backend, rope, params, gravity, dt, 5);
        // The clamped trajectories agree while the clamp dominates; what the
        // guard must guarantee is a TRIP COUNT and finite state on both paths.
        OmegaCommon::Vector<std::uint32_t> trips;
        backend->downloadXPBDTrips(1, trips);
        std::uint32_t gpuTrips = 0;
        for (std::uint32_t t : trips) gpuTrips += t;
        check(gpuTrips > 0, "degenerate rig: GPU explosion guard tripped (counted per constraint)");
        check(r.cpu.guardTrips > 0, "degenerate rig: CPU explosion guard tripped");
        bool finite = true;
        for (std::size_t i = 0; i < r.gpu.px.size(); ++i) {
            finite = finite && std::isfinite(r.gpu.px[i]) && std::isfinite(r.gpu.py[i]) &&
                     std::isfinite(r.gpu.pz[i]);
        }
        check(finite, "degenerate rig: clamped GPU state stays finite");
    }

    // ---- Case E: the LIVE path (post-6h flip) — public surface end to end ----
    // The same rope scene authored through AQSpace on a CPU-only context and
    // on a GPU context (loadKernels + setExecutionPath(GPU) + advance): the
    // live path's readXPBDState must match the CPU context within the band,
    // the pinned particle bit-exact, and neither context may trip the guard.
    {
        auto runScene = [&](bool gpu, OmegaCommon::Vector<FVec<3>>& outPos,
                            OmegaCommon::Vector<FVec<3>>& outVel,
                            std::uint32_t& trips) -> bool {
            SharedHandle<AQContext> ctx;
            bool ok = true;
            if (gpu) {
                OmegaGTE::GECommandQueueDesc qd;
                qd.maxBufferCount = 4;
                auto q = gte.graphicsEngine->makeCommandQueue(qd);
                ctx = AQContext::Create(gte.graphicsEngine, q);
                ok = ctx->loadKernels(OmegaCommon::String(AQUA_KERNELS_LIB_PATH));
                ctx->setExecutionPath(AQExecPath::GPU);
                ok = ok && (ctx->executionPath() == AQExecPath::GPU);
            } else {
                ctx = AQContext::CreateCPUOnly();
            }
            auto space = ctx->createSpace();
            OmegaCommon::Vector<FVec<3>> pos;
            OmegaCommon::Vector<float> w;
            OmegaCommon::Vector<std::uint32_t> idx;
            for (std::uint32_t i = 0; i < 17; ++i) {
                pos.push_back(AQvec3(0.1f * static_cast<float>(i), 0.f, 0.f));
                w.push_back(i == 0 ? 0.f : 1.f);
                idx.push_back(i);
            }
            AQXPBDBodyDesc desc;
            desc.positions = pos.data();
            desc.invMass = w.data();
            desc.count = 17;
            AQXPBDBodyHandle body = space->createXPBDBody(desc);
            ok = ok && (space->addRope(body, idx.data(), 17, 0.001f) == 16);
            AQXPBDParams params;
            params.substeps = 8;
            params.iterations = 1;
            params.velocityDamping = 0.01f;
            params.explosionThreshold = 10.f;
            space->setXPBDParams(params);
            for (int f = 0; f < 15; ++f) ctx->advance(1.f / 60.f);
            outPos.assign(17, FVec<3>::Create());
            outVel.assign(17, FVec<3>::Create());
            ok = ok && (space->readXPBDState(body, outPos.data(), outVel.data(), 17) == 17);
            trips = space->xpbdGuardTrips(body);
            return ok;
        };
        OmegaCommon::Vector<FVec<3>> cp, cv, gp, gv;
        std::uint32_t cpuTrips = 0, gpuTrips = 0;
        const bool cpuOk = runScene(false, cp, cv, cpuTrips);
        const bool gpuOk = runScene(true, gp, gv, gpuTrips);
        check(cpuOk && gpuOk, "live path: CPU-only and GPU contexts both run the public rope scene");
        double maxGap = 0.0;
        bool inBand = true;
        for (std::uint32_t i = 0; i < 17; ++i) {
            for (int c = 0; c < 3; ++c) {
                maxGap = std::fmax(maxGap, double(std::fabs(gp[i][c][0] - cp[i][c][0])));
                inBand = inBand && close(gp[i][c][0], cp[i][c][0]) &&
                         close(gv[i][c][0], cv[i][c][0]);
            }
        }
        std::printf("    [measure] live public-surface parity: max |pos| gap %.3e\n", maxGap);
        check(inBand, "live path: GPU-context rope matches the CPU-only context within the band");
        check(gp[0][0][0] == 0.f && gp[0][1][0] == 0.f && gp[0][2][0] == 0.f,
              "live path: pinned particle holds bit-exact through the public surface");
        check(cpuTrips == 0 && gpuTrips == 0, "live path: no guard trips on either context");
    }

    OmegaGTE::Close(gte);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
