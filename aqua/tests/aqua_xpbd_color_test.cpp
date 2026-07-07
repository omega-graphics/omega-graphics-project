// AQUA Phase 7 §13 7b — XPBD storage, authoring, and deterministic graph
// coloring.
//
// INTERNAL test: drives AQXPBDBody (src/AQSpaceImpl.h) directly plus the
// public authoring surface (createXPBDBody / addDistanceConstraint / addRope /
// readXPBDConstraints). The coloring is a CORRECTNESS precondition for the GPU
// path (two constraints sharing a particle in one batch is a write race, brief
// §2.3), so the invariants here are hard:
//   * no two constraints in a color batch share a particle;
//   * batches are contiguous, disjoint, and cover every constraint in
//     (color, authoring-index) total order;
//   * the coloring is a pure function of authoring order (bitwise identical
//     across two identically-authored bodies);
//   * a chain colors with exactly 2 colors (greedy on a path graph);
//   * topology edits mark the coloring dirty and the next read reflects them.

#include "AQSpaceImpl.h"

#include <aqua/AQContext.h>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Shared invariant: within every batch, no particle appears twice; batches
// tile [0, constraintCount) contiguously; within a batch, authoring indices
// ascend (the fixed within-color order determinism rides on).
static bool colorInvariantsHold(const AQXPBDBody& body) {
    std::uint32_t covered = 0;
    for (const auto& batch : body.batches) {
        if (batch.firstConstraint != covered) return false;   // contiguity
        std::set<std::uint32_t> touched;
        std::uint32_t prevAuthoring = 0;
        for (std::uint32_t k = batch.firstConstraint;
             k < batch.firstConstraint + batch.constraintCount; ++k) {
            const AQDistanceConstraint& c = body.distanceSorted[k];
            if (c.color != batch.color) return false;
            if (!touched.insert(c.a).second) return false;    // shared particle!
            if (!touched.insert(c.b).second) return false;
            const std::uint32_t authoring = body.sortedAuthoring[k];
            if (k > batch.firstConstraint && authoring <= prevAuthoring) return false;
            prevAuthoring = authoring;
        }
        covered += batch.constraintCount;
    }
    return covered == body.distanceSorted.size() &&
           body.distanceSorted.size() == body.distance.size();
}

static void buildChain(AQXPBDBody& body, std::uint32_t particles) {
    OmegaCommon::Vector<FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    for (std::uint32_t i = 0; i < particles; ++i) {
        pos.push_back(AQvec3(static_cast<float>(i), 0.f, 0.f));
        w.push_back(1.f);
    }
    body.addParticles(pos.data(), w.data(), particles);
    for (std::uint32_t i = 0; i + 1 < particles; ++i)
        body.addDistance(i, i + 1, 1.f, 0.f);
}

static void testChainColoring() {
    AQXPBDBody body;
    buildChain(body, 32);
    body.recolor();

    check(colorInvariantsHold(body), "chain: batches contiguous, ordered, conflict-free");
    check(body.batches.size() == 2, "chain: greedy colors a path graph with exactly 2 colors");
    // Alternating pattern: authoring constraint i gets color i % 2.
    bool alternating = true;
    for (std::uint32_t i = 0; i < body.distance.size(); ++i)
        alternating = alternating && (body.distance[i].color == (i % 2u));
    check(alternating, "chain: colors alternate along the chain (authoring-order greedy)");
}

static void testStarAndSharedHub() {
    // A hub particle shared by 6 constraints — they must ALL land in different
    // colors (a star graph's edges pairwise conflict).
    AQXPBDBody body;
    OmegaCommon::Vector<FVec<3>> pos;
    for (int i = 0; i < 7; ++i) pos.push_back(AQvec3(static_cast<float>(i), 0.f, 0.f));
    body.addParticles(pos.data(), nullptr, 7);
    for (std::uint32_t leaf = 1; leaf <= 6; ++leaf) body.addDistance(0, leaf, 1.f, 0.f);
    body.recolor();

    check(colorInvariantsHold(body), "star: batches conflict-free");
    check(body.batches.size() == 6, "star: 6 constraints on one hub take 6 colors");
    bool oneEach = true;
    for (const auto& b : body.batches) oneEach = oneEach && (b.constraintCount == 1);
    check(oneEach, "star: every color batch holds exactly one hub constraint");
}

