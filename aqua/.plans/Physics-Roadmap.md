# AQUA — Roadmap to a Complete Physics Engine

**AQUA** is the project's physics / simulation engine — the simulation
counterpart to **OmegaGTE** (graphics). It is consumed by **Omega kREATE** (the
3D game engine) the same way kREATE consumes OmegaGTE for rendering.

This document describes, end to end, what it takes to grow AQUA from the
original scaffold into a complete simulator across three pillars — the Newtonian
pillar is built (Phases 0–5); the particle and soft-body pillars are ahead:

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

The entire **Newtonian pillar** is built. Phases 0–5 below have all landed —
rigid-body dynamics, collision, the contact solver, joints/queries, and the
compute execution substrate. What exists and works today:

- **AQContext** — the central class. Holds the OmegaGTE graphics engine +
  `GECommandQueue` that GPU physics is dispatched through, creates and retains
  simulation spaces (`createSpace`), and keeps simulation time with a
  **fixed-timestep accumulator** (`advance(realDt)`), including a
  spiral-of-death clamp. Selects the execution substrate as *data*
  (`AQExecPath::Auto/CPU/GPU`), never a compile-time fork. An engine-less
  `CreateCPUOnly` factory backs the pure-CPU tests and headless tools.
- **AQSpace** — holds rigid bodies, shapes, joints, and the contact / query
  machinery, and is stepped by the context. The integrator is the **Phase 1
  body-frame symplectic-Lie + implicit-gyroscopic** step (no longer a
  placeholder), followed by broadphase, narrowphase, and the constraint solve.
  `stepInternal` is private and driven only by `AQContext`.
- **AQRigidBody** — full linear **and** rotational state: position, orientation,
  linear and (world-frame) angular velocity, mass + inertia (diagonal moments,
  a full 3×3 tensor that `addBody` diagonalizes, or shape-derived), the
  force/torque/impulse API, conserved-quantity accessors (L, E), damping,
  gravity scale, material coefficients, activation/sleep, triggers, CCD, and
  kinematic control. Static / Dynamic / Kinematic.
- **Collision** — sphere / box / capsule / plane / convex-hull shapes owned and
  instanced by the space, a fattened-AABB sort-based-grid broadphase with
  layer/mask filtering, and a GJK/EPA + specialized-primitive narrowphase
  producing 1–4-point manifolds.
- **Solver** — a sequential-impulse (PGS) velocity sweep with Coulomb friction,
  warm-started across frames, split-impulse position correction, soft
  (Catto-compliant) constraint rows, and per-island sleeping. Five joint types
  (distance, ball-socket, hinge, slider, fixed) with optional limits + motors.
- **Queries** — raycast / shapecast / overlap and trigger Enter/Stay/Exit
  events, walking the same per-step broadphase grid.
- **Math** — AQUA-owned, `Ty`-generic (`float` production, `double` oracle):
  quaternions, 3×3/3×1 matrices, exp/log maps, orientation integration, inertia
  builders, transforms, and AABBs (`AQMath.h`).
- **Execution** — **both substrates are live.** The hot stages (integration,
  broadphase, contact solve) run on the GPU through OmegaSL compute kernels in
  `src/kernels/` (integrate, broadphase, narrowphase, solver, probe) dispatched
  on the `AQContext` command queue, or on the equivalent CPU reference path —
  chosen from device capability. The D3D12 and Vulkan backends are both
  bring-up-verified.

In one sentence: **AQUA is a complete rigid-body simulator on both CPU and
GPU** — the non-rigid pillars (particles, cloth and ropes, deformable solids,
fluids; Phases 6–10) are what remain ahead.

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

The Newtonian phases below have already used that substrate to turn "move a
point under gravity" into "simulate a rigid-body world." The remaining phases
build on the same footing to add **the non-rigid pillars**: particle systems,
Position-Based Dynamics, cloth / ropes / hair, deformable solids, and fluids.

---

## 2. What "complete" means — subsystem inventory

A complete simulator is the union of these subsystems. Every Newtonian and
shared row is now shipped (✓); the particle and soft-body rows are what remain.

