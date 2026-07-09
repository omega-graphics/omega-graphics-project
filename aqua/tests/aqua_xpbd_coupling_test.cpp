// AQUA Phase 7g §13.6 — rigid↔XPBD contact coupling, driven through the PUBLIC
// AQContext/AQSpace surface. The Phase 6 one-way particle push-out generalized to
// XPBD bodies AND made two-way: XPBD particles read rigid collider poses and are
// pushed out of penetration; dynamic rigid bodies receive the reaction impulse
// (the seam Phases 8/9 build cloth/solid↔rigid collision on).
//
//   1. One-way parity: free XPBD particles fall onto a STATIC box / plane and
//      settle on the surface — no tunnelling (static colliders reproduce the
//      Phase 6 push-out; they receive no reaction).
//   2. Two-way momentum: a dynamic sphere (no gravity) strikes a FREE XPBD
//      particle — total linear momentum is conserved across the seam and both
//      end up moving. Control (coupling off) passes straight through.
//   3. Two-way support: a dynamic sphere falls onto a bed of PINNED XPBD
//      particles and is HELD UP against gravity by the reaction. Control falls
//      through.
//   4. Determinism + finiteness of the coupled solve.

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}
static float vlen(const OmegaGTE::FVec<3>& v) { return std::sqrt(OmegaGTE::dot(v, v)); }

// ---------------------------------------------------------------------------
// 1 — one-way: free particles settle on a static collider, no tunnelling.
// ---------------------------------------------------------------------------

static void testOneWayStatic() {
    AQXPBDParams params;
    params.substeps = 8;
    params.velocityDamping = 0.1f;      // settle the landed particles

    // (a) static box, half-extent 0.5 at the origin (top face at y = 0.5).
    {
        auto ctx = AQContext::CreateCPUOnly();
        ctx->setFixedTimestep(1.f / 60.f);
        auto sp = ctx->createSpace();
        sp->setXPBDParams(params);
        auto boxShape = sp->createBoxShape(AQvec3(0.5f, 0.5f, 0.5f));
        AQBodyDesc bd; bd.type = AQBodyType::Static; bd.shape = boxShape;
        sp->addBody(bd);

        OmegaCommon::Vector<OmegaGTE::FVec<3>> pos;
        OmegaCommon::Vector<float>             w;
        for (int gx = -1; gx <= 1; ++gx)
            for (int gz = -1; gz <= 1; ++gz) {
                pos.push_back(AQvec3(0.2f * float(gx), 1.2f, 0.2f * float(gz)));
                w.push_back(1.f);
            }
        AQXPBDBodyDesc desc;
        desc.positions = pos.data(); desc.invMass = w.data();
        desc.count = static_cast<std::uint32_t>(pos.size());
        auto body = sp->createXPBDBody(desc);
        sp->setXPBDCollisionEnabled(body, true, 0.02f);

        for (int f = 0; f < 6 * 60; ++f) ctx->advance(1.f / 60.f);
        OmegaCommon::Vector<OmegaGTE::FVec<3>> op(desc.count, OmegaGTE::FVec<3>::Create());
        OmegaCommon::Vector<OmegaGTE::FVec<3>> ov(desc.count, OmegaGTE::FVec<3>::Create());
        sp->readXPBDState(body, op.data(), ov.data(), desc.count);
        float minY = 1e9f, maxY = -1e9f, maxSpd = 0.f;
        for (std::uint32_t i = 0; i < desc.count; ++i) {
            minY = std::min(minY, op[i][1][0]);
            maxY = std::max(maxY, op[i][1][0]);
            maxSpd = std::max(maxSpd, vlen(ov[i]));
        }
        check(minY > 0.45f && maxY < 0.62f, "one-way box: particles settle on the box top (no tunnel)");
        check(maxSpd < 0.2f, "one-way box: cluster comes to rest");
    }

    // (b) static ground plane (normal +Y at y = 0).
    {
        auto ctx = AQContext::CreateCPUOnly();
        ctx->setFixedTimestep(1.f / 60.f);
        auto sp = ctx->createSpace();
        sp->setXPBDParams(params);
        auto planeShape = sp->createPlaneShape(AQvec3(0.f, 1.f, 0.f), 0.f);
        AQBodyDesc pd; pd.type = AQBodyType::Static; pd.shape = planeShape;
        sp->addBody(pd);

        OmegaGTE::FVec<3> p = AQvec3(0.f, 1.2f, 0.f);
        float w = 1.f;
        AQXPBDBodyDesc desc; desc.positions = &p; desc.invMass = &w; desc.count = 1;
        auto body = sp->createXPBDBody(desc);
        sp->setXPBDCollisionEnabled(body, true, 0.02f);

        for (int f = 0; f < 6 * 60; ++f) ctx->advance(1.f / 60.f);
        OmegaGTE::FVec<3> rp = OmegaGTE::FVec<3>::Create(), rv = OmegaGTE::FVec<3>::Create();
        sp->readXPBDState(body, &rp, &rv, 1);
        check(rp[1][0] > -0.02f && rp[1][0] < 0.1f, "one-way plane: particle settles at the surface");
        check(vlen(rv) < 0.2f, "one-way plane: particle comes to rest");
    }
}

