// AQUA Phase 6 §13.3 6b — particle pool / free-list / stable-compaction census.
//
// This is the "genuinely hard part" (§2): a particle system fails by leaking,
// double-freeing, or miscounting — bugs with NO visible dynamics tell. So this
// test is census-based, not trajectory-based. It drives the internal
// AQParticleSystem directly (an INTERNAL test — includes src/AQSpaceImpl.h and
// links AQUA, the same shape as the GPU stage-isolation tests) and asserts the
// §9 invariants: census conservation, stable survivor order, recycle-exactly-
// once (no leak / no double-free), deterministic slot reuse, and the NaN guard.

#include "AQSpaceImpl.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// liveCount + freeCount == capacity is the exact census only when deadPending is
// zero (post-compaction / no kills yet). partitionOK holds at ALL times.
static bool censusExact(const AQParticleSystem& s) {
    return s.liveCount + s.freeCount() == s.capacity && s.deadPending() == 0;
}

static void tagPosition(AQParticleSystem& s, std::uint32_t slot, float x) {
    s.positions[slot] = AQvec3(x, 0.f, 0.f);
}
static float posX(const AQParticleSystem& s, std::uint32_t slot) {
    return s.positions[slot][0][0];
}

static void testResetAndAllocate() {
    AQParticleSystem s;
    s.reset(8);
    check(s.capacity == 8 && s.freeCount() == 8 && s.liveCount == 0,
          "reset(8): 8 free, 0 live");
    check(censusExact(s) && s.partitionOK(), "reset: census exact + partition ok");

    OmegaCommon::Vector<std::uint32_t> out;
    std::uint32_t n = s.allocate(3, out);
    check(n == 3 && out.size() == 3, "allocate(3): took 3");
    check(out[0] == 0 && out[1] == 1 && out[2] == 2,
          "allocate hands out smallest indices first (0,1,2)");
    check(s.liveCount == 3 && s.freeCount() == 5, "after allocate(3): 3 live, 5 free");
    check(censusExact(s) && s.partitionOK(), "after allocate: census exact + partition ok");

    OmegaCommon::Vector<std::uint32_t> out2;
    s.allocate(2, out2);
    check(out2[0] == 3 && out2[1] == 4, "second allocate continues ascending (3,4)");
}

static void testStableCompaction() {
    AQParticleSystem s;
    s.reset(8);
    OmegaCommon::Vector<std::uint32_t> out;
    s.allocate(8, out);                       // fill the pool
    for (std::uint32_t i = 0; i < 8; ++i) tagPosition(s, i, static_cast<float>(i) * 10.f);

    s.kill(1); s.kill(3); s.kill(5); s.kill(7);   // survivors: 0,2,4,6
    check(s.liveCount == 4 && s.freeCount() == 0 && s.deadPending() == 4,
          "after 4 kills: 4 live, 0 free, 4 dead-pending");
    check(s.partitionOK(), "mid-frame (dead-pending) still partitions ok");

    s.compact();
    check(s.liveCount == 4 && s.freeCount() == 4, "after compact: 4 live, 4 free");
    check(censusExact(s) && s.partitionOK(), "after compact: census exact + partition ok");
    // Survivors packed to the prefix IN ORDER (stable): 0,20,40,60.
    check(posX(s, 0) == 0.f && posX(s, 1) == 20.f && posX(s, 2) == 40.f && posX(s, 3) == 60.f,
          "compaction is STABLE: survivors keep relative order in the prefix");
    // The freed tail is reused smallest-first: next allocate yields 4,5.
    OmegaCommon::Vector<std::uint32_t> out2;
    s.allocate(2, out2);
    check(out2[0] == 4 && out2[1] == 5, "post-compact reuse is deterministic (4,5)");
    check(s.partitionOK(), "post-compact reuse still partitions ok (no double-free)");
}

static void testRecycleExactlyOnce() {
    // Churn many allocate/kill/compact cycles; partitionOK after every step is
    // the no-leak / no-double-free guarantee (each slot appears exactly once
    // across ALIVE ∪ free-list, no duplicates).
    AQParticleSystem s;
    s.reset(16);
    bool ok = true;
    OmegaCommon::Vector<std::uint32_t> scratch;
    for (int frame = 0; frame < 50; ++frame) {
        scratch.clear();
        s.allocate(5, scratch);
        // kill every other freshly-allocated slot
        for (std::size_t i = 0; i < scratch.size(); i += 2) s.kill(scratch[i]);
        s.compact();
        ok = ok && s.partitionOK() && censusExact(s);
    }
    check(ok, "50 allocate/kill/compact cycles: recycle exactly once, census holds every frame");
}

static void testDeterminism() {
    auto drive = [](AQParticleSystem& s) {
        s.reset(12);
        OmegaCommon::Vector<std::uint32_t> o;
        s.allocate(7, o);
        s.kill(0); s.kill(2); s.kill(6);
        s.compact();
        o.clear();
        s.allocate(4, o);
        s.kill(3);
        s.compact();
    };
    AQParticleSystem a, b;
    drive(a); drive(b);
    bool same = (a.liveCount == b.liveCount) && (a.freeList.size() == b.freeList.size());
    for (std::size_t i = 0; same && i < a.freeList.size(); ++i)
        same = same && (a.freeList[i] == b.freeList[i]);
    for (std::uint32_t i = 0; same && i < a.capacity; ++i)
        same = same && (a.flags[i] == b.flags[i]);
    check(same, "same op sequence -> identical free-list + flags (within-path determinism)");
}

static void testSaturationAndNaNGuard() {
    AQParticleSystem s;
    s.reset(4);
    OmegaCommon::Vector<std::uint32_t> out;
    std::uint32_t n = s.allocate(10, out);      // ask for more than capacity
    check(n == 4 && s.freeCount() == 0, "over-allocation caps at capacity (saturation)");
    OmegaCommon::Vector<std::uint32_t> out2;
    check(s.allocate(1, out2) == 0, "allocate on an exhausted pool takes 0");

    check(!s.anyNonFinite(), "clean pool: no non-finite particle");
    s.positions[0] = AQvec3(std::nanf(""), 0.f, 0.f);
    check(s.anyNonFinite(), "NaN in a LIVE particle is caught by the guard");
    s.kill(0);
    check(!s.anyNonFinite(), "NaN in a DEAD slot is ignored (only live scanned)");
}

int main() {
    testResetAndAllocate();
    testStableCompaction();
    testRecycleExactlyOnce();
    testDeterminism();
    testSaturationAndNaNGuard();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
