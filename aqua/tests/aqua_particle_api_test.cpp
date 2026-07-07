// AQUA Phase 6 §13.3 6d — public particle API + one-way collision + step wiring.
//
// PUBLIC-surface test: drives everything through AQContext / AQSpace exactly as
// kREATE would (no src/ internals). Covers the §10 API round-trip, the collision
// no-tunneling hard invariant, the collision on/off toggle, and cross-run
// determinism through the public surface. The full census-based fountain
// deliverable + double oracle is 6e; this proves the wiring is real.

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQMath.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using OmegaGTE::FVec;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Build a downward-spraying point emitter above a floor plane. `collide` toggles
// one-way particle/collider collision. Returns (ctx, space, handle) via out-refs.
static AQEmitter downwardEmitter() {
    AQEmitter em;
    em.shapeKind    = AQEmitPoint;
    em.origin       = AQvec3(0.f, 3.f, 0.f);
    em.baseVelocity = AQvec3(0.f, -1.f, 0.f);
    em.rate         = 600.f;
    em.lifetime     = 3.f;
    em.mass         = 1.f;
    em.radius       = 0.05f;
    em.seed         = 0xC0FFEE;
    em.enabled      = 1;
    return em;
}

static AQForceField gravityField() {
    AQForceField f;
    f.kind = AQFieldGravity;
    f.axis = AQvec3(0.f, -1.f, 0.f);
    f.p.gravity = {9.81f};
    f.enabled = 1;
    return f;
}

static void addFloor(const SharedHandle<AQSpace>& space) {
    AQBodyDesc desc;
    desc.type        = AQBodyType::Static;
    desc.position    = AQvec3(0.f, 0.f, 0.f);
    desc.restitution = 0.3f;
    desc.shape       = space->createPlaneShape(AQvec3(0.f, 1.f, 0.f), 0.f);   // y = 0 half-space
    space->addBody(desc);
}

static void testApiRoundTrip() {
    auto ctx   = AQContext::CreateCPUOnly();
    auto space = ctx->createSpace();

    AQParticleSystemHandle sys = space->createParticleSystem(2048);
    check(sys.valid(), "createParticleSystem returns a valid handle");
    check(space->liveParticleCount(sys) == 0, "fresh system has zero live particles");

    space->addEmitter(sys, downwardEmitter());
    space->addForceField(sys, gravityField());

    for (int i = 0; i < 30; ++i) ctx->advance(1.f / 60.f);

    const std::uint32_t live = space->liveParticleCount(sys);
    check(live > 0, "after advancing, the emitter has produced live particles");

    std::vector<FVec<3>> pos(live, FVec<3>::Create());
    std::vector<float>   life(live, 0.f);
    const std::uint32_t got = space->readParticleState(sys, pos.data(), nullptr, life.data(), nullptr, live);
    check(got == live, "readParticleState returns the live count");
    bool lifetimesPositive = got > 0;
    for (std::uint32_t i = 0; i < got; ++i) lifetimesPositive = lifetimesPositive && (life[i] > 0.f);
    check(lifetimesPositive, "readback lifetimes are all positive (live set)");

    // Unknown handle is a safe zero; destroy then read back nothing.
    AQParticleSystemHandle bogus; bogus.id = 999999;
    check(space->liveParticleCount(bogus) == 0, "unknown handle -> zero live count");
    space->destroyParticleSystem(sys);
    check(space->liveParticleCount(sys) == 0, "destroyed system -> zero live count");
}

// Returns the minimum particle Y observed across the whole run.
static float runFountain(bool collide, int frames) {
    auto ctx   = AQContext::CreateCPUOnly();
    auto space = ctx->createSpace();
    addFloor(space);
    AQParticleSystemHandle sys = space->createParticleSystem(4096);
    space->addEmitter(sys, downwardEmitter());
    space->addForceField(sys, gravityField());
    space->setParticleCollisionEnabled(sys, collide);

    float minY = 1e9f;
    std::vector<FVec<3>> pos(4096, FVec<3>::Create());
    for (int i = 0; i < frames; ++i) {
        ctx->advance(1.f / 60.f);
        const std::uint32_t n = space->readParticleState(sys, pos.data(), nullptr, nullptr, nullptr, 4096);
        for (std::uint32_t p = 0; p < n; ++p) minY = std::min(minY, pos[p][1][0]);
    }
    return minY;
}

static void testCollisionNoTunnel() {
    const float minY = runFountain(/*collide=*/true, 240);
    // The floor is the y=0 plane; exact closed-form push-out must keep every live
    // particle centre on or above it (never a tunnel). Hard invariant, not a band.
    check(minY > -1e-3f, "collision ON: no particle ever tunnels below the floor plane");
}

static void testCollisionToggle() {
    const float minY = runFountain(/*collide=*/false, 240);
    // With collision OFF the same particles fall straight through under gravity.
    check(minY < -0.5f, "collision OFF: particles fall through the floor (toggle has effect)");
}

namespace {
struct Rig {
    SharedHandle<AQContext> ctx;
    SharedHandle<AQSpace>   space;
    AQParticleSystemHandle  sys;
};
Rig buildRig() {
    Rig r;
    r.ctx   = AQContext::CreateCPUOnly();
    r.space = r.ctx->createSpace();
    addFloor(r.space);
    r.sys = r.space->createParticleSystem(4096);
    r.space->addEmitter(r.sys, downwardEmitter());
    r.space->addForceField(r.sys, gravityField());
    r.space->setParticleCollisionEnabled(r.sys, true);
    return r;
}
} // namespace

static void testDeterminismThroughApi() {
    Rig a = buildRig();
    Rig b = buildRig();

    bool same = true;
    std::uint32_t lastCount = 0;
    std::vector<FVec<3>> pa(4096, FVec<3>::Create()), pb(4096, FVec<3>::Create());
    for (int i = 0; i < 120 && same; ++i) {
        a.ctx->advance(1.f / 60.f);
        b.ctx->advance(1.f / 60.f);
        const std::uint32_t na = a.space->readParticleState(a.sys, pa.data(), nullptr, nullptr, nullptr, 4096);
        const std::uint32_t nb = b.space->readParticleState(b.sys, pb.data(), nullptr, nullptr, nullptr, 4096);
        lastCount = na;
        same = same && (na == nb);                       // census matches cross-run
        for (std::uint32_t p = 0; same && p < na; ++p)   // and positions bitwise
            for (int c = 0; c < 3; ++c)
                same = same && (pa[p][c][0] == pb[p][c][0]);
    }
    check(same && lastCount > 0, "same setup + advance sequence -> bitwise-identical live sets");
}

int main() {
    testApiRoundTrip();
    testCollisionNoTunnel();
    testCollisionToggle();
    testDeterminismThroughApi();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
