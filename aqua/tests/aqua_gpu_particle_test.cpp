// AQUA Phase 6h/6i — the LIVE GPU particle path vs the double oracle (§14.3).
//
// Unlike the rigid pillar's stage-isolation tests, this drives the PUBLIC
// surface end to end: AQContext::loadKernels flips the particle pillar live,
// setExecutionPath(GPU) selects it, and advance() runs AQUA's first live GPU
// step — host-side deterministic bookkeeping (seeded emission, integer death,
// census, compaction permutation) + device-side float physics (field
// integrate, SDF collide, stable scan/scatter compaction).
//
// Acceptance mirrors the 6e CPU deliverable (§9): the census must match the
// double oracle EXACTLY every frame (integer bookkeeping is shared, so
// count AND slot assignment agree by construction — any mismatch is a real
// bug); trajectories carry the float band; the no-tunnel invariant is checked
// on the GPU readback against the analytic SDF; the GPU path is run-to-run
// deterministic (byte-identical readbacks); and the pool sustains scale.
//
// INTERNAL test (the oracle shares src/AQParticleMath.h +
// src/AQParticleCollision.h), device-guarded, skips on headless CI.

#include "AQSpaceImpl.h"          // AQParticleCollider spec types
#include "AQParticleMath.h"
#include "AQParticleCollision.h"

#include <aqua/AQContext.h>
#include <aqua/AQSpace.h>
#include <aqua/AQMath.h>

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GE.h>
#include <omegaGTE/GECommandQueue.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using OmegaGTE::FVec;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// ---------------------------------------------------------------------------
// Double-precision oracle — the 6e reference with the 6f integer death model,
// replaying the exact ACAC schedule AQContext::advance runs.
// ---------------------------------------------------------------------------
struct ColliderD { AQShape shape; AQTransform<double> xform; double restitution; };

struct Oracle {
    std::uint32_t capacity = 0;
    std::vector<AQVec3<double>> pos, vel;
    std::vector<double> invMass, life, rad;
    std::vector<std::uint32_t> cd;
    std::vector<std::uint32_t> flags, freeList;
    std::uint32_t liveCount = 0;

    std::vector<AQEmitter>    emitters;
    std::vector<AQForceField> fields;
    std::vector<ColliderD>    colliders;
    std::vector<double>        carry;
    std::vector<std::uint64_t> ordinal;
    bool collide = false;

    float accumulator = 0.f;
    float fixedDt     = 1.f / 120.f;   // AQContext default

