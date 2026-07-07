// AQUA Phase 6 §13.3 6c — emission, force fields, and integration.
//
// INTERNAL test: drives AQParticleSystem (src/AQSpaceImpl.h) and the scalar-
// generic particle math (src/AQParticleMath.h) directly. Covers per-field
// acceleration correctness, the semi-implicit Euler closed form, deterministic
// emission (count + fractional carry + attribute reproducibility), spawn-shape
// bounds, aging, and a whole-pipeline census smoke. Cross-path (float vs the
// double oracle) is 6e's job; here everything is the float fast-path, so
// determinism is asserted BITWISE.

#include "AQSpaceImpl.h"
#include "AQParticleMath.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

static bool vclose(const AQVec3<float>& v, float x, float y, float z, float tol = 1e-4f) {
    return std::fabs(v[0][0] - x) <= tol && std::fabs(v[1][0] - y) <= tol && std::fabs(v[2][0] - z) <= tol;
}

static AQForceField gravity(float g, float ax, float ay, float az) {
    AQForceField f; f.kind = AQFieldGravity; f.axis = AQvec3(ax, ay, az); f.p.gravity = {g}; f.enabled = 1; return f;
}

static void testFields() {
    const AQVec3<float> pos = AQvec3(4.f, 0.f, 0.f);
    const AQVec3<float> vel = AQvec3(2.f, 0.f, 0.f);

    // Gravity: g along (auto-normalized) axis, velocity-independent.
    check(vclose(AQevalField<float>(gravity(9.81f, 0.f, -1.f, 0.f), pos, vel), 0.f, -9.81f, 0.f),
          "field gravity: accel = g * normalize(axis)");
    check(vclose(AQevalField<float>(gravity(9.81f, 0.f, -5.f, 0.f), pos, vel), 0.f, -9.81f, 0.f),
          "field gravity: axis normalized (non-unit input)");

    // Drag: -k * vel.
    AQForceField drag; drag.kind = AQFieldDrag; drag.p.drag = {0.5f}; drag.enabled = 1;
    check(vclose(AQevalField<float>(drag, pos, vel), -1.f, 0.f, 0.f), "field drag: accel = -k * vel");

    // Wind: constant accel along axis * speed.
    AQForceField wind; wind.kind = AQFieldWind; wind.axis = AQvec3(1.f, 0.f, 0.f); wind.p.wind = {3.f}; wind.enabled = 1;
    check(vclose(AQevalField<float>(wind, pos, vel), 3.f, 0.f, 0.f), "field wind: accel = speed * normalize(axis)");

    // Point attractor/repulsor at the origin, no falloff.
    AQForceField pt; pt.kind = AQFieldPoint; pt.position = AQvec3(0.f, 0.f, 0.f); pt.p.point = {2.f, 0.f}; pt.enabled = 1;
    check(vclose(AQevalField<float>(pt, pos, vel), -2.f, 0.f, 0.f), "field point: +strength attracts toward centre");
    pt.p.point = {-2.f, 0.f};
    check(vclose(AQevalField<float>(pt, pos, vel), 2.f, 0.f, 0.f), "field point: -strength repels from centre");
    pt.p.point = {2.f, 0.f}; pt.radiusOfInfluence = 1.f;    // pos is 4 units out
    check(vclose(AQevalField<float>(pt, pos, vel), 0.f, 0.f, 0.f), "field point: outside radiusOfInfluence => no force");

    // Vortex about +Y at the origin: tangential, perpendicular to axis and radius.
    AQForceField vx; vx.kind = AQFieldVortex; vx.axis = AQvec3(0.f, 1.f, 0.f); vx.position = AQvec3(0.f, 0.f, 0.f);
    vx.p.vortex = {1.f, 0.f}; vx.enabled = 1;
    const AQVec3<float> va = AQevalField<float>(vx, AQvec3(2.f, 0.f, 0.f), vel);
    check(vclose(va, 0.f, 0.f, -1.f), "field vortex: swirl is tangential (⟂ axis, ⟂ radius)");
}

