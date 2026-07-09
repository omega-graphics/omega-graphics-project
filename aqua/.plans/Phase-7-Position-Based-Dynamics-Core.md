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

## 13. Implementation phasing (settled 2026-07-07)

§1–§12 are the *proposal*; this section is the *implementation contract* the code lands against, written after a ground-truth pass over the shipped Phase 5/6 substrate (the same discipline Phase 6 §13 established — where §1–§12 and §13 disagree, §13 governs).

### 13.0 The §11 #1 fork — developer decision: UNIFIED destination, vet-corrected migration path (settled 2026-07-07, sweep folded in same day)

The developer overrode the brief's hybrid lean: **the destination architecture is unified — one position-based constraint substrate, with rigid bodies eventually folded onto it — with the explicit proviso that any domain where a dedicated solver measurably beats the unified path keeps its dedicated solver.** Per the developer's own directive the algorithms were vetted first (the 2026-07-07 recency sweep, findings in §13.5-A); the vet **confirmed the unified destination but rejected rigid-XPBD as the migration vehicle**:

- Catto's Solver2D (2024) cross-solver study: **TGS-Soft (impulse + substepping + soft constraints) beat XPBD for rigid** under equal budgets — XPBD showed friction trouble and a far-from-origin precision failure — and TGS-Soft is what shipped in Box2D v3. The 2023 rigid-XPBD survey (arXiv 2311.09327) concedes the same weak spot: stable rigid stacking.
- **No production engine (2026) runs rigid on XPBD or VBD as its primary path** — Newton (MuJoCo-Warp rigid), PhysX 5 (PGS/TGS), Chaos (RBAN/TGS-style) all keep impulse/convex-constraint rigid solvers and use the position-based family for deformables.
- The credible unified-rigid algorithm is **AVBD (Giles, Diaz, Yuksel — SIGGRAPH 2025)**: augmented-Lagrangian VBD handling rigid stacking, friction, joints, and extreme mass ratios at remarkable GPU scale — but it is ~1 year old, self-benchmarked, demo-grade in the open, and its fluids story is unsettled. The right posture is *prototype-and-gate*, not *bet-the-rigid-pillar*.
- A key ground truth surfaced by the vet: **AQUA's rigid pillar already embodies the solver2d winner's recipe** — small fixed sub-steps (AQContext, 1/120 default), Catto-2011 soft constraints (`AQConstraintRow::compliance`, folded as `1/(Keff + compliance/dt²)` + CFM), sequential-impulse PGS with warm starting. There is no quality gap on the rigid side waiting for a recast to fix.

What the decision means concretely:

- **Phase 7 ships the unified substrate's core** — the XPBD solver, typed-constraint interface, and colored batching — and every downstream deformable phase (8/9/10) plus PBF fluids authors against it. Fork-independent; DONE (13.3).
- **One clock already unifies the loop.** Rigid, particles, and XPBD all step inside `AQContext::advance`'s single fixed sub-step (the sweep's "unified substep loop, coupled at contacts" — the Newton/PhysX shape) — so the near-term "unified" is architectural: one timestep authority, one coloring machine, one constraint vocabulary, coupled at contacts (7g).
- **Rigid stays impulse for now; the migration target for true unification is AVBD-class, prototyped behind the interface and oracle-gated.** The typed-constraint + colored-batch layout was purpose-shaped so a vertex-block/augmented-Lagrangian pass is a solver swap, not a rewrite. Any migration must pass the *existing Phase 3/4 battery* (plus a high-mass-ratio stack case added for the solver2d failure mode) at equal-or-better quality AND performance before an impulse capability retires; whatever the impulse solver still wins, it keeps — the developer's "separate solvers where reliability demands it" proviso, enforced by oracle rather than by taste. Rigid-XPBD (Müller 2020 / the pre-seeded row compliance) remains the documented fallback recast if AVBD stalls, but it is no longer the scheduled path.

`Physics-Roadmap.md` §7 item 2 records the decision.

### 13.1 Substrate corrections (what the proposal got wrong, verified against source 2026-07-07)

