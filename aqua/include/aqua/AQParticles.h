#ifndef AQUA_AQPARTICLES_H
#define AQUA_AQPARTICLES_H

// AQUA Phase 6 — particle-system POD types (Phase-6 brief §7, corrected per the
// §13.1 substrate audit). AQUA-owned, AQ-prefixed (no namespace, per AGENTS.md).
// Every type here is trivially-copyable / standard-layout so an SoA buffer or a
// parameter block uploads to a GPU buffer with NO repacking — the same
// discipline AQShape and AQDebugLine follow. No virtuals, no backend types.
//
// Correction to the §7 draft: there is no `AQVec3f` in this engine (a "Vec3" is
// the borrowed template `OmegaGTE::Matrix<Ty,3,1>`; there is no non-template
// alias). Vector members are `OmegaGTE::FVec<3>` — the public-API convention the
// rest of AQUA uses (AQShape's free functions, AQDebugLine). FVec's default ctor
// is private, so FVec members are default-initialized via its `Create()` factory
// (the AQDebugLine idiom); this keeps the struct trivially COPYABLE (what a
// memcpy upload needs) even though the default ctor is non-trivial. Primitive
// variant params stay RAW FLOATS inside the unions so the unions carry no FVec
// (whose private default ctor cannot be a union member — the AQShape rule).

#include "AQBase.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>

// --- Particle flags --------------------------------------------------------
// `flags` is a uint32 bitfield (bit 0 = alive). uint, never bool: the GPU path
// has no bool storage class worth depending on, and the census invariants (§9)
// count ALIVE lanes. Higher bits are reserved for user tags.
enum AQParticleFlags : std::uint32_t {
    AQParticleDead  = 0u,
    AQParticleAlive = 1u << 0,
};

// --- Emitter ---------------------------------------------------------------
enum AQEmitShapeKind : std::uint32_t {
    AQEmitPoint  = 0,
    AQEmitSphere = 1,   ///< radius
    AQEmitBox    = 2,   ///< half-extents
    AQEmitCone   = 3,   ///< radius + half-angle (fountains)
    AQEmitDisc   = 4,   ///< radius (flat)
};

// POD emitter (Reeves 1983 attributes). Primitive params live as raw floats in
// a union so the whole struct stays trivially-copyable and blits to a GPU
// constant buffer. The FVec members (origin/baseVelocity) sit OUTSIDE the union.
struct AQEmitter {
    AQEmitShapeKind shapeKind = AQEmitPoint;
    union {
        struct { float _pad; }                 point;
        struct { float radius; }               sphere;
        struct { float hx, hy, hz; }           box;
        struct { float radius, halfAngleRad; } cone;
        struct { float radius; }               disc;
    } shape;
    OmegaGTE::FVec<3> origin       = OmegaGTE::FVec<3>::Create();  ///< emitter placement, world space
    OmegaGTE::FVec<3> baseVelocity = OmegaGTE::FVec<3>::Create();  ///< initial velocity direction * speed
    float    speedJitter    = 0.f;   ///< +/- spread on speed
    float    dirJitterRad   = 0.f;   ///< cone half-angle of direction spread
    float    rate           = 0.f;   ///< particles per second
    float    lifetime       = 1.f;   ///< seconds
    float    lifetimeJitter = 0.f;   ///< +/- spread on lifetime
    float    mass           = 1.f;   ///< per-particle mass (invMass = 1/mass)
    float    radius         = 0.f;   ///< per-particle collision radius
    std::uint64_t seed      = 0;     ///< deterministic RNG seed (per-emitter)
    std::uint32_t enabled   = 1;     ///< 0/1 (no bool on the GPU path)

    AQEmitter() : shapeKind(AQEmitPoint) { shape.point = {0.f}; }
};

// --- Force field -----------------------------------------------------------
enum AQForceFieldKind : std::uint32_t {
    AQFieldGravity = 0,   ///< uniform accel (axis * g)
    AQFieldDrag    = 1,   ///< -k * vel
    AQFieldWind    = 2,   ///< push toward a target velocity along axis * speed
    AQFieldVortex  = 3,   ///< swirl about position+axis
    AQFieldPoint   = 4,   ///< attractor (+) / repulsor (-) at position
};

// POD tagged union. Raw floats inside the union (no FVec in the variants) so it
// stays standard-layout and GPU-uploadable; the FVec members are outside it.
struct AQForceField {
    AQForceFieldKind kind = AQFieldGravity;
    OmegaGTE::FVec<3> position = OmegaGTE::FVec<3>::Create();  ///< vortex axis origin / point centre
    OmegaGTE::FVec<3> axis     = OmegaGTE::FVec<3>::Create();  ///< gravity/wind/vortex direction
    union {
        struct { float g; }                 gravity;   ///< magnitude
        struct { float k; }                 drag;      ///< coefficient
        struct { float speed; }             wind;      ///< strength
        struct { float strength, falloff; } vortex;    ///< swirl + 1/r^n
        struct { float strength, falloff; } point;     ///< +attract / -repel
    } p;
    float    radiusOfInfluence = 0.f;  ///< 0 == infinite
    std::uint32_t enabled      = 1;    ///< 0/1

    AQForceField() : kind(AQFieldGravity) { p.gravity = {0.f}; }
};

// --- Particle pool (SoA view) ----------------------------------------------
// Parallel arrays; the actual backing storage is owned behind the pimpl (the
// CPU host SoA in Phase 6; pooled GTE buffers when the GPU path lands). These
// pointers are VIEWS for the CPU path / oracle / readback. One index == one
// particle across ALL arrays.
struct AQParticlePool {
    OmegaGTE::FVec<3>* positions  = nullptr;   ///< [capacity]
    OmegaGTE::FVec<3>* velocities = nullptr;   ///< [capacity]
    OmegaGTE::FVec<3>* accels     = nullptr;   ///< [capacity]  (scratch, per-step)
    float*             invMass    = nullptr;   ///< [capacity]
    float*             lifetime   = nullptr;   ///< [capacity]  (remaining, seconds)
    float*             radius     = nullptr;   ///< [capacity]
    std::uint32_t*     flags      = nullptr;   ///< [capacity]  (AQParticleAlive / AQParticleDead | user bits)
    std::uint32_t*     freeList   = nullptr;   ///< [capacity]  (stack of free slot indices)
    std::uint32_t      capacity   = 0;         ///< fixed backing size
    std::uint32_t      liveCount  = 0;         ///< packed live prefix length after compaction
    std::uint32_t      freeCount  = 0;         ///< top of freeList stack
};

// Opaque handle handed back to callers (kREATE). The real system state lives
// behind the pimpl; this is a stable ID, never a pointer into the pool, so a
// caller can never dangle into a pool that compaction relocated.
struct AQParticleSystemHandle {
    std::uint64_t id = 0;
    AQUA_NODISCARD bool valid() const { return id != 0; }
};

#endif // AQUA_AQPARTICLES_H
