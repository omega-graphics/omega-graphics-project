// AQUA Phase 5c — integration-kernel stage-isolation parity test (plan §9,
// §13 5c). The GPU AQIntegrateVelocity/AQIntegratePosition kernels run the
// same bodies the CPU AQStepBodyVelocity/AQStepBodyPosition templates step,
// from identical initial state, over many sub-steps; the per-field divergence
// must stay inside the cross-path tolerance band (plan §8 — float
// reassociation/FMA + libm-vs-GPU trig are the only expected gaps), and two
// GPU runs must be BYTE-IDENTICAL (within-path determinism, deliverable #4).
//
// INTERNAL test: includes AQUA's src/ headers directly (like
// aqua_compute_test) and loads the precompiled AQKernels.omegasllib whose
// build path arrives as AQUA_KERNELS_LIB_PATH. Skips cleanly when no GTE
// device exists (headless CI).

#include "AQBodySoA.h"
#include "AQComputeBackend.h"

#include <aqua/AQIntegrator.h>
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

// Cross-path tolerance (plan §8): the 1e-4-class band Phase 1's float/double
// oracle uses. Applied as |a-b| <= tol * max(1, |a|, |b|) per component so it
// is absolute near zero and relative for large magnitudes. The spinning-body
// case runs the 4-iteration Newton solve + GPU trig for 120 sub-steps, which
// is the worst accumulation in this harness.
constexpr float kTol = 1e-4f;

