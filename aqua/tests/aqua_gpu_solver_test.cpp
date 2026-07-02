// AQUA Phase 5f — colored-solver stage parity test (plan §9, §13 5f). One
// full sub-step of the ported pipeline runs entirely through the backend
// stage entry points — velocity half-step, broadphase, narrowphase, CPU
// greedy coloring, warm-start + colored PGS velocity sweeps, colored
// split-impulse position solve, position half-step — on a penetrating box
// stack over a ground plane, and the post-sub-step body state must match one
// CPU `advance` within the cross-path band (plan §8: the colored traversal
// order and float reassociation are the only expected gaps). The per-contact
// accumulated impulses are compared against the CPU manifolds' post-solve
// accumulators within the measured-relaxed impulse band, and the divergences
// are PRINTED so the pinned constants stay honest.
//
// Solver iteration counts mirror the space defaults (48 velocity / 16
// position, AQSpace.cpp). Within-path determinism: the full GPU sub-step is
// re-run from the same initial state and must reproduce identical bytes.

#include "AQBodySoA.h"
#include "AQComputeBackend.h"

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQCollision.h>
#include <aqua/AQContact.h>
#include <aqua/AQIntegrator.h>
#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GE.h>
#include <omegaGTE/GECommandQueue.h>

#include <cmath>
#include <cstdio>
#include <string>

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Cross-path bands (plan §8), pinned from the divergence MEASURED on this
// stack at 48/16 iterations (Metal host, 2026-07-01): position 1.7e-4,
// velocity 3.1e-4, impulses 2.9e-4 relative. The colored traversal reorders
// the sweep and GPU codegen contracts FMAs, so the runs converge to the same
// fixed point along different float paths; the bands hold the measurements
// with ~3-17x headroom and are regression constants, not guesses.
constexpr float kStateTol = 1e-3f;
constexpr float kImpulseTol = 5e-3f;