static void testDeterminism() {
    AQXPBDBody a, b;
    buildChain(a, 64);
    buildChain(b, 64);
    // Cross-link some non-adjacent particles to make the graph less trivial —
    // same authoring order on both bodies.
    for (std::uint32_t i = 0; i + 7 < 64; i += 5) {
        a.addDistance(i, i + 7, 2.f, 1e-4f);
        b.addDistance(i, i + 7, 2.f, 1e-4f);
    }
    a.recolor();
    b.recolor();

    bool same = a.distanceSorted.size() == b.distanceSorted.size() &&
                a.batches.size() == b.batches.size();
    for (std::size_t i = 0; same && i < a.distanceSorted.size(); ++i) {
        same = a.distanceSorted[i].a == b.distanceSorted[i].a &&
               a.distanceSorted[i].b == b.distanceSorted[i].b &&
               a.distanceSorted[i].color == b.distanceSorted[i].color &&
               a.sortedAuthoring[i] == b.sortedAuthoring[i];
    }
    check(same && colorInvariantsHold(a),
          "determinism: identical authoring yields an identical coloring + layout");
}

static void testTopologyChangeRecolors() {
    AQXPBDBody body;
    buildChain(body, 8);
    body.recolor();
    check(!body.colorsDirty && body.batches.size() == 2, "recolor clears the dirty bit (2 colors)");

    // A third constraint on particle 3 (already touched by chain constraints
    // 2-3 and 3-4) forces a 3rd color once re-colored.
    body.addDistance(3, 6, 3.f, 0.f);
    check(body.colorsDirty, "topology edit marks the coloring dirty");
    body.recolor();
    check(colorInvariantsHold(body) && body.batches.size() == 3,
          "recolor reflects the new topology (3rd color for the 3rd edge at one particle)");
}

static void testPublicSurface() {
    auto ctx = AQContext::CreateCPUOnly();
    auto space = ctx->createSpace();

    OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
    OmegaCommon::Vector<float> w;
    for (int i = 0; i < 5; ++i) {
        pos.push_back(AQvec3(0.5f * static_cast<float>(i), 2.f, 0.f));
        w.push_back(i == 0 ? 0.f : 1.f);   // pin the first
    }
    AQXPBDBodyDesc desc;
    desc.positions = pos.data();
    desc.invMass = w.data();
    desc.count = 5;

    const AQXPBDBodyHandle body = space->createXPBDBody(desc);
    check(body.valid(), "public: createXPBDBody returns a valid handle");

    AQXPBDBodyDesc bad;
    check(!space->createXPBDBody(bad).valid(), "public: empty desc is rejected");

    std::uint32_t chain[5] = {0, 1, 2, 3, 4};
    check(space->addRope(body, chain, 5, 0.f) == 4, "public: addRope chains 5 particles with 4 constraints");
    check(space->addRope(body, chain, 1, 0.f) == 0, "public: addRope rejects a 1-particle rope");

    const AQConstraintHandle extra = space->addDistanceConstraint(body, 0, 4, -1.f, 1e-3f);
    check(extra.valid() && extra.index == 4, "public: addDistanceConstraint appends at authoring index 4");
    check(!space->addDistanceConstraint(body, 0, 0, 1.f, 0.f).valid(), "public: self-pair rejected");
    check(!space->addDistanceConstraint(body, 0, 99, 1.f, 0.f).valid(), "public: out-of-range particle rejected");

    AQDistanceConstraint out[8];
    OmegaCommon::Vector<AQConstraintBatch> batches;
    const std::uint32_t n = space->readXPBDConstraints(body, out, 8, &batches);
    check(n == 5, "public: readXPBDConstraints returns all 5 constraints");
    check(!batches.empty(), "public: batch table populated (lazy recolor on read)");
    // rest length -1 derives from current spacing: particles 0 and 4 sit 2.0 apart.
    bool derivedRest = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        if ((out[i].a == 0 && out[i].b == 4) || (out[i].a == 4 && out[i].b == 0))
            derivedRest = std::fabs(out[i].restLength - 2.f) < 1e-5f;
    }
    check(derivedRest, "public: negative restLength derives from current particle distance");

    OmegaGTE::FVec<3> readPos[5] = {
        OmegaGTE::FVec<3>::Create(), OmegaGTE::FVec<3>::Create(), OmegaGTE::FVec<3>::Create(),
        OmegaGTE::FVec<3>::Create(), OmegaGTE::FVec<3>::Create() };
    check(space->readXPBDState(body, readPos, nullptr, 5) == 5 &&
          std::fabs(readPos[3][0][0] - 1.5f) < 1e-6f,
          "public: readXPBDState round-trips authored positions");

    space->destroyXPBDBody(body);
    check(space->readXPBDState(body, readPos, nullptr, 5) == 0,
          "public: destroyed handle resolves to nothing (no dangle)");
}

int main() {
    testChainColoring();
    testStarAndSharedHub();
    testDeterminism();
    testTopologyChangeRecolors();
    testPublicSurface();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
