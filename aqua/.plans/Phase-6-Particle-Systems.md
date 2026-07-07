# AQUA Phase 6 — Particle Systems

**Prior-art brief & proposal.** This document covers the sixth roadmap phase of AQUA: simulating large populations of mass points — the *Particle pillar*. It surveys how the incumbent engines (PhysX/FleX, Chaos/Niagara) approach particles, states the canonical literature we build on (Reeves, Sims, Green, Teschner/Ihmsen, Macklin), locates the openings AQUA's compute substrate opens up, and proposes a concrete pipeline plus the new AQ-prefixed POD types it needs. In scope: particle *pools* (allocation, recycling, stream compaction), *emitters*, *force fields*, one-way *particle↔collider* collision against Phase 2 static geometry, and *neighbor queries* reusing the Phase 2 grid. Out of scope, by design: the *unified-particle solver* (rigid/cloth/fluid as constrained particles — that is Phase 7's fork, flagged here as the forward link but not adopted), two-way coupling (Phase 9), and all rendering/visual concerns (kREATE's job — AQUA simulates and hands back state). This brief follows the conventions of the Phase 2 and Phase 5 briefs; §11's open decisions should be settled before implementation begins.

---

## 1. Scope & deliverable

**Goal.** Simulate large populations of mass points — tens to hundreds of thousands of independent particles — entirely on the Phase 5 compute substrate, with a CPU fallback at parity. This is the pillar the compute substrate pays off on most: particles are the canonical *embarrassingly-parallel* physics workload — no order-dependent joints, no colored constraint graph, just a per-element `advect → collide → age → recycle` loop that maps one particle to one lane.

**Runnable deliverable.** A GPU-simulated **particle fountain**: an emitter spraying tens to hundreds of thousands of particles upward under gravity + drag, falling back and colliding (one-way) with a Phase 2 static scene — a floor plane plus a few boxes/spheres. Particles push out of colliders via `AQshapeSupport`/signed-distance; expired particles are recycled into the free-list; new particles stream in at the emitter rate. Paired with it: a **headless test** whose reference oracle is a *slow, single-threaded, obviously-correct `double`-precision CPU particle stepper* that the production GPU/CPU-fast paths must match within tolerance. The oracle's correctness is legible by inspection — one particle, one force accumulation, one substep, one collider push-out, in the order a human would do it on paper — so a divergence is always the fast path's fault, never the oracle's.

**Included groundwork (lands first).**
- `include/aqua/AQParticles.h` — the POD types (§7): `AQParticlePool`, `AQEmitter`, `AQForceField`, opaque `AQParticleSystemHandle`.
- Free-list allocator + stream-compaction of dead particles built on the Phase 5 Blelloch scan primitive (stable compaction — deterministic survivor order).
- New debug-draw flag bits `AQDebugParticle` and `AQDebugForceField` appended to the existing `AQDebug.h` bus (no new bus).

**What earlier phases have already closed for us.**
- **Phase 2** shipped the sort-based uniform-grid broadphase (Green 2010): hash cell → radix-sort `(cellHash,index)` → scan cell runs, plus `AQShape` (POD tagged union sphere/box/capsule/plane/hull), `AQAABB<Ty>`/`FAABB`, `AQshapeSupport(...)`, `AQBroadphasePair`, `AQCollisionFilter`. Phase 6 **reuses this grid verbatim** for particle neighbor queries and reuses `AQShape`/`AQshapeSupport` for particle↔collider push-out — we add *nothing* to the collider representation. Phase 2's own recency audit flagged **compact hashing (Teschner 2003; Ihmsen 2011)** as the Phase-6 particle-pillar memory-layout swap on top of the same sort-based grid; we pick that up here (§6, §11).
- **Phase 5** shipped the compute execution substrate: `AQComputeBackend` (pimpl-only owner of engine + queue), `AQExecPath` + `setExecutionPath`/`executionPath`, `AQContext::Create(engine, queue)` (engine required) and `AQContext::CreateCPUOnly()`, SoA body buffers over pooled GTE buffers, and kernels precompiled from `src/kernels/*.omegasl` → `AQKernels.omegasllib` → `loadShaderLibrary`. It also shipped the two GPU primitives GTE does not supply: a **Blelloch scan** and a **Merrill–Grimshaw radix sort**. Determinism stance: *stable-cross-path, bitwise-within-path*. Phase 6 dispatches through this same buffer-pool + command-encode machinery — in Phase 5's own words, "so Phase 6's particles dispatch through the same substrate without a rewrite." Stream compaction is *just another scan*; the neighbor grid is *just another radix sort*. We are assembling parts, not forging them.

**Out of scope here, by design.** Unified-particle constraints (Phase 7), two-way particle→collider momentum transfer (Phase 9), SPH pressure/viscosity forces (a fluids sub-pillar, later), GPU particle *rendering* and sprite/mesh instancing (kREATE), and any emission *visual* (color-over-life, texture atlases — kREATE reads AQUA's raw state and decorates it).

## 2. Why particle systems are their own problem

1. **Population is unbounded and churning.** Rigid-body counts are authored and roughly stable; particle counts are *emergent* — an emitter births thousands per second and lifetimes kill thousands per second, so the live set is a moving window over a fixed backing pool. The cost model is dominated not by per-particle physics (trivial) but by **allocation, recycling, and compaction** of that window. A solver that nails the integration but leaks pool slots or compacts non-deterministically has failed at the part that is actually hard.

2. **The failure modes are demographic, not dynamic.** A rigid solver fails by drifting, jittering, or exploding. A particle system fails by *leaking* (dead particles never recycled → pool exhaustion → silent emission stall), *double-freeing* (a slot recycled twice → two live particles alias one backing index), or *miscounting* (total count diverges from `emitted − expired`). These are bookkeeping bugs with no visible dynamics tell — the fountain still looks like a fountain right up until the pool is full and emission silently stops. Validation (§9) is therefore *census-based*: conserve the count, recycle each slot exactly once.

3. **The parallelism is real but the compaction is the catch.** Advection is perfectly parallel — one particle, one lane, no cross-talk. But *deleting* dead particles from a packed SoA buffer is a stream-compaction (prefix-sum) problem, and doing it **stably** (survivors keep their relative order across frames) is what makes the whole thing deterministic across the CPU/GPU divide. Unstable compaction — the naïve "atomic append survivors" — reorders survivors by lane-scheduling luck and breaks bitwise reproducibility. The parallel win is free; the parallel *bookkeeping* is the design work.

4. **Neighbor queries scale differently than colliders.** A few thousand rigid bodies fit a dense uniform grid comfortably. Hundreds of thousands of particles in a fountain occupy a *sparse* region of a huge domain — a dense grid over the bounding volume is mostly empty cells and blows the memory budget. The neighbor structure has to be **sparse-first** (compact hashing), which is a different memory layout than the rigid grid even though it sits on the same sort-based skeleton.

## 3. Prior art — how the incumbents solve it

*Descriptions below are representative of the published/observable design of these systems, not quotations, and may lag their current internals.*

**NVIDIA PhysX 5 / FleX.** PhysX's particle story runs through **FleX** (Macklin et al. 2014), whose entire representation is *"everything is a particle"*: rigids, cloth, fluids, and gas are all populations of mass points bound by position-based constraints solved in a unified Gauss–Seidel-style loop. Neighbor finding is a GPU sort-based grid (the Green 2010 lineage). This is powerful and unified — but it is a *constraint solver*, and its per-step cost is dominated by constraint iteration, not by the free advection AQUA's Phase 6 targets. FleX is the destination of AQUA's *Phase 7*, not Phase 6: we reach it by first shipping the pool/emitter/field substrate and only then layering constraints on top.

**Epic Chaos (+ Niagara for particles).** Chaos is Unreal's rigid/destruction solver; particle *systems* in Unreal live in **Niagara**, a GPU-simulation graph where emitters, spawn modules, and force modules compile to compute shaders. Niagara is a general VFX authoring system — its strength is artist-facing modularity (stack any force/spawn/event module), and it runs particle sim on the GPU with sort-based neighbor grids for the cases that need them. Collision against scene geometry is typically approximate (depth-buffer / distance-field collision) rather than the exact analytic push-out AQUA does against `AQShape`. Niagara is a *superset* of what Phase 6 delivers, weighted toward visual authoring; AQUA's Phase 6 is deliberately the *simulation core* only, with kREATE owning the authoring/visual layer Niagara fuses in.

**The shared shape.** Strip the branding and all three converge on the same skeleton: **SoA particle state in GPU buffers**, a **sort-based uniform grid** for neighbor queries (Green 2010), **force accumulation → integration** per particle per lane, and **GPU-side lifetime/recycling**. The disagreements are at the edges — FleX makes *everything* a constrained particle (Phase 7 territory), Niagara makes *everything* an artist-composable module (kREATE territory), and both approximate collision where AQUA can afford exact analytic push-out because it already owns `AQshapeSupport` from Phase 2. Phase 6 adopts the shared skeleton and diverges only where AQUA's existing substrate gives it a cheaper, more exact, or more deterministic option.

## 4. The literature we build on

- **Reeves 1983** — *"Particle Systems—A Technique for Modeling a Class of Fuzzy Objects"* (ACM TOG 2(2)). The foundational emitter/lifetime model: particles are born at an emitter with randomized initial attributes, age over a lifetime, and die. Every emitter parameter in §7's `AQEmitter` (shape, rate, initial-velocity spread, lifetime) traces directly to this paper.
- **Sims 1990** — *"Particle Animation and Rendering Using Data Parallel Computation"* (SIGGRAPH '90). The data-parallel formulation: one particle per processing element, forces accumulated independently, no inter-particle dependency in the base loop. This is *exactly* the shape of AQUA's advection kernel — Sims is the reason the base step maps to one particle per GPU lane with zero contention.
- **Green 2010** — *"Particle Simulation using CUDA"* (NVIDIA technical report). The GPU sort-based uniform-grid neighbor search: hash each particle to a cell, radix-sort by cell hash, scan for cell start/end runs. Already Phase 2's broadphase — Phase 6 reuses it directly.
- **Teschner et al. 2003** — *"Optimized Spatial Hashing for Collision Detection of Deformable Objects"* (VMV '03). Hash 3D cells into a 1D table with a spatial hash function, sidestepping a dense grid over the whole domain. This is the *sparse-set* answer for hundreds of thousands of particles occupying a small fraction of a large volume.
- **Ihmsen et al. 2011** — *"A Parallel SPH Implementation on Multi-core CPUs"* (Computer Graphics Forum 30(1)). Compact hashing built on the same sort/scan skeleton: store only occupied cells, keep a handle array so the neighbor scan touches only live cells — the memory-layout swap Phase 2's audit earmarked for this phase.
- **Macklin et al. 2014** — *"Unified Particle Physics for Real-Time Applications"* (FleX, ACM TOG 33(4)). The unified-particle representation — rigids/cloth/fluids as constrained mass points in one solver. **Forward link only:** this is Phase 7's fork, cited here so the boundary is explicit, *not* adopted in Phase 6.
- **Blelloch 1990** — *"Prefix Sums and Their Applications"* (CMU technical report / *Synthesis of Parallel Algorithms* chapter). The work-efficient parallel scan underpinning stream compaction. Phase 5 already shipped a Blelloch scan; Phase 6's dead-particle compaction is an application of it.
- **Survey grounding — Bender, Koschier, Weiler et al.** — the SPH/position-based STAR surveys (e.g. *"Smoothed Particle Hydrodynamics Techniques for the Physics Based Simulation of Fluids and Solids"*, Eurographics tutorial/STAR 2019) — consulted for the modern consensus on hashing choices and neighbor-structure trade-offs at scale.

**Throughline.** The classical line (Reeves → Sims) established *what* a particle system is; the parallel line (Green → Teschner/Ihmsen) established *how* to make its neighbor queries scale on data-parallel hardware. AQUA's Phase 5 substrate — SoA GPU buffers, a Blelloch scan, a Merrill–Grimshaw radix sort — is precisely the machine those parallel papers assume. We are not porting their ideas onto a foreign runtime; the runtime was built (Phase 5) with these ideas as the target workload.

## 5. Where AQUA diverges — the openings

- **We already own the neighbor grid.** Green 2010 is not a thing we implement for Phase 6 — it *is* Phase 2's broadphase. Particles hash into the same grid, and the compact-hashing variant (Teschner/Ihmsen) is a *layout swap* on the same radix-sort skeleton, not a new algorithm. Rigid bodies and particles can even share one grid build when they coexist in a scene.
- **We already own scan and sort.** Phase 5 shipped Blelloch scan and Merrill–Grimshaw radix sort as AQUA primitives (GTE supplies neither). Stream compaction of dead particles is one scan; the neighbor grid is one sort. Phase 6 adds *zero* new GPU primitives — it composes the two it inherited.
- **Exact analytic collision, not depth-buffer approximation.** Because Phase 2 gives us `AQshapeSupport` and signed-distance against `AQShape`, particle↔collider push-out is *exact* against the analytic shape (sphere/box/capsule/plane/hull), not a screen-space or SDF approximation the way Niagara typically does it. A particle never tunnels a floor plane because the plane's signed distance is closed-form.
- **Cross-path determinism is a first-class constraint, not a nicety.** Phase 5's *stable-cross-path, bitwise-within-path* stance means Phase 6's compaction must be **stable** (survivors keep relative order) and emission must be deterministic (same seed → same particles in the same slots). Most VFX particle systems don't care about this — they're decorative. AQUA cares because a `double` CPU oracle has to match the GPU path within tolerance for the deliverable's test to mean anything.
- **Simulation-only boundary.** AQUA hands back raw particle state (positions, velocities, ages, flags) and stops. No color-over-life, no sprite atlas, no mesh instancing — kREATE consumes the SoA readback and renders. This keeps `AQParticles.h` free of any visual concept and keeps the pool a pure physics structure.

**Gaps we must fill.** Phase 6 must add, on top of the inherited substrate: (a) the SoA particle pool with a free-list allocator and stable stream-compaction recycling; (b) the emitter model (Reeves attributes, deterministic seeded spawn); (c) the force-field tagged union and its accumulation kernel; (d) the particle↔collider push-out kernel over `AQShape`; and (e) the compact-hashing neighbor layout for sparse large populations. None of these needs a new GPU primitive — they need new *kernels* over the existing ones.

## 6. Proposed algorithm — the "advect–collide–age–compact" pipeline (ACAC)

The per-step pipeline is four per-element-parallel passes plus a per-frame emission/compaction bookkeeping pass. Every pass maps one particle (or one grid cell) to one lane; there are no order-dependent atomics on the float path (per Phase 5's determinism stance).

```
# Fixed sub-step dt_sub; N_sub sub-steps per frame (deterministic).
step(dt):
  for s in 0 .. N_sub-1:                      # fixed sub-step loop (deterministic)

    # --- Pass A: emit (bookkeeping, per-frame at s==0) ---
    if s == 0:
      k = emitter.rate * dt (seeded, deterministic count)
      slots = freelist.pop(k)                  # exact k slots, sorted ascending
      for each new slot (one lane per slot):   # Reeves attributes, seeded RNG
        pool.pos[slot]  = sampleEmitShape(emitter, seed, slot)
        pool.vel[slot]  = emitter.baseVel + sampleSpread(emitter, seed, slot)
        pool.life[slot] = emitter.lifetime + sampleLifeJitter(...)
        pool.invMass[slot] = 1/emitter.mass
        pool.flags[slot] = ALIVE

    # --- Pass B: accumulate forces (one particle per lane) ---
    for p in live:                             # Sims 1990: no inter-particle dep
      f = 0
      for field in forceFields:                # gravity/drag/wind/vortex/point
        f += evalField(field, pool.pos[p], pool.vel[p], pool.invMass[p])
      pool.accel[p] = f * pool.invMass[p]      # (drag reads vel; still per-lane)

    # --- Pass C: integrate (semi-implicit Euler, per lane) ---
    for p in live:
      pool.vel[p] += pool.accel[p] * dt_sub
      pool.pos[p] += pool.vel[p]   * dt_sub

    # --- Pass D: collide vs Phase-2 static shapes (one-way, per lane) ---
    for p in live:
      grab candidate colliders from Phase-2 grid cell of pool.pos[p]
      for shape in candidates:
        d = signedDistance(shape, pool.pos[p])        # AQshapeSupport / SDF
        if d < radius:
          n = surfaceNormal(shape, pool.pos[p])
          pool.pos[p] += n * (radius - d)             # push out (exact)
          vn = dot(pool.vel[p], n)
          if vn < 0: pool.vel[p] -= (1+restitution)*vn*n   # reflect/damp

    # --- Pass E: age + mark dead (per lane) ---
    for p in live:
      pool.life[p] -= dt_sub
      if pool.life[p] <= 0: pool.flags[p] = DEAD

  # --- Pass F: compact (once per frame) ---
  flags_alive = (pool.flags == ALIVE) ? 1 : 0
  offsets = Blelloch_scan(flags_alive)         # stable prefix sum (Phase 5)
  stable_scatter live particles to packed prefix; push freed slots to freelist
  rebuild Phase-2 grid over live particles (radix sort, compact-hash layout)
```

**Why these choices.**
- **Semi-implicit (symplectic) Euler** for integration: cheap, one force eval per sub-step, and stable under gravity+drag at the sub-step sizes a fountain needs. Particles are not stiff constraint systems — there is no case for an implicit solve here. (If a specific field ever demands it, that's a per-field concern, not a pipeline change.)
- **Fixed sub-step, seeded emission** so the `double` oracle and the GPU path see the identical count and identical spawn attributes each frame — the precondition for the deliverable's tolerance test to be meaningful.
- **Stable stream compaction via Blelloch scan** (Pass F) rather than atomic-append: survivors keep relative order, so CPU and GPU produce the same packed buffer and the free-list evolves identically. This is the single most important determinism choice in the phase.
- **Collision reuses the Phase-2 grid + `AQshapeSupport`** (Pass D): no new broadphase, exact analytic push-out, no tunneling of the floor because the plane distance is closed-form. One-way only in Phase 6 — the collider is read, never written.
- **Force fields as a tagged union evaluated per-lane** (Pass B): each field kind is a branch in one kernel; drag/vortex read velocity but only the *particle's own* velocity, so the pass stays per-lane with no cross-talk.

**Alternative considered — always-compact (no free-list).** Instead of a free-list with periodic compaction, keep the pool densely packed at all times: every dead particle triggers an immediate compaction. This is simpler to reason about (the live set is always `[0, count)`) but pays a full scan every frame regardless of churn and makes emission a append-then-resort. The free-list + periodic-compaction path (chosen) amortizes the scan and lets emission be an O(k) slot-pop. We keep always-compact in our back pocket as the fallback if free-list fragmentation ever proves pathological (§11.1).

## 7. New types AQUA must add — `include/aqua/AQParticles.h` (draft)

```cpp
// include/aqua/AQParticles.h
// AQUA-owned, AQ-prefixed, no namespace. All POD / trivially-copyable /
// standard-layout so an SoA buffer uploads to a GPU buffer with NO repacking.
// No virtuals anywhere on this data path. Vec3 is AQUA-owned (AQVec3f);
// Matrix/Quaternion, when needed, are borrowed from OmegaGTE's GTEMath.h.
// Backend types (devices, queues, OmegaSL) never appear here (pimpl discipline).

#include "AQMath.h"     // AQVec3<Ty>, AQVec3f  (AQUA-owned)
#include "AQShape.h"    // AQShape, AQAABB (Phase 2)
#include <cstdint>

// --- Emitter ---------------------------------------------------------------
enum AQEmitShapeKind : uint32_t {
    AQEmitPoint  = 0,
    AQEmitSphere = 1,   // radius
    AQEmitBox    = 2,   // half-extents
    AQEmitCone   = 3,   // radius + half-angle (fountains)
    AQEmitDisc   = 4,   // radius (flat)
};

// POD emitter. Primitive params live as RAW FLOATS inside a union so the
// whole struct is trivially-copyable and blits to a GPU constant buffer.
struct AQEmitter {
    AQEmitShapeKind shapeKind;
    union {
        struct { float _pad; }                              point;
        struct { float radius; }                            sphere;
        struct { float hx, hy, hz; }                        box;
        struct { float radius, halfAngleRad; }              cone;
        struct { float radius; }                            disc;
    } shape;
    AQVec3f  origin;          // emitter placement in world space
    AQVec3f  baseVelocity;    // initial velocity direction * speed
    float    speedJitter;     // +/- spread on speed
    float    dirJitterRad;    // cone half-angle of direction spread
    float    rate;            // particles per second
    float    lifetime;        // seconds
    float    lifetimeJitter;  // +/- spread on lifetime
    float    mass;            // per-particle mass (invMass = 1/mass)
    float    radius;          // per-particle collision radius
    uint64_t seed;            // deterministic RNG seed (per-emitter)
    uint32_t enabled;         // 0/1 (no bool on the GPU path)
};

// --- Force field -----------------------------------------------------------
enum AQForceFieldKind : uint32_t {
    AQFieldGravity  = 0,   // uniform accel (dir * g)
    AQFieldDrag     = 1,   // -k * vel
    AQFieldWind     = 2,   // uniform vel target + strength
    AQFieldVortex   = 3,   // swirl about an axis
    AQFieldPoint    = 4,   // attractor (+) / repulsor (-) at a point
};

// POD tagged union. RAW FLOATS inside the union — no AQVec3f members inside
// the variants, so it stays standard-layout and GPU-uploadable.
struct AQForceField {
    AQForceFieldKind kind;
    AQVec3f  position;        // used by vortex axis origin / point center
    AQVec3f  axis;            // used by gravity/wind/vortex direction
    union {
        struct { float g; }                       gravity;   // magnitude
        struct { float k; }                       drag;      // coefficient
        struct { float speed; }                   wind;      // strength
        struct { float strength, falloff; }       vortex;    // swirl + 1/r^n
        struct { float strength, falloff; }       point;     // +attract/-repel
    } p;
    float    radiusOfInfluence;  // 0 == infinite
    uint32_t enabled;            // 0/1
};

// --- Particle pool (SoA) ---------------------------------------------------
// The pool is described here as parallel arrays; the actual backing buffers
// are pooled GTE buffers owned behind the pimpl (Phase 5 machinery). These
// pointers are views for the CPU path / oracle; the GPU path binds the same
// SoA as compute buffers. One index == one particle across ALL arrays.
struct AQParticlePool {
    AQVec3f* positions;    // [capacity]
    AQVec3f* velocities;   // [capacity]
    AQVec3f* accels;       // [capacity]  (scratch, per-step)
    float*   invMass;      // [capacity]
    float*   lifetime;     // [capacity]  (remaining, seconds)
    float*   radius;       // [capacity]
    uint32_t* flags;       // [capacity]  (ALIVE=1 / DEAD=0 | user bits)
    uint32_t* freeList;    // [capacity]  (stack of free slot indices)
    uint32_t  capacity;    // fixed backing size
    uint32_t  liveCount;   // packed live prefix length after compaction
    uint32_t  freeCount;   // top of freeList stack
};

// Opaque handle handed back to callers (kREATE). The real system state lives
// behind the pimpl; this is a stable ID, never a pointer into the pool.
struct AQParticleSystemHandle { uint64_t id; };
```

These types are POD by intent, not by accident. `AQEmitter` and `AQForceField` are `std::is_trivially_copyable` and `std::is_standard_layout` because every variant param is a raw `float` inside a union — no `AQVec3f` *inside* the union (the `AQVec3f` members that do appear, like `origin`/`position`/`axis`, are themselves trivially-copyable POD and sit outside the union so alignment is predictable). That means an array of emitters or fields uploads to a GPU constant/structured buffer with a plain `memcpy` — no serialization, no repacking, no per-field marshalling. `AQParticlePool` is SoA precisely so each attribute is a contiguous buffer the compute kernel can bind independently and stream coalesced. `bool` never appears on this path — `enabled`/`flags` are `uint32_t` because the GPU has no bool storage class we want to depend on. `AQParticleSystemHandle` is a bare `uint64_t` ID rather than a pointer so the caller can never dangle into a pool that compaction relocated — the pimpl resolves the ID to live state each call.

## 8. Data layout & GPU/numeric specialization

**SoA, always.** The pool is nine parallel arrays (§7), not an array of a `Particle` struct. Advection reads `positions`/`velocities`, writes `accels`, then `positions`/`velocities` — each a coalesced streaming access. An AoS `Particle{pos,vel,accel,...}` would force each lane to stride over the whole struct to touch one field; SoA is the difference between memory-bound-at-peak and memory-bound-at-a-fraction. The CPU fallback uses the *same* SoA (it's just as cache-friendly single-threaded), which keeps the oracle and the fast path reading identical memory.

**Pooling on the Phase 5 buffer pool.** The nine arrays are pooled GTE buffers acquired through Phase 5's `AQComputeBackend` pool — Phase 6 does not allocate raw device memory. Growing the pool (rare — capacity is fixed at creation by design) goes through the same pool. Command encoding for all six passes (§6) rides Phase 5's command-encode machinery. Phase 6 adds kernels, not infrastructure.

**Free-list + stream compaction.** Recycling is a stack (`freeList`, `freeCount`): compaction (Pass F) scatters live particles to a packed prefix via a Blelloch scan and pushes vacated slots onto the free-list; emission (Pass A) pops `k` slots. To keep the pop deterministic, freed slots are pushed in *sorted* order and popped from a defined end, so the same frame always reuses the same slots — the oracle and GPU path agree on *which index* a newly-emitted particle lands in, not just how many.

**Determinism stance (inherited, sharpened).** Phase 5's *stable-cross-path, bitwise-within-path* applies unchanged. The two places it bites in Phase 6: (1) **compaction must be stable** — survivors keep relative order (guaranteed by scan-offset scatter, never atomic-append); (2) **emission must be seeded and count-deterministic** — the same `AQEmitter.seed` and the same sub-step produce the same particle attributes and the same slot assignments on both paths. No order-dependent float atomics appear anywhere in Passes A–F (force accumulation is per-lane over the particle's own state; there is no scatter-add across particles in Phase 6 — that only arrives with SPH/unified constraints in Phase 7).

**The `double` oracle.** The reference is a single-threaded `double`-precision replay of the exact same pipeline: same fixed sub-step, same seeded emission, same free-list discipline, same push-out math — just in `double` and in obvious sequential code. It is *not* a different algorithm; it is the same algorithm with the parallelism and the `float` removed, so any divergence localizes to precision or to a parallel-bookkeeping bug, never to an algorithmic mismatch. The fast paths must match it within a stated tolerance on positions/velocities and *exactly* on the census (counts, which slot recycled when).

## 9. Validation — how we measure "better"

Measurable acceptance criteria for the deliverable, all against the `double` oracle unless noted:

- **Trajectory match.** Per-particle position/velocity of the GPU path vs. the `double` oracle stays within a stated tolerance (accumulated over the fountain's flight) — a fixed relative-error band, pinned like the Phase 5 colored-solve bands, once measured. CPU-fast vs. oracle matches within a tighter band (both `float`, same order).
- **No tunneling — exact.** No live particle is ever found on the far side of the floor plane (signed distance strictly ≥ −ε against every static `AQShape`) at any frame. This is a *hard* invariant, checked every frame in the test, not a tolerance — a plane's closed-form distance leaves no excuse for a tunnel.
- **Census conservation — exact.** `liveCount(frame) == liveCount(frame-1) + emitted(frame) − expired(frame)` holds bitwise every frame. Total slots accounted for: `liveCount + freeCount == capacity` always.
- **Recycling exactness.** Every dead slot is pushed to the free-list *exactly once* (no leak, no double-free) — instrumented by a per-slot recycle counter that must never exceed the number of deaths of that slot. A leaked slot (recycled zero times when it should be one) fails the test even though the fountain still *looks* fine.
- **Determinism.** Two runs with the same seed produce bitwise-identical pools frame-for-frame (within-path). CPU and GPU produce identical *censuses and slot assignments* (cross-path), and trajectories within tolerance.
- **Scale.** The deliverable sustains tens-to-hundreds of thousands of live particles; the compact-hash neighbor layout keeps memory sub-linear in domain volume (only occupied cells stored) as the fountain sweeps a small fraction of a large box.

**Debug-draw metrics.** New flag bits on the existing `AQDebug.h` bus (no new bus): `AQDebugParticle` draws per-particle velocity vectors / live-set bounds, and `AQDebugForceField` draws each field's influence region (vortex axis, point-field radius, wind direction) as `AQDebugLine`s appended to the same buffer `AQSpace::drainDebugLines` already drains. A count/high-water-mark line renders the live-count and free-count so the on-call engineer *sees* pool pressure.

**Loud guard for the 3am engineer.** The step path is loud, never silent: a **NaN guard** on any particle position/velocity fires immediately (a NaN'd particle from a degenerate field division is the classic silent corruptor — it must scream, not propagate). A **saturation guard** fires when `freeCount` approaches zero (pool near-exhaustion — the moment before emission silently stalls). A **census guard** fires if `liveCount + freeCount != capacity` (a leak or double-free has already happened). None of these default-returns or clamps quietly; each surfaces the particle index, the field/emitter id, and the frame, so the failure is reproducible from the log alone. The whole point of the census-based validation (§2) is that these are the failures with *no visible dynamics tell* — so the guards, not the eye, are the safety net.

## 10. Public API additions

Additions to `AQSpace` / `AQContext` — all pimpl-safe (no backend types cross the header; handles are opaque IDs). Marked `// new`.

```cpp
// include/aqua/AQSpace.h  (additions)
#include "AQParticles.h"

class AQSpace {
public:
    // ... existing rigid-body / collider / debug surface ...

    // --- Particle systems (Phase 6) ------------------------------------ // new
    // Creates a particle system backed by a fixed-capacity pool. Returns an
    // opaque handle; the pool lives behind the pimpl on the Phase 5 substrate.
    AQParticleSystemHandle createParticleSystem(uint32_t capacity);        // new
    void                   destroyParticleSystem(AQParticleSystemHandle);  // new

    // Attach an emitter to a system. Emitters are POD; copied by value.
    void addEmitter(AQParticleSystemHandle, const AQEmitter&);             // new
    void setEmitterEnabled(AQParticleSystemHandle, uint32_t idx, bool on); // new

    // Add a force field to a system (gravity/drag/wind/vortex/point).
    void addForceField(AQParticleSystemHandle, const AQForceField&);       // new
    void setForceFieldEnabled(AQParticleSystemHandle, uint32_t idx,        // new
                              bool on);

    // Static-collider coupling: particles collide (one-way) against the
    // colliders already registered in this AQSpace (Phase 2 AQShapes).
    // Two-way coupling is deferred to Phase 9.
    void setParticleCollisionEnabled(AQParticleSystemHandle, bool on);     // new

    // --- Readback for kREATE (simulation-only boundary) ---------------- // new
    // Snapshot live particle state into caller SoA buffers for rendering.
    // AQUA fills [0, outCount) with the packed live set; visuals are kREATE's.
    uint32_t readParticleState(AQParticleSystemHandle,                     // new
                               AQVec3f* outPositions,   // caller-owned
                               AQVec3f* outVelocities,  // may be null
                               float*   outLifetimes,   // may be null
                               uint32_t* outFlags,      // may be null
                               uint32_t  maxCount) const;

    uint32_t liveParticleCount(AQParticleSystemHandle) const;              // new

    // --- Debug bus (new flag bits appended to the existing surface) ---- // new
    // AQDebugParticle / AQDebugForceField live in AQDebug.h; set/drain via
    // the existing setDebugFlags / debugFlags / drainDebugLines surface.
};
```

`readParticleState` is the whole simulation/rendering boundary: AQUA fills caller-owned SoA with the packed live set and returns the count; kREATE decides color, sprite, and mesh. Nothing visual crosses into AQUA. `createParticleSystem(capacity)` fixes the pool size up front (by design — §2's unbounded population is bounded *at creation*, and the guards in §9 fire before it's exceeded).

## 11. Open decisions for this phase

1. **Pool allocation strategy — free-list + periodic stream compaction, or always-compact?** Free-list makes emission O(k) and amortizes the compaction scan; always-compact keeps the live set trivially `[0, count)` at the cost of a scan every frame. *Lean:* **free-list + periodic compaction** (compact when the dead fraction crosses a threshold, e.g. 25%), with always-compact retained as the fallback if free-list fragmentation ever proves pathological. Revisit only if measured fragmentation hurts.

2. **Neighbor query structure at scale — dense sort-based grid, or compact hashing?** *Lean:* **both, by role.** Keep the Phase 2 sort-based grid for rigid/particle *coexistence* (one grid build serves both when a scene mixes them), and use the **compact-hashing variant (Teschner 2003 / Ihmsen 2011)** for the sparse particle pillar at hundreds of thousands — exactly the split Phase 2's §12 recency audit earmarked. The compact-hash layout stores only occupied cells, keeping memory sub-linear in domain volume.

3. **Particle↔collider coupling — one-way or two-way?** *Lean:* **one-way first** — particles feel colliders (push-out + reflect), colliders do not feel particles (no momentum transfer back). This is the deliverable's target and keeps the collider read-only in the step path. **Two-way is deferred to Phase 9**, where it belongs with the broader coupling work; adding it here would drag a scatter-add of impulses onto the collider path and break the "no order-dependent atomics" cleanliness of Phase 6.

4. **Where is lifetime/recycling determinism enforced?** *Lean:* enforce it in **Pass F (compaction) + Pass A (emission)** jointly: stable scan-offset scatter for survivor order, and sorted-push / defined-end-pop of the free-list so the *same slot* recycles the *same frame* on both paths. Determinism is a property of these two passes' discipline, not a global flag — document it as an invariant the tests assert (§9), not a runtime toggle.

5. **How many force-field kinds ship in the first cut?** *Lean:* ship **five** — gravity, drag, wind, vortex, point attractor/repulsor — as the §7 `AQForceField` union. These cover the fountain deliverable and the common VFX vocabulary while keeping the accumulation kernel a small fixed branch set. Additional kinds (turbulence/noise, curl fields, SDF fields) are additive later — the union grows a variant, no pipeline change.

## 12. Recency-principle audit (addendum, 2026-07-01)

A sweep of the last ~5 years to check whether any sub-choice above is out of date. Per-choice verdict: adopt-now / flagged / no-divergence.

- **Data-parallel base loop (Sims 1990) — no-divergence.** Nothing has displaced "one particle, one lane, forces accumulated independently" as the correct shape for free (unconstrained) particle advection on a GPU. Modern engines still map it this way. Adopt as-is.
- **Sort-based grid neighbor search (Green 2010) — no-divergence, already inherited.** Still the workhorse; it's Phase 2's broadphase. No newer *general-purpose* neighbor structure beats it for our mixed rigid+particle case on the substrate we have.
- **Compact / sparse hashing (Teschner 2003; Ihmsen 2011) — adopt-now for the particle pillar.** Confirmed still the standard sparse-set answer; the SPH/PBD survey line (Koschier, Bender, Solenthaler, Teschner, *"Smoothed Particle Hydrodynamics Techniques…"*, Eurographics STAR 2019/2022) reaffirms compact hashing / cell-linked-lists as the scalable neighbor layout for large sparse particle sets. This is the memory-layout swap Phase 2 §12 earmarked — pick it up here (decision §11.2).
- **Unified-particle solver (Macklin et al. 2014, FleX) — flagged, Phase 7, not adopted.** The unified-particle line (and PBD/XPBD successors — Macklin, Müller, Chentanez *"XPBD"* 2016; Macklin et al. *"Small Steps in Physics Simulation"* 2019) is the modern real-time trend, but it is a *constraint solver*, which is Phase 7's fork. Phase 6 deliberately stays at pools/emitters/fields + neighbor search. Forward link, not a Phase 6 divergence.
- **RT-core / hardware-accelerated broadphase (Wang et al., arXiv:2409.09918, 2024) — flagged, hardware-gated, already earmarked.** Using ray-tracing cores for collision/broadphase acceleration remains a Phase 5.x hardware-gated acceleration behind a `GTEDeviceFeatures` capability check (flagged in Phase 5's audit). Not a Phase 6 baseline — the sort-based grid is the portable path; RT-core is an opt-in accelerator where the hardware exposes it. No divergence for Phase 6.
- **GPU stream-compaction refinements — no-divergence, monitor.** Post-Blelloch work on decoupled-look-back single-pass scan (Merrill & Garland, *"Single-pass Parallel Prefix Scan with Decoupled Look-back"*, NVIDIA 2016) can make compaction cheaper, but it is an *implementation* of the same primitive Phase 5 already owns, not an algorithmic change to Phase 6's pipeline. If compaction ever dominates the frame, swap the scan implementation behind the same interface — no pipeline change. Monitor, don't adopt blind.
- **Neural / learned particle methods — no-fit, rejected.** Learned fluid/particle surrogates (graph-network simulators and their kin, ~2020–present) are impressive but are the wrong tool for a *deterministic real-time* substrate: they trade exact conservation and bitwise reproducibility for amortized speed on offline-trained distributions. AQUA's whole validation stance (§9 — exact census, `double` oracle, cross-path determinism) is incompatible with a learned approximator. Explicitly out.

**Net for Phase 6.** For particle *systems* — pools, emitters, force fields, plus grid neighbor search — the classical answer holds: the data-parallel base loop (Sims), the sort-based grid (Green, inherited from Phase 2), and compact hashing for the sparse large-population case (Teschner/Ihmsen) remain the substrate-correct choice, and they compose from primitives Phase 5 already shipped (scan, radix sort). The genuinely newer idea — the *unified-particle solver* (FleX/PBD/XPBD line) — is not a better way to do Phase 6; it is *Phase 7's* different problem. Adopting it here would be building the aircraft when the phase asks for the motorcycle. Nothing in the last five years moves a Phase 6 sub-choice from its classical answer; the two "flagged" items (unified solver, RT-core) are future forks already accounted for on the roadmap.

**Re-audit due.** Before Phase 7 (unified-particle solver) breaks ground — that fork will want a fresh sweep of the PBD/XPBD line and the FleX successors, since it *is* the phase where the newer literature becomes the baseline rather than the forward link.

## 13. Implementation phasing (settled 2026-07-06)

The §1–§12 material above is the *proposal*. This section is the *implementation contract* the code lands against. It was written after a ground-truth verification pass over the Phase 2 and Phase 5 substrate the proposal leans on. **That pass found the proposal's central premise — "we are assembling parts, not forging them; Phase 6 adds zero new GPU primitives" — is not backed by the shipped code.** The corrections below govern where they contradict §1–§12.

### 13.1 Substrate corrections (what the proposal got wrong about §5/§7/§10)

Verified against source on 2026-07-06. Each item overrides the proposal:

- **There is no reusable scan primitive.** The "Blelloch scan" the proposal composes (§5, §6 Pass F, §8) is in reality a *Hillis–Steele* scan (`AQPrefixScan`, `aqua/src/kernels/AQNarrowphase.omegasl:798`) implemented as a **local lambda inside `encodeNarrowphase`** bound to narrowphase buffers. It is not callable as a standalone primitive.
- **There is no reusable sort primitive.** The "Merrill–Grimshaw radix sort" (§5, §6 Pass F, §8) is in reality an **O(n²) stable rank sort** (`AQSortEntries*`/`AQSortPairs*`, `AQBroadphase.omegasl`) inlined into the private `AQComputeBackend::runBroadphaseChain`. Merrill–Grimshaw is explicitly flagged in-kernel as an *unshipped* 5.x upgrade.
- **There is no point-vs-shape signed distance to reuse.** §5/§6 (Pass D) claim "exact analytic push-out reusing `AQshapeSupport`/signed-distance." Only `AQshapeSupport` exists (`aqua/include/aqua/AQCollision.h:118`), and its *plane* case is a degenerate GJK placeholder. `AQshapeSignedDistance`/`surfaceNormal` appear **only in `.plans/*.md`, never in source.** Phase 6 must implement the point-vs-shape SDF+normal itself.
- **There is no live GPU step anywhere in AQUA.** The entire rigid pillar runs CPU-live: `AQContext::advance` (`AQContext.cpp:66`) drives only `runBroadphase` + `stepInternal` (both pure CPU); `AQSpace` holds no compute-backend handle; `AQComputeBackend::usable()` is hard-`false` ("*5f/5g flips this true once the GPU step is end-to-end*"). GPU stage kernels land one at a time behind **stage-isolation parity tests** and are never wired into the live step. There is no GPU-dispatch hook for a "GPU-simulated fountain" to imitate.
- **Naming/location corrections for the §7 header draft.** `AQVec3f` **does not exist** — the vector type is `AQVec3<Ty> = OmegaGTE::Matrix<Ty,3,1>` (float form `OmegaGTE::FVec<3>`); construct via the `AQvec3(x,y,z)` helper, index as `v[i][0]`. There is no `AQShape.h` — `AQShape` lives in `AQCollision.h`. New debug bits are `AQDebugParticle = 1U<<16` and `AQDebugForceField = 1U<<17` (bit 15 is the highest currently occupied). No `AQMath.h` vector `normalize`/`length`; use `OmegaGTE::dot` + `std::sqrt` (the test idiom's local `vlen`).

### 13.2 Settled decisions (resolves §11)

| §11 | Decision | Consequence |
|-----|----------|-------------|
| 11.1 Pool strategy | **Free-list + periodic compaction** | O(k) slot-pop emission; compact when dead fraction crosses ~25%. Free-list uses sorted-push / defined-end-pop for cross-path determinism. |
| 11.2 Neighbor structure | **Deferred to Phase 7** | The fountain is one-way particle↔*static-collider* and exercises **no** particle-particle neighbor queries. Compact hashing earns its place in Phase 7 (SPH/unified) where particles interact. Phase 6 ships **no** neighbor grid. |
| 11.3 Coupling | **One-way** (as proposed) | Collider read-only in the step path; two-way deferred to Phase 9. |
| 11.4 Determinism | **Enforced in emission + compaction discipline** (as proposed) | Seeded count-deterministic spawn; stable prefix-scatter compaction; sorted free-list. Asserted by tests, not a runtime flag. |
| 11.5 Force fields | **All five now** — gravity/drag/wind/vortex/point | `evalField` is a five-branch switch; the fountain uses gravity+drag, the other three are exercised by unit tests. |

**GPU scope (new decision, not in §11 because the proposal wrongly assumed GPU-reuse was free): CPU-first, mirror the engine.** Phase 6 builds the full ACAC pipeline and the `double` oracle on the **CPU live path**, exactly as Phases 1–5 run today. Particle GPU kernels are a *later* stage-isolation parity sub-phase (like 5c–5e), out of scope for this deliverable. This keeps Phase 6 to the motorcycle: the genuinely hard part (§2 — census/recycling/compaction correctness + one-way exact collision + cross-`double`-oracle determinism), not forging AQUA's first live GPU step.

### 13.3 Increment breakdown (each lands as a reviewable, testable unit)

The CPU particle step slots into `AQContext::advance`'s existing structure: **emit once per advance-frame** (before the sub-step loop, like `runBroadphase`) → **sub-step loop** runs force-accumulate/integrate/collide/age per fixed `fixedDt` → **compact once per advance-frame** (after the loop). Determinism is a function of `(emitter.seed, frame count, sub-step count)`, and the fractional particles-per-frame carry accumulates deterministically.

**File structure (settled with dev).** `AQSpace::Impl` (and the two space-owned records `AQManifoldCacheEntry` / `AQJointRecord`) were extracted verbatim from `AQSpace.cpp` into a private header **`src/AQSpaceImpl.h`** so a second TU shares the one `Impl` layout (ODR). The Phase-6 particle implementation lives in its own **`src/AQSpaceParticles.cpp`** rather than growing the 110 KB `AQSpace.cpp`. The internal per-system state `AQParticleSystem` (SoA + free-list) and the `Impl` particle table live in `AQSpaceImpl.h`; both the particle TU and the internal tests include it.

- **6a — Types + point-SDF groundwork** — **DONE, verified.** `include/aqua/AQParticles.h` (POD types, §7 draft with 13.1 name corrections; `static_assert` trivially-copyable); `AQshapeSignedDistance` + `AQShapeSample` in `AQCollision.{h,cpp}` for plane/sphere/box/capsule (hull → `+inf`, deferred); `AQDebugParticle`=`1<<16`/`AQDebugForceField`=`1<<17`. Test `aqua_particle_sdf_test` (24 assertions, incl. translated + rotated bodies).

- **6b — Pool + free-list + stable compaction** — **DONE, verified.** `AQParticleSystem` in `AQSpaceImpl.h` + impl in `AQSpaceParticles.cpp`: SoA host backing; free-list held DESCENDING so `pop_back()` reuses the smallest slot first (deterministic); stable in-place stream compaction (survivors keep order); `kill` defers reclaim to `compact`; guards `anyNonFinite` (NaN) + `partitionOK` (no leak / no double-free) + saturation cap in `allocate`. Test `aqua_particle_pool_test` (21 assertions: census conservation, stable order, recycle-exactly-once over 50 churn cycles, within-path determinism, NaN guard). Extraction of `Impl` confirmed non-regressing (full CPU suite green).

- **6c — Emitter + force fields + integration** — **DONE, verified.** Scalar-generic math in `src/AQParticleMath.h` (both the float path and the 6e `double` oracle share it): `AQParticleRng` (SplitMix64, integer stream path-identical); `AQemitCount` computes the emitted count with a **`double` carry on all paths** so the census is bit-identical cross-path; Reeves attribute samplers for all five emit shapes; five-branch `AQevalField` (gravity/drag/wind/vortex/point, acceleration-space; `invMass` retained for later); semi-implicit Euler. Passes `emit`/`accumulateAndIntegrate`/`age` on `AQParticleSystem`. Test `aqua_particle_step_test` (20 assertions: per-field accel, integration closed form, count carry, bitwise attribute determinism, spawn bounds, aging, 120-frame fountain census smoke).

- **6d — Collision push-out + step wiring + public API** — **DONE, verified.** `AQParticleSystem::collide` (Pass D) — one-way push-out via the 6a `AQshapeSignedDistance` against an `AQParticleCollider` snapshot gathered once per advance from the space's bodies-with-shapes (public `position()/orientation()/restitution()` accessors, matching the `AQshapeAABB` transform convention). Wired into `AQContext::advance`: emit once per advance for the simulated slice `nSub*fixedDt` → per sub-step `accumulateAndIntegrate + collide + age` → compact once per advance. Full §10 public API on `AQSpace` (`createParticleSystem`/`destroy`/`addEmitter`/`setEmitterEnabled`/`addForceField`/`setForceFieldEnabled`/`setParticleCollisionEnabled`/`readParticleState`/`liveParticleCount`), opaque-id resolution via the pimpl. Test `aqua_particle_api_test` (10 assertions, PUBLIC surface: API round-trip, **no-tunnel hard invariant**, collision on/off toggle, cross-run determinism). Rigid suite confirmed unaffected by the `advance` change.

- **6e — Debug draw + the deliverable + `double` oracle** — **DONE, verified.** SDF made scalar-generic (`src/AQParticleCollision.h::AQshapeSignedDistanceGeneric<Ty>`) as the single source of truth; the public float `AQshapeSignedDistance` delegates to it (6a test proves no regression), and the oracle uses `<double>`. `AQDebugParticle`/`AQDebugForceField` line emission in `particlesCompact` through the existing `drainDebugLines` bus (velocity vectors + field influence markers). The deliverable `aqua/tests/aqua_particle_test.cpp`: a fountain vs a `double` reference oracle that replays the identical ACAC pipeline in sequential `double` code (reusing `AQParticleMath.h` + `AQParticleCollision.h`). Measured/verified: free-flight trajectory tracks the oracle to **2.4e-5** pos / 3.9e-5 vel (band 5e-3); **census matches the oracle exactly every frame** in both free-flight and collision scenes; **zero penetration** of floor/box/sphere (exact no-tunnel); **135k** live particles at scale without exceeding the pool; debug bus gated by the flags.
  - **Determinism refinement found + fixed here:** particle **lifetime is now carried in `double` on every path** (sampled via `AQsampleLifetime<double>` from the same integer RNG draw, aged by the same `double` dt). Death is a threshold on an accumulated value, so a `float` lifetime let a boundary-straddling particle die one sub-step apart from the `double` oracle → the cross-path census diverged by one at frame 129 of the long run. Carrying lifetime in `double` makes birth AND death cross-path exact (position/velocity stay `float` for the trajectory band). This is the §9 "identical censuses cross-path" guarantee made real.

**Phase 6 status: CPU-first pillar COMPLETE (6a–6e landed + verified; full CPU suite green, rigid pillar unaffected).** The GPU particle kernels remain a deliberately-deferred follow-on (stage-isolation parity, like rigid 5c–5e) — Phase 6 as scoped in §13.2 is done. New files: `include/aqua/AQParticles.h`; `src/AQSpaceImpl.h`, `src/AQParticleMath.h`, `src/AQParticleCollision.h`, `src/AQSpaceParticles.cpp`; tests `aqua_particle_{sdf,pool,step,api}_test.cpp` + the deliverable `aqua_particle_test.cpp`.

## 14. GPU sub-phase — the compute payoff (plan, settled 2026-07-07)

The CPU pillar (§13) is the reference. This sub-phase moves the per-particle arithmetic onto the Phase 5 compute substrate. **Unlike the rigid pillar (whose GPU stages are validated in isolation but never wired into the live step), this ships a LIVE GPU particle path** — AQUA's first — selectable via `AQExecPath::GPU`, with the CPU path as the at-parity fallback. Settled with the developer 2026-07-07: **live path**, **thin-slice-first sequencing**, **a reusable GPU scan primitive** (not a third one-off).

### 14.1 The timestep is SHARED — GPU is an executor swap, not a second loop

The decision that governs the whole design: the GPU path uses the **same timestep as the CPU path**. `AQContext::advance` remains the single clock — same accumulator, same `fixedDt`, same `nSub`, same **emit-once → sub-step-loop → compact-once** schedule (§13.3). The GPU only changes *where the per-particle arithmetic executes* (device kernels vs host loops) and *where the pool lives* (resident GPU buffers vs host SoA). It is **mandatory**, not a preference:
1. **Cross-path determinism is the entire validation.** GPU-float must match the `double` oracle census-*exact* and trajectory-*within-band*; the oracle mirrors `advance`'s exact `nSub` count. A separate GPU clock could never match, and every §9 test would be void.
2. **Rigid + particle coexistence.** One `advance` steps rigid (CPU) and particles in the same fixed sub-step loop; particles collide against rigid collider transforms sampled at a defined sub-step boundary. Two clocks in one scene is incoherent.
3. **`AQExecPath` is path *selection*, not a forked engine.** CPU and GPU are the same simulation at parity — they share the clock by definition.

*Shared cadence ≠ per-sub-step sync.* GPU particle state stays **resident** across sub-steps and frames; the host syncs only at **(a) emission** (upload the frame's new particles + the collider snapshot) and **(b) `readParticleState` / debug drain**. No stop-and-readback per sub-step (that would discard the GPU's advantage).

### 14.2 Architecture — host owns the bookkeeping, GPU owns the float physics

The determinism payoff of the shared clock + deterministic emission: **the host can predict the entire census without reading anything back from the GPU.**
- **Emission stays host-side.** OmegaSL has no 64-bit ints (the SplitMix64 RNG doesn't port), and emission is cheap per-frame bookkeeping. The host computes the count (double carry, as today), samples the k new particles with the *same* `AQParticleMath.h` samplers the CPU/oracle use, and uploads them into the packed tail `[liveCount, liveCount+k)`. Emission is therefore bit-identical across CPU/GPU/oracle for free — no GPU RNG to reconcile.
- **Death is an integer sub-step count, not an fp64 threshold.** `fp64` is device-*probed* (`AQProbeDouble`), not guaranteed, so the GPU can't carry the §13.3 6e `double` lifetime. Instead the host computes, in `double` at emission, an integer `deathCountdown = round(lifetime / fixedDt)`; the GPU `age` kernel does an *integer* decrement/compare. Integer death is exact on any hardware → cross-path-exact death **without fp64**. (This replaces the 6e `double` lifetime; see 6f.)
- **The host maintains the authoritative `liveCount` and the compaction permutation** from the integer death schedule alone (it knows every particle's `deathCountdown`). The GPU executes the *same* stable-compaction permutation on the float data. Host and GPU agree by construction — so `liveParticleCount` needs **no** per-frame readback, only `readParticleState` downloads actual positions. This is the clean split: **host = deterministic integer bookkeeping (counts, permutation); GPU = parallel float arithmetic (integrate, collide, scatter).**
- **Pool stays packed-to-prefix.** Because compaction runs every frame, emission always appends to a contiguous tail — the GPU **always-compact** model gives the *same slot layout* as the CPU free-list, so cross-path slot assignment matches (§9).

### 14.3 Increments (thin-slice-first)

- **6f — Integer-death unification (CPU + oracle prep, fp64-free).** Replace the 6e `double` lifetime with a host-computed integer `deathCountdown` (sub-steps remaining), computed in `double` at emission from `lifetime / fixedDt`; `age` decrements the integer; `lifetime` (float) is retained for readback display only. Retrofit the oracle to match. Re-verify the CPU deliverable (census exact, trajectory band). *Touches shipped 6c/6e code — small but real; lands first so CPU/GPU/oracle share one exact death model before any GPU work.*

- **6g — Reusable GPU scan primitive.** A standalone, callable exclusive-prefix-sum in `AQComputeBackend`: its own kernel + an `encodeScan`-style driver with generic in/out buffer bindings, decoupled from the narrowphase `encodeNarrowphase` lambda it currently hides inside. Stage-isolation test: GPU scan vs a CPU prefix sum over randomized inputs (and the byte-identical within-path re-run). *The debt paydown the developer chose — serves Phase 6 compaction now and Phase 7 (SPH/cloth) later; the existing Hillis–Steele kernel is the algorithm seed, lifted into a reusable entry.*

- **6h — Live GPU path: resident pool + host-emit-upload + integrate/age + compaction (collision-free thin slice).** The core. `AQComputeBackend` gains resident particle SoA buffers (position/velocity/deathCountdown/flags/radius/invMass) + encode methods (`encodeParticleIntegrate`, `encodeParticleAge`, `encodeParticleCompact`) + upload-new-particles / download-live-prefix. `integrate` kernel (field accumulate + semi-implicit Euler; fields uploaded once/frame), `age` kernel (integer death), stable compaction via the 6g scan + a scatter kernel. Wire a **live** GPU branch into `AQContext::advance` under `AQExecPath::GPU`, flipping a particle-scoped `usable()`, CPU path as fallback. Collision OFF in this slice. Parity test: live GPU vs CPU/`double`-oracle on a collision-free fountain — census exact, trajectory band, scale. *De-risks residency + compaction + live wiring + timestep before the SDF port.*

- **6i — GPU collision (SDF → OmegaSL) + full fountain.** Port `AQshapeSignedDistanceGeneric`'s closed forms (plane/sphere/box/capsule; hull → +inf) into an OmegaSL `collide` kernel; upload the per-frame collider snapshot buffer; push-out + velocity reflect on-device. Extend the parity test to the full floor + box + sphere fountain: **no-tunnel exact**, census exact, trajectory band. Completes the live GPU deliverable. *Biggest new-kernel authoring risk (transform-inverse + per-shape math in the shader); isolated to last on purpose.*

### 14.4 Risks & notes
- **SDF-in-OmegaSL (6i)** is the largest authoring risk — quaternion-conjugate/rotate + per-shape branches in the shader. The `AQParticleCollision.h` template is the exact spec to transcribe; a small stage test can compare the kernel's distance/normal to `AQshapeSignedDistanceGeneric<float>` before wiring into the pipeline.
- **Determinism stance holds:** GPU-float is *not* bitwise-equal to CPU-float (field-sum rounding order differs) — that is expected and allowed (*stable-cross-path, bitwise-within-path*). Both must match the oracle census-exact + trajectory-band; the emission count (double, host) and death (integer, host) are shared, so only trajectories carry a band.
- **No new GPU primitives beyond the scan** — collision is a kernel over `AQShape` (no broadphase; particle↔particle neighbor queries remain deferred to Phase 7, so no grid on the GPU here either).
- **`AQParticlePool` (public POD view)** becomes meaningful here as the zero-copy/mapped readback surface where the backend allows — wire it in 6h/6i or note it for the kREATE handoff.

---

*Brief status: **CPU-first pillar IMPLEMENTED (6a–6e, §13.3); GPU sub-phase PLANNED (6f–6i, §14) — live GPU path, shared timestep, reusable scan.** The proposal was accepted with corrections; the implementation contract is §13. §11 is settled (§13.2) and the proposal's substrate assumptions are corrected against ground-truth source (§13.1) — where §1–§12 and §13 disagree, §13 governs. Phase 6 is **CPU-first** (mirroring the engine's real architecture: CPU-live step, GPU kernels behind stage-isolation parity tests), ships **no** neighbor grid (deferred to Phase 7), and builds the point-vs-shape SDF, the pool/free-list/compaction, the emitter/field/integration, and the `double`-oracle fountain test as the increments in §13.3. This brief keeps the Phase 2/5 conventions — AQ-prefixed POD types, a CPU path with a `double` reference oracle, and additive debug-bus flag bits — but does NOT inherit reusable scan/sort/SDF primitives (they do not exist as claimed); it builds what it needs on the CPU.*