bool close(float a, float b) {
    const float scale = std::fmax(1.f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= kTol * scale;
}

// The CPU reference for one sub-step of the 5c kernel pair: the two
// AQIntegrator.h half-steps plus the split-impulse position apply from
// AQSpace::stepInternal (pos += pseudoLinear * dt for integrated bodies).
void cpuSubstep(AQBodyState<float>& s, const AQVec3<float>& pseudoLinear,
                const AQVec3<float>& gravity, float dt) {
    AQStepBodyVelocity(s, gravity, dt);
    const bool integrated = (s.invMass != 0.f) &&
                            (s.activation != AQActivationState::Sleeping);
    AQStepBodyPosition(s, dt);
    if (integrated) {
        s.position += pseudoLinear * dt;
    }
}

AQBodyState<float> makeBody(const FVec<3>& pos, const FVec<3>& vel,
                            const FVec<3>& wBody, float invMass,
                            const FVec<3>& invInertia) {
    AQBodyState<float> s;
    s.position = pos;
    s.velocity = vel;
    s.angularVelBody = wBody;
    s.invMass = invMass;
    s.invInertiaBody = invInertia;
    return s;
}

bool statesClose(const AQBodyState<float>& cpu, const AQBodyState<float>& gpu,
                 const char* what, std::size_t bodyIdx) {
    struct Field { const char* name; float c, g; };
    const Field fields[] = {
        {"pos.x", cpu.position[0][0], gpu.position[0][0]},
        {"pos.y", cpu.position[1][0], gpu.position[1][0]},
        {"pos.z", cpu.position[2][0], gpu.position[2][0]},
        {"vel.x", cpu.velocity[0][0], gpu.velocity[0][0]},
        {"vel.y", cpu.velocity[1][0], gpu.velocity[1][0]},
        {"vel.z", cpu.velocity[2][0], gpu.velocity[2][0]},
        {"quat.x", cpu.orientation.x, gpu.orientation.x},
        {"quat.y", cpu.orientation.y, gpu.orientation.y},
        {"quat.z", cpu.orientation.z, gpu.orientation.z},
        {"quat.w", cpu.orientation.w, gpu.orientation.w},
        {"wb.x", cpu.angularVelBody[0][0], gpu.angularVelBody[0][0]},
        {"wb.y", cpu.angularVelBody[1][0], gpu.angularVelBody[1][0]},
        {"wb.z", cpu.angularVelBody[2][0], gpu.angularVelBody[2][0]},
        {"force.x", cpu.forceAccum[0][0], gpu.forceAccum[0][0]},
        {"torque.x", cpu.torqueAccum[0][0], gpu.torqueAccum[0][0]},
    };
    for (const Field& f : fields) {
        if (!close(f.c, f.g)) {
            std::printf("  body %zu %s field %s: cpu=%.9g gpu=%.9g (|d|=%.3g)\n",
                        bodyIdx, what, f.name, double(f.c), double(f.g),
                        double(std::fabs(f.c - f.g)));
            return false;
        }
    }
    return true;
}

// Serialize the GPU-owned dynamics fields of a SoA for the byte-identical
// re-run comparison.
std::string soaBytes(const AQBodySoA& soa) {
    std::string out;
    auto append = [&](const OmegaCommon::Vector<float>& v) {
        out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
    };
    append(soa.posX); append(soa.posY); append(soa.posZ);
    append(soa.velX); append(soa.velY); append(soa.velZ);
    append(soa.quatX); append(soa.quatY); append(soa.quatZ); append(soa.quatW);
    append(soa.wbX); append(soa.wbY); append(soa.wbZ);
    append(soa.forceX); append(soa.forceY); append(soa.forceZ);
    append(soa.torqueX); append(soa.torqueY); append(soa.torqueZ);
    return out;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 5c: GPU integration kernels vs CPU integrator ==\n");

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

    // ---- Scene: eight bodies covering every kernel branch ----
    const float dt = 1.f / 120.f;
    const unsigned kSubsteps = 120;   // one simulated second
    const FVec<3> gravity = AQvec3(0.f, -9.81f, 0.f);

    const std::size_t N = 8;
    AQBodyState<float> bodies[N];
    FVec<3> pseudo[N] = {
        AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f),
        AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f),
        AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f)};

    // 0: pure free-fall (no rotation).
    bodies[0] = makeBody(AQvec3(0.f, 10.f, 0.f), AQvec3(1.f, 0.f, 0.f),
                         AQvec3(0.f, 0.f, 0.f), 1.f, AQvec3(6.f, 6.f, 6.f));
    // 1: fast asymmetric spinner — 4-iteration Newton path + trig-heavy
    //    orientation updates (the Phase 1 tumbling-body configuration).
    bodies[1] = makeBody(AQvec3(0.f, 0.f, 0.f), AQvec3(0.f, 0.f, 0.f),
                         AQvec3(0.5f, 8.f, 0.3f), 2.f,
                         AQvec3(1.f / 0.4f, 1.f / 0.7f, 1.f / 1.1f));
    // 2: slow spinner — single-iteration Newton path.
    bodies[2] = makeBody(AQvec3(1.f, 2.f, 3.f), AQvec3(0.2f, 0.f, -0.1f),
                         AQvec3(0.05f, 0.02f, -0.04f), 1.f,
                         AQvec3(2.f, 3.f, 4.f));
    // 3: damped, linear + angular.
    bodies[3] = makeBody(AQvec3(-2.f, 5.f, 1.f), AQvec3(3.f, 4.f, -1.f),
                         AQvec3(1.f, -2.f, 0.5f), 0.5f, AQvec3(1.f, 1.f, 1.f));
    bodies[3].linearDamping = 0.8f;
    bodies[3].angularDamping = 1.2f;
    // 4: max-angular-speed clamp engaged.
    bodies[4] = makeBody(AQvec3(0.f, 0.f, -4.f), AQvec3(0.f, 0.f, 0.f),
                         AQvec3(0.f, 10.f, 0.f), 1.f, AQvec3(5.f, 5.f, 5.f));
    bodies[4].maxAngularSpeed = 2.f;
    // 5: sleeping — must come back bit-exact (kernels skip it).
    bodies[5] = makeBody(AQvec3(7.f, 8.f, 9.f), AQvec3(1.f, 1.f, 1.f),
                         AQvec3(0.5f, 0.5f, 0.5f), 1.f, AQvec3(1.f, 1.f, 1.f));
    bodies[5].activation = AQActivationState::Sleeping;
    // 6: static (invMass 0) with stray accumulated force — pose must hold and
    //    the accumulators must clear (Phase 1 semantics).
    bodies[6] = makeBody(AQvec3(0.f, -1.f, 0.f), AQvec3(0.f, 0.f, 0.f),
                         AQvec3(0.f, 0.f, 0.f), 0.f, AQvec3(0.f, 0.f, 0.f));
    bodies[6].forceAccum = AQvec3(100.f, 0.f, 0.f);
    bodies[6].torqueAccum = AQvec3(0.f, 100.f, 0.f);
    // 7: gravity-scaled + forces + torque + nonzero split-impulse pseudo shift.
    bodies[7] = makeBody(AQvec3(4.f, 0.f, 2.f), AQvec3(0.f, 1.f, 0.f),
                         AQvec3(0.2f, 0.1f, 0.f), 1.f, AQvec3(2.f, 2.f, 2.f));
    bodies[7].gravityScale = 0.5f;
    bodies[7].forceAccum = AQvec3(0.f, 3.f, 1.f);
    bodies[7].torqueAccum = AQvec3(0.5f, 0.f, -0.2f);
    pseudo[7] = AQvec3(0.05f, 0.1f, -0.02f);

    // NOTE on accumulators over multiple sub-steps: the position half-step
    // clears force/torque each sub-step (both paths), so bodies 6/7 exercise
    // the accumulator path on the FIRST sub-step and the cleared path after.

    // ---- CPU reference ----
    AQBodyState<float> cpu[N];
    for (std::size_t i = 0; i < N; ++i) cpu[i] = bodies[i];
    for (unsigned s = 0; s < kSubsteps; ++s) {
        for (std::size_t i = 0; i < N; ++i) {
            cpuSubstep(cpu[i], pseudo[i], gravity, dt);
        }
    }

    // ---- GPU run ----
    AQBodySoA soa;
    soa.gatherFrom(bodies, N);
    for (std::size_t i = 0; i < N; ++i) {
        soa.pseudoLinX[i] = pseudo[i][0][0];
        soa.pseudoLinY[i] = pseudo[i][1][0];
        soa.pseudoLinZ[i] = pseudo[i][2][0];
    }
    check(backend->uploadBodies(soa), "uploadBodies packs the SoA into the pooled buffers");
    const float g[3] = {gravity[0][0], gravity[1][0], gravity[2][0]};
    check(backend->encodeIntegrate(dt, g, N, kSubsteps),
          "encodeIntegrate dispatches 120 sub-steps (velocity+position per step)");
    AQBodySoA gpuOut;
    gpuOut.resize(N);
    check(backend->downloadBodies(gpuOut), "downloadBodies reads the stepped state back");

    AQBodyState<float> gpu[N];
    for (std::size_t i = 0; i < N; ++i) gpu[i] = bodies[i];   // keep host-only fields
    gpuOut.scatterTo(gpu, N);

    // ---- Parity: every body, every field, inside the band ----
    bool allClose = true;
    for (std::size_t i = 0; i < N; ++i) {
        allClose = statesClose(cpu[i], gpu[i], "after 120 substeps", i) && allClose;
    }
    check(allClose, "CPU vs GPU state agrees within the 1e-4 band after 1 s");

    // Sleeping body must be BIT-exact (never touched by either path).
    check(gpu[5].position[0][0] == bodies[5].position[0][0] &&
          gpu[5].velocity[0][0] == bodies[5].velocity[0][0] &&
          gpu[5].orientation.w == bodies[5].orientation.w,
          "sleeping body state is untouched (bit-exact)");
    // Static body: pose held, accumulators consumed.
    check(gpu[6].position[1][0] == bodies[6].position[1][0] &&
          gpu[6].forceAccum[0][0] == 0.f && gpu[6].torqueAccum[1][0] == 0.f,
          "static body pose holds and accumulators clear");
    // Clamp actually engaged: ||w|| of body 4 is at the cap.
    {
        const auto& w = gpu[4].angularVelBody;
        const float wn = std::sqrt(w[0][0]*w[0][0] + w[1][0]*w[1][0] + w[2][0]*w[2][0]);
        check(std::fabs(wn - 2.f) < 1e-3f, "max-angular-speed clamp holds ||w|| at the cap");
    }
    // The pseudo shift actually moved body 7 (vs a no-pseudo CPU run).
    {
        AQBodyState<float> noPseudo = bodies[7];
        for (unsigned s = 0; s < kSubsteps; ++s) {
            cpuSubstep(noPseudo, AQvec3(0.f, 0.f, 0.f), gravity, dt);
        }
        const float expectedShift = 0.05f * dt * float(kSubsteps);
        const float actualShift = gpu[7].position[0][0] - noPseudo.position[0][0];
        check(std::fabs(actualShift - expectedShift) < 1e-3f,
              "split-impulse pseudoLinear shifts position by pseudo*dt per sub-step");
    }

    // ---- Within-path determinism: repeated re-runs byte-identical (deliverable #4) ----
    // Several BACK-TO-BACK re-runs from identical initial state must all match
    // the first GPU run byte-for-byte. More than one re-run is deliberate: the
    // GED3D12 commit-fence race this test caught let the FIRST sync per queue
    // read correct results while later syncs returned before the GPU finished,
    // so a single re-run could pass by luck. Multiple consecutive syncs are
    // what expose a "first call happens to pass" regression.
    {
        const std::string reference = soaBytes(gpuOut);
        constexpr int kReRuns = 3;
        bool rerunOk = true;
        bool allIdentical = true;
        for (int r = 0; r < kReRuns; ++r) {
            AQBodySoA soaR;
            soaR.gatherFrom(bodies, N);
            for (std::size_t i = 0; i < N; ++i) {
                soaR.pseudoLinX[i] = pseudo[i][0][0];
                soaR.pseudoLinY[i] = pseudo[i][1][0];
                soaR.pseudoLinZ[i] = pseudo[i][2][0];
            }
            bool ok = backend->uploadBodies(soaR) &&
                      backend->encodeIntegrate(dt, g, N, kSubsteps);
            AQBodySoA outR;
            outR.resize(N);
            ok = ok && backend->downloadBodies(outR);
            rerunOk = rerunOk && ok;
            allIdentical = allIdentical && (soaBytes(outR) == reference);
        }
        check(rerunOk, "GPU re-runs from identical initial state complete");
        check(allIdentical,
              "repeated GPU runs are byte-identical (within-path determinism)");
    }

    OmegaGTE::Close(gte);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
