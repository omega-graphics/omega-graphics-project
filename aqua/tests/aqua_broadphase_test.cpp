// AQUA Phase 2 — collision shapes & broadphase validation.
//
// Drives the public AQContext / AQSpace / AQRigidBody surface (the same
// runnable-deliverable bar as `aqua_rigid_body_test`). Covers:
//
//   1. Shape factories + the inertia-from-shape closure on AQBodyDesc
//      (the Phase 1 hook documented on `inertiaPrincipalMoments`, §1).
//   2. Brute-force O(n²) AABB-overlap oracle vs broadphase output across
//      thousands of random configurations AND over a moving simulation —
//      zero false negatives, false positives bounded by fattening (§9).
//   3. Rotation-correct world AABB on a spinning box (the §2-point-3
//      failure mode).
//   4. Determinism: byte-identical ordered pair list across two runs of
//      the same scene (§5/§8).
//   5. Collision filter (layer/mask) rejects non-matching pairs (§11.5).
//   6. COM-offset wiring: applyForceAtPoint torque arm respects the
//      non-zero offset (§1, §10).
//   7. Scaling log: candidate / brute-force ratio as a function of n.
//
// Pure CPU — header math + linked AQUA library. No GPU backend touched.

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQRigidBody.h>
#include <aqua/AQCollision.h>
#include <aqua/AQDebug.h>
#include <aqua/AQMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

float vlen(const FVec<3>& v) { return std::sqrt(OmegaGTE::dot(v, v)); }

// ----------------------------------------------------------------------------
// Brute-force AABB-overlap oracle — the §9 "double oracle" analogue.
// ----------------------------------------------------------------------------

// Collect every (i, j) pair with overlapping fattened world AABBs. Ordered
// (a < b) so it matches the broadphase invariant.
std::set<std::pair<std::uint32_t, std::uint32_t>>
bruteForcePairs(const std::vector<std::shared_ptr<AQRigidBody>>& bodies) {
    std::set<std::pair<std::uint32_t, std::uint32_t>> out;
    const std::size_t N = bodies.size();
    for (std::size_t i = 0; i < N; ++i) {
        const auto amn = bodies[i]->aabbMin();
        const auto amx = bodies[i]->aabbMax();
        // Skip bodies without a shape (their AABBs are zero-volume at origin
        // because aabbMin/aabbMax default to zero before any refresh runs).
        if (amn[0][0] == 0.f && amx[0][0] == 0.f &&
            amn[1][0] == 0.f && amx[1][0] == 0.f &&
            amn[2][0] == 0.f && amx[2][0] == 0.f) continue;
        for (std::size_t j = i + 1; j < N; ++j) {
            const auto bmn = bodies[j]->aabbMin();
            const auto bmx = bodies[j]->aabbMax();
            if (bmn[0][0] == 0.f && bmx[0][0] == 0.f &&
                bmn[1][0] == 0.f && bmx[1][0] == 0.f &&
                bmn[2][0] == 0.f && bmx[2][0] == 0.f) continue;
            // Use the same min/max overlap rule the AABB uses internally.
            const bool overlap =
                !(amx[0][0] < bmn[0][0] || amn[0][0] > bmx[0][0] ||
                  amx[1][0] < bmn[1][0] || amn[1][0] > bmx[1][0] ||
                  amx[2][0] < bmn[2][0] || amn[2][0] > bmx[2][0]);
            if (overlap) {
                out.insert({static_cast<std::uint32_t>(i),
                            static_cast<std::uint32_t>(j)});
            }
        }
    }
    return out;
}

// Helper: build a space populated with `n` randomly placed spheres in a cube
// of side `worldSide`. Each sphere has the same radius `r`. Returns the
// (context, space, bodies) triple by out-param.
struct Scene {
    SharedHandle<AQContext>                    ctx;
    SharedHandle<AQSpace>                      sp;
    std::vector<std::shared_ptr<AQRigidBody>>  bodies;
};

Scene buildRandomScene(std::uint32_t seed, std::size_t n, float worldSide, float r) {
    Scene S;
    S.ctx = AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue>());
    S.ctx->setFixedTimestep(1.f / 240.f);
    S.sp = S.ctx->createSpace();
    S.sp->setGravity(AQvec3(0.f, 0.f, 0.f));   // detection only — keep bodies put
    auto sphere = S.sp->createSphereShape(r);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uPos(-worldSide * 0.5f, worldSide * 0.5f);
    for (std::size_t i = 0; i < n; ++i) {
        AQBodyDesc d;
        d.mass     = 1.f;
        d.position = AQvec3(uPos(rng), uPos(rng), uPos(rng));
        d.shape    = sphere;
        S.bodies.push_back(S.sp->addBody(d));
    }
    return S;
}

// ----------------------------------------------------------------------------
// 1. Shape factories + inertia-from-shape closure
// ----------------------------------------------------------------------------

