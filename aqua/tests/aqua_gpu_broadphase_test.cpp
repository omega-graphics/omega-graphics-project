// AQUA Phase 5d — broadphase stage-isolation parity test (plan §9, §13 5d).
// The GPU grid chain (AQRefreshAABB -> AQGridHash -> stable sort ->
// AQBroadphasePairs -> pair sort) runs the same randomized static scene the
// CPU broadphase publishes candidate pairs for, and the two ordered pair
// lists must be IDENTICAL — the exact-overlap filter makes the final list
// independent of the grid used to find candidates, so this holds exactly,
// not within a tolerance. The AABB refresh kernel itself is compared
// per-field against the CPU fat bounds within a float band.
//
// CPU oracle: the PUBLIC surface (CreateCPUOnly context + static bodies +
// advance one fixed step — static bodies don't move, so the pair list is the
// pure broadphase output). GPU: the 5d backend entry points, fed the same
// bodies/shapes/filters. INTERNAL test (src/ headers + kernel-lib path).

#include "AQBodySoA.h"
#include "AQComputeBackend.h"

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQCollision.h>
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

// Deterministic LCG so the scene is identical on every run/host (no <random>
// distribution variance).
struct Lcg {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    float next() {   // [0, 1)
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return float((s >> 33) & 0xFFFFFFu) / float(0x1000000);
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// A shape as the test authors it — mirrored into BOTH the AQSpace factory
// call and the GPU BroadphaseInputs arrays.
struct TestShape {
    std::uint32_t type = 0;        // AQShapeType value
    float p0 = 0.f, p1 = 0.f, p2 = 0.f, p3 = 0.f;
    std::uint32_t hullFirst = 0, hullCount = 0;
};

constexpr float kFattenMargin = 0.02f;   // AQSpace::Impl::fattenMargin default

bool close(float a, float b, float tol) {
    const float scale = std::fmax(1.f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= tol * scale;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 5d: GPU broadphase vs CPU broadphase oracle ==\n");

    OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
        OmegaGTE::enumerateDevices();
    if (devices.empty()) {
        std::printf("[SKIP] no GTE device on this host — GPU parity test skipped\n");
        return 0;
    }

    // ---- Author the randomized scene (shared by both paths) ----
    Lcg rng;
    const std::size_t N = 64;

    OmegaCommon::Vector<TestShape> shapes(N);
    OmegaCommon::Vector<FVec<3>> positions;
    OmegaCommon::Vector<FQuaternion> orientations;
    OmegaCommon::Vector<std::uint32_t> layers(N, 1u), masks(N, ~0u);
    OmegaCommon::Vector<float> hullPoolX, hullPoolY, hullPoolZ;

    positions.reserve(N);
    orientations.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        positions.push_back(AQvec3(rng.range(-6.f, 6.f), rng.range(-2.f, 6.f),
                                   rng.range(-6.f, 6.f)));
        FQuaternion q{rng.range(-1.f, 1.f), rng.range(-1.f, 1.f),
                      rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)};
        orientations.push_back(q.normalized());
    }

    for (std::size_t i = 0; i < N; ++i) {
        TestShape& s = shapes[i];
        if (i == 0) {
            // Ground plane (+Y up, through the origin).
            s.type = 3;
            s.p0 = 0.f; s.p1 = 1.f; s.p2 = 0.f; s.p3 = 0.f;
            positions[i] = AQvec3(0.f, 0.f, 0.f);
            orientations[i] = FQuaternion::Identity();
            continue;
        }
        if (i >= N - 2) {
            // Two shapeless bodies — broadphase-invisible on both paths.
            s.type = 0xFFFFFFFFu;
            continue;
        }
        const float pick = rng.next();
        if (pick < 0.35f) {
            s.type = 0;                            // sphere
            s.p0 = rng.range(0.3f, 1.2f);
        } else if (pick < 0.7f) {
            s.type = 1;                            // box
            s.p0 = rng.range(0.3f, 1.f);
            s.p1 = rng.range(0.3f, 1.f);
            s.p2 = rng.range(0.3f, 1.f);
        } else if (pick < 0.9f) {
            s.type = 2;                            // capsule
            s.p0 = rng.range(0.25f, 0.6f);
            s.p1 = rng.range(0.2f, 0.9f);
        } else {
            s.type = 4;                            // convex hull (6 points)
            s.hullFirst = static_cast<std::uint32_t>(hullPoolX.size());
            s.hullCount = 6;
            for (int k = 0; k < 6; ++k) {
                hullPoolX.push_back(rng.range(-0.8f, 0.8f));
                hullPoolY.push_back(rng.range(-0.8f, 0.8f));
                hullPoolZ.push_back(rng.range(-0.8f, 0.8f));
            }
        }
    }
    // A restrictive filter group: bodies 10..14 only collide with each other.
    for (std::size_t i = 10; i <= 14; ++i) {
        layers[i] = 2u;
        masks[i] = 2u;
    }