// ---------------------------------------------------------------------------
// 2 — two-way momentum: a dynamic sphere strikes a free particle (no gravity),
//     momentum is conserved across the seam.
// ---------------------------------------------------------------------------

static void testTwoWayMomentum() {
    auto ctx = AQContext::CreateCPUOnly();
    ctx->setFixedTimestep(1.f / 60.f);
    auto sp = ctx->createSpace();
    sp->setGravity(AQvec3(0.f, 0.f, 0.f));      // isolate the exchange
    AQXPBDParams params; params.substeps = 8;
    sp->setXPBDParams(params);

    // Dynamic sphere, mass 1, radius 0.3, moving +x at 2 m/s.
    auto sphereShape = sp->createSphereShape(0.3f);
    AQBodyDesc bd;
    bd.type = AQBodyType::Dynamic; bd.mass = 1.f; bd.shape = sphereShape;
    bd.position = AQvec3(-1.f, 0.f, 0.f);
    bd.linearVelocity = AQvec3(2.f, 0.f, 0.f);
    auto sphere = sp->addBody(bd);

    // One free XPBD particle (mass 1) in the sphere's path at the origin.
    OmegaGTE::FVec<3> p = AQvec3(0.f, 0.f, 0.f);
    float w = 1.f;
    AQXPBDBodyDesc desc; desc.positions = &p; desc.invMass = &w; desc.count = 1;
    auto body = sp->createXPBDBody(desc);
    sp->setXPBDCollisionEnabled(body, true, 0.f);

    const float p0 = 1.f * 2.f;                 // initial total x-momentum
    for (int f = 0; f < 2 * 60; ++f) ctx->advance(1.f / 60.f);

    OmegaGTE::FVec<3> pp = OmegaGTE::FVec<3>::Create(), pv = OmegaGTE::FVec<3>::Create();
    sp->readXPBDState(body, &pp, &pv, 1);
    const float sphereVx = sphere->velocity()[0][0];
    const float partVx   = pv[0][0];
    const float pEnd = 1.f * sphereVx + 1.f * partVx;
    std::printf("       momentum: p0=%.3f pEnd=%.3f (sphere vx=%.3f, particle vx=%.3f)\n",
                p0, pEnd, sphereVx, partVx);
    check(std::abs(pEnd - p0) < 0.05f, "two-way: total linear momentum conserved across the seam");
    check(sphereVx < 1.95f && sphereVx > 0.f, "two-way: the sphere is slowed by the particle");
    check(partVx > 0.3f, "two-way: the particle is driven forward by the sphere");

    // Control: coupling off — the sphere passes straight through, particle stays.
    auto ctx2 = AQContext::CreateCPUOnly();
    ctx2->setFixedTimestep(1.f / 60.f);
    auto sp2 = ctx2->createSpace();
    sp2->setGravity(AQvec3(0.f, 0.f, 0.f));
    sp2->setXPBDParams(params);
    auto ss = sp2->createSphereShape(0.3f);
    AQBodyDesc bd2 = bd; bd2.shape = ss;
    auto sphere2 = sp2->addBody(bd2);
    OmegaGTE::FVec<3> p2 = AQvec3(0.f, 0.f, 0.f); float w2 = 1.f;
    AQXPBDBodyDesc d2; d2.positions = &p2; d2.invMass = &w2; d2.count = 1;
    sp2->createXPBDBody(d2);                            // collision NOT enabled
    for (int f = 0; f < 2 * 60; ++f) ctx2->advance(1.f / 60.f);
    check(sphere2->velocity()[0][0] > 1.99f, "control: coupling off, the sphere is unimpeded");
}