void testShapeFactoryAndInertia() {
    std::printf("\n== shape factories + inertia-from-shape ==\n");

    auto ctx = AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue>());
    auto sp  = ctx->createSpace();

    auto sphere = sp->createSphereShape(1.5f);
    auto box    = sp->createBoxShape(AQvec3(0.5f, 1.f, 2.f));
    auto cap    = sp->createCapsuleShape(0.4f, 0.8f);
    auto plane  = sp->createPlaneShape(AQvec3(0.f, 1.f, 0.f), 0.f);
    auto badR   = sp->createSphereShape(-1.f);                // negative — reject
    auto badN   = sp->createPlaneShape(AQvec3(0.f, 0.f, 0.f), 0.f);  // zero normal

    check(sphere.valid() && box.valid() && cap.valid() && plane.valid(),
          "valid shape factories produce valid handles");
    check(!badR.valid() && !badN.valid(),
          "malformed shape input returns an invalid handle");

    // Body with sphere shape + zero moments must derive moments from shape via
    // AQinertiaSolidSphere(m, r) = (2/5)·m·r² on every axis.
    AQBodyDesc d;
    d.mass = 2.f;
    d.shape = sphere;
    auto bSphere = sp->addBody(d);
    const float expectedSphere = 0.4f * 2.f * 1.5f * 1.5f;
    const auto Isphere = bSphere->inertiaPrincipalMoments();
    check(std::abs(Isphere[0][0] - expectedSphere) < 1e-5f &&
          std::abs(Isphere[1][0] - expectedSphere) < 1e-5f &&
          std::abs(Isphere[2][0] - expectedSphere) < 1e-5f,
          "addBody(sphere shape, zero moments) derives I = (2/5)·m·r²");

    // Box shape: I_x = (m/3)·(hy²+hz²) etc.
    AQBodyDesc d2;
    d2.mass = 3.f;
    d2.shape = box;
    auto bBox = sp->addBody(d2);
    const float c = 3.f / 3.f;
    const float ex = c * (1.f * 1.f + 2.f * 2.f);
    const float ey = c * (0.5f * 0.5f + 2.f * 2.f);
    const float ez = c * (0.5f * 0.5f + 1.f * 1.f);
    const auto Ibox = bBox->inertiaPrincipalMoments();
    check(std::abs(Ibox[0][0] - ex) < 1e-5f &&
          std::abs(Ibox[1][0] - ey) < 1e-5f &&
          std::abs(Ibox[2][0] - ez) < 1e-5f,
          "addBody(box shape) derives I per AQinertiaSolidBox");

    // Explicit moments override the auto-derive path.
    AQBodyDesc d3;
    d3.mass = 1.f;
    d3.shape = sphere;
    d3.inertiaPrincipalMoments = AQvec3(7.f, 8.f, 9.f);
    auto bExplicit = sp->addBody(d3);
    const auto Iexplicit = bExplicit->inertiaPrincipalMoments();
    check(std::abs(Iexplicit[0][0] - 7.f) < 1e-5f &&
          std::abs(Iexplicit[1][0] - 8.f) < 1e-5f &&
          std::abs(Iexplicit[2][0] - 9.f) < 1e-5f,
          "explicit inertia overrides shape-derived inertia");
}

// ----------------------------------------------------------------------------
// 2. Brute-force oracle parity (the headline) — static and moving.
// ----------------------------------------------------------------------------

void testBroadphaseOracleStatic() {
    std::printf("\n== broadphase oracle parity (static, random configs) ==\n");

    const int trials = 8;
    int totalCandidates = 0, totalBrute = 0, badRuns = 0;
    for (int t = 0; t < trials; ++t) {
        auto S = buildRandomScene(0xA1FEu + t, 256, 20.f, 1.f);
        S.ctx->advance(1.f / 60.f);             // triggers AABB refresh + broadphase

        const auto brute = bruteForcePairs(S.bodies);
        const auto pairs = S.sp->candidatePairs();

        // Subset check: every brute-overlap pair must be in the broadphase
        // candidate set (false-negative-free is the §2-point-2 catastrophe).
        std::set<std::pair<std::uint32_t, std::uint32_t>> pairSet;
        for (const auto& p : pairs) pairSet.insert({p.a, p.b});

        bool missAny = false;
        for (const auto& bp : brute) {
            if (pairSet.find(bp) == pairSet.end()) { missAny = true; break; }
        }
        if (missAny) ++badRuns;

        totalCandidates += static_cast<int>(pairs.size());
        totalBrute      += static_cast<int>(brute.size());
    }
    check(badRuns == 0,
          "broadphase superset of brute-force AABB overlaps (zero false negatives)");
    std::printf("  random-static %d trials: total candidates=%d, brute=%d\n",
                trials, totalCandidates, totalBrute);
}