- **There is no `AQVec3f`** — the vector type is `AQVec3<Ty>` = `OmegaGTE::Matrix<Ty,3,1>` (`OmegaGTE::FVec<3>` in float form), constructed via `AQvec3(x,y,z)`, indexed `v[i][0]` (Phase 6 §13.1 found the same). The §7 header draft is corrected accordingly.
- **`AQXPBDParams` carries no `fixedDt` and no `gravity`.** The engine has ONE clock — `AQContext`'s fixed sub-step (Phase 6 §14.1) — and one gravity, the space's. XPBD subdivides the engine sub-step into `substeps` slices of `h = fixedDt/substeps`; it never forks the timestep authority.
- **The generic tagged `AQConstraint` record is dropped.** The logical interface is the `AQConstraintType` enum + per-type typed arrays (the §11 #4 lean, adopted); nothing would ever instantiate a union-of-all-types record, and kernels switch on the *batch's* type, not per-record tags.
- **"GPU-accelerated" is a follow-on sub-phase, not this deliverable.** Ground truth: the engine is CPU-live everywhere (Phase 6 §13.1 — rigid GPU stages are stage-isolation-validated but not live; the Phase 6 GPU sub-phase 6f–6i is itself still planned). Phase 7 mirrors the house architecture: CPU-live core + `double` measurement oracle now, OmegaSL kernels behind parity tests as 13.4's first increment. The colored batching — the GPU correctness precondition — is designed and tested in from day one, so the kernel port is a transliteration, not a redesign.
- **XPBD bodies do NOT live inside `AQParticleSystem`.** The §1 claim "operates on the Phase 6 particle substrate" is corrected: emitter pools expire particles and *stable-compact their slots*, which would corrupt constraint particle indices. `AQXPBDBody` owns persistent SoA arrays (same layout discipline, stable indices for the body's lifetime). Phase 6's neighbor-grid deferral is also moot for Phase 7: a rope's constraint topology is explicit, so no neighbor search ships here (it arrives with Phase 8 self-collision / Phase 10 fluids, where compact hashing per the Phase 2/6 audits earns its place).
- **Debug bits land at `1u<<18` / `1u<<19`** (`AQDebugConstraint`, `AQDebugConstraintColor`) — 16/17 were taken by Phase 6.

### 13.2 Settled decisions (resolves §11)

| §11 | Decision | Consequence |
|-----|----------|-------------|
| 11.1 Hybrid vs unified | **UNIFIED destination, vet-corrected path** (§13.0) | Phase 7 core ships now; rigid stays impulse (already the TGS-Soft family) coupled at contacts under the one clock; AVBD-class is the prototyped, oracle-gated migration target for true unification; rigid-XPBD demoted to documented fallback. |
| 11.2 Substeps vs iterations | **Many substeps × 1 iteration** (as leaned) | `AQXPBDParams{substeps, iterations=1}`; iterations kept as the escape hatch. The rope oracles quantified the residual: 1-iteration Gauss-Seidel on an N-deep chain leaves ~`(N²/2)·g·h²` of unconverged stretch, so stiff scenes buy substeps (quadratic payoff), exactly the Macklin 2019 doctrine. |
| 11.3 Coloring | **CPU-color-first** (as leaned) | Deterministic greedy (authoring order, smallest-free-color), re-colored on topology change only; `AQConstraintBatch` ranges over a (color, authoring-index)-sorted mirror. GPU coloring stays a follow-up behind the same metadata. |
| 11.4 Constraint storage | **Per-type typed arrays** (as leaned) | `AQDistanceConstraint[]` color-sorted for coalesced batch reads; λ lives in the record so the GPU uploads one buffer per type. |
| 11.5 Velocity update | **Position-derived + optional scalar damping** (as leaned) | `v = (x−x_prev)/h`, `velocityDamping ∈ [0,1)`. Cloth-grade velocity damping deferred to Phase 8 as scoped. |

### 13.3 Increment breakdown — CPU-live core (DONE, verified 2026-07-07)

- **7a — Public types + debug bits.** `include/aqua/AQXPBD.h` (§7 draft with 13.1 corrections; `static_assert` trivially-copyable): `AQConstraintType`, `AQDistanceConstraint`, `AQConstraintBatch`, `AQXPBDParams`, `AQXPBDBodyDesc`, `AQXPBDBodyHandle`/`AQConstraintHandle`. `AQDebugConstraint`/`AQDebugConstraintColor` in `AQDebug.h`.
- **7b — Storage + authoring + deterministic coloring.** Internal `AQXPBDBody` (`src/AQSpaceImpl.h`): persistent particle SoA (positions/prevPositions/velocities/invMass), authoring-order constraint array + color-sorted mirror + batches; implementation in the new TU `src/AQSpaceXPBD.cpp` (the Phase 6 second-TU pattern). Public authoring on `AQSpace`: `createXPBDBody`/`destroyXPBDBody`/`addDistanceConstraint` (negative rest ⇒ derive from spacing)/`addRope`/`setXPBDParams`/`xpbdParams`/`readXPBDState`/`readXPBDConstraints`/`xpbdGuardTrips`. Test `aqua_xpbd_color_test` (22 assertions: conflict-free contiguous batches, chain=2 colors alternating, star=degree colors, coloring determinism, topology-change recolor, public round-trip).
- **7c — Solver core + wiring + guards.** Scalar-generic math in `src/AQXPBDMath.h` (`AQxpbdPredict` / `AQxpbdProjectDistance` / `AQxpbdDeriveVelocity` — the float path and the `double` oracle share it, the `AQParticleMath.h` discipline). `AQXPBDBody::advance`: per engine sub-step, `substeps` slices of predict → colored projection (colors serial, fixed order within color) → derive. Guards IN the projection: zero-length-gradient skip (no NaN); per-projection correction clamped to `explosionThreshold` with λ kept consistent, trips counted + latched-loud on stderr naming constraint/color/particles. Wired into `AQContext::advance` beside `particlesSubstep` (one clock). Test `aqua_xpbd_step_test` (22 assertions: hand-computed rigid/compliant/pinned/degenerate/clamped projections, λ accumulation closed form, free-fall closed form across slices, 10 s pendulum rigidity ≤1e-4, loud guard, bitwise determinism). *Measured note:* XPBD's derived velocity `(x−x_prev)/h` re-extracts v through a subtraction at position scale — a ~2e-3 random walk over 240 slices vs 3e-6 for a pure accumulator; inherent to the position-level formulation, tolerated in the oracle.
- **7d — Debug bus.** `xpbdFrameEnd` (once per advance, the Phase 6 compact cadence): `AQDebugConstraint` strain-graded lines (tension→red, compression→blue, guard-tripped flat red), `AQDebugConstraintColor` palette-tinted by batch id.
- **7e — The rope deliverable + `double` oracles.** `aqua_xpbd_rope_test` drives the PUBLIC surface; measurement (never the solve) runs in `double` (§8). Verified: **energy no-injection** (undamped 10 s swing never exceeds release energy ×(1+1e-4)); **monotone damped settle** to a vertical rest (KE < 1e-6 J); **catenary** — two-pin slack rope settles to the analytic `y = a·cosh(x/a)` at **RMS 3.96 mm on a 2 m rope (0.2 % of L)**; **α=0 inextensible** under a 50× end load (max stretch 0.26 %, substeps 64 per the 13.2 residual note); **THE HEADLINE** — same α at 60 Hz×24 vs 240 Hz×8 slices: total stretch 0.4056 m vs 0.3996 m (rel diff **1.5 %**, tolerance 3 %), both bracketing the analytic 0.402 m, and per-segment stretch matches `C_i = α·T_i` within 5 % mid-rope; **bitwise within-path determinism**; **loud guard** on a degenerate rig with finite state after clamping; **debug bus** emits when flagged, silent when not.

**Status: 7a–7e COMPLETE.** Full AQUA suite green (20/20 including all prior-phase regressions and the GPU stage tests). New files: `include/aqua/AQXPBD.h`; `src/AQXPBDMath.h`, `src/AQSpaceXPBD.cpp`; tests `aqua_xpbd_{color,step,rope}_test.cpp`. Touched: `AQDebug.h` (+2 bits), `AQSpace.h` (authoring surface + private hooks), `AQSpaceImpl.h` (+`AQXPBDBody`, impl table), `AQContext.cpp` (advance wiring), `tests/CMakeLists.txt`.

### 13.4 Follow-on increments (planned, in order)

- **7f — GPU sub-phase. DONE (implemented + verified 2026-07-07).** OmegaSL kernels in `src/kernels/AQXPBD.omegasl` — `AQXPBDPredict` / `AQXPBDZeroLambda` / `AQXPBDProjectDistance` / `AQXPBDDerive`, transliterated from `AQXPBDMath.h` with the CPU float operation order preserved (the 5c discipline; guards ride IN the projection: zero-length-gradient skip, λ-consistent clamp, per-constraint trip counters — plain writes, no atomics, since a constraint belongs to exactly one thread of one color dispatch). Dispatch shape as designed: one dispatch per color over the CONTIGUOUS color-sorted range (no indirection table — the 7b layout paid off), colors serial, per-engine-sub-step slices batched into one command buffer with one sync. Backend (`src/AQComputeXPBD.cpp`, the second-TU pattern): per-body resident pools keyed by the body's opaque id (`XPBDPoolGPU` — pos/vel Universal, prev Readback, con with in-record λ + trips Universal), `uploadXPBDParticles`/`uploadXPBDConstraints` (topology change only)/`encodeXPBDAdvance` (frame-batchable via `engineSubsteps`)/`downloadXPBDParticles`/`downloadXPBDTrips`/`xpbdReleasePool`.
  - *Stage-isolation parity* (`aqua_gpu_xpbd_test`, device-guarded): rigid swing (α=0, 17 particles, 30 sub-steps) max pos gap **2.2e-06** / vel **2.3e-04** (band 2e-3); compliant damped settle (α=0.002, 2 s) max pos gap **3.0e-05**, identical tension stretch (0.9707 vs 0.8 rest) on both paths; pinned particle bit-exact; two GPU runs byte-identical; degenerate rig trips the guard on BOTH paths with finite clamped state.
  - *Live-path flip* (post-6h, same day — the 6h conventions this was gated on landed first): `AQContext::loadKernels` + `setExecutionPath(GPU)` route `xpbdSubstep`'s work to `AQSpace::xpbdGpuFrame` — whole advance-frame (nSub engine sub-steps × substeps slices) in one encode per body, uploads on topology change only (`colorsDirty`/`gpuUploadNeeded`, with a host re-base download before re-seeding), `readXPBDState`/debug-draw as the lazy readback sync points, guard trips folded from the device's cumulative counters into `guardTrips` + the flat-red debug grading as per-frame deltas (loud once per body). Verified through the public surface: GPU-context rope vs CPU-only context max pos gap **1.9e-06**, pinned bit-exact, zero trips.
- **7g — Rigid↔XPBD contact coupling (the seam Phases 8/9 need). DONE 2026-07-08 — implementation contract + measured results in §13.6.** Two-way coupling at the contact level under the one clock: XPBD particles read rigid collider poses (the Phase 6 one-way push-out generalized), rigid bodies receive the reaction back. Shipped as a velocity-level split-impulse (the position-impulse first cut was wrong; §13.6); one-way + per-contact momentum-conserving two-way + single-contact support all green; multi-point resting of a dynamic rigid body on a curved bed deferred (needs a PGS-grade solve). The near-term "unified" (§13.0): one substep loop, coupled at contacts — the Newton/PhysX shape.
- **7h — Convergence + robustness upgrades (flagged from the 7e data and the sweep). DONE 2026-07-08 (concrete items) — §13.7.** (1) Long-range attachments for pinned ropes/cloth — SHIPPED (multi-layer XPBD (Mercier-Aubin 2024) recorded as the deferred general alternative). (2) Far-from-origin hardening (origin-relative solve) — SHIPPED. (3) Gather-based Jacobi occupancy path — EVALUATED, NOT BUILT (per §13.5-B this supersedes the "VBD vertex coloring" phrasing; per-color dispatch is the deterministic baseline and the rope/cloth color counts leave no occupancy bottleneck; recorded with a measured-stall trigger).
- **7i — AVBD prototype behind the interface, oracle-gated (the §13.0 unification target). DEFERRED (developer scope decision 2026-07-08 + trigger not fired).** At the pre-Phase-8 re-audit: prototype an augmented-Lagrangian vertex-block pass (AVBD, SIGGRAPH 2025) behind the same typed-constraint + batch metadata; validate rigid stacking (incl. a high-mass-ratio case for the solver2d failure mode), friction, joints, and within-path determinism against the Phase 3/4 battery on this substrate. Migrate rigid capabilities only on equal-or-better quality AND performance; rigid-XPBD (Müller 2020, pre-seeded row compliance) stays the documented fallback recast. Not started this pass — its promotion trigger (§13.5-B: Newton de-experimentalizing `SolverVBD`, or a second independent production adoption) has not fired, and the developer scoped this session to 7g + 7h.

### 13.5 Prior-phase recency audits — application record (per the 2026-07-07 developer directive)

Sweep of Phases 1–6 §12/§13 audits for adopt-now performance items, and what happened to each:

- **Phase 3: Nesterov-accelerated GJK (Montaut 2024) — the one adopt-now item — ALREADY LANDED** in `src/AQGJK.cpp` (verified: momentum-blended search direction + Frank-Wolfe duality-gap fallback). No action.
- **Phase 1: Lie-group variational integrators — stays deferred**; wants whole-step ownership that conflicts with the solver interrupt, and the unified path (13.0) supersedes the question for bodies that migrate (XPBD *is* the integrator there).
- **Phase 2/5: RT-core broadphase (Wang 2024) — stays hardware-gated** (vendor-specific vs the all-three-backends contract); PLOC++/PRBVH stays the escape-hatch citation (grid still wins for this substrate).
- **Phase 4: parallel union-find (Jaiganesh/Burtscher) — still parked** for the eventual GPU island port; nothing live-GPU exists to attach it to yet.
- **Phase 5: VBD/GPU-XPBD "Phase 7 candidates" — resolved by this phase**: XPBD shipped as the core (matches the PBF/cloth constraint vocabulary Phases 8–10 assume), VBD/AVBD held as the behind-the-interface swap (7h).
- **Phase 6: compact hashing for the particle pillar — correctly still deferred**; no neighbor queries exist in Phase 7's explicit-topology constraints. Lands with Phase 8 self-collision / Phase 10 fluids.

### 13.5-A The 2026-07-07 recency sweep (the §12 "re-audit due" delivered — fresh PBD/XPBD/FleX-successor pass)

Run per the Phase 6 audit's demand ("fresh sweep of the PBD/XPBD line and the FleX successors before Phase 7 breaks ground") and the developer's vet directive. Findings that drove §13.0, with citations:

- **Catto, Solver2D (2024)** (box2d.org/posts/2024/02/solver2d, github.com/erincatto/solver2d): cross-solver rigid comparison under equal iteration budgets (PGS/PGS-Soft/TGS variants/XPBD). **TGS-Soft won and shipped in Box2D v3**; XPBD lost with concrete failure modes — friction quality and a far-from-origin precision bug (fixed by solving on position deltas; carried into our 7h #2). The transferable insight: **substepping + soft constraints are the load-bearing modern ingredients, orthogonal to position projection** — and AQUA's rigid pillar already has both (Phase 1 small steps; Phase 4 Catto-2011 row compliance).
- **AVBD — Giles, Diaz, Yuksel, SIGGRAPH 2025 (TOG 44(4) #90): VERIFIED.** Augmented-Lagrangian layer over VBD: hard constraints at effectively infinite stiffness, rigid stacking + friction + joints + high mass ratios; self-reported 110k-block pile at 3.5 ms solver time on an RTX 4090; parallelizes on VBD's *vertex* coloring + one parallel dual-update pass. The first credible "one solver including rigid." Caveats held against it in §13.0: ~1 year old, all benchmarks self-reported (no independent matched-accuracy reproduction found), open implementations are demo-grade, no bitwise-determinism claim, VBD-native fluids still research (SIGGRAPH Asia 2025 "Implicit Position-Based Fluids" is adjacent, not settled).
- **Production census (2026): no engine runs rigid on XPBD/VBD as primary.** NVIDIA Newton: MuJoCo-Warp rigid (primary), XPBD for deformables + secondary contact option, VBD for cloth/cable. Genesis: MuJoCo-lineage Newton-type rigid. PhysX 5.x: PGS/TGS rigid, FEM+PBD deformables. Chaos: RBAN (TGS-style) rigid, XPBD cloth/flesh. The position-based family is the industry's *deformable* substrate — corroborating Phase 7's shape.
- **PBF fluids compose with XPBD, not (yet) VBD** — density constraints are compliant constraints in this exact projection loop; a VBD core would complicate Phase 10. Corroborates XPBD as the shipped core.
- **Coloring: VBD's vertex coloring beats Vivace-style constraint coloring on color count** (paper: 3 vs 7 for triangles, 8 vs 76 for tets) → better GPU occupancy; carried as 7h #3 for the 7f kernel port. Also flagged for later: JGS2 (arXiv 2506.06494), MGPBD (SIGGRAPH 2025) as GPU-convergence lines; colored GS remains within-path deterministic by construction (no paper claims cross-vendor bitwise — our stance already assumes only stable-cross-path).
- **Flags (unverified, recorded honestly):** the arXiv 2311.09327 survey's first-author attribution ("Fei et al." in §12) could not be confirmed — title/venue verified, surname not; Newton's "primary rigid" is described inconsistently across sources (MuJoCo-Warp per the preponderance); all XPBD-vs-VBD-vs-TGS speed numbers are author-reported.

**Net:** unified stays the destination (developer), XPBD stays the shipped deformable core (fluids fit + maturity + this phase's passing oracles), rigid stays impulse near-term (it is already the winning family), and **AVBD replaces rigid-XPBD as the unification vehicle**, prototyped and oracle-gated at 7i. Re-audit at Phase 8 start as §12 already scheduled.

### 13.5-B The 2026-07-07 GPU-port recency sweep (run before 7f broke ground, per the developer's "GPU side + recency audits" directive)

A second same-day sweep, scoped to the KERNEL-PORT choices (dispatch/coloring structure, small-steps, portable scan, compaction determinism, AVBD/Newton status, emission/death practice). Verdicts, with what the 7f implementation did about each:

- **Q1 — coloring/dispatch: no-divergence; per-color stays the deterministic baseline; one determinism trap flagged.** Vivace-style constraint coloring remains the textbook baseline, but *shipping* GPU-XPBD does something else: NVIDIA Warp's `XPBDIntegrator` uses **no coloring at all** — Jacobi with `wp.atomic_add` into per-particle delta buffers (verified from Warp v1.7.0 source), and FleX is Jacobi+SOR. That route is a trap for AQUA: **float atomic accumulation is scheduler-ordered and NOT bitwise-within-path** — adopting the "shipping norm" would break the determinism stance. 7f therefore shipped per-color dispatch (deterministic by construction). The deterministic *evolution* path, if color count ever bottlenecks occupancy, is **gather-based Jacobi** (each particle sums its incident constraints in fixed order — no atomics; also the structural on-ramp to VBD's vertex coloring), NOT scatter-atomics. Supersedes the phrasing of 7h #3: vertex coloring per se is a solver-family move (VBD ≠ XPBD; no published precedent for vertex-colored XPBD-proper); the transferable idea is the gather-based per-vertex application.
- **Q2 — small steps: no-divergence.** Nothing 2024–26 overturns n×1 (MGPBD SIGGRAPH 2025 / JGS2 arXiv:2506.06494 sharpen where it's *insufficient* — global-convergence regimes — without changing the substep doctrine; both self-reported). `AQXPBDParams.iterations` stays the exposed knob (default 1), as shipped.
- **Q3 — portable scan (consumed by Phase 6's 6g, recorded here for the shared primitive): adopt-now = multi-pass reduce-then-scan; decoupled look-back is a hard no-go.** Merrill & Garland 2016 single-pass requires forward-progress guarantees no portable target provides — named-FPG-lacking hardware includes Apple M1 Max/M3 (Smith, SPAA 2025), i.e. it can DEADLOCK on Metal; portable libraries (FidelityFX Parallel Sort; GPUSorting's DeviceRadixSort, whose README states the portability rationale explicitly) ship multi-pass. 6g implemented exactly that (block shared-memory scans + recursive block sums + uniform add).
- **Q4 — compaction: no-divergence.** Exclusive-scan + stable-scatter remains the only order-preserving family; integer 0/1-mask scans are bit-exact cross-device. Confirms the 6h design; the real risk (a float-derived alive flag) is exactly what 6f's integer death removed.
- **Q5 — AVBD/Newton: flagged-for-later; the 7i posture is unchanged with a sharper trigger.** AVBD verified (Giles/Diaz/Yuksel, TOG 44(4) 2025, DOI 10.1145/3731195; scale claims still self-reported; open code demo-grade). NEW: Newton's `SolverVBD` now ships a unified VBD+AVBD path with augmented-Lagrangian rigid contacts (v1.2.0) — but v1.3.0 marks it **experimental**, with MuJoCo/Featherstone still primary. **7i re-evaluation trigger, recorded: promote the AVBD prototype when Newton de-experimentalizes SolverVBD or a second independent production adoption appears.** VBD-formulation fluids confirmed as research (Implicit Position-Based Fluids, SIGGRAPH Asia 2025, DOI 10.1145/3757377.3764005).
- **Q6 — emission/death (Phase 6's model, shared record): adopt-now (design confirmed); counter-based RNG flagged.** Mainstream GPU emission is atomic-append + float age — documented-nondeterministic (Unity VFX Graph concedes it); host-seeded emission + integer countdown targets exactly those failure modes. Roadmap flag: **counter-based/stateless RNG (Philox/Threefry, Salmon et al. SC11)** keyed on stable particle ids would allow GPU-side emission *without* losing determinism — adopt if host-emit upload ever bottlenecks.

**Net for the port:** ship per-color XPBD dispatch (done, 7f), multi-pass scan (done, 6g), integer-death compaction (done, 6f/6h) — all three "shipping-practice shortcuts" the sweep surveyed (float-atomic Jacobi, decoupled look-back, GPU atomic-append emission) were rejected for the same reason: they trade AQUA's bitwise-within-path determinism for throughput AQUA doesn't yet need.

---

### 13.6 Increment 7g — rigid↔XPBD contact coupling (implementation contract, 2026-07-08)

The seam Phases 8/9 depend on ("Cloth/hair↔rigid collision — two-way coupling with the rigid solver" in the roadmap's Phase 8). It **generalizes the Phase 6 one-way particle push-out** (`AQParticleSystem::collide` → `AQshapeSignedDistanceGeneric`) to XPBD bodies and makes it **two-way**: XPBD particles read rigid collider poses; rigid bodies receive the reaction impulse. This is the near-term "unified" of §13.0 — one substep loop, coupled at contacts, the Newton/PhysX shape. Under ~500 lines, so a single note per the AGENTS.md small-feature rule, not a sub-bullet breakdown.

- **Where it runs.** Inside the XPBD slice loop, *after* the colored distance projection and *before* the velocity derive (contact is the last position correction of the slice, so the derived `v = (x−x_prev)/h` already reflects the inelastic push-out — no explicit restitution term needed, the position-based-contact win). Opt-in per body: `AQXPBDBody::collisionEnabled` (default off, so every existing oracle is untouched).
- **The collider snapshot** is gathered at the top of `xpbdSubstep` from `impl->bodies` via the *public* `AQRigidBody` accessors (the particle-path convention): `{shape, xform (position+orientation), invMass, worldInverseInertia, restitution, bodyIndex, dynamic}`. Read once per engine sub-step so it sees post-`stepInternal` poses (more current than the frozen advance-start particle snapshot). Gathered only when some XPBD body has collision on. Hull shapes return +inf ⇒ no contact (Phase 6 parity; convex-hull XPBD contact is deferred with the rigid hull collision).
- **The two-way projection — VELOCITY-LEVEL split-impulse (corrected during implementation; the first cut, a position-impulse `Δλ/h`, was WRONG — see the note below).** For particle `i` (inverse mass `w_p = invMass[i]`, optional radius) vs collider with outward surface normal `n` and signed distance `sd`: `pen = radius − sd`; skip if `pen ≤ 0` (no penetration). Body effective inverse mass along `n` is the Catto-2005 term the rigid solver already uses: `w_b = invMassBody + (r×n)·I⁻¹(r×n)`, `r = contact − com` (0 for static/kinematic). Relative normal velocity `vrelN = (v_particle − v_bodyContact)·n`, where `v_particle = (x − x_prev)/h` (the particle's implied velocity; 0 for a pinned lane) and `v_bodyContact = vel + ω×r`. Two parts: **VELOCITY** — a pure inelastic stop `λ = max(0, −vrelN/(w_p+w_b))`, realized on the particle as a POSITION shift `w_p·λ·h·n` (so the slice's derive ÷h reproduces the velocity change, keeping the particle inside the position-based framework) and on the body as an impulse `−λ·n`. **POSITION** — a split-impulse pseudo-recovery `β·(pen−slop)` (β=0.2): a real push-out share on the particle, and a pseudo-position push on the body (`setPosition`, no velocity ⇒ no energy). The stop targets `vrelN=0`, *not* a separating bias — a bias would launch a resting body off the surface. Static bodies (`w_b=0`) reproduce the one-way push-out.
  - *Why not the position-impulse.* `J_b = −(Δλ/h)·n` (correct full penetration in one slice, converted to a velocity impulse) explodes when a fast body is caught after sinking a whole frame: `pen/h` with the tiny XPBD slice `h` gives a huge impulse. The velocity-level form is bounded by the *approach speed*, and the frozen-pose sub-step is handled by evolving the collider's snapshot velocity across slices so `λ` self-limits (slice 0 stops the body; slices 1..n add nothing).
- **Reaction application.** Buffered as `{bodyIndex, impulse, point, posCorrection}` on the body during `advance()` (fixed body→particle→collider order ⇒ deterministic), drained by `xpbdSubstep`: velocity impulses go through `AQRigidBody::applyImpulseAtPoint` (the same path the PGS sweep uses, COM-offset correct) and sum; the split-impulse position corrections are applied as a per-body **MAX** (a bed of contacts all want the same lift — summing would launch it). A non-negligible impulse wakes a sleeping dynamic body (`wakeUp`).
- **Friction (opt-in, default μ=0 ⇒ frictionless, matching the Phase 6 particle path).** Velocity-level Coulomb: a tangential impulse removing the tangential relative velocity, clamped to `μ·λ`; particle realized as a shift, body as an impulse (momentum-conserving).
- **Scope — single-contact vs a resting bed.** Two-way coupling is exact and momentum-conserving *per contact*, and a body held by a **single** vertical contact rests stably. **Multi-point resting of a dynamic rigid body on a curved particle bed is NOT solved here**: it needs a simultaneous/iterative (PGS-grade) solve — with the single-pass self-limiting scheme, off-centre contacts on a curved surface (glancing normals) let the body pick up spin/lateral velocity so `vrelN≈0` along those normals and the vertical fall is not fully arrested. That is exactly the resting-contact problem Phase 3/4 spent a phase on; it stays deferred (Phase 8 / a coupling follow-up). Cloth/particles resting on a static-or-heavy collider is the ONE-WAY case and works.
- **GPU path.** Coupling reads rigid poses and writes rigid impulses, both CPU-resident (the rigid pillar is not GPU-live), and the 7f GPU path batches the *whole* frame on-device with no per-sub-step host hook. So a **collision-enabled body runs the CPU solve even when the context resolved GPU**; non-colliding bodies keep the GPU batch. Wiring: `xpbdSubstep(dt, gpuActive)` runs on both paths (skipping bodies the GPU handles), `xpbdGpuFrame` skips collision-enabled bodies. CPU-path feature for now; a device-side coupling would need the rigid pillar on-GPU (7i-adjacent).
- **New public surface + test.** `AQSpace::setXPBDCollisionEnabled(handle, on, particleRadius=0, friction=0)`. Test `aqua_xpbd_coupling_test` (all green): free particles settle on a static box top / ground plane (one-way, no tunnel); a no-gravity dynamic sphere strikes a free particle — **total linear momentum conserved exactly (p0=2.000 → 2.000, equal-mass inelastic ⇒ both at 1.0 m/s)**, sphere slowed, particle driven, control (off) unimpeded; a single pinned particle holds a falling dynamic sphere against gravity (rest at centre ≈ radius, speed 0.0), control falls through; bitwise determinism; no NaN escapes.

### 13.7 Increment 7h — convergence + robustness upgrades (implementation contract, 2026-07-08)

Two concrete, data-flagged items built; #3 evaluated-and-deferred per the developer's scope decision (7g + 7h concrete, defer 7i).

- **7h #1 — long-range attachments (LRA; Kim/Chentanez/Müller 2012). DONE, green.** Per-body opt-in (`AQXPBDBody::longRangeAttach`, default off). On `recolor()` (topology change), a multi-source Dijkstra relaxation over the distance-constraint graph from every pinned particle (`invMass==0`), accumulating rest lengths (total-order tie-break: smaller pin index wins an equal distance ⇒ path-independent), gives each dynamic particle its nearest pin and geodesic max distance. Each slice, after projection, apply a **unilateral** clamp: if `|x − x_pin| > maxDist`, pull the particle straight back to `x_pin + d·(maxDist/|d|)` (pin fixed ⇒ particle-only move; no shared dynamic particle across LRA constraints ⇒ conflict-free, no coloring). Attacks the `~(N²/2)·g·h²` 1-iteration residual the 7e data quantified. *Measured:* a 40-link inextensible chain starved to 1 substep × 1 iteration over-stretches to **5.97 m** vs its 3.90 m geodesic; LRA holds it at **3.90 m** (bitwise-deterministic; no-op with no pins). Off by default because it would wrongly clamp a *compliant* chain (α>0) — it is for inextensible pinned ropes/cloth (Phase 8's structural case). Multi-layer XPBD (Mercier-Aubin 2024) recorded as the deferred general multigrid alternative; LRA is the simpler, directly-targeted first cut.
- **7h #2 — far-from-origin hardening (the solver2d XPBD precision bug). DONE, green.** Opt-in via `AQXPBDParams.originRelative` (default **false ⇒ byte-identical** to today — the existing oracles are unperturbed). Positions/prevPositions are stored as offsets from a per-body float `origin` (default `0` ⇒ offset==world, one code path, zero behavior change when off). When on: `origin` initializes to the body's centroid, and `advance()` **re-bases** in double when the offset centroid drifts past a threshold (`origin += δ`, offsets and prevPositions `−= δ` together, so `x−x_prev`, `xa−xb`, and world position are all invariant). The precision-critical differences (`derive`, projection) then always run on small offsets → full float precision as the body travels far. CPU-path feature (the GPU batch keeps `origin=0`/world; origin management lives only in `advance()`); boundary reconstructions (`readXPBDState`, coupling's world SDF query) add `origin`, no-ops at 0. Caveat recorded: authoring positions *directly* at huge float coordinates is already input-lossy — the fix targets the realistic **dynamic-drift** case (a body simulated far from where it started), the solver2d scenario. *Measured failure mode (corrected from the initial "jitter" guess):* at world ~1e5 the flag-OFF rope is STARVED, not jittery — a slice's `g·h²` step is near the float ulp and mostly rounds away, so the free end droops only **0.46 m** vs the correct **1.91 m**; flag-ON droops **1.906 m**, matching the near-origin solve to a **3.0 mm** local-shape RMS. Test `aqua_xpbd_origin_test`.
- **7h #3 — gather-based Jacobi: EVALUATED, NOT BUILT.** Per §13.5-B, per-color dispatch is the deterministic baseline and the rope/cloth color counts here (2 for a chain) leave no occupancy bottleneck to fix. Gather-based Jacobi (each particle sums its incident constraints in a fixed order — no atomics) is the deterministic *evolution* path IF color count ever caps GPU occupancy; it is a solver-shape change to the 7f kernels, not warranted now. Recorded, deferred with a clear trigger (measured color-count occupancy stall).
- **7i confirmed deferred.** Its promotion trigger (§13.5-B: Newton de-experimentalizing `SolverVBD`, or a second independent production adoption) has not fired; building a full AVBD prototype now would contradict this plan's own gating and risk the shipped, oracle-passing rigid pillar. Stays the documented unification target for the pre-Phase-8 re-audit.

---

*Brief status: **CPU-live core IMPLEMENTED (7a–7e, §13.3); GPU sub-phase IMPLEMENTED AND LIVE (7f, §13.4); rigid↔XPBD contact coupling IMPLEMENTED (7g, §13.6 — velocity-level split-impulse, one-way + momentum-conserving two-way + single-contact support, verified 2026-07-08); convergence/robustness upgrades IMPLEMENTED (7h #1 long-range attachments + #2 far-from-origin, §13.7; #3 gather-Jacobi evaluated-not-built); AVBD unification (7i) DEFERRED (trigger not fired + developer scope).** Full AQUA suite green (26/26 incl. the three new 7g/7h tests, all prior XPBD oracles, and CPU/GPU parity). Two implementation corrections worth carrying forward: the two-way body reaction is a VELOCITY-level split-impulse, not the position-impulse `Δλ/h` the §7 draft implied (that explodes on a deep catch); and multi-point resting of a dynamic rigid body on a curved particle bed is NOT solved by the single-pass self-limiting coupling (glancing normals) — it wants a PGS-grade simultaneous solve and stays deferred. §11 #1 is settled: UNIFIED destination (developer), vet-corrected path (§13.0/§13.5-A) — rigid stays impulse near-term, AVBD is the oracle-gated unification target (7i trigger in §13.5-B). Where §1–§12 and §13 disagree, §13 governs.*