| Subsystem | Pillar | Today | Target |
|---|---|---|---|
| Integration & timestep | shared | ✓ Implicit-gyroscopic body-frame step, fixed sub-stepping, warm-started, deterministic | Sub-stepping, warm-started, deterministic |
| Math | shared | ✓ `Matrix` + `Quaternion` borrowed from GTE's `GTEMath.h`; AQUA-owned `Vec3`, inertia tensor, AABB, transforms | Same |
| Collision shapes | Newtonian | ✓ Sphere, box, capsule, plane, convex hull | + heightfield, mesh (future) |
| Broadphase | shared | ✓ Sort-based uniform grid (GPU-friendly) | SAP / BVH / uniform grid |
| Narrowphase | Newtonian | ✓ GJK/EPA + specialized-primitive contact manifolds | GJK/EPA + SAT contact manifolds |
| Contact solving | Newtonian | ✓ Sequential-impulse (PGS), friction, restitution, split-impulse, stacking | Same |
| Joints / constraints | Newtonian | ✓ Distance, ball, hinge, slider, fixed (+ limits/motors, softness) | Fixed, hinge, ball, slider, distance |
| Queries | Newtonian | ✓ Raycast, shapecast, overlap, triggers | Same |
| Sleeping / islands | shared | ✓ Island grouping + per-island sleep | Same |
| Continuous detection | Newtonian | ✓ Speculative + continuous CCD (opt-in per body) | CCD for fast/thin bodies |
| Particle systems | Particle | None | Pools, emitters, force fields, particle↔collider collision |
| PBD / XPBD core | shared | None | Constraint-projection solver with compliance |
| Cloth, ropes & hair | Soft body | None | Distance + bending constraints, pinning, self-collision; strand hair (many inextensible strands, strand–strand friction) |
| Deformable solids | Soft body | None | Volumetric soft bodies, two-way rigid coupling |
| Fluids — liquids & gases *(optional)* | Particle | None | SPH / position-based fluids for liquids **and** compressible/smoke gas on the same particle substrate |
| Compute execution | execution | ✓ OmegaSL kernels for the hot loops (D3D12 + Vulkan verified), CPU fallback at parity | OmegaSL kernels for hot loops, CPU fallback at parity |
| Debug & tooling | shared | ✓ Drainable debug-line stream (contacts, AABBs, joints, islands, …), NaN guards, loud failures | Debug draw, validation, loud failures |

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

**Recency principle (standing — applies to every phase).** The default
answer to any subsystem question is **the newest viable algorithm from the
literature**, not the incumbents'. Incumbent solutions are the *baseline to
beat*, not the destination. Before any solver lands, the prior-art brief
must explicitly survey the last ~5 years of research on the subsystem
(SIGGRAPH, Eurographics, CGF, TOG, SCA, plus the physics-based-animation
literature index) and identify the most recent paper that does *better* on
AQUA's substrate — faster, more stable, more numerically robust, more
parallel-friendly, or substrate-aware in a way the incumbent isn't. **Only
when no substantively-newer alternative offers a real improvement** — i.e.
the incumbent's algorithm remains the best available for AQUA's compute-
first, small-step, GPU-targeted substrate — does the phase adopt the
incumbent's solution, and the brief must state explicitly that the audit
ran and produced nothing better. "We did the incumbent because it's what
PhysX does" is never an acceptable rationale by itself; "we did the
incumbent because the 2024 audit found no improvement that fits our
substrate" is. This is a stronger reading of point 3 above and supersedes
any prior phase whose brief did not carry an explicit recency audit — those
phases must add a research-note addendum (see §5 Phase 3 and Phase 4 for
the current entries). Re-audit any phase whose brief is more than two years
old before re-implementing or extending it.

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
backend hidden behind the public API. This was the starting scaffold; Phases 1–5
have since replaced the placeholder integrator with the full rigid-body engine.

---

### Phase 1 — Dynamics & math core *(done)* — [Newtonian]

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

**Research note (recency audit, 2026-06-06).** One substantive newer
alternative — **Lie-group variational integrators on SO(3) / SE(3)**
(Lee, Leok, McClamroch 2007+; dual-quaternion variant Xu & Halse 2017) —
provably symplectic + momentum-preserving + energy-bounded for
exponentially long simulations, strictly better than Euler + Newton-
gyroscopic on long-running asymmetric tumblers. **Not adopted now:**
variational integrators want to own the whole step with no mid-step
interrupt, which collides with the Phase 3 PGS-between-half-steps split
that already shipped. Defer to a long-horizon-determinism revisit
(roadmap §6). Müller-Macklin 2020 XPBD-for-rigid is the same §7.2 fork
recorded for Phase 3 / 4 — same defer. Macklin 2019 small-steps already
captured in the Phase 1 doc §11.5. Quaternion exponential map / Jacobi
diagonalization / high-order symplectic splittings: no substantive
divergence at game-physics dt. Full detail:
`aqua/.plans/Phase-1-Dynamics-Math-Core.md` §13.

