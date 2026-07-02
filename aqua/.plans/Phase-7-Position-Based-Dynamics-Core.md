# AQUA Phase 7 — Position-Based Dynamics Core (XPBD)

**Prior-art brief & proposal.**

---

## 1. Scope & deliverable

**Goal.** Build the constraint-projection core — an XPBD solver — that cloth (Phase 8), deformable solids (Phase 9), and fluids (Phase 10) all sit on. This is the *shared spine* of the deformable half of the roadmap. Get the interface and the numerics right here, once, and the three downstream phases become "author the right constraint type and hand it to the same solver." Get it wrong, and we pay for it three times.

This phase also settles the single biggest fork in the whole roadmap — roadmap §7.2, *hybrid vs. unified* (whether we keep the Phase 3 impulse rigid solver alongside XPBD, or recast everything, rigid included, under one position-based particle solver). See §11 #1. The roadmap lean is **hybrid**, and this brief argues it from the literature and from what our substrate already gives us.

**Runnable deliverable.** A **rope** — a chain of particles linked by distance constraints — pinned at one end, swinging under gravity and settling under XPBD, GPU-accelerated, held to an **energy/settle oracle**:

- Total mechanical energy decreases monotonically to a rest floor once released to hang — no spurious energy injection, no limit-cycle jitter.
- The settled rope hangs in a **catenary**, matched to the analytic curve within a stated tolerance (mass/segment/gravity given).
- **compliance = 0 ⇒ rigid, inextensible** — segment lengths hold to rest length within solver tolerance regardless of load.
- **compliance > 0 ⇒ measurable, timestep-independent stretch** — the *same* compliance produces the *same* steady-state stretch at 60 Hz and at 240 Hz sub-stepping. This is the whole point of XPBD over classic PBD and it is what the oracle exists to prove.

**Included groundwork.**
- The `AQXPBDSolver` (opaque handle, pimpl) driving predict → project → derive-velocity over fixed sub-steps.
- The **general constraint interface** (`AQConstraintType`, POD `AQConstraint` tagged record, `AQDistanceConstraint`) — distance first, but shaped so Phase 8/9/10 constraints drop in without touching the solver core.
- **Constraint-graph coloring / batching** so projection parallelizes on the GPU with no write conflicts — the direct descendant of Phase 5's colored Gauss-Seidel.
- Debug-bus bits for constraint and color visualization (`AQDebugConstraint`, `AQDebugConstraintColor`).

**What earlier phases have already closed for us.**
- **Phase 3** shipped the impulse (sequential-impulse PGS) rigid contact solver — `AQConstraintRow`, `AQContactManifold`, warm-starting, split-impulse. Critically, the Phase 3/4 `AQConstraintRow::compliance` field was added *specifically* so a Phase-7 unified-XPBD recast could reuse the row layout. So XPBD's compliance parameter surface is already partly present in our contact rows — a fact §6 and §11 both lean on.
- **Phase 5** shipped the compute substrate — `AQComputeBackend`, `AQExecPath`, SoA buffers over pooled GTE buffers, `src/kernels/*.omegasl`, AQUA-owned Blelloch scan + radix sort, and **colored Gauss-Seidel** (constraint-graph coloring so no two constraints in a color share a body → no write conflicts). Phase 5's recency audit explicitly named the GPU-XPBD / unified-particle line and Vertex Block Descent as "the Phase 7 candidates where the algorithm itself is on the table." Phase 7 is where that fork gets decided. Determinism stance: stable-cross-path, bitwise-within-path.
- **Phase 6** shipped `AQParticlePool` (SoA positions / velocities / inverse-mass) and the sort-based grid neighbor search. XPBD here operates *on* that particle substrate — it does not re-invent particle storage.

