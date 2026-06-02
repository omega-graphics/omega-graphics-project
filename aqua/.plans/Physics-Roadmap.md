# AQUA — Roadmap to a Complete Physics Engine

**AQUA** is the project's physics / simulation engine — the simulation
counterpart to **OmegaGTE** (graphics). It is consumed by **Omega kREATE** (the
3D game engine) the same way kREATE consumes OmegaGTE for rendering.

This document describes, end to end, what it will take to grow AQUA from today's
scaffold into a complete simulator across three pillars:

- **Newtonian physics** — rigid-body dynamics: forces, collision, constraints.
- **Particle physics** — large populations of mass points under force fields.
- **Soft-body physics** — cloth, ropes, and deformable solids.

It is deliberately incremental: every phase is meant to land as a **runnable
milestone** with a `tests/` simulation that demonstrates the new capability, not
a big-bang rewrite. The ordering is a proposal, not a contract — phases can be
resequenced as priorities and findings change.

Two guiding rules thread through everything below:

1. ***Compute-first, with a CPU fallback at parity.*** AQUA dispatches simulation
   to OmegaGTE compute kernels (OmegaSL → D3D12 / Metal / Vulkan) and keeps an
   equivalent CPU path for devices without usable compute, selected by querying
   `GTEDeviceFeatures` — not a compile-time fork. (See `About.rst`.)
2. ***The backend stays hidden — shared math excepted.*** The simulation backend
   (devices, command queues, compute kernels) lives behind the public surface
   (pimpl), exactly as OmegaGTE hides its own backends, and no OmegaSL types leak
   into `include/aqua/*`. The one deliberate exception is **math types**: AQUA
   borrows `Matrix` and `Quaternion` from OmegaGTE's `GTEMath.h` rather than
   re-deriving numerically-sensitive linear algebra. AQUA already links OmegaGTE
   (it holds a `GECommandQueue`), so this adds no new dependency. Everything else
   the math layer needs — `Vec3`, inertia tensors, transforms, bounding volumes —
   AQUA provides itself.

---

## 1. Where AQUA is today

What exists and works:

- **AQContext** — the central class. Holds the OmegaGTE `GECommandQueue` that all
  physics work will be submitted through, creates and retains simulation spaces
  (`createSpace`), and keeps simulation time with a **fixed-timestep
  accumulator** (`advance(realDt)`), including a spiral-of-death clamp.
- **AQSpace** — holds rigid bodies and is stepped by the context. Its integrator
  is a **placeholder semi-implicit Euler under global gravity, with no
  collision**. `stepInternal` is private and driven only by `AQContext`.
- **AQRigidBody** — `position`, `velocity`, a `type` (Static / Dynamic), and an
  inverse mass. Linear state only — no orientation, no angular velocity.
- **Math** — `Vec3` only (add / subtract / scale). No quaternions, matrices,
  inertia tensors, or bounding volumes yet.
- **Execution** — **CPU only.** `src/kernels/` is an empty placeholder; compute
  dispatch and the production CPU solver are planned, not yet built.

In one sentence: **AQUA can make a handful of dynamic bodies fall under gravity
on the CPU** — everything else (rotation, collision, constraints, particles,
soft bodies, and GPU dispatch) is ahead of us.

### What OmegaGTE already gives us to build on

AQUA is not starting from the GPU up. GTE already provides, as backend-agnostic
APIs callable from AQUA's internals:

- **Compute pipelines & OmegaSL** — the dispatch substrate for data-parallel
  simulation across all three backends from a single kernel source.
- **Command submission** — typed command queues, command buffers, and fences for
  submit/sync. `AQContext` already holds the queue.
- **GPU buffers** — storage for body/particle/contact arrays the kernels operate
  on.
- **`GTEDeviceFeatures`** — the capability set AQUA queries to choose the compute
  path vs. the CPU fallback.