---

### Phase 2 — Collision shapes & broadphase *(done)* — [shared]

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

**Research note (recency audit, 2026-06-06).** No algorithmic divergence
to adopt now for the sort-based uniform grid lead — Green 2010 + Karras
2012 + fattened analytic AABBs remain the right answer for AQUA's
compute-first, all-three-backends, particle-coexistent substrate. One
**citation update**: the §11.1 LBVH alternative is recharacterized as
**PLOC++ / PRBVH (Meister & Bittner 2018a/b, 2022)** so the future BVH
path skips a generation past Karras 2012. Two future-work items
recorded: **(a) hardware RT-core broadphase** (Wang et al. arXiv
2409.09918, 2024; Mochi arXiv 2402.14801, 2024) as a Phase 5.x
acceleration path gated on `GTEDEVICE_FEATURE_RAYTRACING` (vendor-
specific today — NVIDIA RTX, with AMD and Apple closing the gap;
AQUA's three-backends-required posture blocks an unconditional lead);
**(b) compact hashing** (Teschner 2003, Ihmsen 2011) as a Phase 6
particle-pillar memory layout swap on top of the same sort-based grid.
Full detail: `aqua/.plans/Phase-2-Collision-Shapes-Broadphase.md` §12.

---

### Phase 3 — Narrowphase & contact solving *(done)* — [Newtonian]

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

**Research note (post-implementation literature audit, 2026-06-06; addendum
under the §4 recency principle).** Phase 3's brief predates the recency
principle as a standing rule; the audit ran retroactively and produced one
actionable improvement, plus two flagged-for-later findings and one
non-applicable.

- **Narrowphase — Montaut, Le Lidec, Petrik, Sivic, Carpentier,
  "Collision Detection Accelerated: An Optimization Perspective" (RSS 2024;
  the **Nesterov / Polyak-accelerated GJK** line, shipped in the Coal
  library, formerly HPP-FCL). Genuine drop-in improvement.** Recasts GJK as
  a Frank-Wolfe step in convex optimization and applies Nesterov
  acceleration to the iteration; the paper reports up to ~2× speedup on
  typical convex pairs and as much as 5–15× on the hard cases, with
  identical correctness guarantees (no false negatives) and the **same
  support-function interface** as classical GJK. AQUA's `AQshapeSupport`
  (Phase 2 §7) is exactly that interface, so the adoption is mechanical:
  swap the iteration in `src/AQGJK.cpp` for the accelerated variant; the
  EPA fallback is unchanged. This is a clear "newer-and-better-for-our-
  substrate" finding under the recency principle — **adopt now**, as a
  Phase 3.x maintenance follow-up. The Phase 3 brief's GJK citations stay
  (Gilbert-Johnson-Keerthi 1988 for the algorithm shape, van den Bergen
  2001 for EPA), with Montaut 2024 added for the iteration.
- **Contact solver — Müller, Macklin, Chentanez, Jeschke, "Detailed Rigid
  Body Simulation with Extended Position Based Dynamics" (CGF 2020).
  Substantive divergence; deferred because it is the §7.2 fork.** The same
  paper that drives the Phase 4 joint note (above) also recasts *contact*
  response as XPBD position projection rather than Catto-style velocity-
  impulse PGS, with the same `n` substeps × 1 iteration posture. Adopting
  it for Phase 3 contacts would be making the §7.2 unified-XPBD decision
  early — the architectural fork the roadmap defers to Phase 7. The
  Phase 3 lean stays Catto-style PGS + split-impulse (already shipped),
  with the row schema's `compliance` field (added in the Phase 4 plan's
  groundwork) carrying enough of XPBD's parameter surface that a Phase-7
  recast reuses the row layout. Müller 2020 is cited for the field; the
  algorithm using it is still PGS.
- **Friction — Ly, Casati, Bertails-Descoubes, Béthune, Cohen-Steiner,
  "Primal-Dual Non-Smooth Friction for Rigid Body Animation" (SIGGRAPH
  2024). Flagged, not adopted.** Converts the Coulomb cone's non-smooth
  static-friction problem into an unconstrained smooth problem via
  logarithmic barriers, getting stable static friction *and* fast
  convergence (the two qualities incumbent solvers historically trade off).
  Substantively newer than the iterative cone-clipping in Catto's PGS, but
  heavier per-iteration and aimed at the robotics-fidelity regime
  (high-stack stability under coarse `dt`). At AQUA's small-step posture
  (1/120 s default, smaller for fast rotators) the cone-clipping is
  already in its sweet spot; the Phase 3 incline-friction deliverable
  closes on the analytic answer without this. **Revisit only if profiles
  show static-friction artifacts in large stacks at production dt** —
  noted in the Phase 3 brief §11.6 (anisotropic friction) as the future-
  work neighbour. Not the lead under the recency principle because the
  bar is "real improvement *for AQUA's substrate*," and the substrate
  doesn't surface the failure mode the paper targets.
