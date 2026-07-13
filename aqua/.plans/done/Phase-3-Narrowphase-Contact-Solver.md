# AQUA Phase 3 — Narrowphase & Contact Solver

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a subsystem is written: what PhysX and Chaos
do for narrowphase and contact response, which papers we improve on, what we
change for AQUA's substrate, and how we will measure "better." It covers **Phase
3 — Narrowphase & contact solving [Newtonian]** only: turning the candidate
*pairs* Phase 2 produces into contact *manifolds*, and turning those manifolds
into velocity changes that resolve interpenetration, respect friction, and let a
stack of boxes settle and stay settled. No joints/queries/sleeping (Phase 4); no
GPU port of the solver (Phase 5); no XPBD coupling or soft-body contact (Phase
7/8) — but every choice here is made so those phases inherit it cleanly.

---

## 1. Scope & deliverable

**Goal.** Take each Phase 2 candidate pair, decide whether the shapes actually
touch, build a small manifold of contact points (positions in world, a shared
normal, per-point penetration depth), and run a velocity-level constraint solve
that resolves penetration with **friction** and **restitution**, frame after
frame, deterministically.

**Runnable deliverable.** Two scenes, both driven through the public
`AQContext` / `AQSpace` / `AQRigidBody` surface, asserted in `tests/`:
1. **The settling stack** — 10 boxes dropped onto a static plane (sphere-on-plane
   ≡ Phase 2's plane shape, with the plane case keeping its Phase 2 special-case
   AABB). Within 3 simulated seconds every body has `‖v‖ < ε_v` and
   `max contact penetration < ε_p`, and *stays there* for another 2 s — the
   classic "settled stack doesn't jitter or drift" target. This is the §3
   roadmap deliverable verbatim.
2. **The incline** — a box on an inclined plane whose static-friction coefficient
   `μ_s · cos θ ≥ sin θ`: the box must remain at rest for ≥ 5 s, and the box on
   `μ_s · cos θ < sin θ` must slide. This isolates the friction model from the
   normal-force solve and gives the Coulomb constants a hand-computable oracle
   (`f_max = μ · N`).

These run alongside `aqua_broadphase_test` and `aqua_rigid_body_test`. Like
Phase 1's `double` integrator parity and Phase 2's brute-force AABB-overlap
oracle, **each deliverable is built around a slow, obviously-correct reference**
the fast path must match: the stack's analytic resting normal force (= mass ·
gravity, per body) and the incline's analytic friction cone. That parallel is
deliberate and is what keeps the deliverable honest — we are not measuring
"looks roughly right," we are measuring against numbers we can derive by hand.

**Included groundwork (lands first in Phase 3).** Phase 2 deferred two hooks to
here; we close them before contact response:
- `AQBodyDesc::restitution` and `AQBodyDesc::friction` were reserved in Phase 1
  to "avoid descriptor churn" (`AQRigidBody.h`). Phase 3 wires them: contact
  pairs combine them by the chosen rules (§11.5) and consume them in the solver.
- `AQshapeSupport` ships a minimal-correct landing in Phase 2 precisely so the
  Phase 3 GJK consumer is a port and not a "now define support." Phase 3 promotes
  the support functions to the production-narrowphase path (the box and hull
  cases get full coverage; sphere/capsule were already exact).

**What Phase 2 has already closed for us (audited 2026-06-05).** Phase 2 shipped
the substrate this phase needs; the work below now consumes rather than defines
them. **Do not re-add these; they are already in `include/aqua/` and
`src/`:**
- **`AQShape` + handle + space-owned table — DONE.** `AQCollision.h` ships the
  POD tagged union (sphere/box/capsule/plane/convex hull) and `AQShapeHandle`;
  `AQSpace::createSphereShape/createBoxShape/createCapsuleShape/createPlaneShape
  /createConvexHullShape` are the named-ctor factories. Phase 3 reads `AQShape`
  by handle in the per-pair branch table.
- **Per-pair candidates with deterministic ordering — DONE.** `AQBroadphasePair`
  (always `a < b`); `AQSpace::candidatePairs()` returns the sorted, deduplicated
  list. Phase 3 consumes it in place — that ordering becomes the solve order
  (§5 / §6).
- **`AQCollisionFilter` + the symmetric layer/mask rule — DONE.** Phase 3 does
  not re-filter: the broadphase already dropped any pair that fails
  `AQfilterAccepts`.
- **`AQshapeAABB` + `AQshapeSupport` — DONE.** Phase 2 needed the AABB; Phase 3
  needs support for GJK/EPA on the convex-hull case. The Phase-2 support
  bodies for sphere/box/capsule are already production-grade; the hull case is
  a max-dot scan, which is the canonical GJK support. The float-precision pad
  in `AQaabbOfOrientedBox` (post-Phase-2 fix) carries over.
- **COM-offset wiring — DONE.** `applyForceAtPoint` and
  `applyImpulseAtPoint` already use `comWorld = position + R·comOffset`. Phase
  3's contact impulses go through `applyImpulseAtPoint`, so the wiring
  transfers transparently — a non-zero COM offset gets the right torque arm
  from the contact point automatically.
- **Material fields on `AQBodyDesc` — RESERVED.** `restitution` and `friction`
  exist on the descriptor; Phase 3 propagates them onto the body and combines
  per-pair (§11.5).
- **Drainable debug bus + per-flag bits — DONE.** Phase 3 *extends* `AQDebugFlags`
  with three new bits (`AQDebugContactPoint`, `AQDebugContactNormal`,
  `AQDebugContactImpulse`) and appends into the existing
  `AQSpace::drainDebugLines` buffer.

The phase's remaining work is therefore: contact-manifold POD + table, the
narrowphase branch table (specialized + GJK/EPA fallback), the sequential-
impulse solver with friction and split-impulse position correction, contact
persistence for warm-starting, the material-combine pass, and the new debug
bits. The shape representation, support, filter, and broadphase pipeline are
already shipped.