    // ---- CPU oracle: the public surface ----
    OmegaCommon::Vector<std::uint32_t> cpuPairs;   // flattened (a, b)
    OmegaCommon::Vector<float> cpuFatMin, cpuFatMax;
    {
        auto ctx = AQContext::CreateCPUOnly();
        auto space = ctx->createSpace();

        OmegaCommon::Vector<SharedHandle<AQRigidBody>> handles;
        for (std::size_t i = 0; i < N; ++i) {
            AQShapeHandle sh;   // invalid = no shape
            const TestShape& s = shapes[i];
            if (s.type == 0) {
                sh = space->createSphereShape(s.p0);
            } else if (s.type == 1) {
                sh = space->createBoxShape(AQvec3(s.p0, s.p1, s.p2));
            } else if (s.type == 2) {
                sh = space->createCapsuleShape(s.p0, s.p1);
            } else if (s.type == 3) {
                sh = space->createPlaneShape(AQvec3(s.p0, s.p1, s.p2), s.p3);
            } else if (s.type == 4) {
                OmegaCommon::Vector<FVec<3>> pts;
                for (std::uint32_t k = 0; k < s.hullCount; ++k) {
                    pts.push_back(AQvec3(hullPoolX[s.hullFirst + k],
                                         hullPoolY[s.hullFirst + k],
                                         hullPoolZ[s.hullFirst + k]));
                }
                sh = space->createConvexHullShape(pts.data(), pts.size());
            }
            AQBodyDesc desc;
            desc.type = AQBodyType::Static;
            desc.position = positions[i];
            desc.orientation = orientations[i];
            desc.shape = sh;
            desc.filter.layer = layers[i];
            desc.filter.mask = masks[i];
            handles.push_back(space->addBody(desc));
        }

        ctx->advance(1.f / 120.f);   // one fixed step: broadphase runs, statics hold

        auto pairs = space->candidatePairs();
        for (const auto& p : pairs) {
            cpuPairs.push_back(p.a);
            cpuPairs.push_back(p.b);
        }
        cpuFatMin.resize(N * 3);
        cpuFatMax.resize(N * 3);
        for (std::size_t i = 0; i < N; ++i) {
            const auto lo = handles[i]->aabbMin();
            const auto hi = handles[i]->aabbMax();
            for (int c = 0; c < 3; ++c) {
                cpuFatMin[i * 3 + c] = lo[c][0];
                cpuFatMax[i * 3 + c] = hi[c][0];
            }
        }
        std::printf("  CPU oracle: %zu candidate pairs\n", cpuPairs.size() / 2);
        check(cpuPairs.size() >= 2, "CPU oracle produced a non-trivial pair list");
    }

    // ---- GPU path ----
    OmegaGTE::GTE gte = OmegaGTE::Init(devices[0]);
    OmegaGTE::GECommandQueueDesc queueDesc;
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto backend = AQComputeBackend::TryCreate(gte.graphicsEngine, queue);
    check(backend != nullptr, "AQComputeBackend::TryCreate succeeds");
    if (!backend) { OmegaGTE::Close(gte); return 1; }
    check(backend->loadKernelLibrary(OmegaCommon::String(AQUA_KERNELS_LIB_PATH)),
          "loadKernelLibrary loads the merged AQKernels.omegasllib");

    // Body pose/velocity via the shared body buffers (velocities zero, so the
    // fatten dilation term drops out exactly as it does on the CPU).
    {
        OmegaCommon::Vector<AQBodyState<float>> states(N);
        for (std::size_t i = 0; i < N; ++i) {
            states[i].position = positions[i];
            states[i].orientation = orientations[i];
            states[i].invMass = 0.f;   // static
        }
        AQBodySoA soa;
        soa.gatherFrom(states.data(), N);
        check(backend->uploadBodies(soa), "uploadBodies (poses for the AABB kernel)");
    }

    // Shape/filter tables.
    {
        AQComputeBackend::BroadphaseInputs in;
        in.shapeType.resize(N); in.hullFirst.resize(N); in.hullCount.resize(N);
        in.hasShape.resize(N);
        in.paramX.resize(N); in.paramY.resize(N); in.paramZ.resize(N); in.paramW.resize(N);
        in.localPosX.assign(N, 0.f); in.localPosY.assign(N, 0.f); in.localPosZ.assign(N, 0.f);
        in.localQuatX.assign(N, 0.f); in.localQuatY.assign(N, 0.f); in.localQuatZ.assign(N, 0.f);
        in.localQuatW.assign(N, 1.f);
        in.filterLayer.resize(N); in.filterMask.resize(N);
        in.hullVertX = hullPoolX; in.hullVertY = hullPoolY; in.hullVertZ = hullPoolZ;
        for (std::size_t i = 0; i < N; ++i) {
            const TestShape& s = shapes[i];
            const bool has = (s.type != 0xFFFFFFFFu);
            in.shapeType[i] = has ? s.type : 0u;
            in.hullFirst[i] = s.hullFirst;
            in.hullCount[i] = s.hullCount;
            in.hasShape[i] = has ? 1u : 0u;
            in.paramX[i] = s.p0; in.paramY[i] = s.p1;
            in.paramZ[i] = s.p2; in.paramW[i] = s.p3;
            in.filterLayer[i] = layers[i];
            in.filterMask[i] = masks[i];
        }
        check(backend->uploadBroadphaseInputs(in), "uploadBroadphaseInputs (shape/filter tables)");
    }