The roadmap below is mostly about **what AQUA must add on top**: the math,
collision, solver, and constraint machinery that turn "move a point under
gravity" into "simulate a world."

---

## 2. What "complete" means — subsystem inventory

A complete simulator is the union of these subsystems. AQUA has the first row
partially and the rest not at all.

| Subsystem | Pillar | Today | Target |
|---|---|---|---|
| Integration & timestep | shared | Semi-implicit Euler, fixed step | Sub-stepping, warm-started, deterministic |
| Math | shared | `Vec3` only | `Matrix` + `Quaternion` borrowed from GTE's `GTEMath.h`; AQUA-owned `Vec3`, inertia tensor, AABB, transforms |
| Collision shapes | Newtonian | None | Sphere, box, capsule, plane, convex hull, heightfield, mesh |
| Broadphase | shared | None | SAP / BVH / uniform grid (GPU-friendly) |
| Narrowphase | Newtonian | None | GJK/EPA + SAT contact manifolds |
| Contact solving | Newtonian | None | Sequential-impulse (PGS), friction, restitution, stacking |
| Joints / constraints | Newtonian | None | Fixed, hinge, ball, slider, distance |
| Queries | Newtonian | None | Raycast, shapecast, overlap, triggers |
| Sleeping / islands | shared | None | Island grouping + sleep for idle bodies |
| Continuous detection | Newtonian | None | CCD for fast/thin bodies |
| Particle systems | Particle | None | Pools, emitters, force fields, particle↔collider collision |
| PBD / XPBD core | shared | None | Constraint-projection solver with compliance |
| Cloth & ropes | Soft body | None | Distance + bending constraints, pinning, self-collision |
| Deformable solids | Soft body | None | Volumetric soft bodies, two-way rigid coupling |
| Fluids *(optional)* | Particle | None | SPH / position-based fluids on the particle substrate |
| Compute execution | execution | CPU only, kernels empty | OmegaSL kernels for hot loops, CPU fallback at parity |
| Debug & tooling | shared | None | Debug draw (contacts, AABBs, constraints), validation, loud failures |

---

## 3. Guiding principles

1. **Every phase ships something runnable.** No phase is "infrastructure only."
   Each ends with a `tests/` simulation that demonstrates the new capability —
   bodies that stack, a cloth that drapes, a particle fountain that collides.
2. **Hide the backend, share the math.** OmegaGTE backend types (devices, queues,
   kernels) and OmegaSL types stay out of `include/aqua/*`; the pimpl discipline
   that `AQSpace` and `AQContext` already follow is non-negotiable as the surface
   grows. The deliberate exception is **math**: `Matrix` and `Quaternion` come
   from OmegaGTE's `GTEMath.h` (AQUA already links GTE), while AQUA owns `Vec3`
   and the physics-specific types built on top.
3. **Compute-first, CPU-fallback at parity.** Every solver gets both a CPU path
   and an OmegaSL compute path that produce equivalent results; the device's
   `GTEDeviceFeatures` selects between them at runtime. A feature is never
   GPU-only or CPU-only — it is the *same simulation* on whichever path runs.