    void reset(std::uint32_t cap) {
        capacity = cap;
        const AQVec3<double> z = AQvec3<double>(0, 0, 0);
        pos.assign(cap, z); vel.assign(cap, z);
        invMass.assign(cap, 0.0); life.assign(cap, 0.0); rad.assign(cap, 0.0);
        cd.assign(cap, 0u);
        flags.assign(cap, AQParticleDead);
        freeList.resize(cap);
        for (std::uint32_t i = 0; i < cap; ++i) freeList[i] = cap - 1u - i;
        liveCount = 0;
    }
    std::uint32_t allocate(std::uint32_t k, std::vector<std::uint32_t>& out) {
        const std::uint32_t avail = static_cast<std::uint32_t>(freeList.size());
        const std::uint32_t n = (k < avail) ? k : avail;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t s = freeList.back(); freeList.pop_back();
            flags[s] = AQParticleAlive; out.push_back(s); ++liveCount;
        }
        return n;
    }
    void kill(std::uint32_t s) {
        if (s >= capacity || (flags[s] & AQParticleAlive) == 0u) return;
        flags[s] = AQParticleDead; --liveCount;
    }
    void compact() {
        std::uint32_t w = 0;
        for (std::uint32_t r = 0; r < capacity; ++r) {
            if ((flags[r] & AQParticleAlive) == 0u) continue;
            if (w != r) { pos[w] = pos[r]; vel[w] = vel[r]; invMass[w] = invMass[r];
                          life[w] = life[r]; cd[w] = cd[r]; rad[w] = rad[r];
                          flags[w] = AQParticleAlive; }
            ++w;
        }
        for (std::uint32_t i = w; i < capacity; ++i) flags[i] = AQParticleDead;
        freeList.clear();
        for (std::uint32_t i = capacity; i-- > w; ) freeList.push_back(i);
        liveCount = w;
    }
    void emit(float dt) {
        if (carry.size()   != emitters.size()) carry.resize(emitters.size(), 0.0);
        if (ordinal.size() != emitters.size()) ordinal.resize(emitters.size(), 0u);
        std::vector<std::uint32_t> slots;
        for (std::size_t e = 0; e < emitters.size(); ++e) {
            const AQEmitter& em = emitters[e];
            if (!em.enabled) continue;
            const std::uint32_t count = AQemitCount(em.rate, dt, carry[e]);
            if (count == 0) continue;
            slots.clear();
            const std::uint32_t got = allocate(count, slots);
            for (std::uint32_t i = 0; i < got; ++i) {
                const std::uint64_t ord = ordinal[e]++;
                AQParticleRng rng(AQmixSeed(em.seed, ord));
                const std::uint32_t s = slots[i];
                pos[s]     = AQsampleEmitPosition<double>(em, rng);
                vel[s]     = AQsampleEmitVelocity<double>(em, rng);
                life[s]    = AQsampleLifetime<double>(em, rng);
                cd[s]      = AQdeathCountdown(life[s], fixedDt);
                invMass[s] = (em.mass > 0.f) ? (1.0 / em.mass) : 0.0;
                rad[s]     = em.radius;
            }
        }
    }
    void integrate(double dt) {
        for (std::uint32_t s = 0; s < capacity; ++s) {
            if ((flags[s] & AQParticleAlive) == 0u) continue;
            AQVec3<double> a = AQvec3<double>(0, 0, 0);
            for (const AQForceField& f : fields) a = a + AQevalField<double>(f, pos[s], vel[s]);
            AQintegrateSemiImplicit<double>(pos[s], vel[s], a, dt);
        }
    }
    void collidePass() {
        for (std::uint32_t s = 0; s < capacity; ++s) {
            if ((flags[s] & AQParticleAlive) == 0u) continue;
            const double r = rad[s];
            for (const ColliderD& c : colliders) {
                const AQShapeSampleT<double> sd = AQshapeSignedDistanceGeneric<double>(c.shape, pos[s], c.xform);
                if (sd.distance >= r) continue;
                pos[s] = pos[s] + sd.normal * (r - sd.distance);
                const double vn = OmegaGTE::dot(vel[s], sd.normal);
                if (vn < 0.0) vel[s] = vel[s] - sd.normal * ((1.0 + c.restitution) * vn);
            }
        }
    }
    void age() {
        for (std::uint32_t s = 0; s < capacity; ++s) {
            if ((flags[s] & AQParticleAlive) == 0u) continue;
            if (cd[s] <= 1u) { cd[s] = 0u; kill(s); }
            else             { --cd[s]; }
        }
    }
    void advance(float realDt) {
        if (realDt <= 0.f) return;
        if (realDt > 0.25f) realDt = 0.25f;
        accumulator += realDt;
        int nSub = 0;
        for (float acc = accumulator; acc >= fixedDt; acc -= fixedDt) ++nSub;
        const float simDt = static_cast<float>(nSub) * fixedDt;
        if (nSub > 0) emit(simDt);
        while (accumulator >= fixedDt) {
            integrate(static_cast<double>(fixedDt));
            if (collide) collidePass();
            age();
            accumulator -= fixedDt;
        }
        if (nSub > 0) compact();
    }
};

// ---------------------------------------------------------------------------
// Shared scene builders (identical for production and oracle).
// ---------------------------------------------------------------------------
static AQEmitter fountainEmitter(float rate) {
    AQEmitter em;
    em.shapeKind    = AQEmitCone;
    em.shape.cone   = {0.15f, 0.3f};
    em.origin       = AQvec3(0.f, 0.2f, 0.f);
    em.baseVelocity = AQvec3(0.f, 9.f, 0.f);
    em.speedJitter  = 1.5f;
    em.dirJitterRad = 0.35f;
    em.rate         = rate;
    em.lifetime     = 1.5f;
    em.lifetimeJitter = 0.2f;
    em.mass         = 1.f;
    em.radius       = 0.04f;
    em.seed         = 0x5EED1234ull;
    em.enabled      = 1;
    return em;
}
static AQForceField gravityF() {
    AQForceField f; f.kind = AQFieldGravity; f.axis = AQvec3(0.f, -1.f, 0.f); f.p.gravity = {9.81f}; f.enabled = 1; return f;
}
static AQForceField dragF() {
    AQForceField f; f.kind = AQFieldDrag; f.p.drag = {0.15f}; f.enabled = 1; return f;
}