- **Mesh-barrier contact — Huang, Paik, Ferguson, Panozzo, Zorin,
  "Geometric Contact Potential" (TOG 2024); Li, Kaufman, Jiang et al.
  IPC (2020). Not applicable.** Barrier-potential contact models (the IPC
  line, now refined by Geometric Contact Potential) are the most-active
  newer thread in the contact-simulation literature, but they target
  **triangle-mesh / FEM contact** on deforming surfaces; their failure
  mode (intersection during deformation) does not exist in AQUA's
  analytic-shape rigid-body world. Same conclusion as the Phase 4 CCD
  audit on Tight Inclusion (Wang et al. 2021): mesh-targeted, not for
  analytic shapes. Revisit if/when the soft-body pillar grows a
  deforming-mesh collider.
- **TGS (PhysX 5's Temporal Gauss-Seidel) — already documented.** The
  Phase 3 brief §6 "Alternative considered — TGS" notes the choice; at
  AQUA's already-small `dt` the TGS win narrows. Revisit in Phase 4 if
  joint stacks need it; the audit reaffirms.

**Net conclusion for Phase 3:** the recency audit returns **one
adopt-now finding (accelerated GJK)** plus three flagged-or-deferred
items. The `src/AQGJK.cpp` follow-up is a clean Phase 3.x maintenance
patch — same interface, same correctness, faster. See
`aqua/.plans/Phase-3-Narrowphase-Contact-Solver.md` §4 (literature) for
where this note gets cross-referenced when the patch lands.

---

### Phase 4 — Joints, queries & sleeping *(done)* — [Newtonian → complete]

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

**Research note (post-Phase-3 literature audit, 2026-06-06).** The §4
methodology asks: is there a *newer* paper that improves on what PhysX/Chaos
ship for Phase 4's four subsystems? Audit done; the answer is "for one of the
four, yes — but it's the §7.2 fork applied early; for the other three the
incumbents' answers are still the right answer for AQUA's substrate."

- **Joints — Müller, Macklin, Chentanez, Jeschke, "Detailed Rigid Body
  Simulation with Extended Position Based Dynamics" (CGF 2020) is the
  substantive divergence.** It applies XPBD's compliance-form constraint
  projection (Macklin et al. MIG 2016) directly to *rigid* bodies — distance,
  ball-socket, hinge, slider, fixed — with `n` substeps × 1 iteration
  instead of 1 substep × `n` PGS iterations, claims unconditional stability
  at any stiffness (compliance = 0 ⇒ infinitely stiff with no Baumgarte-style
  energy injection), and removes joint warm-starting. The 2023 survey
  (Fei et al., "Survey of Rigid Body Simulation with XPBD," arXiv 2311.09327)
  confirms it as the modern alternative to Catto-2011 PGS for joints, and
  Mercier-Aubin's "Multi-layer Solver for XPBD" (CGF 2024) refines it
  further. **But adopting it for Phase 4 joints would be making the §7.2
  unified-XPBD decision early** — the architectural fork the roadmap
  explicitly defers to Phase 7. The Phase 4 lean therefore stays
  *Catto-2011 soft constraints on the PGS row buffer*, with the
  `AQConstraintRow::compliance` field carrying enough of XPBD's parameter
  surface that a Phase-7 unified-XPBD recast reuses the row layout without
  rewriting it. Müller 2020 is the citation for that compliance field, even
  while the algorithm using it is the PGS one.
- **CCD — no divergence.** Wang, Ferguson, Schneider, Panozzo et al.,
  "A Large-Scale Benchmark and an Inclusion-Based Algorithm for CCD" (TOG
  2021; the "Tight Inclusion" line, used by IPC: Li, Kaufman, Jiang et al.
  2020) is the most-cited new CCD work since Mirtich 1997, and it is
  provably-correct (no false negatives, no false positives) — but it
  targets **triangle-mesh vertex-face / edge-edge pairs in deforming
  simulations** (FEM cloth, soft bodies), not rigid bodies with analytic
  support functions. For AQUA's analytic shape vocabulary (sphere / box /
  capsule / plane / convex hull), conservative-advancement on
  `AQshapeSupport` (Mirtich 1997) is the right answer; Tight Inclusion's
  guarantees buy nothing because GJK on convex shapes does not suffer the
  near-zero false-negative regime that motivates inclusion-based CCD. The
  incumbents' speculative + conservative-advancement two-tier is what
  Phase 4 ships. (Revisit if/when AQUA grows a deforming-mesh collider
  type in the soft-body pillar.)