4. **Deterministic fixed step.** All solvers run on `AQContext`'s fixed sub-step.
   Determinism is a stated goal because kREATE may want lockstep netcode or
   replay (see kREATE's cross-cutting "Determinism"); design choices that break
   it must be deliberate.
5. **Thin slice first, breadth later.** One sphere resting on a plane, one rope
   of constrained particles, one cloth patch — working end to end — before
   generalizing to the full shape/constraint set.
6. **Author for the 3am on-call engineer.** Debug-draw hooks for contacts, AABBs,
   and constraints from the start; loud failures and NaN guards in the solver;
   no silent default-returns in the step path. A physics bug you cannot *see* is
   a physics bug you cannot fix.

---

## 4. Research methodology — study the incumbents, surpass them from the literature

Every phase below names a well-trodden problem: broadphase, GJK/EPA narrowphase,
sequential-impulse contact solving, XPBD constraint projection. The mature
engines — chiefly **NVIDIA PhysX 5** (with FleX) and **Epic's Chaos** — have
shipped hardened answers to all of them. We treat those answers as a *map of the
terrain we have to cross*, not as code to copy. The goal is not parity with them;
it is to arrive a generation later and land somewhere better, because we get to
choose our substrate (OmegaSL compute, GTE's `GTEMath.h` matrix/quaternion types,
the GPU's specialized numeric formats) instead of inheriting theirs.

The discipline, run once per subsystem **before** its solver is written:

1. **Survey the incumbents.** Read how PhysX 5 and Chaos solve the subsystem —
   the algorithm, the data structures, the numerical tricks, and *especially* the
   tradeoffs they accepted. PhysX is open-source and Chaos ships with Unreal's
   source; both are studyable. We read them to understand the problem's shape and
   the failure modes they hit, not to transcribe.
2. **Find why their choice was theirs, not ours.** Their algorithms are tuned for
   *their* constraints — a CPU/SIMD heritage, generic `float4` math, a particular
   determinism stance, decades of backward compatibility. List where those
   constraints diverge from AQUA's: we are compute-first (§3), we borrow GTE's
   `Matrix`/`Quaternion` and own our `Vec3`, and we can target the GPU's numeric
   types directly. Each divergence is an opening for a better algorithm, not just
   a faster transcription of theirs.
3. **Go to the literature, not the shipped binary.** Research usually leads the
   engines by years. Pull the primary sources — SIGGRAPH / Eurographics papers,
   the XPBD and small-steps line of work, GPU-collision and constraint-coloring
   papers — and look for the *newer, more refined* algorithm that the incumbents
   predate or never adopted. Cite the paper in the implementation.
4. **Derive a variant optimized for our types and substrate.** Adapt the chosen
   algorithm to AQUA's reality: SoA layouts for compute (§5 Phase 5),
   constraint-graph coloring that maps to OmegaSL dispatch without write
   conflicts, math expressed in GTE's matrix/quaternion types, and
   reductions/accumulations chosen to fit the determinism target (§5 Phase 5) and
   the GPU's numeric formats. This is where "more optimized for the matrix types
   and specialized GPU numeric types" gets cashed out — as a concrete kernel, not
   a slogan.
5. **Prove it against the incumbent as a baseline.** The incumbent's *behavior* is
   the reference oracle, not its code. A box stack must settle as cleanly; a cloth
   must drape as plausibly. Hold our variant to that bar on the phase's runnable
   deliverable, and to the CPU/GPU parity test (§6).

Two boundaries on this work:

- **Clean-room, by derivation.** We study published descriptions and read source
  to *understand*, then implement from the math independently. We do not paste or
  line-by-line port their code; the value is the refined algorithm, not their
  expression of an older one. (Their licenses differ from ours — derive, don't
  lift.)
- **Each phase opens with a one-page prior-art brief.** Before the solver lands, a
  short brief records: what PhysX/Chaos do, which paper we're improving on, what we
  changed for our substrate, and how we'll measure "better." This is the research
  analogue of the runnable-deliverable rule (§3) — the brief is the artifact that
  justifies the kernel, and it lives next to the phase's `tests/` simulation.

This methodology threads through **every** phase in §5 and feeds directly into the
per-phase **Key decisions** (§7): each fork there — broadphase structure,
narrowphase strategy, solver architecture — is exactly the kind of choice this
research loop is meant to settle with evidence rather than by defaulting to
whatever the incumbents happened to ship.

---

## 5. Phased roadmap

Each phase lists its **goal**, the **runnable deliverable** that proves it, the
**work**, what it **depends on**, and any **key decisions** to make first. The
pillar each phase serves is tagged in brackets.

### Phase 0 — Foundation *(done)* — [shared]

`AQContext` (command queue + space ownership + fixed-timestep accumulator) and
`AQSpace` (rigid-body container + placeholder gravity integrator), with the
backend hidden behind the public API. This is the current state.

---

### Phase 1 — Dynamics & math core — [Newtonian]

**Goal:** Promote bodies from points to rigid bodies, and grow the math from
"one `Vec3`" into what dynamics needs.

**Deliverable:** A spinning body under an applied torque and an off-center
impulse, integrating orientation correctly with no collision yet.

**Work:**
- Adopt **`Matrix` and `Quaternion` from OmegaGTE's `GTEMath.h`** for orientation
  and linear algebra (`FMatrix<3,3>` / `FMatrix<4,4>`, `Quaternion<float>`)
  instead of re-deriving them. GTE's matrix/quaternion operate on GTE's own
  vector type, so add a thin `Vec3` ↔ GTE-vector bridge so AQUA's `Vec3`
  interoperates at call sites.
- AQUA-owned math the borrowed types don't cover: inertia tensors, a `Transform`
  (position + orientation), bounding volumes, and any `Vec3` ops still missing
  (dot / cross / normalize / length).
- Rigid-body state: orientation, angular velocity, inverse inertia tensor
  (world-space), center of mass.
- Rotational integration alongside the existing linear path.
- A **force/torque/impulse API** on `AQRigidBody` (`applyForce`,
  `applyImpulse`, `applyTorque`), accumulated and consumed each sub-step.
- Extend `AQBodyDesc` with material params (restitution, friction) and a
  collision-shape handle (defined in Phase 2).

**Depends on:** Phase 0.

---

### Phase 2 — Collision shapes & broadphase — [shared]

**Goal:** Give bodies geometry and find the pairs that might touch.

**Deliverable:** A debug-draw view of many bodies' AABBs with overlapping pairs
highlighted — no contact response yet, just pair detection.

**Work:**
- Collision shapes: sphere, box, capsule, plane/half-space, convex hull;
  later heightfield and triangle mesh (static).
- Per-shape AABB computation; world-AABB update each step.
- **Broadphase** producing candidate pairs (see key decision).
- Collision filtering (layers / masks).

**Depends on:** Phase 1.

**Key decision:** **Broadphase structure — sweep-and-prune vs. BVH vs. uniform
grid.** A uniform grid parallelizes cleanly on the GPU (Phase 5) and suits the
large particle counts of Phase 6; a BVH handles widely varying object sizes
better. This choice is shared across all three pillars, so make it before
building broadphase.

---

### Phase 3 — Narrowphase & contact solving — [Newtonian]

**Goal:** Bodies collide, respond, and come to rest.

**Deliverable:** A stack of boxes dropped onto a static floor that settles and
stays settled, with friction holding a box on an incline.

**Work:**
- **Narrowphase**: contact-manifold generation — GJK/EPA for general convex
  pairs, specialized paths for common cases (sphere/sphere, box/box via SAT).
- **Contact solver**: sequential-impulse / projected Gauss-Seidel velocity solve
  with restitution and Coulomb friction.
- **Warm-starting** (cache contact impulses across frames) for stable stacking.
- Penetration recovery (Baumgarte or split-impulse).

**Depends on:** Phase 2.

**Key decision:** **Narrowphase approach — one general GJK/EPA path vs. a table
of specialized per-pair functions.** General is less code and extends to convex
hulls; specialized is faster and more numerically robust for the common shapes.
A hybrid (specialized fast paths + GJK/EPA fallback) is the likely answer.

---

### Phase 4 — Joints, queries & sleeping — [Newtonian → complete]

**Goal:** Finish the rigid-body feature set kREATE's Phase 8 needs.

**Deliverable:** A jointed ragdoll or a swinging bridge of constrained bodies,
plus gameplay raycasts that hit it and a body that goes to sleep when idle.

**Work:**
- **Joints/constraints**: fixed, hinge, ball-socket, slider, distance — solved
  in the same iterative solver as contacts.
- **Queries**: raycast, shapecast, overlap tests against the broadphase.
- **Kinematic bodies** (animated, infinite mass) and **trigger volumes**
  (overlap events, no response) for gameplay callbacks.
- **Islands & sleeping**: group connected bodies, sleep idle islands.
- **Continuous collision detection (CCD)** for fast/thin bodies (see decision).

**Depends on:** Phase 3. **This phase is what kREATE's
`Engine-Roadmap.md` Phase 8 consumes** — after it, kREATE's rigid-body
integration is unblocked.

**Key decision:** **CCD scope** — which bodies get continuous detection
(everything is expensive; nothing risks tunneling). Typically opt-in per body
plus automatic for high-velocity dynamics.

---

### Phase 5 — Compute execution substrate — [execution]

**Goal:** Make good on the compute-first promise: move the hot loops onto the
GPU, with the CPU path retained at parity as the fallback.

**Deliverable:** The Phase 3/4 scene running its broadphase, integration, and
contact solve as OmegaSL compute kernels dispatched through `AQContext`'s queue,
producing results equivalent to the CPU path — and falling back to CPU on a
device that reports no compute capability.

**Work:**
- Author the first real kernels in `src/kernels/`: integration, broadphase
  (the grid/BVH from Phase 2), and the contact/constraint solve.
- Convert hot data to a **GPU-friendly layout** (see decision) in pooled GTE
  buffers; encode dispatches and barriers on the command queue.
- Establish the **CPU/GPU parity** test harness: the same scene must produce
  equivalent results on both paths within tolerance.
- Wire `GTEDeviceFeatures` gating so path selection is data, not `#ifdef`.

**Depends on:** Phases 2–4 (there must be solvers worth porting). Can also be
started earlier in spirit — the data layout decision below should be made before
Phase 3 writes the solver, so the CPU solver is already SoA-friendly.

**Key decisions:**
- **Data layout — array-of-structs vs. struct-of-arrays.** Compute kernels want
  SoA; the CPU solver is happy either way. Pick SoA early so the port in this
  phase isn't a rewrite.
- **Determinism guarantee — bitwise-deterministic vs. stable-but-not-bitwise.**
  Bitwise determinism across CPU and GPU (and across GPU vendors) is hard and
  constrains float usage and reduction order. Decide the target here, because it
  governs how the kernels are written.

---

### Phase 6 — Particle systems — [Particle]

**Goal:** Simulate large populations of mass points — the pillar the compute
substrate pays off on most.

**Deliverable:** A GPU-simulated particle fountain (tens to hundreds of
thousands of particles) colliding with the Phase 2 static geometry.

**Work:**
- A **particle pool**: positions, velocities, and inverse masses as SoA buffers,
  with allocation/recycling.
- **Emitters**: emission shape, rate, initial velocity, lifetime.
- **Force fields**: gravity, drag, wind, vortex, point attractors/repulsors.
- **Particle↔collider collision** against the Phase 2 shapes (one-way first).
- **Spatial hashing** for particle neighbor queries (reusing the Phase 2 grid).
- Simulation only — emission *visuals* and rendering are kREATE's job; AQUA
  hands back particle state.

**Depends on:** Phases 2 (colliders, grid) and 5 (compute — particles are the
canonical massively-parallel workload).

---

### Phase 7 — Position-Based Dynamics core — [shared: particle + soft]

**Goal:** Build the constraint-projection framework that cloth, ropes, and
deformable solids all sit on.

**Deliverable:** A rope — a chain of particles linked by distance constraints —
swinging and settling under XPBD, GPU-accelerated.

**Work:**
- An **XPBD** (Extended Position-Based Dynamics) solver: predict positions,
  project constraints over sub-steps, derive velocities. Compliance gives
  constraints physically meaningful stiffness.
- A general **constraint interface** (distance first) the later soft-body
  constraints implement.
- Constraint-graph **coloring/batching** so the projection parallelizes on the
  GPU without write conflicts.

**Depends on:** Phase 6 (shares the particle substrate and the compute path).

**Key decision (foundational — the single biggest fork in this roadmap):**
**Solver architecture — keep the impulse-based rigid solver and XPBD soft solver
separate (hybrid), or unify *everything* — including rigid bodies — under one
XPBD/particle solver** (the approach taken by GPU engines like NVIDIA FleX and
PhysX 5).
- *Unified XPBD* is elegant, exceptionally GPU-friendly, and gives two-way
  coupling between rigid, particle, and soft bodies almost for free — but pure
  position-based rigid stacking is historically less accurate than a dedicated
  impulse solver (XPBD narrows, doesn't erase, this gap).
- *Hybrid* keeps the battle-tested impulse solver from Phase 3 for rigid bodies
  and uses XPBD only for deformables/particles, coupling the two at the contact
  level — more code and a coupling seam to get right, but each domain uses the
  method it's best at.

This must be decided **before** Phase 7, because Phases 8–9 build directly on the
answer. Lean: **hybrid**, unless GPU throughput and unified coupling outweigh
rigid-stacking fidelity for the project's use cases.

---

### Phase 8 — Cloth & ropes — [Soft body I]

**Goal:** The first deformables: 1D ropes and 2D cloth.

**Deliverable:** A cloth sheet pinned at two corners draping over a Phase 2
sphere, with the rope from Phase 7 generalized to arbitrary chains.

**Work:**
- Build cloth from a grid: **distance constraints** (structural/shear) +
  **bending constraints**.
- **Pinning** (attach particles to kinematic/rigid bodies or world points).
- **Cloth↔rigid collision** (two-way coupling with the rigid solver per the
  Phase 7 decision).
- **Self-collision** (basic, via the spatial hash) — known-hard; start
  conservative.

**Depends on:** Phase 7 (XPBD core), Phase 2 (colliders).

---

### Phase 9 — Deformable solids — [Soft body II]

**Goal:** Volumetric soft bodies — squishy, recoverable solids.

**Deliverable:** A deformable cube dropped onto a stack of Phase 3 rigid boxes,
with both the cube deforming and the boxes reacting (two-way coupling).

**Work:**
- Volumetric soft bodies via **shape-matching** or **XPBD volume/FEM
  constraints** (tetrahedral mesh) — pick per the Phase 7 architecture.
- Volume preservation and recovery to rest shape.
- **Two-way coupling** with rigid bodies and cloth.
- Optional: plasticity (permanent deformation) and tearing.

**Depends on:** Phases 7, 8, and 3 (rigid coupling).

---

### Phase 10 — Fluids *(optional / advanced)* — [Particle + soft]

**Goal:** Liquids and gases on the particle substrate.

**Deliverable:** A dam-break: a volume of fluid particles collapsing and sloshing
inside Phase 2 static geometry.

**Work:** **SPH** (smoothed-particle hydrodynamics) or **position-based fluids
(PBF)** layered on the Phase 6 particle pool and Phase 7 constraint solver;
density/pressure constraints; optional surface extraction handed to kREATE for
rendering.

**Depends on:** Phases 6, 7. Optional for a "complete" engine — gate on whether
the project needs fluids.

---

## 6. Cross-cutting concerns

These thread through several phases and should be designed early even if built
gradually:

- **Compute/CPU parity & determinism** — every solver maintains both paths and a
  test that holds them equivalent; the determinism target (§5 Phase 5) governs
  how kernels reduce and accumulate.
- **The kREATE integration boundary** — AQUA exposes its own types (`Vec3`,
  bodies, shapes) plus the borrowed GTE math types (`Matrix`, `Quaternion`);
  kREATE converts to its own `Kreate::Vec3` / `Mat4` / `Quat` at the boundary (it
  already links GTE, so the borrowed types cost it nothing). What stays out of
  `include/aqua/*` is OmegaGTE *backend* types (devices, queues, kernels) and any
  third-party SDK types, mirroring how GTE's backends are hidden.
- **Numerical robustness** — fixed step + sub-stepping, warm-starting, NaN/inf
  guards in the solver, contact caching. A blown-up simulation should fail loud,
  not silently spew NaNs.
- **Memory / buffer management** — pooled GTE buffers for bodies, particles, and
  contacts; SoA layouts; avoid per-frame allocation in the step path.
- **Continuous collision detection** — threads through the Newtonian pillar
  (Phase 4); revisit for fast particles.
- **Debug visualization** — contact points/normals, AABBs, constraints, and
  islands as debug-draw data AQUA emits and kREATE renders.

---

## 7. Key decisions to make early

The forks that are expensive to reverse, pulled out of the phases so they can be
decided deliberately:

1. **Build vs. integrate** *(governs everything)* — grow AQUA's own solver, or
   wrap a third-party physics SDK behind the public API? kREATE's roadmap
   explicitly defers this to AQUA. The phases above are written for a **custom
   solver**; integrating an SDK would re-shape them into an *adapter* roadmap
   while keeping the same public surface and the same compute/CPU story.
2. **Solver architecture — hybrid (impulse rigid + XPBD soft) vs. unified XPBD.**
   (Before Phase 7.) The single most far-reaching technical choice; Phases 8–9
   assume the answer. Lean: hybrid.
3. **Data layout — AoS vs. SoA.** (Before Phase 3.) SoA so the Phase 5 compute
   port isn't a rewrite.
4. **Determinism guarantee — bitwise vs. stable.** (Before Phase 5.) Constrains
   float usage and cross-platform GPU reductions; needed if kREATE wants lockstep
   netcode or replay.
5. **Broadphase structure — SAP / BVH / uniform grid.** (Phase 2.) Grid favors
   GPU and particles; BVH favors mixed object sizes.
6. **Narrowphase — general GJK/EPA vs. specialized per-pair (vs. hybrid).**
   (Phase 3.)
7. **CCD scope.** (Phase 4.) Which bodies get continuous detection.
8. **Fluids in scope?** (Phase 10.) Optional for "complete"; decide before
   committing to the particle substrate's surface-output design.

---

## 8. Dependency overview

```
Phase 0  Foundation (context, space, accumulator)   (done)
   │
Phase 1  Dynamics & math core        [Newtonian]
   │
Phase 2  Collision shapes & broadphase   [shared] ──────────────┐
   │                                                            │
Phase 3  Narrowphase & contact solving   [Newtonian]            │
   │                                                            │
Phase 4  Joints, queries, sleeping       [Newtonian]            │
   │     ◄── kREATE Engine-Roadmap Phase 8 consumes this        │
   │                                                            │
Phase 5  Compute execution substrate     [execution]            │
   │     (parallelizes Phases 1–4; CPU path kept at parity)     │
   │                                                            │
Phase 6  Particle systems                [Particle] ◄───────────┘
   │
Phase 7  Position-Based Dynamics core    [shared]
   │     ◄── solver-architecture decision gates Phases 8–9
   ├─────────────────────────┐
Phase 8  Cloth & ropes        Phase 9  Deformable solids   [Soft body]
   │                              │
   └──────────────┬───────────────┘
                  │
Phase 10  Fluids (optional)              [Particle + soft]
```

The critical path to **"kREATE can use AQUA for rigid-body physics"** runs
**Phase 1 → 4**. The compute substrate (Phase 5) and the particle/soft-body
pillars (Phases 6–10) then proceed as a second track once the rigid core and the
solver-architecture decision are settled.

---

*This roadmap is a living document. Phases will be split, merged, and reordered
as we learn. The intent is to keep AQUA demonstrable at every step — a simulation
you can watch run — rather than gated on a distant "done."*