    check(backend->encodeRefreshAABB(N, kFattenMargin, 1.f / 120.f),
          "encodeRefreshAABB dispatches the AABB kernel");

    // AABB parity (float band — same formulas, GPU codegen may contract FMAs).
    OmegaCommon::Vector<float> fatMin, fatMax;
    check(backend->downloadFatAABBs(fatMin, fatMax, N), "downloadFatAABBs reads bounds back");
    {
        bool aabbOk = true;
        for (std::size_t i = 0; i < N; ++i) {
            if (shapes[i].type == 0xFFFFFFFFu) continue;   // no shape: CPU keeps stale bounds
            for (int c = 0; c < 3; ++c) {
                if (!close(fatMin[i * 3 + c], cpuFatMin[i * 3 + c], 1e-5f) ||
                    !close(fatMax[i * 3 + c], cpuFatMax[i * 3 + c], 1e-5f)) {
                    std::printf("  body %zu axis %d: GPU [%g, %g] CPU [%g, %g]\n", i, c,
                                double(fatMin[i * 3 + c]), double(fatMax[i * 3 + c]),
                                double(cpuFatMin[i * 3 + c]), double(cpuFatMax[i * 3 + c]));
                    aabbOk = false;
                }
            }
        }
        check(aabbOk, "GPU fat AABBs match the CPU fat AABBs (all shape types)");
    }

    // Grid cell size: the 27-neighborhood completeness bound — max fat extent
    // over grid (non-plane) bodies.
    float cellSize = 1.f;
    for (std::size_t i = 0; i < N; ++i) {
        if (shapes[i].type == 0xFFFFFFFFu || shapes[i].type == 3) continue;
        for (int c = 0; c < 3; ++c) {
            cellSize = std::fmax(cellSize, fatMax[i * 3 + c] - fatMin[i * 3 + c]);
        }
    }

    check(backend->encodeBroadphase(N, cellSize, 64),
          "encodeBroadphase runs the grid chain (deliberately small pair hint — exercises overflow regrow)");
    OmegaCommon::Vector<std::uint32_t> gpuPairs;
    check(backend->downloadPairs(gpuPairs), "downloadPairs reads the sorted pair list");
    std::printf("  GPU: %zu candidate pairs\n", gpuPairs.size() / 2);

    {
        bool same = (gpuPairs.size() == cpuPairs.size());
        if (same) {
            for (std::size_t k = 0; k < gpuPairs.size(); ++k) {
                if (gpuPairs[k] != cpuPairs[k]) { same = false; break; }
            }
        }
        if (!same) {
            const std::size_t nShow = 12;
            std::printf("  first pairs (cpu | gpu):\n");
            for (std::size_t k = 0; k * 2 + 1 < std::max(cpuPairs.size(), gpuPairs.size()) && k < nShow; ++k) {
                std::printf("    (%u,%u) | (%u,%u)\n",
                            k * 2 + 1 < cpuPairs.size() ? cpuPairs[k * 2] : 9999u,
                            k * 2 + 1 < cpuPairs.size() ? cpuPairs[k * 2 + 1] : 9999u,
                            k * 2 + 1 < gpuPairs.size() ? gpuPairs[k * 2] : 9999u,
                            k * 2 + 1 < gpuPairs.size() ? gpuPairs[k * 2 + 1] : 9999u);
            }
        }
        check(same, "GPU ordered pair list is IDENTICAL to the CPU broadphase oracle");
    }

    // Within-path determinism: rerun the chain, byte-identical list.
    {
        check(backend->encodeBroadphase(N, cellSize, gpuPairs.size() / 2),
              "encodeBroadphase re-run completes");
        OmegaCommon::Vector<std::uint32_t> gpuPairs2;
        check(backend->downloadPairs(gpuPairs2), "downloadPairs re-run reads back");
        bool same = (gpuPairs2.size() == gpuPairs.size());
        if (same) {
            for (std::size_t k = 0; k < gpuPairs.size(); ++k) {
                if (gpuPairs[k] != gpuPairs2[k]) { same = false; break; }
            }
        }
        check(same, "two GPU broadphase runs produce identical pair lists");
    }

    OmegaGTE::Close(gte);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