- **Islands & sleeping — no divergence.** Union-find with path compression
  + union-by-rank (Tarjan 1975) is still the answer; recent parallel
  union-find work (Patwary et al. PPoPP 2012; Jaiganesh & Burtscher 2018)
  is the Phase 5 GPU port. There is no newer algorithm shifting the
  connected-component / sleep-state shape of the problem.
- **Queries — no divergence.** 3D-DDA grid traversal (Amanatides & Woo
  1987) over the Phase 2 sort-based grid is still the right answer;
  modern alternatives (neural-BVH ray traversal, locality-sensitive
  hashing) target wholly different cost regimes. No opening.

**Net conclusion:** the §4 research loop returns one phase-defining finding
(Müller 2020 *is* the newer, divergent path for joints — and is the §7.2
fork). The Phase 4 plan's `AQConstraintRow::compliance` field is the
specific design move that captures the finding *without* prejudging §7.2.
See `aqua/.plans/Phase-4-Joints-Queries-Sleeping.md` §4 (literature) and
§11.2 (open decision on the parameterization) for the implementation
consequences.

---

### Phase 5 — Compute execution substrate *(done)* — [execution]

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

**Research note (recency audit, 2026-06-18).** The §4 methodology asks
whether a *newer* parallel solver beats colored-Gauss-Seidel for a faithful
GPU port of the shipped Phase 3 PGS. Audit done; the answer is "the newer
solvers are genuinely better *as GPU solvers* but are different algorithms,
so they belong to the §7.2 fork, not to a port." The newest threads —
**Vertex/Block Descent (Chen, Macklin et al., SIGGRAPH 2024)** and the
**GPU-XPBD / unified-particle line (Müller 2020; Macklin 2014 FleX)** — would
replace the sequential-impulse iteration the CPU oracle runs, breaking the
parity contract that *is* this phase's deliverable; they are flagged as the
Phase 7 candidates. The port therefore adopts **colored-Gauss-Seidel (Tonge
et al. 2012 mass-splitting / batched-constraint lineage)** as the parallel
form of the exact CPU iteration, the **canonical sort-based GPU grid (Green
2010)** for broadphase, and **Blelloch scan / Merrill–Grimshaw radix sort**
as the AQUA-owned primitives GTE doesn't supply. Determinism leans
**stable-cross-path, bitwise-within-path** (the §7.4 decision): a single GPU
path is run-to-run deterministic because the colored solve uses no
order-dependent float atomics, while CPU↔GPU agreement is tolerance-equivalent
(float reassociation + colored traversal order), with bitwise-cross-path left
as a documented future mode if kREATE lockstep demands it. **RT-core
broadphase (Wang 2024)** is the recorded hardware-gated Phase 5.x acceleration
(gated on `GTEDEVICE_FEATURE_RAYTRACING`, vendor-specific). Full detail:
`aqua/.plans/Phase-5-Compute-Execution-Substrate.md` §12.

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

**Full prior-art brief:** `aqua/.plans/Phase-6-Particle-Systems.md`.

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
answer. Please check recency audits in the Offical phased plans of AQUA. (There should an official XPBD algortithim that is good for the GPU that we will share)

**Full prior-art brief:** `aqua/.plans/Phase-7-Position-Based-Dynamics-Core.md`.

---

### Phase 8 — Cloth, ropes & hair — [Soft body I]

**Goal:** The first deformables: 1D ropes, 2D cloth, and 1D **hair** — the three
strand/sheet structures that all sit directly on the Phase 7 XPBD substrate.

**Deliverable:** A cloth sheet pinned at two corners draping over a Phase 2
sphere; the rope from Phase 7 generalized to arbitrary chains; and a **head of
hair** — tens of thousands of rooted strands with bending stiffness and
strand–strand friction — settling under gravity and swept by a moving collider.