**Out of scope here, by design:** joints and joint constraints (Phase 4),
queries / raycasts (Phase 4), sleeping / islands (Phase 4), continuous detection
(Phase 4), the OmegaSL solver kernel (Phase 5), particle/soft contact (Phase
7+), and any unified-XPBD recast of the rigid solver (Phase 7 §11.2 — the
architecture decision in the roadmap §7.2). We design the contact data + solver
shape so the OmegaSL port is a layout port, not a rewrite, and so the Phase 4
joint constraints reuse the solver's row machinery unmodified.

---

## 2. Why response is its own problem

Contact *response* is not a smaller version of contact *detection*; it is a
different discipline with a different cost model, a different failure mode, and
a different definition of "correct."

1. **Penetration is the ground truth, but the math destabilizes on it.** A
   contact between two rigid bodies is *defined* by the bodies overlapping —
   that's the only signal narrowphase has. But a position-level "push them
   apart" correction injects velocity; a naïve velocity-level correction leaves
   them overlapping for one more step, which destabilizes the solver next
   frame. Splitting position from velocity (split-impulse, §6) is the
   incumbents' answer, and the Phase 1 integrator's small-step posture is
   exactly what makes it work cleanly.
2. **The error is not symmetric — but the failure modes are.** A
   missed contact (false negative from narrowphase) means objects pass through
   each other and the bug shows up downstream as "the physics is broken." A
   spurious contact (false positive) means the solver applies a kicking impulse
   on no real intersection, and the *energy injection* destabilizes the scene.
   The Phase 2 broadphase was tuned to bias-conservative — fat AABBs, never
   miss; Phase 3 narrowphase has to bias the other way — never invent a
   contact that does not exist. The two layers cross opposite cliffs.
3. **Stacking is a global problem solved by a local algorithm.** A 10-box stack
   has 18 contact pairs (10 box/box + 9 box/floor wait — actually 9 stack
   contacts + 1 floor contact = 10), but they are coupled: the bottom box's
   normal force has to carry the whole stack, and a sequential-impulse / PGS
   sweep that visits contacts in the wrong order takes many iterations to
   converge to the global solution. Warm-starting (carrying impulses across
   frames) and a stable visit order are the two cheap wins; both have to be
   *deterministic*, because the Phase 5 GPU port has to reproduce them.
4. **Friction is rate-dependent in a way contact response is not.** Static
   friction is a clamp on a tangential impulse magnitude; kinetic friction is
   that same clamp at a different coefficient. A clean Coulomb model can't tell
   the regime mid-iteration, so the solver picks one, projects onto the
   friction cone, and lets the residual speak. The dt-independence of this
   projection (compared to a force-based friction model) is *the* reason
   sequential-impulse is the incumbents' standard — and the reason Phase 1's
   impulse API is already the right primitive for the solver to call.

A correct Phase 3 handles all four at once, on a path that will later run as a
GPU constraint-solver kernel over SoA buffers — the Phase 5 port.

---

## 3. Prior art — how the incumbents solve it

Studied to understand the terrain and the failure modes, **not** to transcribe.
Descriptions are drawn from published talks, docs, and source structure and are
representative, not a claim to quote current internals.

**NVIDIA PhysX 5.**
- **Narrowphase** is a branch table over `(typeA, typeB)`: specialized closed-
  form contact functions for the common shapes (sphere/sphere, sphere/box,
  sphere/capsule, capsule/capsule, box/box via SAT) and **GJK** + **EPA** as the
  general convex/convex fallback (used for convex hulls). The specialized paths
  exist because they are faster *and* more numerically robust at the corners
  where GJK is touchy.
- **Contact manifolds** are 1–4 points sharing a single normal, with
  *persistent* contacts: each contact carries a feature-pair key so that the
  same point reappears across frames and the solver can warm-start with the
  previous frame's impulse.
- **Solver** is a hardened **Sequential-Impulse / Projected-Gauss-Seidel** loop
  with **Baumgarte stabilization** historically and **split-impulse / pseudo-
  velocity position correction** more recently (PhysX moved to a TGS — Temporal
  Gauss-Seidel — variant for stacking quality, but the per-contact constraint
  shape is the same). Friction is two tangential rows per contact under
  Coulomb's cone (`|f_t| ≤ μ|f_n|`).
- **GPU broadphase + GPU narrowphase** for large scenes; the CPU path remains
  the deterministic reference.

**Epic Chaos (Unreal Engine 5).**
- **Narrowphase** also uses specialized contact functions plus a general convex
  path; the implicit-shape vocabulary from Phase 2 (sphere/box/capsule/convex)
  feeds straight in, with GJK doing the heavy lifting for convex/convex.
- **Solver** is iterative PGS with materials defined per shape (restitution and
  friction combined per-pair). Restitution combine is configurable: max, min,
  multiply, or average — PhysX-style.
- **Persistence** is by contact feature-pair, like PhysX, with a
  contact-cache that ages out entries that don't reappear.

**Bullet / Box2D heritage.**
- The line all three engines trace back to: **Erin Catto's sequential-impulse
  PGS** (Box2D Lite, GDC 2005), Box2D's incremental refinements, and Bullet's
  contact-cache + 4-point persistent manifolds. Reading these is the cheapest
  way to *understand* the shape of the solve before facing PhysX/Chaos's much
  larger surface area.

**The shared shape:** all three engines (i) split narrowphase into "specialized
fast paths + GJK/EPA fallback," (ii) maintain a small persistent manifold (1–4
contact points) with warm-started impulses, (iii) solve velocity-level
constraints with PGS, (iv) clip friction onto the Coulomb cone every iteration,
and (v) handle position correction *outside* the velocity solve (split-impulse
or pseudo-velocity), not as a Baumgarte term inside it — exactly because the
Baumgarte term injects energy. **All three converged on the same approximate
shape**, which is the strongest signal in the prior-art literature: this is
the path. The bar to clear is not "find a different shape." It is "make the
same shape work on our substrate, in our determinism stance, on the GPU later."

---

## 4. The literature we build on