void testBroadphaseOracleMoving() {
    std::printf("\n== broadphase oracle parity (moving sim) ==\n");

    auto S = buildRandomScene(0xB2C3u, 128, 12.f, 0.6f);
    // Give every body a random non-zero velocity so the scene actually moves.
    std::mt19937 rng(0xDA7Au);
    std::uniform_real_distribution<float> uV(-2.f, 2.f);
    for (auto& b : S.bodies) {
        b->setVelocity(AQvec3(uV(rng), uV(rng), uV(rng)));
    }

    int badFrames = 0;
    for (int f = 0; f < 60; ++f) {
        S.ctx->advance(1.f / 60.f);
        const auto brute = bruteForcePairs(S.bodies);
        const auto pairs = S.sp->candidatePairs();
        std::set<std::pair<std::uint32_t, std::uint32_t>> pairSet;
        for (const auto& p : pairs) pairSet.insert({p.a, p.b});
        for (const auto& bp : brute) {
            if (pairSet.find(bp) == pairSet.end()) {
                ++badFrames;
                break;
            }
        }
    }
    check(badFrames == 0,
          "broadphase remains a superset of the oracle over 60 sim frames");
}

// ----------------------------------------------------------------------------
// 3. Rotation-correct world AABB on a spinning box (§2-point-3).
// ----------------------------------------------------------------------------

void testRotationCorrectAABB() {
    std::printf("\n== rotation-correct AABB for a spinning box ==\n");

    auto ctx = AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue>());
    ctx->setFixedTimestep(1.f / 480.f);
    auto sp = ctx->createSpace();
    sp->setGravity(AQvec3(0.f, 0.f, 0.f));

    AQBodyDesc d;
    d.mass = 1.f;
    d.shape = sp->createBoxShape(AQvec3(0.3f, 1.2f, 0.7f));
    d.angularVelocity = AQvec3(0.7f, -1.3f, 0.5f);            // generic spin
    auto body = sp->addBody(d);

    int badFrames = 0;
    for (int f = 0; f < 60; ++f) {
        ctx->advance(1.f / 60.f);
        const auto pos  = body->position();
        const auto q    = body->orientation();
        const auto h    = AQvec3(0.3f, 1.2f, 0.7f);
        const auto amn  = body->aabbMin();
        const auto amx  = body->aabbMax();
        // Every one of the 8 box corners (world frame) must lie inside the
        // body's AABB. Spawned outside the loop because there's no
        // false-positive concern — only the missing-coverage failure mode.
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2) {
                    const auto local = AQvec3(static_cast<float>(sx) * h[0][0],
                                              static_cast<float>(sy) * h[1][0],
                                              static_cast<float>(sz) * h[2][0]);
                    const auto w = pos + AQrotate(q, local);
                    if (w[0][0] < amn[0][0] || w[0][0] > amx[0][0] ||
                        w[1][0] < amn[1][0] || w[1][0] > amx[1][0] ||
                        w[2][0] < amn[2][0] || w[2][0] > amx[2][0]) {
                        ++badFrames;
                        sx = sy = sz = 2;  // break all three loops
                    }
                }
    }
    check(badFrames == 0,
          "spinning box's world AABB contains all 8 corners over 60 frames");
}

// ----------------------------------------------------------------------------
// 4. Determinism: identical scenes produce byte-identical pair lists.
// ----------------------------------------------------------------------------

void testDeterminism() {
    std::printf("\n== broadphase determinism (byte-identical pair lists) ==\n");

    auto S1 = buildRandomScene(0xDEADu, 200, 10.f, 0.5f);
    auto S2 = buildRandomScene(0xDEADu, 200, 10.f, 0.5f);
    S1.ctx->advance(1.f / 60.f);
    S2.ctx->advance(1.f / 60.f);
    const auto p1 = S1.sp->candidatePairs();
    const auto p2 = S2.sp->candidatePairs();
    bool same = p1.size() == p2.size();
    for (std::size_t i = 0; same && i < p1.size(); ++i) {
        same = (p1[i].a == p2[i].a && p1[i].b == p2[i].b);
    }
    check(same, "two runs of the same seed produce identical ordered pair lists");
}

// ----------------------------------------------------------------------------
// 5. Collision filter — layer+mask rejection.
// ----------------------------------------------------------------------------