float relDiff(float a, float b) {
    const float scale = std::fmax(1.f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) / scale;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 5f: GPU colored solve vs CPU PGS (one sub-step) ==\n");

    OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
        OmegaGTE::enumerateDevices();
    if (devices.empty()) {
        std::printf("[SKIP] no GTE device on this host — GPU parity test skipped\n");
        return 0;
    }

    // ---- Scene: ground plane + a 5-box stack with ~8 mm interpenetration
    // (past the 5 mm slop, so the split-impulse position solve is active). ----
    const std::size_t N = 6;
    const float dt = 1.f / 120.f;
    const int kVelIters = 48;
    const int kPosIters = 16;
    const float boxMass = 2.f;
    const float he = 0.5f;

    OmegaCommon::Vector<FVec<3>> positions;
    positions.push_back(AQvec3(0.f, 0.f, 0.f));           // plane
    positions.push_back(AQvec3(0.f, 0.492f, 0.f));
    positions.push_back(AQvec3(0.f, 1.482f, 0.f));
    positions.push_back(AQvec3(0.f, 2.472f, 0.f));
    positions.push_back(AQvec3(0.f, 3.462f, 0.f));
    positions.push_back(AQvec3(0.f, 4.452f, 0.f));

    // ---- CPU reference: one advance == one sub-step ----
    OmegaCommon::Vector<FVec<3>> cpuPos, cpuVel;
    OmegaCommon::Vector<AQContactManifold> cpuManifolds;
    OmegaCommon::Vector<std::uint32_t> cpuPairs;
    {
        auto ctx = AQContext::CreateCPUOnly();
        auto space = ctx->createSpace();
        OmegaCommon::Vector<SharedHandle<AQRigidBody>> handles;
        for (std::size_t i = 0; i < N; ++i) {
            AQBodyDesc desc;
            if (i == 0) {
                desc.type = AQBodyType::Static;
                desc.shape = space->createPlaneShape(AQvec3(0.f, 1.f, 0.f), 0.f);
            } else {
                desc.type = AQBodyType::Dynamic;
                desc.mass = boxMass;
                desc.shape = space->createBoxShape(AQvec3(he, he, he));
            }
            desc.position = positions[i];
            desc.friction = 0.5f;
            desc.restitution = 0.f;
            handles.push_back(space->addBody(desc));
        }
        ctx->advance(dt);
        for (std::size_t i = 0; i < N; ++i) {
            cpuPos.push_back(handles[i]->position());
            cpuVel.push_back(handles[i]->velocity());
        }
        cpuManifolds = space->contactManifolds();
        auto pairs = space->candidatePairs();
        for (const auto& p : pairs) {
            cpuPairs.push_back(p.a);
            cpuPairs.push_back(p.b);
        }
        std::printf("  CPU: %zu manifolds over %zu pairs\n",
                    cpuManifolds.size(), cpuPairs.size() / 2);
    }

    // ---- GPU pipeline ----
    OmegaGTE::GTE gte = OmegaGTE::Init(devices[0]);
    OmegaGTE::GECommandQueueDesc queueDesc;
    queueDesc.maxBufferCount = 8;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto backend = AQComputeBackend::TryCreate(gte.graphicsEngine, queue);
    check(backend != nullptr, "AQComputeBackend::TryCreate succeeds");
    if (!backend) { OmegaGTE::Close(gte); return 1; }
    check(backend->loadKernelLibrary(OmegaCommon::String(AQUA_KERNELS_LIB_PATH)),
          "loadKernelLibrary loads the merged AQKernels.omegasllib");

    // Body states mirroring addBody's derivation (box inertia from the shape).
    OmegaCommon::Vector<AQBodyState<float>> states(N);
    for (std::size_t i = 0; i < N; ++i) {
        states[i].position = positions[i];
        if (i > 0) {
            states[i].invMass = 1.f / boxMass;
            const FVec<3> moments = AQinertiaSolidBox(boxMass, he, he, he);
            states[i].invInertiaBody = AQvec3(1.f / moments[0][0],
                                              1.f / moments[1][0],
                                              1.f / moments[2][0]);
        } else {
            states[i].invMass = 0.f;
            states[i].invInertiaBody = AQvec3(0.f, 0.f, 0.f);
        }
    }

    auto runGpuSubstep = [&](OmegaCommon::Vector<AQBodyState<float>>& outStates,
                             OmegaCommon::Vector<AQComputeBackend::RowOut>& outRows,
                             OmegaCommon::Vector<AQComputeBackend::ManifoldOut>& outManifolds,
                             std::size_t& outPairCount) -> bool {
        AQBodySoA soa;
        soa.gatherFrom(states.data(), N);
        if (!backend->uploadBodies(soa)) { return false; }

        AQComputeBackend::BroadphaseInputs in;
        in.shapeType.resize(N); in.hullFirst.assign(N, 0u); in.hullCount.assign(N, 0u);
        in.hasShape.assign(N, 1u);
        in.paramX.resize(N); in.paramY.resize(N); in.paramZ.resize(N); in.paramW.assign(N, 0.f);
        in.localPosX.assign(N, 0.f); in.localPosY.assign(N, 0.f); in.localPosZ.assign(N, 0.f);
        in.localQuatX.assign(N, 0.f); in.localQuatY.assign(N, 0.f); in.localQuatZ.assign(N, 0.f);
        in.localQuatW.assign(N, 1.f);
        in.filterLayer.assign(N, 1u); in.filterMask.assign(N, ~0u);
        for (std::size_t i = 0; i < N; ++i) {
            if (i == 0) {
                in.shapeType[i] = 3;    // plane
                in.paramX[i] = 0.f; in.paramY[i] = 1.f; in.paramZ[i] = 0.f;
            } else {
                in.shapeType[i] = 1;    // box
                in.paramX[i] = he; in.paramY[i] = he; in.paramZ[i] = he;
            }
        }
        if (!backend->uploadBroadphaseInputs(in)) { return false; }

        // Advance-level broadphase (pre-kick poses/velocities, CPU cadence).
        if (!backend->encodeRefreshAABB(N, 0.02f, dt)) { return false; }
        OmegaCommon::Vector<float> fatMin, fatMax;
        if (!backend->downloadFatAABBs(fatMin, fatMax, N)) { return false; }
        float cellSize = 1.f;
        for (std::size_t i = 1; i < N; ++i) {
            for (int c = 0; c < 3; ++c) {
                cellSize = std::fmax(cellSize, fatMax[i * 3 + c] - fatMin[i * 3 + c]);
            }
        }
        if (!backend->encodeBroadphase(N, cellSize, 32)) { return false; }
        OmegaCommon::Vector<std::uint32_t> gpuPairs;
        if (!backend->downloadPairs(gpuPairs)) { return false; }
        outPairCount = gpuPairs.size() / 2;

        // Sub-step: velocity half-step FIRST (the narrowphase's restitution
        // bias reads post-kick velocities, like the CPU).
        const float g[3] = {0.f, -9.81f, 0.f};
        if (!backend->encodeVelocityHalfStep(dt, g, N)) { return false; }

        OmegaCommon::Vector<float> rest(N, 0.f), fric(N, 0.5f);
        OmegaCommon::Vector<std::uint32_t> trig(N, 0u);
        if (!backend->uploadNarrowphaseInputs(rest, fric, trig)) { return false; }
        if (!backend->encodeNarrowphase(outPairCount, dt, 0u, 0u, 0)) { return false; }

        OmegaCommon::Vector<std::uint32_t> fallback;
        if (!backend->downloadNarrowphase(outManifolds, outRows, fallback, outPairCount)) {
            return false;
        }

        // CPU-side greedy coloring over the manifold groups (plan §11.3).
        OmegaCommon::Vector<AQComputeBackend::SolveGroupIn> groups;
        std::uint32_t rowCursor = 0;
        for (std::size_t mi = 0; mi < outManifolds.size(); ++mi) {
            const auto& m = outManifolds[mi];
            AQComputeBackend::SolveGroupIn grp;
            grp.firstRow = rowCursor;
            grp.rowCount = m.pointCount * 3;
            grp.manifoldIndex = static_cast<std::uint32_t>(mi);
            grp.bodyA = m.a;
            grp.bodyB = m.b;
            grp.finiteA = states[m.a].invMass > 0.f;
            grp.finiteB = states[m.b].invMass > 0.f;
            groups.push_back(grp);
            rowCursor += grp.rowCount;
        }
        if (!backend->setSolveGroups(groups)) { return false; }

        // Warm start + colored sweeps + split-impulse position solve, then
        // the position half-step (which consumes pseudoLinear).
        if (!backend->encodeSolve(dt, kVelIters, kPosIters, true, N)) { return false; }
        if (!backend->encodePositionHalfStep(dt, g, N)) { return false; }

        // Post-solve rows (accumulated impulses) + post-step state.
        OmegaCommon::Vector<std::uint32_t> fb2;
        if (!backend->downloadNarrowphase(outManifolds, outRows, fb2, outPairCount)) {
            return false;
        }
        AQBodySoA outSoa;
        outSoa.resize(N);
        if (!backend->downloadBodies(outSoa)) { return false; }
        outStates.resize(N);
        for (std::size_t i = 0; i < N; ++i) { outStates[i] = states[i]; }
        outSoa.scatterTo(outStates.data(), N);
        return true;
    };

    OmegaCommon::Vector<AQBodyState<float>> gpuStates;
    OmegaCommon::Vector<AQComputeBackend::RowOut> gpuRows;
    OmegaCommon::Vector<AQComputeBackend::ManifoldOut> gpuManifolds;
    std::size_t pairCount = 0;
    check(runGpuSubstep(gpuStates, gpuRows, gpuManifolds, pairCount),
          "GPU sub-step pipeline (velocity -> narrowphase -> colored solve -> position)");
    std::printf("  GPU: %zu manifolds over %zu pairs, %zu rows\n",
                gpuManifolds.size(), pairCount, gpuRows.size());
    check(gpuManifolds.size() == cpuManifolds.size(),
          "GPU and CPU built the same manifold set");

    // ---- State parity ----
    {
        float maxPosDiff = 0.f, maxVelDiff = 0.f;
        for (std::size_t i = 0; i < N; ++i) {
            for (int c = 0; c < 3; ++c) {
                maxPosDiff = std::fmax(maxPosDiff,
                                       relDiff(cpuPos[i][c][0], gpuStates[i].position[c][0]));
                maxVelDiff = std::fmax(maxVelDiff,
                                       relDiff(cpuVel[i][c][0], gpuStates[i].velocity[c][0]));
            }
        }
        std::printf("  max relative divergence: position %.3g, velocity %.3g "
                    "(band %.3g)\n",
                    double(maxPosDiff), double(maxVelDiff), double(kStateTol));
        check(maxPosDiff < kStateTol && maxVelDiff < kStateTol,
              "post-sub-step body state matches the CPU within the cross-path band");
    }

    // ---- Accumulated-impulse parity (the §8 measured-relaxed band) ----
    {
        float maxImpDiff = 0.f;
        bool structureOk = true;
        std::size_t rowIdx = 0;
        for (std::size_t mi = 0; mi < cpuManifolds.size() && structureOk; ++mi) {
            const AQContactManifold& c = cpuManifolds[mi];
            const AQComputeBackend::ManifoldOut& g = gpuManifolds[mi];
            structureOk = (c.a == g.a) && (c.b == g.b) && (c.pointCount == g.pointCount);
            for (std::uint32_t k = 0; structureOk && k < c.pointCount; ++k) {
                maxImpDiff = std::fmax(maxImpDiff,
                                       relDiff(c.points[k].accumNormal,
                                               gpuRows[rowIdx].accumImpulse));
                maxImpDiff = std::fmax(maxImpDiff,
                                       relDiff(c.points[k].accumFriction[0],
                                               gpuRows[rowIdx + 1].accumImpulse));
                maxImpDiff = std::fmax(maxImpDiff,
                                       relDiff(c.points[k].accumFriction[1],
                                               gpuRows[rowIdx + 2].accumImpulse));
                rowIdx += 3;
            }
        }
        std::printf("  max relative impulse divergence: %.3g (band %.3g)\n",
                    double(maxImpDiff), double(kImpulseTol));
        check(structureOk, "manifold structure matches for the impulse comparison");
        check(maxImpDiff < kImpulseTol,
              "accumulated impulses match the CPU PGS within the measured-relaxed band");
    }

    // ---- Within-path determinism: full sub-step re-run, identical bytes ----
    {
        OmegaCommon::Vector<AQBodyState<float>> gpu2;
        OmegaCommon::Vector<AQComputeBackend::RowOut> rows2;
        OmegaCommon::Vector<AQComputeBackend::ManifoldOut> mans2;
        std::size_t pc2 = 0;
        check(runGpuSubstep(gpu2, rows2, mans2, pc2), "GPU sub-step re-run completes");
        bool same = gpu2.size() == gpuStates.size() && rows2.size() == gpuRows.size();
        for (std::size_t i = 0; same && i < N; ++i) {
            same = gpu2[i].position[0][0] == gpuStates[i].position[0][0] &&
                   gpu2[i].position[1][0] == gpuStates[i].position[1][0] &&
                   gpu2[i].position[2][0] == gpuStates[i].position[2][0] &&
                   gpu2[i].velocity[0][0] == gpuStates[i].velocity[0][0] &&
                   gpu2[i].velocity[1][0] == gpuStates[i].velocity[1][0] &&
                   gpu2[i].velocity[2][0] == gpuStates[i].velocity[2][0];
        }
        for (std::size_t i = 0; same && i < rows2.size(); ++i) {
            same = rows2[i].accumImpulse == gpuRows[i].accumImpulse;
        }
        check(same, "two full GPU sub-steps are byte-identical (within-path determinism)");
    }

    OmegaGTE::Close(gte);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