The research again leads the shipped engines by years. The pieces we combine:

- **Catto, "Iterative Dynamics with Temporal Coherence" (GDC 2005).** The
  canonical sequential-impulse / PGS exposition: velocity-level constraints,
  Jacobian rows, the iteration sweep, warm-starting from the previous frame's
  λ. Reads short and tightly because it *is* the algorithm; Bullet and Box2D
  ship its descendants.
- **Catto, "Modeling and Solving Constraints" (GDC 2009).** The reference for
  *position* correction: Baumgarte's β/dt term and its energy-injection problem,
  the **split-impulse / pseudo-velocity** alternative, and the comparative
  stability characterization. This is the citation for §6's split-impulse pick.
- **Coumans, "Box-Box Contact Manifolds" (Bullet docs / SIGGRAPH 2014 course).**
  The closed-form box/box manifold via face-clipping with **Sutherland-Hodgman
  polygon clipping** under SAT axes. The closed form is faster *and* more
  stable than running GJK + EPA on two boxes; it is the canonical specialized
  path.
- **Gilbert, Johnson, Keerthi, "A Fast Procedure for Computing the Distance
  Between Complex Objects" (IEEE 1988) — GJK; van den Bergen, "Proximity
  Queries and Penetration Depth Computation" (2001) — EPA.** The pair of
  algorithms that turn `AQshapeSupport` into a real general convex/convex
  contact. GJK gives the closest-point pair, EPA recovers the penetration
  depth and normal once GJK terminates inside the Minkowski difference. The
  Phase 2 support functions are exactly what these consume — no new shape
  vocabulary needed.