**Out of scope, by design.**
- Cloth bending/shear constraints, self-collision (Phase 8).
- Volume / strain / shape-matching solid constraints, tetrahedral meshes (Phase 9).
- Density/PBF fluid constraints (Phase 10).
- Rigid-body *replacement*. We are not deleting the Phase 3 impulse solver; we are coupling to it. That decision is the content of §11 #1.
- Vertex Block Descent as a shipped solver — surveyed in §12, kept as a behind-the-interface upgrade.

---

## 2. Why the constraint-projection core is its own problem

1. **It works at the position level, not the velocity level.** Phase 3's impulse solver reasons about velocities and accumulated impulses; a contact is satisfied when relative normal velocity is non-negative. XPBD reasons about *positions* — it moves particles directly so a constraint function `C(x)` goes to zero, then *derives* velocities from the position change. Different unknowns, different Jacobian bookkeeping, different failure modes (position drift vs. velocity bias). It is not a reskin of the PGS row loop, so it earns its own solver, its own types, and its own validation.

2. **Compliance must decouple stiffness from timestep and iteration count.** Classic PBD's stiffness parameter is a lie — a "0.5-stiff" spring gets stiffer as you add iterations and changes behavior when you change the frame rate. XPBD (Macklin et al. 2016) fixes this by introducing **compliance** `α = 1/k` with an explicit `α̃ = α/Δt²` term in the constraint update, so a given compliance maps to a physical, iteration-count-independent stiffness. Getting that decoupling *provably* right — the timestep-independent-stretch oracle in §1 — is the hard technical heart of this phase, and it's why classic PBD is not good enough to be the shared core.

3. **Parallel projection needs graph coloring to avoid write conflicts.** Two distance constraints that share a particle both want to move that particle's position in the same sub-step. Run them concurrently and you get a read-modify-write race — corrupt positions, non-determinism, occasionally NaN. The answer is the same one Phase 5 already built for colored Gauss-Seidel: partition constraints into **color batches** where no two constraints in a batch touch a shared particle, then project one color at a time, all constraints within a color in parallel. Coloring is not an optimization here; it is a correctness precondition for the GPU path.

4. **It is the shared spine of Phases 8–10, so the interface choice is the most consequential decision in the phase.** Cloth is distance + bending constraints. Deformable solids are volume/strain constraints. Fluids are density constraints (PBF). All three are *the same solver* projecting *different constraint types*. If the constraint interface is virtual/polymorphic, every downstream phase inherits a vtable indirection on the GPU-hostile hot path. If it's a POD tagged union with typed arrays, every downstream phase gets coalesced reads and a kernel that switches on a tag. We choose the second (§6, §7), and we choose it now because it is nearly impossible to change once three phases depend on it.

---

## 3. Prior art — how the incumbents solve it

**NVIDIA PhysX 5 / FleX (unified particles).** FleX (Macklin et al. 2014) represents *everything* — rigids, cloth, ropes, fluids, gases, deformables — as particles connected by constraints, solved with a unified position-based solver. Rigids are shape-matched particle clusters; fluids are density-constrained particles; cloth is distance + bending. It's the purest expression of the "one particle solver to rule them all" thesis, and it is beautiful on the GPU because there is exactly one data path. PhysX 5 folds this lineage in as its soft-body / particle system. The cost FleX pays — and the reason we do not simply copy it (§11 #1) — is that rigid stacking through shape-matched particles is *softer* and less accurate than a dedicated impulse contact solver. FleX chose uniformity over per-domain accuracy. We are choosing differently.

**Epic Chaos (its constraint solver).** Chaos, Unreal's physics engine, runs a constraint-based solver with an XPBD-family projection path for its cloth and (increasingly) rigid work, sitting alongside more traditional contact handling. The instructive thing about Chaos for us is that it is *not* dogmatically unified — it uses position-based projection where position-based projection shines (cloth, soft constraints) and keeps dedicated machinery where that machinery is better. It is, in shape, a hybrid. That is corroboration for the roadmap lean, from a shipping AAA engine.