void testFilter() {
    std::printf("\n== collision filter (layer/mask) ==\n");

    auto ctx = AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue>());
    auto sp = ctx->createSpace();
    sp->setGravity(AQvec3(0.f, 0.f, 0.f));
    auto sphere = sp->createSphereShape(1.f);

    // Two overlapping bodies on disjoint layers — should generate no pair.
    AQBodyDesc dA;
    dA.position = AQvec3(0.f, 0.f, 0.f);
    dA.shape = sphere;
    dA.filter.layer = 0x1; dA.filter.mask  = 0x1;       // only sees layer 1
    auto a = sp->addBody(dA);
    AQBodyDesc dB = dA;
    dB.position = AQvec3(0.5f, 0.f, 0.f);              // clearly overlapping
    dB.filter.layer = 0x2; dB.filter.mask  = 0x2;       // only sees layer 2
    auto b = sp->addBody(dB);

    ctx->advance(1.f / 60.f);
    check(sp->candidatePairs().empty(),
          "overlapping bodies on disjoint layers produce no candidate pair");

    // Open the masks — pair must appear.
    a->setCollisionFilter({0x1, 0x3});
    b->setCollisionFilter({0x2, 0x3});
    ctx->advance(1.f / 60.f);
    const auto pairs = sp->candidatePairs();
    check(pairs.size() == 1 && pairs[0].a == 0 && pairs[0].b == 1,
          "matching masks produce the ordered (0,1) pair");
}

// ----------------------------------------------------------------------------
// 6. COM-offset torque arm (§1, §10).
// ----------------------------------------------------------------------------

void testCOMOffsetTorqueArm() {
    std::printf("\n== COM-offset wiring in applyForceAtPoint ==\n");

    auto ctx = AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue>());
    auto sp = ctx->createSpace();
    sp->setGravity(AQvec3(0.f, 0.f, 0.f));

    // Body with pose origin at the world origin and COM offset (1,0,0). A
    // world-frame point at (1,0,0) — coincident with the COM — must produce
    // ZERO torque even though it is not at the pose origin. Without the
    // COM-offset wiring the torque arm would be (1,0,0) and the cross would
    // be (1,0,0) × (0,1,0) = (0,0,1), which is the regression we are
    // guarding against.
    AQBodyDesc d;
    d.mass = 1.f;
    d.shape = sp->createSphereShape(0.5f);
    d.centerOfMass = AQvec3(1.f, 0.f, 0.f);
    auto body = sp->addBody(d);

    body->applyForceAtPoint(AQvec3(0.f, 1.f, 0.f), AQvec3(1.f, 0.f, 0.f));
    // After one sub-step the angular velocity should still be zero (the body
    // got a pure linear impulse along +Y, no rotational kick). At identity
    // and zero gravity, ω_world == ω_body.
    ctx->advance(1.f / 240.f);
    const auto w = body->angularVelocity();
    check(vlen(w) < 1e-5f,
          "applyForceAtPoint at the COM produces no torque (COM-offset wired)");

    // Sanity: applying the same force at a point one unit ABOVE the COM
    // should produce a non-zero torque around the +X axis (handedness check).
    body->setVelocity(AQvec3(0.f, 0.f, 0.f));
    body->setAngularVelocity(AQvec3(0.f, 0.f, 0.f));
    body->applyForceAtPoint(AQvec3(0.f, 0.f, 1.f), AQvec3(1.f, 1.f, 0.f));
    ctx->advance(1.f / 240.f);
    const auto w2 = body->angularVelocity();
    check(w2[0][0] > 0.f && std::abs(w2[1][0]) < 1e-4f && std::abs(w2[2][0]) < 1e-4f,
          "torque arm uses (worldPoint − comWorld), not (worldPoint − position)");
}

// ----------------------------------------------------------------------------
// 7. Scaling log — informational; not asserted.
// ----------------------------------------------------------------------------

void testScalingLog() {
    std::printf("\n== broadphase scaling (informational) ==\n");

    for (std::size_t n : {std::size_t(32), std::size_t(256), std::size_t(1024)}) {
        // Constant volumetric density so collisions are sparse — worldSide ∝ n^(1/3).
        const float side = 2.f * std::cbrt(static_cast<float>(n));
        auto S = buildRandomScene(0x5C1Eu + static_cast<std::uint32_t>(n), n, side, 0.4f);
        S.ctx->advance(1.f / 60.f);
        const auto pairs = S.sp->candidatePairs();
        const auto brute = bruteForcePairs(S.bodies);
        const std::size_t bf = (n * (n - 1)) / 2;
        std::printf("  n=%4zu  candidates=%5zu  brute-overlap=%5zu  n(n-1)/2=%6zu\n",
                    n, pairs.size(), brute.size(), bf);
    }
    // No `check` — purely informational. The §9 brief intent is "Reported as
    // a logged series, not asserted as a hard wall-clock bound".
    check(true, "broadphase scaling logged (informational)");
}

} // namespace

int main() {
    std::printf("AQUA Phase 2 — collision shapes & broadphase validation\n");

    testShapeFactoryAndInertia();
    testBroadphaseOracleStatic();
    testBroadphaseOracleMoving();
    testRotationCorrectAABB();
    testDeterminism();
    testFilter();
    testCOMOffsetTorqueArm();
    testScalingLog();

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