- **Mirtich, "Impulse-based Dynamics" (PhD, 1996).** The wider impulse-model
  setting — friction-cone projection, restitution clipping, the formal
  treatment of Coulomb's law in an impulse solve. We do not adopt his full
  collision model (it's analytic and not iterative), but the friction-cone
  algebra carries directly to PGS.
- **Macklin et al, "Small Steps in Physics Simulation" (SCA 2019).** The
  modern case for *small* timesteps as a structural simplification: a fine sub-
  step reduces the work each iteration has to do, makes the implicit terms
  near-linear, and improves convergence dramatically — exactly the posture
  the Phase 1 integrator and `AQContext::setFixedTimestep` already enforce.
  The relevant detail here: the smaller the sub-step, the closer split-impulse
  approximates a properly-constrained constraint solve, because the position
  drift it has to correct is itself small.
- **Macklin, Müller, Chentanez, "XPBD" (MIG 2016) — referenced, not adopted in
  Phase 3.** The Phase 7 roadmap fork. We carry the reference because the
  Phase 3 solver's constraint-row shape is deliberately compatible with an
  XPBD recast (the Jacobian and the compliance term differ; the bookkeeping
  doesn't), so if Phase 7 lands the unified-XPBD lean we are not rebuilding
  the rigid-side data layout from zero.

The throughline: the incumbents' specialized-+-fallback narrowphase and PGS
solver line is mature and battle-tested, but the *newer* small-steps and
split-impulse refinements are AQUA-substrate-friendly in a way the
incumbents' CPU/SIMD heritage and their long backward-compatibility surface
constrains. That is the opening.

---

## 5. Where AQUA diverges — the openings

Grounded in the actual post-Phase-2 surface (`AQCollision.h`, `AQMath.h`,
`AQIntegrator.h`, `AQRigidBody.h`, `AQSpace.h`):

- **Hybrid narrowphase as a branch table, GJK/EPA only for convex-hull pairs.**
  PhysX and Chaos ship a hybrid because the specialized paths are faster *and*
  more numerically robust than GJK at the corners that matter (resting
  contacts, face-on-face). Phase 2 already enumerates the shape vocabulary
  (sphere, box, capsule, plane, hull); Phase 3's narrowphase is therefore a
  *fixed-size* `(type, type)` table of specialized contact functions, with
  GJK/EPA routed to only when at least one operand is a convex hull. There is
  no "general GJK for everything" path on AQUA; the table size is bounded by
  the shape vocabulary, and the cost is the table not the algorithm.
- **The solver loop is per-contact, the constraint row is the unit of layout.**
  PhysX's hot loop is over contacts; ours is too, but Phase 5 will want SoA
  arrays of *constraint rows*, not contacts — a contact contributes 1 normal
  row + 2 friction rows + (optionally) 1 position-correction row. AQUA
  organizes around the row from day one (§8). A future XPBD recast (Phase 7)
  iterates the same row buffer with a different compliance/inverse-mass
  treatment; the layout transfers.
- **Split-impulse, not Baumgarte, for position correction — to match small-step
  posture.** The Phase 1 integrator is already on a fine sub-step (1/120 s
  default, smaller for fast rotators) per the Phase-1 doc §11.5 "small steps"
  posture, exactly the regime Macklin's small-steps work argues for. In that
  regime, Baumgarte's energy injection is harder to tune *and* the split-
  impulse alternative is cheaper to converge — the small dt makes the pseudo-
  velocity update tiny. The incumbents inherited Baumgarte for a different
  CPU-heritage step size; we get to skip that inheritance.
- **Deterministic solve order falls out of the Phase 2 ordering invariant.**
  Phase 2's candidate pair list is sorted and de-duplicated; Phase 3 iterates
  *that* order for both narrowphase and the solver sweep. No per-frame hash
  table, no insertion-order quirks, no scatter-gather that could reorder
  contacts based on contact-cache lookup. The cache is keyed by the deterministic
  pair index pair, so the warm-started impulse comes back in the same order it
  left. The Phase 5 GPU sweep gets the same ordering for free (the GPU sort
  produces it).
- **Material combine rules are configurable per space, not hardcoded.** PhysX
  exposes `Mat::Combine::Avg/Min/Max/Multiply` and a per-shape override; we
  ship the same vocabulary (§11.5). Hardcoding "max restitution wins" or
  "multiply frictions" reads as a forced design call to the user — three lines
  of policy in the descriptor is the better surface.
- **Persistence keyed by ordered pair + per-shape feature index, deterministic
  across runs.** The contact cache is an `unordered_map<PairKey, ManifoldRecord>`
  on the CPU; the GPU port will be a sorted array (the same shape as the
  candidate pair list) so the cache itself parallelizes. The feature index
  inside a manifold is shape-determined (box: vertex/edge/face IDs;
  sphere/capsule: degenerate). This is **deliberately compatible** with the
  Phase 5 SoA port: a contact persistence buffer keyed by sorted pair index
  is just a sorted array, no hashing on the GPU.

**Gaps we must fill (this is Phase 3's work):** there is no `AQContact`, no
manifold, no GJK, no EPA, no constraint row, no solver, no friction model, no
warm-start cache. None of it depends on a third-party physics SDK; it is
narrowphase + solver code we own and build on the Phase 2 collision substrate.

---

## 6. Proposed algorithm — hybrid narrowphase + sequential-impulse PGS with split-impulse

The synthesis, per sub-step, in the order it runs:

**A. Narrowphase — branch over `(typeA, typeB)` (one branch per candidate
pair).**
```
for each pair (a, b) in candidatePairs:                       // Phase 2 output
    shapeA = shapes[bodies[a].shapeIndex]
    shapeB = shapes[bodies[b].shapeIndex]
    manifold = narrowphase[typeA][typeB](shapeA, shapeB, bodyA.xform, bodyB.xform)
    if manifold.contactCount > 0:
        persist(manifold, key=(a, b, featurePairs))           // warm-start cache
        constraintRows.append(buildRows(manifold, a, b))      // §8 row layout
```
Specialized closed-form paths land for: sphere/sphere, sphere/box,
sphere/capsule, sphere/plane, capsule/capsule, capsule/plane, capsule/box,
box/box (SAT + Sutherland-Hodgman face clipping per Coumans), box/plane,
plane/plane (excluded — Phase 2's broadphase doesn't emit those pairs).
**GJK + EPA** ship as the convex-hull/convex-hull, convex-hull/box,
convex-hull/sphere, convex-hull/capsule, and convex-hull/plane paths. The
inputs to GJK are exactly `AQshapeSupport` results (Phase 2 §7) — no new
shape vocabulary.

**B. Material combine (one pass).**
For each contact: combine the two bodies' `restitution` and `friction` per
the space's combine rule (§11.5). The combined coefficients live on the
constraint row; the solver does no per-iteration lookup into body state for
materials.

**C. Warm-start.**
Apply the cached normal+friction impulses from the previous frame at each
contact row. This is Catto 2005's warm-start: the solver starts close to the
right answer and converges in fewer iterations. Cache miss (a new contact)
warm-starts with zero.

**D. Sequential-impulse iteration sweep (N times, §11.4).**
```
for iter in 0..N:
    for each contact c:
        # Normal row — velocity constraint v_n ≤ 0 with restitution bias
        rel_v_n = (vB + ωB × rB - vA - ωA × rA) · n
        bias    = bounceBias(rel_v_n, restitution)                # §11.5
        lambda_n  = -(rel_v_n + bias) / Keff_n                    # impulse magnitude
        lambda_n  = max(0, c.accum_n + lambda_n) - c.accum_n      # non-penetration clamp
        applyImpulseAtPoint(b, +lambda_n · n, contactPoint)       # uses Phase 1 API
        applyImpulseAtPoint(a, -lambda_n · n, contactPoint)
        c.accum_n += lambda_n

        # Friction rows (two tangents, Coulomb cone)
        for t in [t1, t2]:
            rel_v_t = (vB + ωB × rB - vA - ωA × rA) · t
            lambda_t = -rel_v_t / Keff_t
            lambda_t = clamp(c.accum_t + lambda_t, ±μ · c.accum_n) - c.accum_t
            applyImpulseAtPoint(...)
            c.accum_t += lambda_t
```
`Keff` is the effective mass for the constraint row; for a single contact it
is `1/m_A + 1/m_B + (rA × n)ᵀ · I⁻¹_A · (rA × n) + (rB × n)ᵀ · I⁻¹_B · (rB ×
n)` — every quantity already on the body (Phase 1.1's `worldInverseInertia`
returns the I⁻¹ term). The `applyImpulseAtPoint` call goes through the
Phase 1 API and inherits the Phase 2 COM-offset wiring automatically.

**E. Split-impulse position correction (separate from the velocity sweep).**
```
for iter in 0..N_pos:
    for each contact c if c.depth > slop:
        # Pseudo-velocity row: same Jacobian, depth-only RHS, NO restitution.
        pen_v = max(0, c.depth - slop) * (1 / dt)
        lambda_p = pen_v / Keff_n
        # Pseudo-velocity is integrated against the *position* path only.
        applyPseudoImpulseAtPoint(...)
```
Pseudo-velocities are *not* added to the body's real velocity; they advance
the position at the end of the sub-step as a separate "position drift." This
is Catto 2009's split-impulse: penetration recovery without injecting
energy into the velocity field.

**F. Integrate.** Phase 1's `AQStepBody` consumes the impulse-modified
velocities exactly as before. The pseudo-velocities advance `position` only
in a second pass at end-of-step; orientation is unaffected (position
correction does not rotate bodies).

**G. Persistence handoff.** The accumulated `accum_n`/`accum_t` per contact go
back into the cache, keyed by `(pairIndex, feature)`; the next frame's warm-
start reads them. Misses (contacts that don't recur) age out after one frame.

Why this combination:
- **Hybrid narrowphase** is what the incumbents ship and what the literature
  agrees on; specialized paths are faster *and* more robust than GJK at
  resting contacts, GJK is the right fallback for convex hulls. The phase-2
  shape vocabulary makes the branch table closed-size.
- **Sequential-impulse PGS** is Catto's hardened algorithm. It maps cleanly
  to a per-row sweep, parallelizes via constraint-graph coloring (Phase 5),
  and accepts warm-started λ as initial conditions — no architecture change.
- **Split-impulse** keeps the velocity field clean of position-correction
  energy. The Phase 1 small-step posture makes the pseudo-velocity tiny per
  step, which is the regime where split-impulse is provably stable
  (Catto 2009 §3).
- **Deterministic order** falls out of Phase 2's ordered pair list and the
  CPU-fixed-time iteration sweep. The Phase 5 GPU port replicates it with a
  stable sort over the constraint row buffer.

**Alternative considered — pure GJK/EPA on every pair.** Cleaner code,
*much* worse numerical behavior on resting-contact stacks (EPA's polytope
expansion is touchy at near-zero penetration depth), and slower (each
specialized path is 10–100× the throughput of GJK/EPA on its target case).
Kept as the convex-hull fallback only.

**Alternative considered — Baumgarte position correction inside the velocity
sweep.** Simpler bookkeeping (no second pseudo-velocity pass), but the
β-tuning problem and the energy-injection on settled stacks are exactly the
failure mode the incumbents migrated away from. Documented in §11.3 as the
fallback option if split-impulse's two passes prove too expensive in
profile.

**Alternative considered — Temporal Gauss-Seidel (TGS, PhysX 5).** A
substepped variant where each "sub-iteration" is a full sub-step including
integration. Better stacking quality at coarse dt, more code. With AQUA's
already-small dt posture the TGS win narrows; revisit in Phase 4 if joint
stacks need it.

---

## 7. New types AQUA must add — `include/aqua/AQContact.h` (draft)

AQUA-owned, AQ-prefixed, no namespace (per `AGENTS.md`). All public surface
types here are trivially-copyable / standard-layout so they upload to a GPU
buffer with no repacking — the Phase 5 PGS solver kernel reads
`AQConstraintRow` records and `AQContactManifold` records as raw arrays.

```cpp
#ifndef AQUA_AQCONTACT_H
#define AQUA_AQCONTACT_H

#include "AQBase.h"
#include "AQCollision.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>

// A single contact point. Up to 4 of these share a manifold + a normal. The
// "feature key" identifies which features of the two shapes produced this
// contact (e.g. for box/box, a face index from each side, or a face+edge);
// the persistence cache keys on (pair, feature) so the warm-started impulse
// follows the contact across frames.
struct AQContactPoint {
    OmegaGTE::FVec<3> positionWorld;     ///< Midpoint on the contact normal
    float             depth = 0.f;       ///< Penetration depth (>=0 means in contact)
    std::uint32_t     featureKey = 0;    ///< Shape-pair-specific feature id

    // Persistent accumulators (warm-start carriers). The solver clears or
    // restores these from the cache at the start of each sub-step.
    float accumNormal  = 0.f;            ///< Cached λ_n
    float accumFriction[2] = {0.f, 0.f}; ///< Cached λ_{t1}, λ_{t2}
};

// A contact manifold between bodies a and b (with a < b — same invariant as
// AQBroadphasePair). All points share a single world-frame normal pointing
// from a into b. 1..4 points; the box/box clip produces 4 in the resting case,
// sphere/sphere always 1.
struct AQContactManifold {
    std::uint32_t      a, b;                       ///< body indices (a < b)
    OmegaGTE::FVec<3>  normalWorld;                ///< unit normal, a → b
    std::uint32_t      pointCount;                 ///< 1..4
    AQContactPoint     points[4];                  ///< unused entries left zeroed
    float              restitutionCombined = 0.f;  ///< per-pair material combine
    float              frictionCombined    = 0.f;
};

// A single constraint row consumed by the PGS sweep. Three rows per contact
// point: 1 normal (lower-bound 0) + 2 friction (cone-clamped to ±μ·λ_n). For
// later joints (Phase 4), additional rows for the joint's degrees-of-freedom
// land in the same buffer with their own bounds.
enum class AQConstraintKind : std::uint32_t {
    ContactNormal,    ///< λ ≥ 0
    ContactFriction,  ///< |λ| ≤ μ·λ_n (peer)
};

struct AQConstraintRow {
    AQConstraintKind  kind;
    std::uint32_t     bodyA, bodyB;                ///< indices into the body SoA
    OmegaGTE::FVec<3> rA, rB;                      ///< contact arm in world (point − comWorld)
    OmegaGTE::FVec<3> direction;                   ///< n for normal, t1/t2 for friction
    float             effectiveMass = 0.f;         ///< 1 / Keff — precomputed once
    float             bias          = 0.f;         ///< restitution bias on normal; 0 on friction
    float             lowerBound    = 0.f;
    float             upperBound    = 1e30f;       ///< +inf for normal; tightened per-iter for friction
    float             accumImpulse  = 0.f;         ///< warm-started or zero
    std::uint32_t     peerRow       = 0;           ///< friction → its normal row index
};

// Material-combine rules (PhysX/Chaos parity), selected per space.
enum class AQMaterialCombine : std::uint8_t {
    Average, Min, Max, Multiply,
};

#endif // AQUA_AQCONTACT_H
```

The `AQContactPoint` and `AQContactManifold` types stay POD and
GPU-uploadable; `AQConstraintRow` is the per-iteration unit and what the
Phase 5 GPU kernel reads coalesced (one thread per row).

The narrowphase per-pair functions are free functions in
`src/AQNarrowphase.cpp` (one branch table entry per `(typeA, typeB)`); GJK
+ EPA live in `src/AQGJK.cpp` and feed off `AQshapeSupport`. Header surface
stays minimal — the user calls `AQSpace::contactManifolds()` and that's it.

---

## 8. Data layout & GPU/numeric specialization

Decided now so Phase 5 is a port, not a rewrite (ties to roadmap §5/§7.3 SoA
decision):

- **SoA at the row level, extending the Phase 1/2 body layout.** Bodies are
  already SoA-friendly (per-body parallel arrays land in Phase 5);
  Phase 3 adds:
  - `manifolds[]` — one `AQContactManifold` per active contact, sorted by
    `(a, b)` per the deterministic invariant.
  - `rows[]` — `AQConstraintRow`, packed; one normal + two friction rows per
    contact point. The friction row's `peerRow` field threads back to the
    normal row's `accumImpulse` for the cone clip. Sorted in the same order
    as `manifolds[]`, so cache locality is by contact pair.
  - `cache[]` — keyed by `(pairIndex, featureKey)` in an
    `unordered_map<uint64_t, float[3]>` on the CPU; the GPU port becomes a
    sorted array + binary search (since the broadphase already sorts pairs,
    the cache key parallel-sorts the same way).
- **Persistence across the integrate.** Each row's `accumImpulse` is the
  iteration accumulator the solver consumes; the cache stores only `(λ_n,
  λ_t1, λ_t2)` per contact point — minimal state, parallel-readable.
- **Solver is stateless per sub-step at the body level.** Body velocity
  updates from impulses go through `applyImpulseAtPoint`, which mutates body
  state; the constraint rows hold no body state themselves. The
  one-thread-per-row Phase 5 kernel needs only the row + the two body indices,
  no manifold lookup in the inner loop.
- **Coloring/batching reserved for Phase 5.** The CPU PGS sweep is sequential;
  the constraint graph (contact rows touching shared bodies) is the input the
  Phase 5 port will color. We *don't* color in Phase 3 (it's wasted work on
  the CPU), but we keep the row layout coloring-friendly: every row already
  carries `bodyA`/`bodyB`, which is exactly the adjacency the coloring needs.
- **Determinism:** fixed solve order from the Phase 2 ordered pair list,
  fixed iteration count per sub-step (Catto 2005 §4 — 4–10 iterations is the
  Box2D-Lite default; we lean 8, §11.4), no per-thread variable loop length.
  The `double`-precision oracle for the solver is the same code at `double`,
  mirroring the Phase 1 / Phase 2 parity story.

---

## 9. Validation — how we measure "better"

The incumbent's *behavior* is the reference, not its code (roadmap §4).

- **Settling stack (the headline).** 10 unit-cube bodies dropped onto a
  static plane. Within 3 simulated seconds, every body has `‖v‖ <
  0.01 m/s` AND `‖ω‖ < 0.01 rad/s`; the max contact penetration is `< 1 mm`;
  and the stack remains under those bounds for another 2 s. This is the §1
  deliverable, asserted in a test. The analytic resting normal force per
  contact in a 10-stack is `(11 - i) · m · g` for the i-th contact from
  the floor; the test asserts the solver's accumulated normal impulse
  agrees with that within 5% — the *quantitative* version of "looks
  settled."
- **Incline friction (the other headline).** Box of mass 1 kg on a 30°
  incline; for `μ_s = 0.7` (`tan 30° ≈ 0.577 < 0.7`), the box must stay at
  rest for ≥ 5 s with `‖v‖ < 1e-3`. For `μ_s = 0.3` (below the cone), the
  box must slide and reach `‖v‖ > 1 m/s` within 2 s. Both are hand-
  computable from Newton's second + Coulomb's cone; we hold the solver to
  the analytic answer.
- **Sphere-on-plane bounce.** Sphere dropped from 1 m with restitution 0.5;
  after the first bounce the peak height must be `0.25 m ± 5%`. Restitution
  is a velocity-level coefficient and this is its direct test.
- **GJK/EPA correctness.** A pair of convex hulls (a tetrahedron and a small
  box) overlap by a known depth and orientation; assert that `narrowphase`
  reports the same depth + normal as a hand-computed answer to within
  1e-4 (the same tolerance band as Phase 1's `float`/`double` parity).
- **Determinism.** Two runs of the same scene produce byte-identical
  manifold lists *and* byte-identical accumulated impulses across the
  whole simulation. The §5/§8 determinism guarantee, asserted directly.
- **Energy non-growth on a quiescent stack.** A settled stack's kinetic
  energy may not *grow* across 1000 sub-steps — split-impulse's
  energy-injection-free property in test form. Baumgarte fails this; that's
  why split-impulse is the lean.
- **Phase 2 regression.** All existing broadphase oracle parity / rotation-
  correct AABB / determinism tests continue to pass — Phase 3 must not
  shift Phase 2's promises.

Metrics emitted as debug-draw / logged series (roadmap §3 principle 6):
contact points (small spheres), contact normals (line segments scaled by
penetration depth), accumulated normal-impulse magnitudes (line segments
scaled by λ_n), and a per-frame loud guard when total contact count
exceeds `4 × candidatePairs.size()` (= manifold reduction failed — a sign
narrowphase is over-generating).

**The debug bus already exists.** Phase 1.1's drainable `AQDebugLine`
stream and Phase 2's per-flag-bit extension are in place; Phase 3 extends
`AQDebugFlags` with `AQDebugContactPoint`, `AQDebugContactNormal`, and
`AQDebugContactImpulse`, and appends into the same buffer the kREATE
adapter already drains. No new transport, no new boundary, no new
consumer contract.

---

## 10. Public API additions

Extends the existing surface across `include/aqua/AQRigidBody.h`,
`include/aqua/AQSpace.h`, and the new `include/aqua/AQContact.h`. New
members marked `// new`; pre-existing-from-Phase-2 members that Phase 3
*uses* are marked `// Phase 2, present`. No OmegaSL or backend types cross
into `include/aqua/*`.

**`AQBodyDesc` (in `AQRigidBody.h`):**
```cpp
struct AQUA_EXPORT AQBodyDesc {
    // ... Phase 1 / Phase 1.1 / Phase 2 fields ...

    // Phase 2 already shipped (do not re-declare):
    //   AQShapeHandle     shape;
    //   AQCollisionFilter filter;
    //
    // Phase 1 reserved (do not re-declare; Phase 3 starts consuming):
    //   float restitution;
    //   float friction;

    // No new fields — Phase 3 reads the existing material fields and the
    // Phase 2 shape handle; no descriptor churn.
};
```

**`AQRigidBody` (in `AQRigidBody.h`):**
```cpp
class AQUA_EXPORT AQRigidBody {
public:
    // ... Phase 1 + 1.1 + 2 surface ...

    OMEGA_NODISCARD float restitution() const;                   // new
    void setRestitution(float r);                               // new (clamped [0, 1])
    OMEGA_NODISCARD float friction() const;                      // new
    void setFriction(float mu);                                 // new (clamped >= 0)
};
```

**`AQSpace` (in `AQSpace.h`):**
```cpp
class AQUA_EXPORT AQSpace {
public:
    // ... Phase 1 + 1.1 + 2 surface ...

    // Material combine policy — applies to every contact pair the
    // narrowphase produces this space. Default Average (the PhysX default).
    void setMaterialCombine(AQMaterialCombine restCombine,
                            AQMaterialCombine fricCombine);     // new
    OMEGA_NODISCARD AQMaterialCombine restitutionCombine() const; // new
    OMEGA_NODISCARD AQMaterialCombine frictionCombine() const;    // new

    // Solver knobs (defaults: 8 velocity, 4 position; §11.4). 0 disables
    // the position pass entirely — useful for the energy-non-growth test.
    void setSolverIterations(int velocityIters, int positionIters); // new
    OMEGA_NODISCARD int velocityIterations() const;                  // new
    OMEGA_NODISCARD int positionIterations() const;                  // new

    // Read-only manifold view (for debug overlays and Phase 4 joint
    // wiring). Stable for the duration of one advance() call; the next
    // advance() refreshes it.
    OMEGA_NODISCARD std::vector<AQContactManifold> contactManifolds() const; // new
};
```

> **Folded-in groundwork (lands first, §1).** `AQBodyDesc::restitution` and
> `AQBodyDesc::friction` did not consume into the body's `Impl` (Phase 1
> stored them as `0.f`/`0.5f` and the integrator never read them). `addBody`
> now stores them on `AQRigidBody::Impl`; the per-pair combine pass reads
> them. The persistence cache lives in `AQSpace::Impl` as
> `std::unordered_map<std::uint64_t, ManifoldRecord>` keyed by
> `(pairIndex, featureKey)`; entries that don't recur for one frame are
> erased. `AQContext::advance` gains a single new call between
> `runBroadphase` and the substep loop: `runNarrowphaseAndSolve()` — a
> private method on `AQSpace` that consumes the broadphase output and
> produces the constraint row buffer the inner loop sweeps. The Phase 2
> per-substep AABB-refresh + broadphase cadence is unchanged.

`AQContactManifold` returned by `contactManifolds()` is value-copyable and
backend-free — it carries the body indices, not pointers. The cache is
private and never crosses the API boundary.

---

## 11. Open decisions for this phase

1. **Narrowphase strategy — hybrid (specialized + GJK/EPA) vs. one-general-
   GJK/EPA vs. specialized-only.** *Lean: hybrid* (the roadmap §7.6 lean
   and what all three incumbents ship). Specialized closed-form paths for
   the Phase 2 shape vocabulary, GJK + EPA for the convex-hull cases only.
   Specialized-only is rejected because it forfeits the convex-hull general
   case; general-GJK-only is rejected because EPA destabilizes at the
   resting-contact corner that the settling-stack deliverable lives at.
   *(Roadmap §7.6 key decision.)*
2. **Constraint solver — sequential-impulse / PGS vs. Temporal Gauss-Seidel
   (TGS) vs. unified XPBD recast.** *Lean: classic SI/PGS* per Catto 2005.
   TGS is PhysX 5's incremental improvement and is worth revisiting in
   Phase 4 if joint-stacking quality demands it; unified XPBD is the
   Phase-7 fork in the roadmap §7.2 — not a Phase 3 decision, but the row
   layout (§7, §8) is chosen so an XPBD recast would not require rewriting
   it.
3. **Position correction — split-impulse / pseudo-velocity vs. Baumgarte vs.
   pure position projection.** *Lean: split-impulse* (Catto 2009; the
   incumbents' current path). Baumgarte is documented as the simpler
   fallback if split-impulse's two-pass cost is too high in profile. Pure
   position projection (the XPBD style) is the Phase 7 path and is
   architecturally different — flagged, not chosen.
4. **Solver iteration counts — velocity vs. position passes; defaults.**
   *Lean: 8 velocity, 4 position* (Box2D's defaults at AQUA's sub-step
   size). Tunable per-space via `setSolverIterations` so the deliverables
   that need stricter convergence (the settling stack) can ratchet up
   without rebuilding.
5. **Material combine rules — restitution and friction combine.** *Lean:
   `Average` for both* (PhysX's default; the most physically defensible
   isotropic-material policy). Per-space selectable via `setMaterialCombine`.
   `Max`/`Min`/`Multiply` are present for gameplay overrides (a "super
   bouncy ball" wants restitution `Max` so the ball's value wins regardless
   of the surface).
6. **Friction model — isotropic Coulomb (two tangents, single μ) vs.
   anisotropic.** *Lean: isotropic, fixed two-tangent basis* — the resting
   contact case is exactly where isotropic Coulomb is correct, and the
   anisotropic generalization is gameplay-flavor not physics-quality.
   Deferred to a future minor phase if a project needs surface-grain
   simulation.
7. **Persistent manifold size — 4 points (Bullet/PhysX) vs. fewer / unbounded.**
   *Lean: 4 points* — the classic box/box rest manifold is exactly 4
   contacts; deeper than that doesn't improve stack stability and just costs
   solver iterations. Manifold reduction (when narrowphase produces more
   than 4) keeps the four with maximum spatial spread + maximum normal
   impulse, the standard heuristic.
8. **Contact cache eviction policy.** *Lean: one-frame grace* — a contact
   that misses one frame is evicted; if it reappears the next frame, it
   starts with a fresh zero warm-start. Two-frame grace is the alternative
   if churn proves problematic on rapidly-tumbling stacks, but it adds
   state and complicates the determinism contract.

---

*Brief status: proposal. Decisions in §11 — above all the narrowphase
strategy (#1) and the position-correction approach (#3) — should be settled
before the solver lands. This document is the Phase 3 entry of the per-phase
prior-art series roadmap §4 establishes, and follows the conventions set by
`Phase-1-Dynamics-Math-Core.md` and `Phase-2-Collision-Shapes-Broadphase.md`.
The unified-XPBD recast (roadmap §7.2) is explicitly out of scope here — the
data layout chosen in §8 keeps that fork open without prejudging it.*

---

## 12. Recency-principle audit (addendum, 2026-06-06)

Roadmap §4 was strengthened post-Phase-3 to make "newest viable algorithm
from the literature" the standing default for every phase, with incumbents
adopted only when no substantively-newer alternative offers a real
improvement for AQUA's substrate (`Physics-Roadmap.md` §4 — "Recency
principle"). The Phase 3 brief predates the explicit rule; the audit ran
retroactively against narrowphase, contact-solver, and friction choices.
Findings recorded here, mirroring `Physics-Roadmap.md` §5 Phase 3.

The Phase 3 picks (hybrid narrowphase: specialized branch table + GJK/EPA
fallback per Gilbert-Johnson-Keerthi 1988 / van den Bergen 2001; PGS solver
per Catto 2005; Coumans box/box face-clipping 2014; split-impulse position
correction per Catto 2009; Coulomb cone-clipping friction; warm-starting
via persistent feature-keyed manifolds) date to a 1988–2014 line. What does
2020-onwards add?

- **Narrowphase — Montaut, Le Lidec, Petrik, Sivic, Carpentier, "Collision
  Detection Accelerated: An Optimization Perspective" (RSS 2024;
  Nesterov / Polyak-accelerated GJK, shipped in the Coal library, formerly
  HPP-FCL). Genuine drop-in improvement — adopt now.** Recasts GJK as a
  Frank-Wolfe step in convex optimization and applies Nesterov
  acceleration; the paper reports up to ~2× speedup on typical convex
  pairs and as much as 5–15× on the hard cases, with identical correctness
  guarantees (no false negatives) and the **same support-function
  interface** as classical GJK. The Phase 2 `AQshapeSupport` is exactly
  that interface, so the adoption is mechanical: replace the iteration
  in `src/AQGJK.cpp` with the accelerated variant; EPA fallback
  unchanged. This is the one substantive newer-and-better finding the
  audit produced. The Phase 3 §4 citations stay (GJK 1988 for algorithm
  shape, van den Bergen 2001 for EPA); **Montaut 2024 is added for the
  iteration kernel** and the §3.x maintenance patch.
- **Contact solver — Müller, Macklin, Chentanez, Jeschke, "Detailed Rigid
  Body Simulation with Extended Position Based Dynamics" (CGF 2020).
  Substantive divergence; deferred because it is the §7.2 fork.** Recasts
  *contact* response as XPBD position projection rather than Catto-style
  velocity-impulse PGS, with `n` substeps × 1 iteration. Adopting it for
  Phase 3 alone makes no architectural sense — the §7.2 unified-XPBD
  decision is the place to settle the integrator + joint + contact
  recast together. The Phase 3 row schema's `compliance` field (added in
  the Phase 4 plan's groundwork, deliberately compatible) makes a Phase 7
  recast a layout port rather than a rewrite. Müller 2020 citation
  forwarded; no Phase 3 change.
- **Friction — Ly, Casati, Bertails-Descoubes, Béthune, Cohen-Steiner,
  "Primal-Dual Non-Smooth Friction for Rigid Body Animation" (SIGGRAPH
  2024). Flagged, not adopted.** Converts the Coulomb cone's non-smooth
  static-friction problem into an unconstrained smooth problem via
  logarithmic barriers, getting stable static friction *and* fast
  convergence (the two qualities incumbent solvers historically trade
  off). Substantively newer than Catto's cone-clipping, but heavier per
  iteration and aimed at the robotics-fidelity regime (high-stack
  stability under coarse `dt`). At AQUA's small-step posture (1/120 s
  default) the cone-clipping is already in its sweet spot; the Phase 3
  incline-friction deliverable closes on the analytic answer without
  this. **Revisit only if profiles show static-friction artifacts in
  large stacks at production dt** — noted as the future-work neighbour
  of §11.6 (anisotropic friction). Not the lead because the recency
  principle's bar is "real improvement *for AQUA's substrate*," and the
  substrate doesn't surface the failure mode this paper targets.
- **Mesh-barrier contact — Huang, Paik, Ferguson, Panozzo, Zorin,
  "Geometric Contact Potential" (TOG 2024); Li, Kaufman, Jiang et al.
  IPC (2020). Not applicable.** Barrier-potential contact models (the
  IPC line, refined by Geometric Contact Potential) target **triangle-
  mesh / FEM contact** on deforming surfaces; their failure mode
  (intersection during deformation) does not exist in AQUA's analytic-
  shape rigid-body world. Same conclusion as the Phase 4 audit on
  Tight Inclusion (Wang et al. 2021): mesh-targeted, not for analytic
  shapes. Revisit if/when the soft-body pillar grows a deforming-mesh
  collider.
- **TGS (PhysX 5 Temporal Gauss-Seidel) — already documented.** §6
  "Alternative considered — TGS" notes the choice; at AQUA's already-
  small `dt` the TGS win narrows. Revisit in Phase 4 if joint stacks
  need it; the audit reaffirms.

**Net for Phase 3:** the audit returns **one adopt-now finding
(accelerated GJK, Montaut 2024)** plus three flagged-or-deferred items
(Müller 2020 XPBD-recast — §7.2 fork; Ly 2024 primal-dual friction —
substrate-mismatch; IPC / GCP — mesh-targeted). The accelerated-GJK
adoption is a clean Phase 3.x maintenance patch in `src/AQGJK.cpp`:
same support interface, same correctness, faster — exactly the shape
the recency principle is meant to surface.

Re-audit due: 2028-06-06 (roadmap §4 two-year freshness rule) or sooner
if the §7.2 fork lands in Phase 7.