struct ColliderSpec { AQShape shape; FVec<3> pos; float restitution; };
static std::vector<ColliderSpec> fountainColliders() {
    std::vector<ColliderSpec> v;
    AQShape floor; floor.type = AQShapeType::Plane;  floor.plane   = {0.f, 1.f, 0.f, 0.f};
    AQShape box;   box.type   = AQShapeType::Box;     box.box       = {1.f, 0.5f, 1.f};
    AQShape sph;   sph.type   = AQShapeType::Sphere;  sph.sphere    = {0.8f};
    v.push_back({floor, AQvec3(0.f, 0.f, 0.f), 0.3f});
    v.push_back({box,   AQvec3(1.5f, 0.5f, 0.f), 0.2f});
    v.push_back({sph,   AQvec3(-1.5f, 0.8f, 0.f), 0.4f});
    return v;
}
static AQShapeHandle registerShape(const SharedHandle<AQSpace>& space, const AQShape& s) {
    switch (s.type) {
    case AQShapeType::Sphere: return space->createSphereShape(s.sphere.radius);
    case AQShapeType::Box:    return space->createBoxShape(AQvec3(s.box.hx, s.box.hy, s.box.hz));
    case AQShapeType::Plane:  return space->createPlaneShape(AQvec3(s.plane.nx, s.plane.ny, s.plane.nz), s.plane.offset);
    case AQShapeType::Capsule:return space->createCapsuleShape(s.capsule.radius, s.capsule.halfHeight);
    default: return AQShapeHandle{};
    }
}
static AQTransform<double> xformD(const FVec<3>& p) {
    AQTransform<double> t; t.p = AQvec3<double>(p[0][0], p[1][0], p[2][0]); return t;
}

// A GPU-path context: engine + queue + kernels loaded + GPU selected.
static SharedHandle<AQContext> makeGpuContext(OmegaGTE::GTE& gte, bool& ok) {
    OmegaGTE::GECommandQueueDesc queueDesc;
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto ctx = AQContext::Create(gte.graphicsEngine, queue);
    ok = ctx->loadKernels(OmegaCommon::String(AQUA_KERNELS_LIB_PATH));
    ctx->setExecutionPath(AQExecPath::GPU);
    ok = ok && (ctx->executionPath() == AQExecPath::GPU);
    return ctx;
}