static void testIntegration() {
    // Semi-implicit Euler under constant accel a: v_N = a·N·dt,
    // x_N = x_0 + a·dt²·N(N+1)/2. Check against the closed form.
    AQVec3<float> pos = AQvec3(0.f, 0.f, 0.f), vel = AQvec3(0.f, 0.f, 0.f);
    const AQVec3<float> a = AQvec3(0.f, -10.f, 0.f);
    const float dt = 0.01f; const int N = 100;
    for (int i = 0; i < N; ++i) AQintegrateSemiImplicit<float>(pos, vel, a, dt);
    check(vclose(vel, 0.f, -10.f, 0.f, 1e-3f), "semi-implicit Euler: v_N = a·N·dt");
    const float expX = -10.f * dt * dt * (float(N) * (N + 1) / 2.f);   // -5.05
    check(std::fabs(pos[1][0] - expX) < 1e-2f, "semi-implicit Euler: x_N matches closed form");
}

static void testEmissionCountCarry() {
    AQParticleSystem s; s.reset(64);
    AQEmitter em; em.shapeKind = AQEmitPoint; em.origin = AQvec3(0.f, 0.f, 0.f);
    em.baseVelocity = AQvec3(0.f, 1.f, 0.f); em.rate = 10.f; em.lifetime = 100.f; em.mass = 1.f; em.seed = 7;
    s.emitters.push_back(em);

    // rate 10 over dt 0.05 = 0.5 particles/frame -> 0,1,0,1,... totalling 10 in 20 frames.
    std::uint32_t total = 0, prevLive = 0;
    for (int frame = 0; frame < 20; ++frame) {
        s.emit(0.05f, 0.05f);
        total += (s.liveCount - prevLive);
        prevLive = s.liveCount;
    }
    check(total == 10, "emission count: fractional carry accumulates to rate*time exactly (10)");
    check(s.partitionOK() && s.liveCount == 10, "emission fills the pool consistently (census ok)");
}

static void testEmissionDeterminism() {
    auto build = []() {
        AQEmitter em; em.shapeKind = AQEmitBox; em.shape.box = {1.f, 2.f, 3.f};
        em.origin = AQvec3(5.f, 0.f, 0.f); em.baseVelocity = AQvec3(0.f, 8.f, 0.f);
        em.speedJitter = 1.5f; em.dirJitterRad = 0.3f; em.rate = 50.f;
        em.lifetime = 2.f; em.lifetimeJitter = 0.4f; em.mass = 1.f; em.radius = 0.1f; em.seed = 0xABCDEF; em.enabled = 1;
        return em;
    };
    AQParticleSystem a, b; a.reset(64); b.reset(64);
    a.emitters.push_back(build()); b.emitters.push_back(build());
    for (int f = 0; f < 5; ++f) { a.emit(0.02f, 0.02f); b.emit(0.02f, 0.02f); }

    bool same = (a.liveCount == b.liveCount);
    for (std::uint32_t s = 0; same && s < a.liveCount; ++s) {
        for (int c = 0; c < 3; ++c) {
            same = same && (a.positions[s][c][0]  == b.positions[s][c][0]);
            same = same && (a.velocities[s][c][0] == b.velocities[s][c][0]);
        }
        same = same && (a.lifetime[s] == b.lifetime[s]);
    }
    check(a.liveCount > 0 && same, "emission attributes are bitwise reproducible for the same seed");
}

static void testSpawnBounds() {
    // Box spawn stays within half-extents about the origin.
    AQParticleSystem s; s.reset(256);
    AQEmitter box; box.shapeKind = AQEmitBox; box.shape.box = {1.f, 2.f, 3.f};
    box.origin = AQvec3(10.f, 0.f, 0.f); box.baseVelocity = AQvec3(0.f, 1.f, 0.f);
    box.rate = 5000.f; box.lifetime = 100.f; box.mass = 1.f; box.seed = 3; box.enabled = 1;
    s.emitters.push_back(box);
    s.emit(0.03f, 0.03f);
    bool inBox = s.liveCount > 20;
    for (std::uint32_t p = 0; p < s.liveCount; ++p) {
        inBox = inBox && std::fabs(s.positions[p][0][0] - 10.f) <= 1.f + 1e-4f
                       && std::fabs(s.positions[p][1][0] - 0.f)  <= 2.f + 1e-4f
                       && std::fabs(s.positions[p][2][0] - 0.f)  <= 3.f + 1e-4f;
    }
    check(inBox, "box emitter: all spawns lie within the half-extents");

    // Sphere spawn stays within the radius.
    AQParticleSystem s2; s2.reset(256);
    AQEmitter sph; sph.shapeKind = AQEmitSphere; sph.shape.sphere = {2.f};
    sph.origin = AQvec3(0.f, 0.f, 0.f); sph.baseVelocity = AQvec3(0.f, 1.f, 0.f);
    sph.rate = 5000.f; sph.lifetime = 100.f; sph.mass = 1.f; sph.seed = 4; sph.enabled = 1;
    s2.emitters.push_back(sph);
    s2.emit(0.03f, 0.03f);
    bool inSphere = s2.liveCount > 20;
    for (std::uint32_t p = 0; p < s2.liveCount; ++p) {
        const float r = std::sqrt(OmegaGTE::dot(s2.positions[p], s2.positions[p]));
        inSphere = inSphere && (r <= 2.f + 1e-4f);
    }
    check(inSphere, "sphere emitter: all spawns lie within the radius");
}