// ---------------------------------------------------------------------------
// 3 — two-way support: a single pinned particle catches a falling dynamic
//     sphere and holds it against gravity (one vertical contact — the reaction
//     sustains the body's weight; multi-point resting on a curved surface needs
//     a PGS-grade simultaneous solve, deferred, §13.6).
// ---------------------------------------------------------------------------

struct BedRig {
    SharedHandle<AQContext> ctx;
    SharedHandle<AQSpace>   space;
    SharedHandle<AQRigidBody> ball;
    AQXPBDBodyHandle body;
};

static BedRig makeBedRig(bool couple) {
    BedRig rig;
    rig.ctx = AQContext::CreateCPUOnly();
    rig.ctx->setFixedTimestep(1.f / 60.f);
    rig.space = rig.ctx->createSpace();
    AQXPBDParams params; params.substeps = 16;
    rig.space->setXPBDParams(params);

    // Dynamic sphere (mass 1, radius 0.3) dropped from y = 1.0 straight down.
    auto sphereShape = rig.space->createSphereShape(0.3f);
    AQBodyDesc bd;
    bd.type = AQBodyType::Dynamic; bd.mass = 1.f; bd.shape = sphereShape;
    bd.position = AQvec3(0.f, 1.0f, 0.f);
    rig.ball = rig.space->addBody(bd);

    // One pinned particle directly below the sphere centre — a single vertical
    // contact (normal stays -Y as the sphere descends, so no glancing/spin).
    OmegaGTE::FVec<3> p = AQvec3(0.f, 0.f, 0.f);
    float w = 0.f;                       // pinned
    AQXPBDBodyDesc desc;
    desc.positions = &p; desc.invMass = &w; desc.count = 1;
    rig.body = rig.space->createXPBDBody(desc);
    if (couple) rig.space->setXPBDCollisionEnabled(rig.body, true, 0.03f);
    return rig;
}

static void testTwoWaySupport() {
    BedRig rig = makeBedRig(true);
    for (int f = 0; f < 4 * 60; ++f) rig.ctx->advance(1.f / 60.f);
    const float y = rig.ball->position()[1][0];
    const float spd = vlen(rig.ball->velocity());
    std::printf("       support: ball y=%.4f speed=%.4f\n", y, spd);
    // The sphere rests on the particle at centre ≈ radius (0.3) + slop.
    check(y > 0.2f, "two-way: the pinned particle holds the dynamic sphere up against gravity");
    check(spd < 0.2f, "two-way: the held sphere settles");

    BedRig off = makeBedRig(false);
    for (int f = 0; f < 4 * 60; ++f) off.ctx->advance(1.f / 60.f);
    check(off.ball->position()[1][0] < -0.5f, "control: coupling off, the sphere falls through");
}