**The shared shape.** Strip the branding and both — plus every academic XPBD implementation — converge on the same loop: **predict** positions from external forces, **project** a set of constraints to correct those positions (iterated or sub-stepped), then **derive** velocities from the net position change. The disagreements are narrow and they are exactly our open decisions (§11): substeps vs. iterations, how you color for the GPU, whether rigid bodies live inside this loop or beside it. The core loop is settled art; the fork is where the engines differ, and where we take a position.

---

## 4. The literature we build on

- **Müller, Heidelberger, Hennix, Ratcliff 2007, "Position Based Dynamics"** (J. Visual Communication & Image Representation) — the original PBD. Introduces the predict/project/derive-velocity loop and Gauss-Seidel constraint projection. This is the ancestor of everything here.
- **Macklin, Müller, Chentanez 2016, "XPBD: Position-Based Simulation of Compliant Constrained Dynamics"** (MIG 2016) — the paper we adopt as the core. Adds **compliance** `α` and the `α̃ = α/Δt²` regularization so stiffness is physical and iteration-count-independent. This is what makes the §1 timestep-independent-stretch oracle achievable.
- **Macklin, Storey, Lu, Terdiman, Chentanez, Jeschke, Müller 2019, "Small Steps in Physics Simulation"** (SCA 2019) — the key stability/accuracy result: **n sub-steps × 1 iteration beats n iterations × 1 step** for the same cost. Sub-stepping shrinks `Δt`, which sharpens compliance's grip and tames stiff constraints without more inner iterations. This dictates our loop shape (§6) and open decision §11 #2.
- **Müller, Macklin, Chentanez, Jeschke 2020, "Detailed Rigid Body Simulation with Extended Position Based Dynamics"** (Computer Graphics Forum 2020) — XPBD extended to *rigid bodies* with full rotational constraints. This is the unified option — the paper that says "you can do rigid in XPBD too." It is exactly the §7.2 fork made concrete, and we cite it precisely because we are choosing *not* to take it as the primary rigid path (§11 #1).
- **Fratarcangeli, Tibaldo, Pellacini 2016, "Vivace: a Practical Gauss-Seidel Method for Stable Soft Body Dynamics"** (ACM TOG / SIGGRAPH Asia 2016) — parallel **graph-colored** PBD/Gauss-Seidel on the GPU. This is our coloring answer, and it is the same idea Phase 5 already implemented for colored-GS contacts. The wheel is not being reinvented; it is being pointed at a new constraint set.
- **Macklin et al. 2014, "Unified Particle Physics for Real-Time Applications"** (FleX, ACM TOG) — the unified-particle representation. We take its *particle substrate* and *constraint vocabulary* while declining its rigid-as-particles stance.
- **Bender, Müller, Macklin 2017, "A Survey on Position Based Dynamics"** (Eurographics tutorial) — the consolidated reference for constraint formulations (distance, bending, volume, density) that Phases 8–10 will draw their specific constraints from.

**Throughline.** PBD (2007) gave us the loop; XPBD (2016) gave compliance the physical meaning classic PBD lacked; small-steps (2019) told us to spend our budget on sub-steps rather than inner iterations; Vivace (2016) told us how to color it for the GPU; and the 2020 rigid-XPBD paper marks the fork we are deliberately not fully taking. That is the exact stack this phase implements — XPBD-with-substeps, graph-colored, on the Phase 6 particle substrate.

---

## 5. Where AQUA diverges — the openings

- **Hybrid by construction, not by accident.** Where FleX unifies everything into particles and pays in rigid accuracy, and where a pure XPBD engine would fold rigid in via the 2020 paper, AQUA *keeps* the Phase 3 impulse solver for rigid stacking and uses XPBD only for deformables/particles. We couple the two at the contact level. The divergence is a deliberate accuracy-per-domain bet (§6 alternative, §11 #1).
- **The compliance surface already exists on our contact rows.** `AQConstraintRow::compliance` was seeded in Phase 3/4 for exactly this moment. That means a *future* unified recast is not a rewrite — it's a re-use of an already-shaped field. We diverge from engines that would have to retrofit compliance: ours was pre-wired.
- **One coloring machine, two solvers.** Phase 5's colored-GS coloring and Phase 7's XPBD coloring are the *same* graph-coloring construction over a constraint graph. AQUA gets to share the coloring pass and its GPU batching across the impulse and position-based solvers, rather than maintaining two.
- **POD tagged constraints, no vtable on the kernel path.** Many reference implementations lean on virtual `Constraint::project()` calls. On our substrate that is a non-starter — see §7. We diverge toward typed constraint arrays and a tag switch inside the kernel, which is the coalesced-read, GPU-friendly shape.
- **Determinism as a first-class deliverable.** Stable-cross-path / bitwise-within-path is inherited from Phase 5 and carried into the XPBD projection order. Coloring order and within-color ordering are *fixed and stable*, not "whatever the scheduler did." This is a divergence from the "close enough" determinism most soft-body demos settle for.

---

## 6. Proposed algorithm — XPBD with small substeps + graph-colored parallel projection

The shared core is **XPBD (Macklin 2016) with small sub-steps (Macklin 2019)** — `n` sub-steps × **1** constraint iteration per sub-step — graph-colored à la **Vivace (Fratarcangeli 2016)** for the GPU. This is the loop Phases 8–10 inherit.

**Sub-step loop (per frame, fixed `Δt`, `n` sub-steps, `h = Δt / n`):**

```
frame(dt, n):
    h = dt / n
    for s in 0 .. n-1:            # sub-steps — Macklin 2019: prefer many substeps
        # 1. predict (unconstrained integrate)
        for each particle i (parallel):
            if invMass[i] == 0: continue          # pinned/kinematic
            x_prev[i] = x[i]
            v[i]     += h * gravity               # + other external accels
            x[i]     += h * v[i]

        # 2. project constraints — ONE iteration, colored
        for each constraint c: c.lambda = 0       # reset Lagrange multiplier per substep
        for color in colorBatches:                # colors are serial…
            for each constraint c in color (parallel):   # …within a color, parallel & conflict-free
                project_xpbd(c, h)

        # 3. derive velocities from the position change
        for each particle i (parallel):
            if invMass[i] == 0: continue
            v[i] = (x[i] - x_prev[i]) / h
            v[i] *= (1 - damping)                 # optional XPBD velocity damping — §11 #5
```

**Single-constraint XPBD projection** (distance constraint, particles `a`,`b`, rest length `L`, compliance `α`):

```
project_xpbd(c, h):
    C      = length(x[a] - x[b]) - L             # constraint value
    n      = normalize(x[a] - x[b])              # gradient direction
    wsum   = invMass[a] + invMass[b]
    if wsum == 0: return
    a_tilde = c.alpha / (h*h)                     # <-- compliance/Δt² : the XPBD term
    dLambda = (-C - a_tilde * c.lambda) / (wsum + a_tilde)
    c.lambda += dLambda
    x[a] +=  invMass[a] * dLambda * n
    x[b] += -invMass[b] * dLambda * n
```

**Compliance formula.** `α = 1/k` (inverse stiffness), regularized per sub-step as `α̃ = α / h²`. `α = 0` ⇒ `α̃ = 0` ⇒ the update reduces to the classic *inextensible* PBD projection (compliance = 0 ⇒ rigid, exactly the §1 oracle). Because `h` (not `Δt`) enters `α̃`, and because `λ` accumulates *within a sub-step*, the steady-state stretch a given `α` produces is timestep-independent — the property the oracle checks at 60 Hz vs. 240 Hz.

**Coloring / batching for the GPU.** Build a constraint graph — nodes are constraints, an edge joins two constraints that share a particle. Greedy-color it so no two constraints in a color share a particle. Each color becomes a **batch** projected in parallel with no write conflicts; colors run serially. This is Phase 5's colored-GS construction pointed at distance constraints instead of contact rows. Coloring is computed CPU-side first (§11 #3), stored as `AQConstraintBatch` metadata, and uploaded once; re-color only on topology change, not per frame. Within-color ordering is fixed for bitwise-within-path determinism.

**Guards for the 3am engineer.** `project_xpbd` NaN-guards `n` (coincident particles → zero-length gradient → skip, don't divide). A **constraint-explosion guard** watches per-sub-step `|dLambda|` and per-particle position delta against a loud threshold; on trip it clamps, flags `AQDebugConstraint`, and emits a diagnostic line naming the constraint index, color, and the two particles — not a silent default-return that hides a diverging solve until the whole rope is at infinity. Pinned particles (`invMass == 0`) are skipped explicitly everywhere, never "handled" by a zero that quietly propagates.

**Alternative considered — unified XPBD for rigid too.** The 2020 rigid-XPBD paper lets us fold rigid bodies into this exact loop with rotational constraints, deleting the Phase 3 impulse solver and the coupling seam entirely. We reject it as the *primary* rigid path (§11 #1): position-based rigid stacking is historically softer and less accurate than sequential-impulse contacts — XPBD-2020 narrows that gap but does not erase it — and Phase 3's impulse solver is battle-tested for exactly the stacking case a game engine leans on hardest. We keep unified-XPBD-rigid documented as a possible future consolidation behind the same constraint interface, made cheap by the pre-seeded `AQConstraintRow::compliance`.

---

## 7. New types AQUA must add — `include/aqua/AQXPBD.h` (draft)

AQUA-owned, `AQ`-prefixed, no namespace. POD / trivially-copyable / standard-layout / GPU-uploadable. **The constraint interface is not virtual or polymorphic on the kernel path** — it is a POD tagged record backed by per-type typed arrays. Backend/OmegaSL types stay out of this header (the solver is pimpl).

```cpp
// include/aqua/AQXPBD.h  (draft)
#pragma once
#include <cstdint>
#include "AQMath.h"            // AQVec3f, AQVec3<Ty>
// Matrix/Quaternion borrowed from OmegaGTE GTEMath.h where needed (OmegaGTE::FVec<3>, OmegaGTE::Matrix)

// ----- constraint taxonomy (Distance first; Phases 8-10 append) -----
enum AQConstraintType : uint32_t {
    AQConstraintDistance = 0,   // Phase 7
    AQConstraintBending  = 1,   // Phase 8 (cloth)
    AQConstraintVolume   = 2,   // Phase 9 (solids)
    AQConstraintDensity  = 3,   // Phase 10 (PBF fluids)
    AQConstraintTypeCount
};

// POD tagged constraint record. NO virtuals. The solver switches on `type`.
// Downstream phases add fields via their own typed arrays, not by subclassing.
struct AQConstraint {
    AQConstraintType type;      // tag
    uint32_t         p[4];      // particle indices (Distance uses p[0],p[1]); AQ_INVALID_INDEX = unused
    float            rest;      // rest value (rest length for Distance)
    float            compliance;// alpha = 1/k ; 0 => rigid/inextensible
    float            lambda;    // Lagrange multiplier accumulator (reset per sub-step)
    uint32_t         color;     // graph-color / batch id (correctness precondition on GPU)
};

// Typed view over the distance-constraint array (coalesced GPU reads).
struct AQDistanceConstraint {
    uint32_t a, b;              // particle indices into AQParticlePool
    float    restLength;
    float    compliance;        // alpha
    float    lambda;
    uint32_t color;
};

// Coloring / batching metadata (built CPU-side first — §11 #3).
struct AQConstraintBatch {
    uint32_t colorCount;        // number of colors
    uint32_t firstConstraint;   // offset into the sorted-by-color constraint array
    uint32_t constraintCount;   // constraints in this color batch
};

// Solve parameters (deterministic fixed sub-step).
struct AQXPBDParams {
    float    fixedDt;           // frame Δt
    uint32_t substeps;          // n : Macklin 2019 — prefer many substeps × 1 iteration
    uint32_t iterations;        // inner iterations per substep (lean: 1)
    float    velocityDamping;   // [0,1) post-derive damping — §11 #5
    AQVec3f  gravity;
    float    explosionThreshold;// loud guard on |dLambda| / position delta
};

// Opaque solver handle — pimpl; backend/OmegaSL kernels hidden behind this.
typedef struct AQXPBDSolver AQXPBDSolver;

AQXPBDSolver* AQXPBDSolverCreate(const AQXPBDParams* params);
void          AQXPBDSolverDestroy(AQXPBDSolver* solver);
```

The `AQConstraint` tagged record plus per-type typed arrays (`AQDistanceConstraint[]`, later `AQBendingConstraint[]`, …) is the interface Phases 8–10 implement against. They add a `AQConstraintType` value and a typed array; they never subclass and never add a virtual to the kernel path.

---

## 8. Data layout & GPU/numeric specialization

- **SoA on the Phase 6 substrate.** Positions, previous positions, velocities, and inverse-mass stay in `AQParticlePool`'s existing SoA buffers; Phase 7 adds `x_prev` if not already present. Constraints live in **per-type typed arrays sorted by color** (`AQDistanceConstraint[]`), not one interleaved heterogeneous array — coalesced reads within a color batch (§11 #4). `AQConstraintBatch[]` indexes color ranges.
- **Compute-first, CPU-fallback at parity.** The predict / project / derive-velocity kernels live in `src/kernels/*.omegasl` and dispatch through `AQComputeBackend` / `AQExecPath`. The CPU path is the *same algorithm*, selected via `GTEDeviceFeatures` — never `#ifdef`. Colored projection on the GPU (one dispatch per color) mirrors the CPU's serial-over-colors / parallel-within-color loop, so both paths visit constraints in the same fixed order.
- **Buffers.** Constraint arrays and batch metadata upload once and re-upload only on topology change. `lambda` resets per sub-step on-device. Buffers come from Phase 5's pooled GTE buffers.
- **Numeric specialization.** Solve runs in `float` on both paths for speed and cross-path stability. The **oracle** (energy sum, catenary fit, stretch measurement) runs in `double` off the hot path — `AQVec3<double>` — so measurement precision never depends on solve precision.
- **Determinism.** Fixed sub-step count, fixed color order, fixed within-color order ⇒ stable-cross-path, bitwise-within-path. Radix-sort of constraints by color (Phase 5's sort) uses a total order (color, then constraint id) so ties are resolved deterministically.

---

## 9. Validation — how we measure "better"

The rope deliverable is the harness; the oracle is `double`-precision and off-path.

1. **Energy monotonicity.** Released to hang, total mechanical energy (kinetic + gravitational PE + a compliance-strain term) must decrease monotonically to a rest floor. Any per-sub-step energy *increase* above a floating-point noise band is a failure — it means the projection is injecting energy (a classic PBD/XPBD misconfiguration). Regression-gated.
2. **Catenary fit.** The settled rope must match the analytic catenary `y = a·cosh(x/a)` for its mass/segment/gravity within a stated RMS tolerance. This catches subtle projection-order or compliance-scaling bugs a length check would miss.
3. **compliance = 0 ⇒ inextensible.** Under load, every segment length holds to rest length within solver tolerance. If segments stretch at `α = 0`, the `α̃` term is mis-derived.
4. **Timestep-independent stretch (the headline oracle).** The *same* `α > 0` under the *same* static load must produce the *same* steady-state stretch at 60 Hz and at 240 Hz sub-stepping, within tolerance. This is the property classic PBD *fails* and XPBD *passes* — it is the reason this core exists, so it is the reason we validate hardest here.
5. **CPU/GPU parity.** Rope settles to the same state on both paths, within the cross-path tolerance; identical color/constraint ordering on both.
6. **Determinism.** Same inputs ⇒ bitwise-identical trajectory within a path across runs.
7. **Guard behavior.** A deliberately over-stiff / degenerate rig trips the explosion guard *loudly* — flagged, clamped, diagnostic line emitted — never a silent NaN that surfaces three frames later.

Visualize via the debug bus: `AQDebugConstraint` draws constraints (color-graded by strain), `AQDebugConstraintColor` draws the graph-coloring so a human can eyeball that no color shares a particle.

---

## 10. Public API additions

New bits on the debug flags (added, not renumbered):

```cpp
// AQDebug.h additions
enum AQDebugFlags : uint32_t {
    /* …existing… */
    AQDebugConstraint      = 1u << /*next*/,   // draw active constraints, strain-graded
    AQDebugConstraintColor = 1u << /*next+1*/, // draw graph-coloring batches
};
```

`AQSpace` gains XPBD body + constraint authoring (constraint-authoring API; kernels/backend hidden by pimpl):

```cpp
// AQSpace additions
// Create an XPBD body over Phase 6 particles; returns a handle.
AQXPBDBodyHandle AQSpace::createXPBDBody(const AQXPBDBodyDesc* desc);

// Author a distance constraint between two particles (Phase 7).
AQConstraintHandle AQSpace::addDistanceConstraint(
    AQXPBDBodyHandle body, uint32_t a, uint32_t b,
    float restLength, float compliance);

// Convenience: a chain of distance constraints (the rope deliverable).
AQConstraintHandle AQSpace::addRope(
    AQXPBDBodyHandle body, const uint32_t* particles, uint32_t count,
    float compliance);

// Solver params + debug bus (existing setDebugFlags/debugFlags/drainDebugLines).
void AQSpace::setXPBDParams(const AQXPBDParams* params);
```

`addDistanceConstraint` is the seed of the whole interface — Phases 8–10 add `addBendingConstraint`, `addVolumeConstraint`, `addDensityConstraint` against the same solver.

---

## 11. Open decisions for this phase

1. **Hybrid vs. unified (roadmap §7.2) — lean HYBRID.** Keep Phase 3's impulse (sequential-impulse PGS) solver for **rigid stacking**; use XPBD only for **deformables/particles**; couple the two **at the contact level** — particles/cloth read rigid contacts, rigid bodies receive impulses back. Rationale: position-based rigid stacking is historically less accurate; the 2020 rigid-XPBD paper narrows the gap but does not erase it; the impulse solver is battle-tested for exactly the case games lean on. **Cost, stated plainly:** a coupling seam between two solvers that must exchange contact data each step and stay deterministic across it. We accept the seam because each domain then uses the method it is genuinely best at, and because the pre-seeded `AQConstraintRow::compliance` keeps the unified option cheap to revisit later.

Vet which algorithims are better/faster/more accurate, and then make a descision. I would lean Unified because it seems that a lot of newer research leans on more GPU accelerated solvers that use a unified XPBD. If we need seperate solvers for other things that need reliable and optimized results, implement those too.

2. **Substeps vs. iterations — lean MANY SUBSTEPS × 1 ITERATION** (Macklin 2019). `n` sub-steps × 1 constraint iteration beats `n` iterations × 1 step at equal cost for stability and accuracy. `AQXPBDParams.iterations` stays configurable (default 1) as an escape hatch, but the tuned path is sub-stepping.
3. **Coloring strategy — lean CPU-COLOR-FIRST.** Greedy graph-coloring on the CPU first (built once, re-colored on topology change), GPU coloring later — mirroring Phase 5's colored-GS progression. CPU coloring is simpler to make deterministic and correct; GPU coloring is a follow-up optimization behind the same `AQConstraintBatch` metadata.
4. **Constraint storage — lean PER-TYPE TYPED ARRAYS.** One interleaved heterogeneous POD array is simpler to author but scatters GPU reads across differently-shaped records. Per-type typed arrays (`AQDistanceConstraint[]`, …) give coalesced reads and a clean tag-switch kernel. The `AQConstraint` tagged record remains the *logical* interface; the *physical* storage is typed.
5. **Velocity update — lean XPBD POSITION-DERIVED VELOCITIES + optional damping.** Derive `v = (x - x_prev)/h` post-projection (standard XPBD), with an optional `velocityDamping` term for overdamped materials (rope/cloth settle). Open sub-question deferred to Phase 8: whether cloth needs the more elaborate velocity-level damping of the 2020/cloth literature, or whether scalar damping suffices.

---

## 12. Recency-principle audit (addendum, 2026-07-01)

Per the recency principle, does anything newer than the 2016–2020 XPBD stack change the shared-core choice?

- **Chen, Macklin et al. 2024, "Vertex Block Descent"** (SIGGRAPH 2024) — a block-coordinate-descent solver: iteratively minimize the incremental potential over vertices/blocks. It is, honestly assessed, **more stable and more accurate than XPBD** as a solver, and it is very GPU-parallel (per-vertex/per-block updates color the same way). This is the strong alternative and we flag it as such — not dismissed. We nonetheless adopt **XPBD-with-substeps as the shared core now**, for three concrete reasons: (a) it is the interface Phases 8–10 *and* the fluids PBF path (Macklin & Müller 2013, "Position Based Fluids") already assume — density constraints are an XPBD constraint; (b) it composes cleanly with the §11 #1 hybrid coupling to the Phase 3 impulse solver, where VBD's coupling story to a separate impulse contact solver is less worked-out; (c) VBD slots in as a **Phase 7.x / 8.x upgrade behind the same `AQConstraint` interface** — the typed-constraint, colored-batch layout is exactly what a VBD vertex-block pass wants, so nothing downstream has to change to adopt it later.
- **Fei et al. 2023, "A Survey of Rigid Body Simulation with XPBD"** (arXiv 2311.09327) — consolidates the rigid-XPBD line (the §11 #1 unified option). Read as corroboration that unified-rigid-XPBD is a real, maturing path — reinforcing our decision to keep it *available* behind the pre-seeded compliance field rather than *primary*.
- **Mercier-Aubin et al. 2024, "A Robust and Efficient Multi-layer XPBD Solver"** (Computer Graphics Forum 2024) — robustness/efficiency improvements *within* XPBD (layered/multi-resolution constraint solving). This is an in-family enhancement we can fold into our XPBD core incrementally without an interface change — the good kind of recency finding.

**Net for Phase 7.** No change to the shared-core choice. Ship **XPBD (Macklin 2016) + small substeps (Macklin 2019) + Vivace graph-coloring (2016)** on the Phase 6 particle substrate, hybrid-coupled to the Phase 3 impulse solver (§11 #1). Keep **VBD (2024) as the documented behind-the-interface upgrade** — the constraint interface and colored-batch layout are chosen so VBD is a solver swap, not a rewrite. Multi-layer XPBD (2024) is an in-family enhancement; rigid-XPBD (2020 / Fei 2023) stays the archived unified option.

**Re-audit due.** Before Phase 8 (cloth) starts — re-check whether VBD adoption should be pulled forward given cloth's stiffness demands, and whether any 2026 follow-up to VBD changes the "upgrade-behind-the-interface" calculus.

---

*Brief status: proposal. Awaiting developer confirmation on §11 #1 (hybrid vs. unified) before the interface in §7 is frozen — that fork is the one downstream phases cannot cheaply undo.*