// ---------------------------------------------------------------------------
// Test 1 (6h) — collision-free fountain: census exact + trajectory band.
// ---------------------------------------------------------------------------
static void testGpuFreeFlight(OmegaGTE::GTE& gte) {
    const std::uint32_t CAP = 8192;
    const float RATE = 4000.f;
    bool gpuOk = false;
    auto ctx = makeGpuContext(gte, gpuOk);
    check(gpuOk, "loadKernels + setExecutionPath(GPU) resolve the live GPU path");
    auto space = ctx->createSpace();
    AQParticleSystemHandle sys = space->createParticleSystem(CAP);
    space->addEmitter(sys, fountainEmitter(RATE));
    space->addForceField(sys, gravityF());
    space->addForceField(sys, dragF());

    Oracle oracle; oracle.reset(CAP); oracle.collide = false;
    oracle.emitters.push_back(fountainEmitter(RATE));
    oracle.fields.push_back(gravityF());
    oracle.fields.push_back(dragF());

    double maxPos = 0.0, maxVel = 0.0;
    std::uint32_t peak = 0;
    bool censusExact = true;
    std::vector<FVec<3>> pp(CAP, FVec<3>::Create()), pv(CAP, FVec<3>::Create());

    for (int frame = 0; frame < 90; ++frame) {
        ctx->advance(1.f / 60.f);
        oracle.advance(1.f / 60.f);

        const std::uint32_t nLive = space->readParticleState(sys, pp.data(), pv.data(), nullptr, nullptr, CAP);
        censusExact = censusExact && (nLive == oracle.liveCount);
        peak = (nLive > peak) ? nLive : peak;

        const std::uint32_t n = (nLive < oracle.liveCount) ? nLive : oracle.liveCount;
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                maxPos = std::max(maxPos, std::fabs(double(pp[i][c][0]) - oracle.pos[i][c][0]));
                maxVel = std::max(maxVel, std::fabs(double(pv[i][c][0]) - oracle.vel[i][c][0]));
            }
        }
    }

    std::printf("    [measure] GPU free-flight max |pos - oracle| = %.3e, max |vel - oracle| = %.3e, peak live = %u\n",
                maxPos, maxVel, peak);
    check(censusExact, "GPU census matches the double oracle exactly every frame (count + slots)");
    check(maxPos < 5e-3, "GPU per-particle position tracks the double oracle within the float band");
    check(maxVel < 5e-3, "GPU per-particle velocity tracks the double oracle within the float band");
    check(peak > 3000, "GPU fountain sustains a large live population");
}

// ---------------------------------------------------------------------------
// Test 2 (6h) — within-path determinism: two full GPU runs, byte-identical.
// ---------------------------------------------------------------------------
static void testGpuDeterminism(OmegaGTE::GTE& gte) {
    auto runOnce = [&](std::string& bytes) -> bool {
        const std::uint32_t CAP = 4096;
        bool gpuOk = false;
        auto ctx = makeGpuContext(gte, gpuOk);
        auto space = ctx->createSpace();
        AQParticleSystemHandle sys = space->createParticleSystem(CAP);
        space->addEmitter(sys, fountainEmitter(2000.f));
        space->addForceField(sys, gravityF());
        space->addForceField(sys, dragF());
        for (int frame = 0; frame < 45; ++frame) ctx->advance(1.f / 60.f);
        std::vector<FVec<3>> pp(CAP, FVec<3>::Create()), pv(CAP, FVec<3>::Create());
        const std::uint32_t n = space->readParticleState(sys, pp.data(), pv.data(), nullptr, nullptr, CAP);
        bytes.clear();
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float p = pp[i][c][0], v = pv[i][c][0];
                bytes.append(reinterpret_cast<const char*>(&p), sizeof(float));
                bytes.append(reinterpret_cast<const char*>(&v), sizeof(float));
            }
        }
        return gpuOk && n > 0;
    };
    std::string a, b;
    const bool ranA = runOnce(a);
    const bool ranB = runOnce(b);
    check(ranA && ranB, "two independent GPU runs complete");
    check(!a.empty() && a == b, "two GPU runs are byte-identical (within-path determinism)");
}

