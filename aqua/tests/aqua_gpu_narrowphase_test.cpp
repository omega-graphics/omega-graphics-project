// AQUA Phase 5e — narrowphase stage-isolation parity test (plan §9, §13 5e).
// The GPU count -> prefix-scan -> build chain runs the same overlapping scene
// the CPU narrowphase produces manifolds for; the dense pair-ordered GPU
// manifolds must match the CPU's `contactManifolds()` within 1e-4 on
// geometry (normal, positions, depths), exactly on structure (a, b,
// pointCount, featureKey), and the GPU constraint rows must match a
// host-side recomputation of the CPU row-build formulas (effective mass,
// tangent basis, restitution bias) from the CPU manifolds. GJK/EPA pairs
// (box/capsule here) are flagged cpuFallback by the GPU and excluded — the
// bounded-iteration GJK port is the recorded follow-up.
//
// Also exercised: the sorted-array warm-start cache (upload a synthetic
// cache, verify each rebuilt row/point picks up its impulses by
// (pair, featureKey) binary search), and within-path determinism (two cold
// runs produce identical rows).

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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

bool close(float a, float b, float tol) {
    const float scale = std::fmax(1.f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= tol * scale;
}

// A quaternion for a rotation of `angle` radians about a unit axis.
FQuaternion axisAngle(float x, float y, float z, float angle) {
    const float s = std::sin(angle * 0.5f);
    return FQuaternion{x * s, y * s, z * s, std::cos(angle * 0.5f)}.normalized();
}

struct TestShape {
    std::uint32_t type = 0xFFFFFFFFu;   // AQShapeType or no-shape sentinel
    float p0 = 0.f, p1 = 0.f, p2 = 0.f, p3 = 0.f;
    std::uint32_t hullFirst = 0, hullCount = 0;
};

// buildTangentBasis (AQSpace.cpp:1524), replicated for the row oracle.
void tangentBasis(const FVec<3>& n, FVec<3>& t1, FVec<3>& t2) {
    const FVec<3> alt = (std::fabs(n[0][0]) < 0.6f) ? AQvec3(1.f, 0.f, 0.f)
                       : (std::fabs(n[1][0]) < 0.6f) ? AQvec3(0.f, 1.f, 0.f)
                                                     : AQvec3(0.f, 0.f, 1.f);
    t1 = OmegaGTE::cross(n, alt);
    const float t1n2 = OmegaGTE::dot(t1, t1);
    t1 = (t1n2 > 1e-12f) ? t1 * (1.f / std::sqrt(t1n2)) : AQvec3(1.f, 0.f, 0.f);
    t2 = OmegaGTE::cross(n, t1);
    const float t2n2 = OmegaGTE::dot(t2, t2);
    t2 = (t2n2 > 1e-12f) ? t2 * (1.f / std::sqrt(t2n2)) : AQvec3(0.f, 1.f, 0.f);
}

} // namespace