static void testAging() {
    AQParticleSystem s; s.reset(8);
    AQEmitter em; em.shapeKind = AQEmitPoint; em.origin = AQvec3(0.f, 0.f, 0.f);
    em.baseVelocity = AQvec3(0.f, 0.f, 0.f); em.rate = 1000.f; em.lifetime = 0.1f; em.lifetimeJitter = 0.f;
    em.mass = 1.f; em.seed = 9; em.enabled = 1;
    s.emitters.push_back(em);
    s.emit(0.01f, 0.05f);                  // spawn a handful; sub-step 0.05
    const std::uint32_t born = s.liveCount;
    check(born > 0, "aging setup: some particles born");
    // Integer death (6f): lifetime 0.1 over sub-step 0.05 freezes to a
    // countdown of ceil(0.1/0.05) = 2 age() calls at emission.
    s.age(0.05f);
    check(s.liveCount == born, "aging: lifetime 0.1 survives the first 0.05 sub-step");
    s.age(0.05f);                          // second of the 2 scheduled sub-steps
    check(s.liveCount == 0, "aging: particles die on their scheduled sub-step (integer countdown)");
}

static void testPipelineSmoke() {
    // A fountain-ish loop: emit once/frame, several sub-steps of integrate+age,
    // compact once/frame. Census + partition + NaN invariants must hold every
    // frame — the §9 net that has no visible dynamics tell.
    AQParticleSystem s; s.reset(4096);
    AQEmitter em; em.shapeKind = AQEmitCone; em.shape.cone = {0.2f, 0.25f};
    em.origin = AQvec3(0.f, 0.f, 0.f); em.baseVelocity = AQvec3(0.f, 12.f, 0.f);
    em.speedJitter = 2.f; em.dirJitterRad = 0.25f; em.rate = 3000.f; em.lifetime = 1.2f; em.lifetimeJitter = 0.2f;
    em.mass = 1.f; em.radius = 0.05f; em.seed = 0x1234; em.enabled = 1;
    s.emitters.push_back(em);
    s.fields.push_back(gravity(9.81f, 0.f, -1.f, 0.f));
    AQForceField drag; drag.kind = AQFieldDrag; drag.p.drag = {0.1f}; drag.enabled = 1;
    s.fields.push_back(drag);

    const float frameDt = 1.f / 60.f; const int subSteps = 2; const float subDt = frameDt / subSteps;
    bool ok = true; std::uint32_t peak = 0;
    for (int frame = 0; frame < 120; ++frame) {
        s.emit(frameDt, subDt);
        for (int sub = 0; sub < subSteps; ++sub) { s.accumulateAndIntegrate(subDt); s.age(subDt); }
        s.compact();
        ok = ok && s.partitionOK() && !s.anyNonFinite();
        if (s.liveCount > peak) peak = s.liveCount;
    }
    check(ok, "fountain pipeline: partition + census + no-NaN hold every frame");
    check(peak > 100 && s.liveCount > 0, "fountain pipeline: sustains a live population");
}

int main() {
    testFields();
    testIntegration();
    testEmissionCountCarry();
    testEmissionDeterminism();
    testSpawnBounds();
    testAging();
    testPipelineSmoke();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