// ---------------------------------------------------------------------------
// 3b — Coulomb friction: on a horizontal plane under ANGLED gravity, a high-μ
//      particle sticks while a frictionless one slides down-slope.
// ---------------------------------------------------------------------------

static float slideX(float friction) {
    auto ctx = AQContext::CreateCPUOnly();
    ctx->setFixedTimestep(1.f / 60.f);
    auto sp = ctx->createSpace();
    sp->setGravity(AQvec3(3.f, -9.81f, 0.f));     // tangential + normal components
    AQXPBDParams params; params.substeps = 8; params.velocityDamping = 0.02f;
    sp->setXPBDParams(params);
    auto planeShape = sp->createPlaneShape(AQvec3(0.f, 1.f, 0.f), 0.f);
    AQBodyDesc pd; pd.type = AQBodyType::Static; pd.shape = planeShape;
    sp->addBody(pd);

    OmegaGTE::FVec<3> p = AQvec3(0.f, 0.3f, 0.f);
    float w = 1.f;
    AQXPBDBodyDesc desc; desc.positions = &p; desc.invMass = &w; desc.count = 1;
    auto body = sp->createXPBDBody(desc);
    sp->setXPBDCollisionEnabled(body, true, 0.02f, friction);

    for (int f = 0; f < 3 * 60; ++f) ctx->advance(1.f / 60.f);
    OmegaGTE::FVec<3> rp = OmegaGTE::FVec<3>::Create(), rv = OmegaGTE::FVec<3>::Create();
    sp->readXPBDState(body, &rp, &rv, 1);
    return rp[0][0];
}

static void testFriction() {
    const float xStick = slideX(0.8f);   // μ·N (7.85) > tangential gravity (3) ⇒ holds
    const float xSlide = slideX(0.f);    // frictionless ⇒ accelerates down-slope
    std::printf("       friction: x(mu=0.8)=%.3f  x(mu=0)=%.3f\n", xStick, xSlide);
    check(std::abs(xStick) < 0.2f, "friction: high-mu particle sticks against tangential gravity");
    check(xSlide > 0.5f, "friction: frictionless particle slides freely down-slope");
    check(xSlide > xStick + 0.5f, "friction: mu clearly resists tangential sliding");
}

// ---------------------------------------------------------------------------
// 4 — determinism + finiteness.
// ---------------------------------------------------------------------------

static void testDeterminismFinite() {
    auto runOnce = [](OmegaGTE::FVec<3>& ball, OmegaGTE::FVec<3>& part) {
        BedRig rig = makeBedRig(true);
        for (int f = 0; f < 2 * 60; ++f) rig.ctx->advance(1.f / 60.f);
        ball = rig.ball->position();
        OmegaGTE::FVec<3> p = OmegaGTE::FVec<3>::Create(), v = OmegaGTE::FVec<3>::Create();
        rig.space->readXPBDState(rig.body, &p, &v, 1);
        part = p;
    };
    OmegaGTE::FVec<3> bA = OmegaGTE::FVec<3>::Create(), bB = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> pA = OmegaGTE::FVec<3>::Create(), pB = OmegaGTE::FVec<3>::Create();
    runOnce(bA, pA); runOnce(bB, pB);
    bool identical = true;
    for (int c = 0; c < 3; ++c) {
        if (bA[c][0] != bB[c][0]) identical = false;
        if (pA[c][0] != pB[c][0]) identical = false;
    }
    check(identical, "determinism: two coupled runs are bitwise-identical");

    bool finite = std::isfinite(bA[1][0]) && std::isfinite(pA[0][0]);
    check(finite, "finite: no NaN/Inf escapes the coupled solve");
}

int main() {
    std::printf("=== AQUA Phase 7g — rigid<->XPBD contact coupling ===\n");
    testOneWayStatic();
    testTwoWayMomentum();
    testTwoWaySupport();
    testFriction();
    testDeterminismFinite();
    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