int main() {
    std::printf("== AQUA Phase 5e: GPU narrowphase vs CPU narrowphase oracle ==\n");

    OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
        OmegaGTE::enumerateDevices();
    if (devices.empty()) {
        std::printf("[SKIP] no GTE device on this host — GPU parity test skipped\n");
        return 0;
    }

    // ---- Author the scene: every specialized branch in contact ----
    const std::size_t N = 12;
    OmegaCommon::Vector<TestShape> shapes(N);
    OmegaCommon::Vector<FVec<3>> positions;
    OmegaCommon::Vector<FQuaternion> orientations;
    OmegaCommon::Vector<float> masses(N, 2.f);
    OmegaCommon::Vector<float> hullPoolX, hullPoolY, hullPoolZ;

    for (std::size_t i = 0; i < N; ++i) {
        positions.push_back(AQvec3(0.f, 0.f, 0.f));
        orientations.push_back(FQuaternion::Identity());
    }

    auto setSphere = [&](std::size_t i, float r) { shapes[i].type = 0; shapes[i].p0 = r; };
    auto setBox = [&](std::size_t i, float hx, float hy, float hz) {
        shapes[i].type = 1; shapes[i].p0 = hx; shapes[i].p1 = hy; shapes[i].p2 = hz;
    };
    auto setCapsule = [&](std::size_t i, float r, float hh) {
        shapes[i].type = 2; shapes[i].p0 = r; shapes[i].p1 = hh;
    };

    // 0: ground plane (+Y through origin), static.
    shapes[0].type = 3; shapes[0].p1 = 1.f;
    masses[0] = 0.f;
    // 1: sphere/plane.
    setSphere(1, 1.f); positions[1] = AQvec3(0.f, 0.9f, 0.f);
    // 2: sphere/sphere with 1 + sphere/plane.
    setSphere(2, 1.f); positions[2] = AQvec3(1.5f, 0.95f, 0.f);
    // 3: box/plane, rotated for an asymmetric corner set.
    setBox(3, 1.f, 1.f, 1.f);
    positions[3] = AQvec3(5.f, 0.9f, 0.f);
    orientations[3] = axisAngle(0.f, 0.f, 1.f, 0.17f);
    // 4: box/box with 3.
    setBox(4, 1.f, 1.f, 1.f);
    positions[4] = AQvec3(5.f, 2.5f, 0.1f);
    orientations[4] = axisAngle(0.f, 1.f, 0.f, 0.3f);
    // 5: sphere/box with 4.
    setSphere(5, 0.6f); positions[5] = AQvec3(5.f, 3.9f, 0.f);
    // 6: capsule/plane (upright).
    setCapsule(6, 0.4f, 0.6f); positions[6] = AQvec3(-4.f, 0.9f, 0.f);
    // 7: capsule/capsule with 6, tilted.
    setCapsule(7, 0.4f, 0.6f);
    positions[7] = AQvec3(-3.6f, 1.8f, 0.f);
    orientations[7] = axisAngle(0.f, 0.f, 1.f, 0.5f);
    // 8: sphere/capsule with 7.
    setSphere(8, 0.5f); positions[8] = AQvec3(-3.6f, 2.7f, 0.f);
    // 9: hull/plane (octahedron, extent 0.8).
    shapes[9].type = 4;
    shapes[9].hullFirst = 0;
    shapes[9].hullCount = 6;
    {
        const float e = 0.8f;
        const float ox[6] = { e, -e, 0, 0, 0, 0 };
        const float oy[6] = { 0, 0, e, -e, 0, 0 };
        const float oz[6] = { 0, 0, 0, 0, e, -e };
        for (int k = 0; k < 6; ++k) {
            hullPoolX.push_back(ox[k]);
            hullPoolY.push_back(oy[k]);
            hullPoolZ.push_back(oz[k]);
        }
    }
    positions[9] = AQvec3(3.f, 0.5f, -3.f);
    // 10/11: box + capsule overlap — a GJK pair the GPU flags cpuFallback.
    setBox(10, 0.5f, 0.5f, 0.5f); positions[10] = AQvec3(-8.f, 5.f, 0.f);
    setCapsule(11, 0.3f, 0.4f); positions[11] = AQvec3(-8.f, 5.6f, 0.f);

    OmegaCommon::Vector<float> restitution(N), friction(N);
    for (std::size_t i = 0; i < N; ++i) {
        restitution[i] = 0.05f * float(i);
        friction[i] = 0.3f + 0.02f * float(i);
    }

    // ---- CPU oracle (public surface, zero gravity so poses hold) ----
    OmegaCommon::Vector<AQContactManifold> cpuManifolds;
    OmegaCommon::Vector<std::uint32_t> cpuPairs;
    {
        auto ctx = AQContext::CreateCPUOnly();
        auto space = ctx->createSpace();
        space->setGravity(AQvec3(0.f, 0.f, 0.f));

        for (std::size_t i = 0; i < N; ++i) {
            AQShapeHandle sh;
            const TestShape& s = shapes[i];
            if (s.type == 0) sh = space->createSphereShape(s.p0);
            else if (s.type == 1) sh = space->createBoxShape(AQvec3(s.p0, s.p1, s.p2));
            else if (s.type == 2) sh = space->createCapsuleShape(s.p0, s.p1);
            else if (s.type == 3) sh = space->createPlaneShape(AQvec3(s.p0, s.p1, s.p2), s.p3);
            else if (s.type == 4) {
                OmegaCommon::Vector<FVec<3>> pts;
                for (std::uint32_t k = 0; k < s.hullCount; ++k) {
                    pts.push_back(AQvec3(hullPoolX[s.hullFirst + k],
                                         hullPoolY[s.hullFirst + k],
                                         hullPoolZ[s.hullFirst + k]));
                }
                sh = space->createConvexHullShape(pts.data(), pts.size());
            }
            AQBodyDesc desc;
            desc.type = (masses[i] > 0.f) ? AQBodyType::Dynamic : AQBodyType::Static;
            desc.mass = masses[i];
            desc.position = positions[i];
            desc.orientation = orientations[i];
            desc.shape = sh;
            desc.restitution = restitution[i];
            desc.friction = friction[i];
            space->addBody(desc);
        }
        ctx->advance(1.f / 120.f);
        cpuManifolds = space->contactManifolds();
        auto pairs = space->candidatePairs();
        for (const auto& p : pairs) {
            cpuPairs.push_back(p.a);
            cpuPairs.push_back(p.b);
        }
        std::printf("  CPU oracle: %zu manifolds over %zu pairs\n",
                    cpuManifolds.size(), cpuPairs.size() / 2);
        check(cpuManifolds.size() >= 9, "CPU oracle produced manifolds for every branch");
    }

    // ---- GPU chain ----
    OmegaGTE::GTE gte = OmegaGTE::Init(devices[0]);
    OmegaGTE::GECommandQueueDesc queueDesc;
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto backend = AQComputeBackend::TryCreate(gte.graphicsEngine, queue);
    check(backend != nullptr, "AQComputeBackend::TryCreate succeeds");
    if (!backend) { OmegaGTE::Close(gte); return 1; }
    check(backend->loadKernelLibrary(OmegaCommon::String(AQUA_KERNELS_LIB_PATH)),
          "loadKernelLibrary loads the merged AQKernels.omegasllib");

    // Body state upload — masses/inertia mirroring what addBody derives.
    OmegaCommon::Vector<AQBodyState<float>> states(N);
    OmegaCommon::Vector<FVec<3>> hullPts;
    for (std::size_t k = 0; k < hullPoolX.size(); ++k) {
        hullPts.push_back(AQvec3(hullPoolX[k], hullPoolY[k], hullPoolZ[k]));
    }
    for (std::size_t i = 0; i < N; ++i) {
        states[i].position = positions[i];
        states[i].orientation = orientations[i];
        if (masses[i] > 0.f) {
            states[i].invMass = 1.f / masses[i];
            AQShape sh;
            const TestShape& s = shapes[i];
            if (s.type == 0) { sh.type = AQShapeType::Sphere; sh.sphere.radius = s.p0; }
            else if (s.type == 1) { sh.type = AQShapeType::Box; sh.box.hx = s.p0; sh.box.hy = s.p1; sh.box.hz = s.p2; }
            else if (s.type == 2) { sh.type = AQShapeType::Capsule; sh.capsule.radius = s.p0; sh.capsule.halfHeight = s.p1; }
            else if (s.type == 4) { sh.type = AQShapeType::ConvexHull; sh.hull.firstVertex = s.hullFirst; sh.hull.vertexCount = s.hullCount; }
            const FVec<3> moments = AQshapeInertiaMoments(sh, masses[i],
                                                          hullPts.data(), hullPts.size());
            states[i].invInertiaBody = AQvec3(
                moments[0][0] > 0.f ? 1.f / moments[0][0] : 0.f,
                moments[1][0] > 0.f ? 1.f / moments[1][0] : 0.f,
                moments[2][0] > 0.f ? 1.f / moments[2][0] : 0.f);
        } else {
            states[i].invMass = 0.f;
            states[i].invInertiaBody = AQvec3(0.f, 0.f, 0.f);
        }
    }
    {
        AQBodySoA soa;
        soa.gatherFrom(states.data(), N);
        check(backend->uploadBodies(soa), "uploadBodies (poses + mass properties)");
    }

    // Shape/filter tables (broadphase inputs feed the narrowphase too).
    {
        AQComputeBackend::BroadphaseInputs in;
        in.shapeType.resize(N); in.hullFirst.resize(N); in.hullCount.resize(N);
        in.hasShape.resize(N);
        in.paramX.resize(N); in.paramY.resize(N); in.paramZ.resize(N); in.paramW.resize(N);
        in.localPosX.assign(N, 0.f); in.localPosY.assign(N, 0.f); in.localPosZ.assign(N, 0.f);
        in.localQuatX.assign(N, 0.f); in.localQuatY.assign(N, 0.f); in.localQuatZ.assign(N, 0.f);
        in.localQuatW.assign(N, 1.f);
        in.filterLayer.assign(N, 1u); in.filterMask.assign(N, ~0u);
        in.hullVertX = hullPoolX; in.hullVertY = hullPoolY; in.hullVertZ = hullPoolZ;
        for (std::size_t i = 0; i < N; ++i) {
            const TestShape& s = shapes[i];
            in.shapeType[i] = (s.type == 0xFFFFFFFFu) ? 0u : s.type;
            in.hullFirst[i] = s.hullFirst;
            in.hullCount[i] = s.hullCount;
            in.hasShape[i] = (s.type == 0xFFFFFFFFu) ? 0u : 1u;
            in.paramX[i] = s.p0; in.paramY[i] = s.p1;
            in.paramZ[i] = s.p2; in.paramW[i] = s.p3;
        }
        check(backend->uploadBroadphaseInputs(in), "uploadBroadphaseInputs");
    }

    // Broadphase to produce the (verified byte-identical) pair list.
    check(backend->encodeRefreshAABB(N, 0.02f, 1.f / 120.f), "encodeRefreshAABB");
    OmegaCommon::Vector<float> fatMin, fatMax;
    check(backend->downloadFatAABBs(fatMin, fatMax, N), "downloadFatAABBs");
    float cellSize = 1.f;
    for (std::size_t i = 0; i < N; ++i) {
        if (shapes[i].type == 0xFFFFFFFFu || shapes[i].type == 3) continue;
        for (int c = 0; c < 3; ++c) {
            cellSize = std::fmax(cellSize, fatMax[i * 3 + c] - fatMin[i * 3 + c]);
        }
    }
    check(backend->encodeBroadphase(N, cellSize, 64), "encodeBroadphase");
    OmegaCommon::Vector<std::uint32_t> gpuPairs;
    check(backend->downloadPairs(gpuPairs), "downloadPairs");
    {
        bool same = gpuPairs.size() == cpuPairs.size();
        for (std::size_t k = 0; same && k < gpuPairs.size(); ++k) {
            same = gpuPairs[k] == cpuPairs[k];
        }
        check(same, "GPU pair list matches the CPU pair list (5d regression)");
    }
    const std::size_t pairCount = gpuPairs.size() / 2;

    // Materials + the narrowphase itself (cold — no warm-start cache).
    {
        OmegaCommon::Vector<std::uint32_t> trig(N, 0u);
        check(backend->uploadNarrowphaseInputs(restitution, friction, trig),
              "uploadNarrowphaseInputs (materials)");
    }
    check(backend->encodeNarrowphase(pairCount, 1.f / 120.f, 0u, 0u, 0),
          "encodeNarrowphase (count -> scans -> build)");

    OmegaCommon::Vector<AQComputeBackend::ManifoldOut> gpuManifolds;
    OmegaCommon::Vector<AQComputeBackend::RowOut> gpuRows;
    OmegaCommon::Vector<std::uint32_t> fallback;
    check(backend->downloadNarrowphase(gpuManifolds, gpuRows, fallback, pairCount),
          "downloadNarrowphase");
    std::printf("  GPU: %zu manifolds, %zu rows\n", gpuManifolds.size(), gpuRows.size());

    // The GJK pair (10, 11) must be flagged for CPU fallback.
    std::set<std::pair<std::uint32_t, std::uint32_t>> flagged;
    for (std::size_t p = 0; p < pairCount; ++p) {
        if (fallback[p] != 0) {
            flagged.emplace(gpuPairs[p * 2], gpuPairs[p * 2 + 1]);
        }
    }
    check(flagged.count({10u, 11u}) == 1 && flagged.size() == 1,
          "exactly the box/capsule GJK pair is flagged cpuFallback");

    // ---- Manifold parity (CPU manifolds minus the flagged pairs) ----
    OmegaCommon::Vector<AQContactManifold> expected;
    for (const auto& m : cpuManifolds) {
        if (flagged.count({m.a, m.b}) == 0) {
            expected.push_back(m);
        }
    }
    {
        bool ok = (expected.size() == gpuManifolds.size());
        if (!ok) {
            std::printf("  manifold count: cpu(expected) %zu vs gpu %zu\n",
                        expected.size(), gpuManifolds.size());
        }
        for (std::size_t i = 0; ok && i < expected.size(); ++i) {
            const AQContactManifold& c = expected[i];
            const AQComputeBackend::ManifoldOut& g = gpuManifolds[i];
            ok = (c.a == g.a) && (c.b == g.b) && (c.pointCount == g.pointCount) &&
                 close(c.normalWorld[0][0], g.normal[0], 1e-4f) &&
                 close(c.normalWorld[1][0], g.normal[1], 1e-4f) &&
                 close(c.normalWorld[2][0], g.normal[2], 1e-4f) &&
                 close(c.restitutionCombined, g.restitutionCombined, 1e-6f) &&
                 close(c.frictionCombined, g.frictionCombined, 1e-6f);
            for (std::uint32_t k = 0; ok && k < c.pointCount; ++k) {
                ok = close(c.points[k].positionWorld[0][0], g.pointPos[k][0], 1e-4f) &&
                     close(c.points[k].positionWorld[1][0], g.pointPos[k][1], 1e-4f) &&
                     close(c.points[k].positionWorld[2][0], g.pointPos[k][2], 1e-4f) &&
                     close(c.points[k].depth, g.pointDepth[k], 1e-4f) &&
                     (c.points[k].featureKey == g.pointKey[k]);
            }
            if (!ok) {
                std::printf("  manifold %zu (%u,%u) diverged: cpu n=(%g,%g,%g) cnt=%u | "
                            "gpu n=(%g,%g,%g) cnt=%u\n", i, c.a, c.b,
                            double(c.normalWorld[0][0]), double(c.normalWorld[1][0]),
                            double(c.normalWorld[2][0]), c.pointCount,
                            double(g.normal[0]), double(g.normal[1]), double(g.normal[2]),
                            g.pointCount);
            }
        }
        check(ok, "GPU manifolds match the CPU narrowphase (geometry 1e-4, structure exact)");
    }

    // ---- Row oracle: recompute the CPU row-build from the CPU manifolds ----
    {
        bool ok = true;
        std::size_t rowIdx = 0;
        for (const auto& m : expected) {
            const AQBodyState<float>& bA = states[m.a];
            const AQBodyState<float>& bB = states[m.b];
            const auto invIA = AQworldInvInertia(bA.orientation, bA.invInertiaBody);
            const auto invIB = AQworldInvInertia(bB.orientation, bB.invInertiaBody);
            const FVec<3> comA = bA.position;   // zero COM offsets in this scene
            const FVec<3> comB = bB.position;
            FVec<3> t1 = AQvec3(1.f, 0.f, 0.f), t2 = AQvec3(0.f, 1.f, 0.f);
            tangentBasis(m.normalWorld, t1, t2);
            auto effMass = [&](const FVec<3>& rA, const FVec<3>& rB, const FVec<3>& dir) {
                const FVec<3> rAxN = OmegaGTE::cross(rA, dir);
                const FVec<3> rBxN = OmegaGTE::cross(rB, dir);
                const float k = bA.invMass + bB.invMass +
                                OmegaGTE::dot(rAxN, invIA * rAxN) +
                                OmegaGTE::dot(rBxN, invIB * rBxN);
                return (k > 1e-12f) ? 1.f / k : 0.f;
            };
            for (std::uint32_t pk = 0; ok && pk < m.pointCount; ++pk) {
                const FVec<3> cp = m.points[pk].positionWorld;
                const FVec<3> rA = cp - comA;
                const FVec<3> rB = cp - comB;
                const std::size_t base = rowIdx;
                if (base + 2 >= gpuRows.size()) { ok = false; break; }
                const auto& nRow = gpuRows[base];
                const auto& f1 = gpuRows[base + 1];
                const auto& f2 = gpuRows[base + 2];
                // Structure.
                ok = nRow.kind == 0 && f1.kind == 1 && f2.kind == 1 &&
                     nRow.bodyA == m.a && nRow.bodyB == m.b &&
                     nRow.peerRow == base && f1.peerRow == base && f2.peerRow == base &&
                     !nRow.isAngular && nRow.compliance == 0.f;
                // Directions + effective masses (velocities are zero -> bias 0).
                ok = ok &&
                     close(nRow.direction[0], m.normalWorld[0][0], 1e-4f) &&
                     close(nRow.direction[1], m.normalWorld[1][0], 1e-4f) &&
                     close(nRow.direction[2], m.normalWorld[2][0], 1e-4f) &&
                     close(f1.direction[0], t1[0][0], 1e-4f) &&
                     close(f2.direction[0], t2[0][0], 1e-4f) &&
                     close(nRow.effectiveMass, effMass(rA, rB, m.normalWorld), 1e-4f) &&
                     close(f1.effectiveMass, effMass(rA, rB, t1), 1e-4f) &&
                     close(f2.effectiveMass, effMass(rA, rB, t2), 1e-4f) &&
                     nRow.bias == 0.f && nRow.accumImpulse == 0.f &&
                     close(f1.frictionCoeff, m.frictionCombined, 1e-6f);
                if (!ok) {
                    std::printf("  row triple %zu (manifold %u,%u pt %u) diverged\n",
                                base, m.a, m.b, pk);
                }
                rowIdx += 3;
            }
            if (!ok) break;
        }
        ok = ok && (rowIdx == gpuRows.size());
        check(ok, "GPU rows match the recomputed CPU row-build (eff mass, tangents, peers)");
    }

    // ---- Warm-start cache round-trip ----
    {
        // Build a cache from the GPU manifolds with distinctive values.
        struct Entry { std::uint32_t lo, hi; float v[3]; };
        OmegaCommon::Vector<Entry> entries;
        float tag = 1.f;
        for (const auto& g : gpuManifolds) {
            for (std::uint32_t k = 0; k < g.pointCount; ++k) {
                Entry e;
                e.lo = (g.a & 0xFFFFu) | ((g.b & 0xFFFFu) << 16);
                e.hi = g.pointKey[k];
                e.v[0] = tag; e.v[1] = tag + 0.25f; e.v[2] = tag + 0.5f;
                tag += 1.f;
                entries.push_back(e);
            }
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
            return (x.hi != y.hi) ? (x.hi < y.hi) : (x.lo < y.lo);
        });
        OmegaCommon::Vector<std::uint32_t> keys;
        OmegaCommon::Vector<float> vals;
        for (const auto& e : entries) {
            keys.push_back(e.lo);
            keys.push_back(e.hi);
            vals.push_back(e.v[0]);
            vals.push_back(e.v[1]);
            vals.push_back(e.v[2]);
        }
        check(backend->uploadWarmStartCache(keys, vals), "uploadWarmStartCache");
        check(backend->encodeNarrowphase(pairCount, 1.f / 120.f, 0u, 0u, entries.size()),
              "encodeNarrowphase (warm-started)");
        OmegaCommon::Vector<AQComputeBackend::ManifoldOut> warmManifolds;
        OmegaCommon::Vector<AQComputeBackend::RowOut> warmRows;
        OmegaCommon::Vector<std::uint32_t> fb2;
        check(backend->downloadNarrowphase(warmManifolds, warmRows, fb2, pairCount),
              "downloadNarrowphase (warm)");
        bool ok = warmManifolds.size() == gpuManifolds.size() &&
                  warmRows.size() == gpuRows.size();
        std::size_t rowIdx = 0;
        for (std::size_t i = 0; ok && i < warmManifolds.size(); ++i) {
            const auto& g = warmManifolds[i];
            for (std::uint32_t k = 0; ok && k < g.pointCount; ++k) {
                const std::uint32_t lo = (g.a & 0xFFFFu) | ((g.b & 0xFFFFu) << 16);
                const Entry* found = nullptr;
                for (const auto& e : entries) {
                    if (e.lo == lo && e.hi == g.pointKey[k]) { found = &e; break; }
                }
                ok = found != nullptr &&
                     g.pointAccum[k][0] == found->v[0] &&
                     g.pointAccum[k][1] == found->v[1] &&
                     g.pointAccum[k][2] == found->v[2] &&
                     warmRows[rowIdx].accumImpulse == found->v[0] &&
                     warmRows[rowIdx + 1].accumImpulse == found->v[1] &&
                     warmRows[rowIdx + 2].accumImpulse == found->v[2];
                rowIdx += 3;
            }
        }
        check(ok, "warm-start cache: every rebuilt point/row picks up its impulses");
    }

    // ---- Within-path determinism: two cold runs, identical rows ----
    {
        check(backend->encodeNarrowphase(pairCount, 1.f / 120.f, 0u, 0u, 0),
              "encodeNarrowphase (cold re-run)");
        OmegaCommon::Vector<AQComputeBackend::ManifoldOut> m2;
        OmegaCommon::Vector<AQComputeBackend::RowOut> r2;
        OmegaCommon::Vector<std::uint32_t> fb3;
        check(backend->downloadNarrowphase(m2, r2, fb3, pairCount),
              "downloadNarrowphase (re-run)");
        bool same = r2.size() == gpuRows.size();
        for (std::size_t i = 0; same && i < r2.size(); ++i) {
            same = r2[i].effectiveMass == gpuRows[i].effectiveMass &&
                   r2[i].bias == gpuRows[i].bias &&
                   r2[i].direction[0] == gpuRows[i].direction[0] &&
                   r2[i].direction[1] == gpuRows[i].direction[1] &&
                   r2[i].direction[2] == gpuRows[i].direction[2] &&
                   r2[i].rA[0] == gpuRows[i].rA[0] &&
                   r2[i].rB[2] == gpuRows[i].rB[2];
        }
        check(same, "two GPU narrowphase runs are bit-identical");
    }

    OmegaGTE::Close(gte);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