// ---------------------------------------------------------------------------
// Test 3 (6i) — the full fountain: floor + box + sphere on-device collision.
// ---------------------------------------------------------------------------
static void testGpuCollisionScene(OmegaGTE::GTE& gte) {
    const std::uint32_t CAP = 8192;
    const float RATE = 4000.f;
    bool gpuOk = false;
    auto ctx = makeGpuContext(gte, gpuOk);
    auto space = ctx->createSpace();

    const std::vector<ColliderSpec> specs = fountainColliders();
    for (const ColliderSpec& s : specs) {
        AQBodyDesc d; d.type = AQBodyType::Static; d.position = s.pos; d.restitution = s.restitution;
        d.shape = registerShape(space, s.shape);
        space->addBody(d);
    }
    AQParticleSystemHandle sys = space->createParticleSystem(CAP);
    space->addEmitter(sys, fountainEmitter(RATE));
    space->addForceField(sys, gravityF());
    space->addForceField(sys, dragF());
    space->setParticleCollisionEnabled(sys, true);

    Oracle oracle; oracle.reset(CAP); oracle.collide = true;
    oracle.emitters.push_back(fountainEmitter(RATE));
    oracle.fields.push_back(gravityF());
    oracle.fields.push_back(dragF());
    for (const ColliderSpec& s : specs) oracle.colliders.push_back({s.shape, xformD(s.pos), s.restitution});

    std::vector<AQTransform<float>> xf;
    for (const ColliderSpec& s : specs) { AQTransform<float> t; t.p = s.pos; xf.push_back(t); }

    double worstPenetration = 0.0;
    double maxPos = 0.0;
    std::uint32_t peak = 0;
    bool censusExact = true, noTunnel = true;
    std::vector<FVec<3>> pp(CAP, FVec<3>::Create());

    for (int frame = 0; frame < 120; ++frame) {
        ctx->advance(1.f / 60.f);
        oracle.advance(1.f / 60.f);

        const std::uint32_t nLive = space->readParticleState(sys, pp.data(), nullptr, nullptr, nullptr, CAP);
        censusExact = censusExact && (nLive == oracle.liveCount);
        peak = (nLive > peak) ? nLive : peak;

        const std::uint32_t n = (nLive < oracle.liveCount) ? nLive : oracle.liveCount;
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                maxPos = std::max(maxPos, std::fabs(double(pp[i][c][0]) - oracle.pos[i][c][0]));
            }
        }
        for (std::uint32_t i = 0; i < nLive; ++i) {
            for (std::size_t c = 0; c < specs.size(); ++c) {
                const AQShapeSample sd = AQshapeSignedDistance(specs[c].shape, pp[i], xf[c]);
                if (sd.distance < 0.f) worstPenetration = std::min(worstPenetration, double(sd.distance));
                if (sd.distance < -2e-3f) noTunnel = false;
            }
        }
    }

    std::printf("    [measure] GPU collision scene: worst penetration = %.3e, max |pos - oracle| = %.3e, peak live = %u\n",
                worstPenetration, maxPos, peak);
    check(noTunnel, "GPU path: no live particle ever tunnels the floor / box / sphere");
    check(censusExact, "GPU collision-scene census matches the oracle every frame");
    // Bounce trajectories are contact-ordering sensitive (a float-band
    // difference at a contact flips a reflection a sub-step earlier/later),
    // so the collision scene pins the band looser than free flight and the
    // hard invariants above carry the correctness weight.
    check(maxPos < 5e-2, "GPU collision-scene positions track the oracle within the contact band");
    check(peak > 3000, "GPU collision fountain sustains a large live population");
}

// ---------------------------------------------------------------------------
// Test 4 — scale on the GPU path (no per-frame readback: census is host-side).
// ---------------------------------------------------------------------------
static void testGpuScale(OmegaGTE::GTE& gte) {
    const std::uint32_t CAP = 262144;
    bool gpuOk = false;
    auto ctx = makeGpuContext(gte, gpuOk);
    auto space = ctx->createSpace();
    AQParticleSystemHandle sys = space->createParticleSystem(CAP);
    space->addEmitter(sys, fountainEmitter(180000.f));
    space->addForceField(sys, gravityF());

    std::uint32_t peak = 0;
    for (int frame = 0; frame < 45; ++frame) {
        ctx->advance(1.f / 60.f);
        const std::uint32_t live = space->liveParticleCount(sys);
        peak = (live > peak) ? live : peak;
    }
    std::printf("    [measure] GPU scale peak live = %u (capacity %u)\n", peak, CAP);
    check(peak > 100000, "GPU scale: sustains > 100k live particles");
    check(peak < CAP,    "GPU scale: fixed pool is never exceeded");
}

int main() {
    std::printf("== AQUA Phase 6h/6i: live GPU particle path vs the double oracle ==\n");

    OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
        OmegaGTE::enumerateDevices();
    if (devices.empty()) {
        std::printf("[SKIP] no GTE device on this host — GPU particle test skipped\n");
        return 0;
    }
    OmegaGTE::GTE gte = OmegaGTE::Init(devices[0]);
    check(static_cast<bool>(gte.graphicsEngine), "GTE initialized a graphics engine");

    testGpuFreeFlight(gte);
    testGpuDeterminism(gte);
    testGpuCollisionScene(gte);
    testGpuScale(gte);

    OmegaGTE::Close(gte);

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