**Work:**
- Build cloth from a grid: **distance constraints** (structural/shear) +
  **bending constraints**.
- **Hair as rooted strands**: many short particle chains pinned at a root, with
  **inextensible distance constraints** and a **bending/curl model** (rest-curvature
  or follow-the-leader) so strands hold shape; **strand–strand friction** and
  cohesion via the spatial hash so the volume behaves like hair, not spaghetti.
- **Pinning** (attach particles/roots to kinematic/rigid bodies or world points).
- **Cloth/hair↔rigid collision** (two-way coupling with the rigid solver per the
  Phase 7 decision).
- **Self-collision** (basic, via the spatial hash) — known-hard; start
  conservative. Hair leans on segment-based repulsion rather than full
  strand-vs-strand intersection.

**Depends on:** Phase 7 (XPBD core), Phase 2 (colliders).

**Full prior-art brief:** `aqua/.plans/Phase-8-Cloth-Ropes-Hair.md`.

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

**Full prior-art brief:** `aqua/.plans/Phase-9-Deformable-Solids.md`.

---

### Phase 10 — Fluids: liquids & gases *(optional / advanced)* — [Particle + soft]

**Goal:** Fluids on the particle substrate — **and fluid is not just liquid.**
Both incompressible **liquids** (water, sloshing, splashes) and compressible
**gases** (smoke, steam, plumes) fall out of the same smoothed-particle machinery
with different equations of state and boundary handling.

**Deliverable:** Two scenes on one solver — a **dam-break** (a volume of liquid
particles collapsing and sloshing inside Phase 2 static geometry) and a **rising
smoke plume** (a buoyant gas emitter whose particles expand, advect, and curl
around a Phase 2 obstacle).

**Work:** **SPH** (smoothed-particle hydrodynamics) or **position-based fluids
(PBF)** layered on the Phase 6 particle pool and Phase 7 constraint solver:
- **Liquids** — a near-incompressibility (density) constraint, surface tension,
  and viscosity; the dam-break is the reference.
- **Gases** — a compressible equation of state with **buoyancy** (temperature /
  density-driven), thermal advection and cooling, and vorticity confinement so
  plumes keep their rolls; the smoke plume is the reference.
- Shared neighbor-finding on the Phase 6 spatial hash; optional surface / volume
  extraction handed to kREATE for rendering (liquid surface mesh, gas density
  field).

**Depends on:** Phases 6, 7. Optional for a "complete" engine — gate on whether
the project needs fluids (§7.8).

**Full prior-art brief:** `aqua/.plans/Phase-10-Fluids-Liquids-Gases.md`.

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
   assume the answer. Lean was hybrid; **DECIDED 2026-07-07 (developer): UNIFIED
   destination, vet-corrected path** — the XPBD constraint core shipped in
   Phase 7 is the substrate for all deformables + PBF fluids; rigid stays on
   the impulse solver near-term (the 2026-07-07 recency sweep found it already
   embodies the winning TGS-Soft family: small steps + soft rows + impulse PGS,
   and rigid-XPBD is a documented downgrade per Catto's Solver2D); everything
   runs under AQContext's one substep clock, coupled at contacts (7g). True
   unification migrates rigid onto the substrate via an **AVBD-class prototype
   (SIGGRAPH 2025), oracle-gated** on the Phase 3/4 battery per capability —
   whatever the impulse solver still measurably wins, it keeps. See Phase-7
   brief §13.0/§13.4/§13.5-A.
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
8. **Fluids in scope — and liquids-only vs. liquids + gases?** (Phase 10.)
   Optional for "complete"; decide before committing to the particle substrate's
   surface/volume-output design. Gases (smoke/steam) reuse the same SPH/PBF
   machinery with a compressible EOS + buoyancy, so "fluids in scope" should be
   read as *both* phases of matter, not liquids alone.

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
Phase 8  Cloth, ropes & hair  Phase 9  Deformable solids   [Soft body]
   │                              │
   └──────────────┬───────────────┘
                  │
Phase 10  Fluids: liquids & gases (opt.)  [Particle + soft]
```

The critical path to **"kREATE can use AQUA for rigid-body physics"** runs
**Phase 1 → 4**. The compute substrate (Phase 5) and the particle/soft-body
pillars (Phases 6–10) then proceed as a second track once the rigid core and the
solver-architecture decision are settled.

---

*This roadmap is a living document. Phases will be split, merged, and reordered
as we learn. The intent is to keep AQUA demonstrable at every step — a simulation
you can watch run — rather than gated on a distant "done."*
