# AQUA Phase 2 — Collision Shapes & Broadphase

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a subsystem is written: what PhysX and Chaos
do for shapes and broadphase, which papers we improve on, what we change for
AQUA's substrate, and how we will measure "better." It covers **Phase 2 —
Collision shapes & broadphase [shared]** only: giving bodies *geometry*, and
finding the *pairs that might touch*. No contact generation, no response (Phase
3); no GPU port of the broadphase yet (Phase 5) — but every choice here is made
so those phases inherit it cleanly.

---

## 1. Scope & deliverable

**Goal.** Give every body a collision shape with a computable bound, keep each
body's world-space AABB current as it moves, and produce — cheaply, not O(n²) —
the set of body pairs whose bounds overlap and could therefore collide.

**Runnable deliverable.** A scene of many moving bodies whose **AABBs and
overlapping candidate pairs** are emitted as debug-draw data, with a headless
test that asserts the broadphase pair set is *exactly* the brute-force
O(n²) AABB-overlap set — **no missed pairs (the unforgivable error) and no
spurious pairs beyond the fattening margin** — across thousands of randomized
configurations and over a moving simulation. No contact response yet; this is the
detection layer that proves Phase 3 will be fed correct candidates. It lives in
`tests/` next to this brief, the way `aqua_math_test` / `aqua_dynamics_test` do.

The brute-force all-pairs overlap is to broadphase what the `double` oracle was
to the Phase 1 integrator: a slow, obviously-correct reference the fast path must
match. That parallel is deliberate and it is how we keep the deliverable honest.

**Included groundwork (lands first in Phase 2).** Phase 1 deferred two hooks to
here; we close them before broadphase:
- `AQBodyDesc` has **no collision-shape handle** yet (Phase 1 §10 explicitly
  parked it). Phase 2 adds it.
- `AQBodyDesc::inertiaPrincipalMoments` documents "Zero ⇒ derive from the body's
  collision shape (Phase 2)" (`AQRigidBody.h`). Phase 2 wires that: a dynamic body
  with a shape and zero moments gets its inertia from the shape via the Phase 1
  helpers (`AQinertiaSolidBox` / `AQinertiaSolidSphere` / `AQinertiaCapsule`,
  `AQMath.h`) — which exist precisely so this is a one-line call, not new math.

**What Phase 1.1 has already closed for us (audited 2026-06-04).** Phase 1.1
shipped four pieces of Phase 2's substrate; the work below now consumes rather
than defines them. **Do not re-add these; they are already in `include/aqua/`:**
- **`AQAABB<Ty>` + oriented-box bound — DONE.** Phase 1.1 landed
  `AQAABB<Ty>` in `AQMath.h` (the §7 draft below, plus the §6.1 rotation-correct
  `AQaabbOfOrientedBox(center, halfExtents, q)` using `|R|·h`). The full surface
  ships: `overlaps / merged / fattened / surfaceArea / center / extents /
  contains` plus the `empty() / fromMinMax / fromCenterHalfExtents` factories.
  `FAABB = AQAABB<float>` is the public alias. Phase 2 just *uses* these.
- **The drainable debug-draw bus — DONE.** `AQDebug.h` carries `AQDebugLine`
  and `AQDebugFlags`; `AQSpace::setDebugFlags / debugFlags / drainDebugLines`
  is the per-space pull surface (§9 "metrics emitted as debug-draw" wires
  through this). Phase 2 *extends* it with new flag bits (`AQDebugAABB`,
  `AQDebugBroadphasePair`, `AQDebugBroadphaseGuard`) and appends into the
  same buffer — the bus, the consumer contract, and the kREATE adapter on
  the other side are all in place.
- **Reserved center-of-mass offset — DONE in the model, OPEN for wiring.**
  `AQBodyDesc::centerOfMass` and `AQBodyState::comOffset` were added by Phase
  1.1 (Phase-1.1 §6.2 / §11.7) precisely so that Phase 2's shape local-
  transforms add geometry rather than refactor body state. The field is
  defaulted to zero and currently has no effect; Phase 2 wires the offset
  into the torque arm of `applyForceAtPoint` and the world position used by
  `angularMomentum` once shapes can produce non-zero offsets.
- **Full-inertia-tensor path — DONE.** `AQBodyDesc::inertiaTensor`
  (`AQMat3F`) + `AQdiagonalizeInertia` wiring + fold-into-orientation
  shipped in Phase 1.1. Useful for Phase 2's convex-hull body whose cooked
  inertia comes out non-diagonal — the consumer just hands `addBody` the
  3×3 tensor and stops thinking about Jacobi.

The phase's remaining work is therefore: shape POD + handle + space-owned
table, the broadphase itself, and the inertia-from-shape wiring. The bounds
and debug machinery are already shipped.

**Out of scope here, by design:** contact-manifold generation and GJK/EPA (Phase
3), the contact/constraint solve (Phase 3), continuous detection (Phase 4), the
OmegaSL broadphase kernel (Phase 5). We design the shape data and the broadphase
structure so those are a port, not a rewrite.

---

## 2. Why detection is its own problem

Collision *detection* is not a smaller version of collision *response*; it is a
different discipline with a different cost model and a different failure mode.

1. **The cost is combinatorial, and the naïve answer is quadratic.** With `n`
   bodies there are `n(n-1)/2` pairs. At 10k bodies that is ~50M pair tests per
   step — untenable. Broadphase exists to reject the ~99.9% of pairs that are
   nowhere near each other in (close to) linear time, leaving narrowphase a short
   candidate list. Every mature engine is, at this layer, a spatial-sorting
   problem wearing a physics hat.
2. **The asymmetry of error is brutal.** A broadphase that reports a pair that
   doesn't actually touch costs one wasted narrowphase test — cheap, self-
   correcting. A broadphase that *misses* a pair that does touch causes objects
   to silently interpenetrate or pass through each other, and the bug surfaces far
   downstream as "the physics is broken," not "the broadphase dropped pair
   (i,j)." **False positives are a tax; false negatives are a catastrophe.** The
   whole layer must be conservative by construction.
3. **Bounds must be cheap *and* correct under rotation.** A body's world AABB is
   recomputed every sub-step from its (now rotating — Phase 1) orientation. A
   tight AABB of a rotated box is not the rotation of its local AABB; getting this
   subtly wrong produces bounds that are too small exactly when the body spins,
   reintroducing failure mode #2.
4. **It is the shared spine of all three pillars.** Rigid bodies (widely varying
   sizes), particles (Phase 6: uniform, hundreds of thousands), and soft bodies
   (Phase 8+) all query the same broadphase. A structure that is excellent for one
   can be poor for another — which is why the structure choice (§11.1) is the
   most consequential decision in this phase.

A correct Phase 2 handles all four at once, on a path that will later run as a GPU
kernel over SoA buffers.

---

## 3. Prior art — how the incumbents solve it

Studied to understand the terrain and the failure modes, **not** to transcribe.
Descriptions are drawn from published talks, docs, and source structure and are
representative, not a claim to quote current internals.

**NVIDIA PhysX 5.**
- **Shapes** are reference-counted and *shared*: one cooked shape (with its
  precomputed mass/inertia and, for convex hulls, the hull + support data) is
  instanced across many actors. Mass properties are "cooked" once.
- **Broadphase** ships in multiple modes: an incremental **sweep-and-prune
  (SAP)** for moderate counts with good frame-to-frame coherence, and a
  **GPU broadphase** (regular grid / parallel pair-gen) for large GPU scenes.
  PhysX chose to keep *both* because no single structure wins everywhere.
- **AABBs are fattened** so a body that barely moves doesn't churn the structure
  every frame — the bound is rebuilt only when the body leaves its fat bound.

**Epic Chaos (Unreal Engine 5).**
- **Bounding-volume hierarchy** over fattened AABBs as the spatial acceleration
  structure, with a grid option; rebuild/refit strategies tuned for the mix of
  static level geometry and dynamic actors.
- **Implicit-object** shape representation (analytic sphere/box/capsule + convex)
  with support functions feeding GJK in narrowphase.
- Filtering via query/sim collision channels — the layer/mask idea, elaborated.

**The shared shape:** both keep shapes analytic where possible (sphere/box/capsule
get specialized treatment, convex is the general case), both *fatten* their bounds
for temporal coherence, both filter early with bitmask channels, and — tellingly —
**both ship more than one broadphase** because the structure is workload-dependent.
That last point is the bar to clear and the trap to avoid: we should choose with
our workload (compute-first, particle-heavy) in mind rather than inheriting a
CPU/SIMD-era default.

---

## 4. The literature we build on

The research leads the shipped engines by years. The pieces we combine:

- **Cohen et al., "I-COLLIDE" / Baraff — Sweep-and-Prune.** The canonical
  coherence-exploiting broadphase: keep the AABB interval endpoints sorted on each
  axis, and frame-to-frame the sort is nearly-sorted so an insertion-sort update
  is near-linear. Excellent on the CPU; the basis of PhysX's incremental mode.
- **Karras, "Maximizing Parallelism in the Construction of BVHs, Octrees, and
  k-d Trees" (HPG 2012)** and **Lauterbach et al., "Fast BVH Construction on
  GPUs" (EG 2009).** **LBVH**: assign each primitive a **Morton code** (Z-order
  curve over its AABB centroid), radix-sort the codes, and build the hierarchy
  from the sorted codes in parallel with no pointer chasing. This is the modern
  GPU-broadphase backbone.
- **Green, "Particle Simulation using CUDA" (NVIDIA, 2010).** The **sort-based
  uniform grid**: hash each body's cell, radix-sort `(cellHash, bodyIndex)`,
  and read contiguous runs per cell to generate same/neighbour-cell pairs — a
  fully data-parallel broadphase with no tree. The canonical massively-parallel
  approach and a perfect fit for Phase 6 particles.
- **Morton (1966) Z-order; Harada / "Real-Time Collision Detection" (Ericson,
  2004).** Spatial hashing, AABB math, the SAH surface-area heuristic, and the
  endpoint-interval machinery — the reference for the bounds and hashing details.
- **"Multi-level / hierarchical grids" (open literature).** A two-level grid
  (coarse cells for big objects, fine for small) is the standard answer to the
  uniform grid's one real weakness — widely varying object sizes — without
  abandoning the grid's GPU-friendliness.

The throughline: the incumbents' SAP/BVH heritage is CPU-coherence-shaped, while
the *newer* sort-based grid and LBVH lines were designed for exactly our substrate
(massively parallel, SoA, GPU radix sort). That is the opening.

---

## 5. Where AQUA diverges — the openings

Grounded in the actual post-Phase-1 surface (`AQMath.h`, `AQIntegrator.h`,
`AQSpace.h`):

- **Broadphase as a sort, not a tree — to match the compute substrate.** PhysX
  and Chaos lead with SAP/BVH because their heritage is CPU/SIMD with strong
  frame coherence. AQUA is compute-first (§3 of the roadmap) and must serve Phase
  6's hundreds of thousands of particles on the *same* structure. A **sort-based
  spatial hash** (Green) reduces broadphase to "compute cell hashes → radix-sort →
  scan runs," every step of which is a known, embarrassingly-parallel GPU kernel —
  the same one-thread-per-element shape `AQStepBody` already has. The incumbents'
  incremental SAP cannot hand us that; a sort can.
- **Shapes are POD value types we own — physics semantics, GPU-uploadable.** GTE
  already ships geometric primitives (`GSphere`, `GCapsule`, `GRectangularPrism`,
  `GCylinder`, … — `GTEBase.h:155-192`), but they are **rendering-oriented**:
  float-only, parameterized for drawing (`GRectangularPrism` is min-corner +
  `w,h,d`; `GCapsule` is bottom-hemisphere-center + `radius,height`), with no
  support function and no inertia. Collision wants centroid-centered half-extents,
  a `support(dir)` for the Phase 3 GJK path, and shape→inertia. So `AQShape` is
  **AQUA-owned** (per the roadmap boundary rule: math is the only borrow), a
  trivially-copyable POD tagged union that uploads to a GTE buffer with no
  repacking. We convert to GTE's primitives only at the **debug-draw** boundary,
  where kREATE already knows how to render them.
- **Shape→inertia closes a Phase 1 loop, from the same code.** Phase 1 shipped
  `AQinertiaSolidBox/Sphere/Capsule` and left `AQBodyDesc::inertiaPrincipalMoments`
  with a documented "derive from shape (Phase 2)" hook. Phase 2 calls those
  helpers when a shape is attached and moments are left zero — no new numerics,
  and the `double`/`float` genericity carries over for the parity harness.
- **One bounds type, generic where it must be — DONE in Phase 1.1.** `AQAABB<Ty>`
  is a small AQUA-owned value type (min/max `AQVec3<Ty>`) with merge / overlap /
  fatten / surface-area, now living in `AQMath.h`. The overlap test is a branch-
  light min/max compare — exactly what a GPU thread wants — and surface-area is
  there so a BVH path (if §11.1 chooses it) has its SAH for free. Phase 1.1 also
  shipped the §6.1 oriented-box bound `AQaabbOfOrientedBox(c, h, q)`, so the
  rotation-correct bound — the §2-point-3 failure-mode mitigator — already
  exists at the math layer and just needs Phase 2 to call it from the per-shape
  bound functions.
- **We own the determinism stance (again).** Broadphase output is a *set* of
  pairs; a set has no canonical order, and unordered output is a determinism leak
  (Phase 5 / roadmap §7.4). We emit pairs in a **fixed sorted order** (`(minIndex,
  maxIndex)` lexicographic) on both the CPU and the future GPU path, so the
  candidate list — and therefore Phase 3's solve order — is reproducible.

**Gaps we must fill (this is Phase 2's work):** there is no shape type, no AABB,
no broadphase, no collision filtering, and `AQBodyDesc` carries no shape. None of
it is numerically-sensitive linear algebra (the roadmap's reason for borrowing
`Matrix`/`Quaternion`); it is collision machinery we own and build on the Phase 1
math.

---

## 6. Proposed algorithm — fattened AABBs + a sort-based spatial hash

The synthesis, per sub-step, all per-element independent (one thread per body for
the AABB pass, one per element for the sort — no atomics in the candidate scan if
we use the run-length approach below):

**A. World-AABB refresh (one thread per body).**
```
for each body b:
    if b moved outside its fat AABB (or b is new):
        local = shape AABB in body frame              // analytic per shape
        world = transform-and-bound(local, b.transform) // §6.1, rotation-correct
        b.fatAABB = fatten(world, margin + velocityDilation(b))   // §11.4
```
Fattening means a slow body keeps its bound across steps, so the structure churns
only for bodies that actually move — the temporal-coherence win the incumbents
get from SAP, without SAP.

**B. Broadphase — sort-based uniform grid (Green), the lean (§11.1).**
```
1. cellSize = chosen from the AABB size distribution (§11.4)
2. for each body b (one thread): hash = mortonOrLinear(cellOf(center(b.fatAABB)))
                                 emit (hash, b)
3. radix-sort the (hash, b) array by hash               // GPU-standard primitive
4. for each non-empty cell run (one thread per run):
       for each pair (i,j) within the run and with the 26 neighbour cells:
           if fatAABB[i] overlaps fatAABB[j] and passFilter(i,j):
               emit ordered pair (min(i,j), max(i,j))
5. de-duplicate (a pair can be found from two cells) — sort pairs + unique,
   which also gives the deterministic ordering from §5.
```

**Rotation-correct AABB (§6.1).** For an oriented box with half-extents `h` and
rotation `R` (from the body quaternion via Phase 1's `AQrotate`), the world
half-extent along each world axis is `|R|·h` (the matrix of absolute values times
`h`) — the standard, cheap, *correct-under-rotation* bound. Sphere and capsule
bounds are orientation-trivial (sphere) or one rotated segment plus radius
(capsule). This is the detail §2 point 3 warns about; it is written once, in the
shape→AABB function, and tested directly.

Why this combination:
- **Sort-based grid** is the one broadphase whose every step is already a GPU
  primitive (hash, radix sort, scan), so Phase 5 is a port and Phase 6's particles
  reuse it wholesale. Its weakness — wildly varying object sizes — is real but
  bounded, and addressed by a **two-level grid** escape hatch (§11.1) rather than
  by abandoning the approach.
- **Fattened AABBs** buy the frame-to-frame coherence SAP is prized for, while
  keeping the rebuild itself a stateless parallel pass (no sorted-endpoint state
  to maintain across steps, which is the part of SAP that resists the GPU).
- **Ordered, de-duplicated output** keeps the candidate list deterministic for
  the Phase 5 parity target and gives Phase 3 a stable solve order.

**Alternative considered — LBVH (Karras).** Equally GPU-friendly to build (Morton
sort + parallel hierarchy) and *better* for widely varying sizes, but it is more
machinery (hierarchy build + traversal) than a hash for the uniform-ish dynamic
case, and the grid is the structure Phase 6 wants anyway. Kept as the lead
alternative and an open decision (§11.1), not discarded. **SAP** is kept as the
CPU-reference simplicity baseline, not the production lead.

---

## 7. New types AQUA must add — `include/aqua/AQCollision.h` (draft)

AQUA-owned, AQ-prefixed, no namespace (per `AGENTS.md`, as settled in Phase 1).
`AQAABB` already lives with the math in `AQMath.h` as of Phase 1.1; the shape
and broadphase types get a new header `include/aqua/AQCollision.h`. The public
surface consumes the `float` forms; AABB math stays `Ty`-generic so the `double`
parity oracle still works.

> **§7.1 `AQAABB` — already shipped in Phase 1.1.** The draft below is preserved
> for historical context; the as-shipped surface in `AQMath.h` is broader. It
> ships: `min`/`max` (`AQVec3<Ty>`, factory-initialized to zero so the type
> default-constructs cleanly through GTE's `Matrix::Create()`); the static
> factories `empty()` (min=+∞, max=−∞ for accumulation), `fromMinMax(lo, hi)`,
> and `fromCenterHalfExtents(c, h)`; member queries `overlaps(o)`, `merged(o)`,
> `fattened(margin)`, `center()`, `extents()`, `surfaceArea()`, `contains(p)`;
> and the free function `AQaabbOfOrientedBox(centerW, halfExtents, q)` from
> §6.1 using `|R|·h`. `FAABB = AQAABB<float>` is the public alias. Phase 2
> *consumes* this — do not re-declare it in `AQCollision.h`.

```cpp
#ifndef AQUA_AQCOLLISION_H
#define AQUA_AQCOLLISION_H

#include "AQMath.h"          // brings in AQAABB / FAABB / AQaabbOfOrientedBox
#include <cstdint>

// --- Axis-aligned bounding box: HISTORICAL DRAFT — landed in Phase 1.1's
// AQMath.h with the surface noted in §7.1 above. Kept for reference only. ---
template<class Ty>
struct AQAABB {
    AQVec3<Ty> min = AQvec3<Ty>( std::numeric_limits<Ty>::max(),
                                 std::numeric_limits<Ty>::max(),
                                 std::numeric_limits<Ty>::max());
    AQVec3<Ty> max = AQvec3<Ty>(-std::numeric_limits<Ty>::max(),
                                -std::numeric_limits<Ty>::max(),
                                -std::numeric_limits<Ty>::max());

    bool overlaps(const AQAABB& o) const;     // branch-light min/max compare
    AQAABB merged(const AQAABB& o) const;
    AQAABB fattened(Ty margin) const;          // symmetric grow (§11.4)
    Ty     surfaceArea() const;                // for a BVH SAH, if §11.1 picks it
    AQVec3<Ty> center() const;
};
using FAABB = AQAABB<float>;

// --- Collision shapes. POD tagged union: trivially copyable, GPU-uploadable,
// no virtuals on the kernel data path. Primitive params are raw floats (not
// FVec<3>) so the union stays a trivial type — OmegaGTE::Matrix has a private
// default ctor and user ctors and cannot be a union member; convert with
// AQvec3() at call sites (the same constraint Phase 1 hit). ---
enum class AQShapeType : std::uint32_t { Sphere, Box, Capsule, Plane, ConvexHull };

struct AQShape {
    AQShapeType        type;
    AQTransform<float> local;          // shape pose relative to body COM (offset / compound)
    union {
        struct { float radius; }                          sphere;
        struct { float hx, hy, hz; }                      box;      // half-extents
        struct { float radius, halfHeight; }              capsule;  // axis = local +Y
        struct { float nx, ny, nz, offset; }              plane;    // n·x = offset, half-space
        struct { std::uint32_t firstVertex, vertexCount; } hull;    // into AQSpace's hull-vertex pool
    };
};

// Free functions over AQShape (declarations; bodies land with the phase). These
// are the analytic per-shape primitives the broadphase and Phase 3 narrowphase
// share. computeInertia bridges to the Phase 1 helpers.
template<class Ty> AQAABB<Ty> AQshapeAABB(const AQShape& s, const AQTransform<Ty>& bodyXform);
AQVec3<float>      AQshapeSupport(const AQShape& s, const AQVec3<float>& dirWorld,
                                  const AQTransform<float>& bodyXform);  // for Phase 3 GJK
AQVec3<float>      AQshapeInertiaMoments(const AQShape& s, float mass);   // -> AQinertia* (AQMath.h)

// --- Opaque handle to a shape owned/instanced by an AQSpace (§11.3). A small
// value (index + generation into the space's shape table), copyable and
// backend-free, so it crosses the pimpl boundary without exposing AQShape
// storage or any backend type. ---
struct AQShapeHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;   // guards against stale handles after removal
    AQUA_NODISCARD bool valid() const { return generation != 0; }
};

// --- A broadphase candidate pair (body indices into the space's SoA arrays). ---
struct AQBroadphasePair {
    std::uint32_t a, b;     // always a < b (deterministic ordering, §5)
};

// --- Collision filtering: 32-bit layer membership + collision mask (§11.5). ---
// Two bodies are a candidate iff (a.layer & b.mask) && (b.layer & a.mask).
struct AQCollisionFilter {
    std::uint32_t layer = 1u;       // which layer(s) this body belongs to
    std::uint32_t mask  = ~0u;      // which layers it collides with
};

#endif // AQUA_AQCOLLISION_H
```

The `AQshapeAABB` body is the **rotation-correct bound** of §6.1 — the one
determinism-and-correctness-critical piece, shown in full when it lands. `AQShape`
is a POD by construction so a Phase 5 kernel reads an array of them coalesced; the
convex-hull case keeps its vertices in a separate pooled buffer referenced by
`(firstVertex, vertexCount)`, exactly as the SoA layout (§8) wants.

---

## 8. Data layout & GPU/numeric specialization

Decided now so Phase 5 is a port, not a rewrite (ties to roadmap §5/§7.3 SoA
decision):

- **SoA, extending the Phase 1 layout.** `AQBodyState` (`AQIntegrator.h`) is
  already the per-body dynamics state; Phase 2 adds parallel pooled arrays:
  `fatAABB[]`, `shapeIndex[]`, `filter[]`, and the transient broadphase scratch
  (`cellHash[]`, sorted `(hash,index)[]`, `pair[]`). Each uploads as one
  contiguous buffer; the broadphase kernels read them coalesced.
- **Shapes pooled and shared.** A space owns a **shape table** (`std::vector` /
  pooled GTE buffer of `AQShape`) plus a **hull-vertex pool**; bodies hold a
  `shapeIndex`, not a shape — PhysX-style instancing (§11.3). One upload per
  shape, reused by every body that references it.
- **Broadphase is stateless per step (the lean).** The sort-based grid keeps no
  cross-frame structure — it rebuilds from `fatAABB[]` each step. Fattening
  provides the coherence; the rebuild is pure data-parallel. (A BVH/SAP lead,
  §11.1, would instead carry refit state — a Phase-5 complication noted here.)
- **Determinism:** fixed cell-hash function and a stable radix sort shared by the
  C++ and (future) OmegaSL paths; ordered, de-duplicated pair output (§5). The
  brute-force O(n²) path is the oracle, and `double`-precision AABBs are available
  for it, mirroring Phase 1.

---

## 9. Validation — how we measure "better"

The incumbent's *behavior* is the reference, not its code (roadmap §4).

- **Correctness against the brute-force oracle (the headline).** For thousands of
  randomized scenes and over a moving simulation, the broadphase pair set must be
  a **superset of every truly-overlapping AABB pair (zero false negatives — the
  catastrophe of §2)** and contain **no pair whose fat AABBs do not overlap**
  (false positives bounded by the fattening margin, and accounted for). This is
  the exact analogue of Phase 1's `double` oracle: a slow, obviously-correct
  reference the fast path is held to.
- **Rotation-correctness.** A spinning box's world AABB must always contain the
  box (sample the 8 corners through `AQrotate`); the bound is never too small —
  the §2-point-3 failure mode, tested directly.
- **Determinism & order.** The same scene yields a byte-identical ordered pair
  list across runs (and, in Phase 5, across CPU/GPU within the chosen tolerance).
- **Scaling.** Pair-generation time grows ~linearly with body count on
  uniformly-distributed scenes, versus the O(n²) baseline — the reason the layer
  exists. Reported as a logged series, not asserted as a hard wall-clock bound
  (which is machine-dependent).

Metrics emitted as debug-draw / logged series (roadmap §3 principle 6, "author for
the 3am engineer"): per-body fat AABBs, the highlighted candidate pairs, candidate
count vs. brute-force count, and a **loud guard** when the candidate count
approaches O(n²) (a sign the cell size is mistuned for the scene — the broadphase
analogue of Phase 1's fast-spin warning).

**The debug bus already exists.** Phase 1.1 shipped the drainable `AQDebugLine`
stream and the per-space `setDebugFlags / debugFlags / drainDebugLines` surface
(`AQDebug.h`, `AQSpace.h`); Phase 2 extends the existing `AQDebugFlags` enum
with new bits — `AQDebugAABB` (one merged-corners outline per body's fat AABB,
12 line segments), `AQDebugBroadphasePair` (one segment from `center(a)` to
`center(b)` per emitted candidate, color-coded by filter result), and
`AQDebugBroadphaseGuard` (a single red line emitted once per step when the
loud-guard fires) — and appends into the same buffer the kREATE adapter
already drains. No new transport, no new boundary, no new consumer contract.

---

## 10. Public API additions

Extends the existing surface — split across `include/aqua/AQRigidBody.h`
(`AQBodyDesc`, `AQRigidBody` — Phase 1.1 split them out of `AQSpace.h`) and
`include/aqua/AQSpace.h` (`AQSpace`) — without breaking the pimpl discipline.
New members marked `// new`; pre-existing-from-Phase-1.1 members that Phase 2
*uses* are marked `// Phase 1.1, present`. No OmegaSL or backend types cross
into `include/aqua/*`; only AQUA types, the `AQShape`/`AQAABB`/filter types
from §7, and the borrowed `FVec`/`FQuaternion` appear.

**`AQBodyDesc` (in `AQRigidBody.h`):**
```cpp
struct AQUA_EXPORT AQBodyDesc {
    // ... Phase 1 fields (pose, motion, mass, inertia, restitution, friction) ...

    // Phase 1.1 already shipped (do not re-declare):
    //   AQMat3F           inertiaTensor;     // optional full 3×3 (diagonalized)
    //   OmegaGTE::FVec<3> centerOfMass;      // reserved COM offset — Phase 2 wires
    //   float linearDamping, angularDamping, gravityScale, maxAngularSpeed;

    AQShapeHandle    shape;                          // new — geometry (see AQSpace::createShape)
    AQCollisionFilter filter;                        // new — layer / mask (§11.5)
    // inertiaPrincipalMoments left zero now *derives from `shape`* (Phase 1 hook closed).
    // When the shape's cooked inertia is non-diagonal (convex hull), descriptors
    // may instead fill `inertiaTensor` directly — the Phase 1.1 path. The
    // shape-handle convenience exists so the simple sphere/box/capsule cases
    // don't need to touch either.
};
```

**`AQRigidBody` (in `AQRigidBody.h`):**
```cpp
class AQUA_EXPORT AQRigidBody {
public:
    // ... Phase 1 linear/angular/mass/force API ...

    // Phase 1.1 already shipped (do not re-declare):
    //   worldInverseInertia() / linearMomentum() / angularMomentum() / kineticEnergy()
    //   setLinearDamping/setAngularDamping/setGravityScale/setMaxAngularSpeed (+ getters)

    AQUA_NODISCARD AQShapeHandle shape() const;                 // new
    void setShape(const AQShapeHandle &s);                      // new (re-derives inertia if auto)
    AQUA_NODISCARD OmegaGTE::FVec<3> aabbMin() const;           // new — current world (fat) AABB
    AQUA_NODISCARD OmegaGTE::FVec<3> aabbMax() const;           // new

    AQUA_NODISCARD AQCollisionFilter collisionFilter() const;   // new
    void setCollisionFilter(const AQCollisionFilter &f);        // new
};
```

**`AQSpace`:**
```cpp
class AQUA_EXPORT AQSpace {
public:
    // ... Phase 1 gravity / addBody / removeBody / bodyCount ...

    // Phase 1.1 already shipped (do not re-declare):
    //   setDebugFlags(std::uint32_t) / debugFlags() / drainDebugLines()
    //   — Phase 2 only adds NEW BITS to AQDebugFlags (see §9), the surface stays.

    // Shape factory — shapes are owned/instanced by the space (§11.3) and
    // referenced by handle from descriptors. AQShape stays out of the call site;
    // these mirror GTE's named-ctor idiom.
    AQShapeHandle createSphereShape(float radius);                                 // new
    AQShapeHandle createBoxShape(const OmegaGTE::FVec<3> &halfExtents);             // new
    AQShapeHandle createCapsuleShape(float radius, float halfHeight);              // new
    AQShapeHandle createPlaneShape(const OmegaGTE::FVec<3> &normal, float offset); // new
    AQShapeHandle createConvexHullShape(const OmegaGTE::FVec<3> *pts, std::size_t n); // new

    // Broadphase result access — for debug-draw now, Phase 3 narrowphase next.
    // Returns the current ordered, de-duplicated candidate pairs.
    AQUA_NODISCARD std::vector<AQBroadphasePair> candidatePairs() const;           // new
};
```

> **Folded-in groundwork (lands first, §1).** `AQBodyDesc::shape` did not exist;
> `addBody` (`AQSpace.cpp`) gains: store the shape handle on the body's `Impl`; if
> the body is dynamic and `inertiaPrincipalMoments` is zero, fill it from the
> shape via `AQshapeInertiaMoments` → the Phase 1 `AQinertia*` helpers (closing
> the documented hook in `AQRigidBody.h`). `stepInternal` gains the world-AABB
> refresh (§6.A) before/after the integrate, and the broadphase runs once per
> `advance` tick (not per sub-step — pair *candidates* are stable within a
> frame's sub-steps; revisit if CCD/fast bodies in Phase 4 need finer).
> `AQBodyState::comOffset` (reserved by Phase 1.1, currently zero) gets wired
> in here: the torque arm in `applyForceAtPoint` uses `worldPoint − (position +
> R · comOffset)`, and `angularMomentum`'s world position picks up the offset.
> The existing Phase 1.1 accessor tests pin the zero-COM behaviour as a
> regression guard — non-zero COM cases get their own Phase 2 tests.

`AQShapeHandle` is a small opaque value (index + generation into the space's shape
table), copyable and backend-free — it is **not** a pointer to a backend type, so
the pimpl boundary holds.

---

## 11. Open decisions for this phase

1. **Broadphase structure — sort-based uniform grid vs. LBVH vs. SAP (vs. a
   per-pillar split).** *Lean: sort-based uniform grid*, with a **two-level grid**
   escape hatch for size variance — because every step is already a GPU primitive
   (hash / radix-sort / scan), it is the structure Phase 6 particles want, and
   fattened AABBs supply the coherence SAP would. LBVH is the strong alternative
   if rigid-body **size variance** dominates the project's scenes; SAP is kept as
   the CPU-simplicity reference, not the lead. This is the §4 research-loop
   decision of the phase and the single biggest fork here — settle it before the
   broadphase lands. *(Roadmap §7.5 / Phase 2 key decision.)*
2. **One shared broadphase vs. per-pillar.** The incumbents ship more than one
   (§3). *Lean: one shared structure* (the grid) to start, since rigid and
   particle workloads both map to it; split only if Phase 6 profiling shows the
   rigid mix needs a different structure. Tied to #1.
3. **Shape representation & ownership — AQUA-owned POD tagged union, instanced via
   the space, vs. reuse GTE's `GSphere`/`GCapsule`/`GRectangularPrism`
   (`GTEBase.h:155-192`) vs. a polymorphic class hierarchy.** *Lean: AQUA-owned
   POD `AQShape`, shape table in the space, referenced by handle* — physics
   semantics (half-extents, support fn, inertia), GPU-uploadable, no virtuals on
   the kernel path; convert to GTE primitives only at the debug-draw boundary. The
   reuse option keeps one geometry vocabulary across graphics+physics but inherits
   rendering-shaped parameterization and no support/inertia — flagged, not chosen
   unilaterally.
4. **AABB fattening / cell-size policy.** *Lean: a small fixed margin plus a
   velocity-proportional dilation* (`v·dt`), and a cell size derived from the
   median fat-AABB size. Governs the false-positive rate (§9) and rebuild
   frequency; revisit when CCD (Phase 4) changes the velocity assumptions.
5. **Collision filtering model — 32-bit layer+mask vs. group IDs vs. callback.**
   *Lean: layer+mask bitfields* (the `AQCollisionFilter` of §7) — cheap, branch-
   light, GPU-friendly; the symmetric `(a.layer & b.mask) && (b.layer & a.mask)`
   rule. A "never collide connected bodies" filter is deferred to Phase 4 (joints).
6. **Convex hull in Phase 2 or deferred.** *Lean: land sphere / box / capsule /
   plane first (the thin slice, roadmap §3 principle 5), convex hull as the Phase
   2 stretch*; heightfield and static triangle mesh deferred to a Phase 2.x / the
   static side of Phase 3. *(2026-07-07: the Phase 2.x proposal now exists —
   `.plans/future/Phase-2.x-Static-Mesh-Colliders.md` — covering cooked static
   triangle meshes with a PLOC-built midphase BVH, internal-edge filtering, and
   the static world set beside the grid; heightfield stays a follow-on there.)*
7. **Where the candidate list is owned & handed off.** Exposed via
   `candidatePairs()` for debug now; the real consumer is Phase 3 narrowphase.
   *Lean: the space owns the SoA pair buffer; Phase 3 reads it in place* (no copy),
   and `candidatePairs()` is a convenience view. Decide the handoff type now so
   Phase 3 doesn't reshape it.

---

*Brief status: proposal. Decisions in §11 — above all the broadphase structure
(#1) — should be settled before the broadphase lands. This document is the Phase 2
entry of the per-phase prior-art series roadmap §4 establishes, and follows the
conventions set by `Phase-1-Dynamics-Math-Core.md`.*

*Audit 2026-06-04 (post-Phase 1.1). Items now closed and removed from Phase 2's
implementation scope: `AQAABB<Ty>` + `AQaabbOfOrientedBox` (in `AQMath.h`); the
drainable `AQDebugLine` / `AQDebugFlags` / `AQSpace::drainDebugLines` bus (in
`AQDebug.h` + `AQSpace.h`); the reserved `AQBodyDesc::centerOfMass` /
`AQBodyState::comOffset` model fields (Phase 2 still owns the wiring); the
full-tensor `AQBodyDesc::inertiaTensor` path with diagonalize-and-fold (useful
for cooked convex hulls). The body-side header was split out of `AQSpace.h` to
`AQRigidBody.h` — §10 paths updated accordingly. Phase 2's remaining scope is:
shape POD + handle + space-owned table, the broadphase algorithm + tests,
inertia-from-shape wiring, COM-offset wiring, and the new debug-flag bits.*

---

## 12. Recency-principle audit (addendum, 2026-06-06)

Roadmap §4 was strengthened to make "newest viable algorithm from the
literature" the standing default for every phase, with incumbents adopted
only when no substantively-newer alternative offers a real improvement for
AQUA's substrate (`Physics-Roadmap.md` §4 — "Recency principle"). The
Phase 2 brief predates the explicit rule; the audit ran retroactively on
all three sub-choices (shape representation, world-AABB recomputation,
broadphase structure). Findings recorded here, mirrored as a one-line
entry in `Physics-Roadmap.md` §5 Phase 2.

The Phase 2 picks (fattened analytic AABBs + sort-based uniform spatial
grid per Green 2010, with LBVH per Karras 2012 as the §11.1 escape hatch
for size variance) date to a 2010-2012 line. What does 2018-onwards add?

- **Hardware-accelerated RT-core broadphase — substantively newer,
  flagged-for-Phase-5, not adopted now.** Two recent papers exploit
  RTX/RT-core BVH-traversal hardware for collision detection: **Wang et
  al., "Hardware-Accelerated Ray Tracing for Discrete and Continuous
  Collision Detection on GPUs" (arXiv 2409.09918, 2024)** uses the BVH
  traversal hardware as a black-box broadphase; **Liu et al., "Mochi:
  Fast & Exact Collision Detection" (arXiv 2402.14801, 2024)** combines
  OBB-BVH broad-phase with a novel object-object intersection narrow-
  phase. Both report substantial speedups vs. software broadphases on
  RTX-capable hardware. **Not adopted for Phase 2** because: (a) the
  hardware is **NVIDIA-RTX-specific** today (AMD's patented unified
  BVH for ray tracing + physics is filed but not yet shipping; Apple
  Metal RT and Vulkan VK_KHR_ray_tracing are gaining ground but
  feature-uneven across GPUs), and AQUA's compute-first promise
  (`About.rst` / roadmap §3) is *all three backends, software path
  required, hardware path optional*. (b) The win is at the broadphase
  query stage; AQUA's broadphase is already a sort-based grid whose hot
  path is `O(n + p)` where `p` is candidate pairs — the cliff RT cores
  flatten is BVH traversal, which we don't pay. **Revisit as a
  Phase 5.x optional acceleration path** when the OmegaGTE backend has
  feature flags for hardware RT (`GTEDEVICE_FEATURE_RAYTRACING` — per
  the `project_rt_runtime_feature_check` memory). Note recorded in
  §11.1 as a third path joining grid / LBVH / SAP.
- **PLOC / PLOC++ / PRBVH (Meister & Bittner 2018a, 2018b, 2022) — the
  Karras 2012 LBVH successor.** PLOC (Parallel Locally-Ordered
  Clustering) and PRBVH (Parallel Reinsertion BVH) produce
  substantially-higher-quality BVHs than Karras's LBVH at competitive
  build cost (Meister & Bittner 2018a/b, EUROGRAPHICS 2018; Meister &
  Bittner 2022 selects per-node between object and spatial splits).
  **If the §11.1 escape hatch fires** — i.e. AQUA's workload mix proves
  to need a BVH instead of the grid for size-variance reasons —
  **PLOC/PRBVH is what we build, not Karras LBVH.** Phase 2 picks the
  grid as the lead so this doesn't bite yet; the citation is recorded
  so the eventual BVH path skips a generation. The §11.1 decision text
  is updated accordingly (LBVH → "PLOC++ / PRBVH per Meister & Bittner
  2018/2022").
- **Compact hashing (Teschner et al. 2003; Lefebvre & Hoppe 2006;
  refined Ihmsen et al. 2011) — minor refinement, not a blocker.** A
  sparse-cell hash table feeding the sort step trades the uniform
  grid's full-allocation cost for a hashing one, with the same
  candidate-pair output. For AQUA's dense-mid-volume rigid-body
  workload the uniform grid's bounded allocation is fine; for the
  Phase 6 particles workload with hundreds of thousands of points
  spread sparsely, compact hashing is the cleaner memory pattern.
  **Carry as a Phase 6 note**: the per-pillar split option
  (§11.2 — "one shared broadphase vs. per-pillar") becomes specifically
  "compact-hashing variant of the same sort-based grid for the particle
  pillar," which is a layout swap not an algorithm swap.
- **GPU spatial hashing with thread-block-level cooperation
  (FLIP / SPH literature, 2024+) — adopted in spirit.** Recent
  GPU-spatial-hashing refinements (Liu et al. 2024 MDPI FLIP paper +
  earlier SPH lines) emphasize cooperative-group reduction and L1
  cache-friendly traversal patterns; these are *kernel-level
  optimizations* of the same sort-based grid Phase 2 picks, not
  algorithmic divergence. Roll into the Phase 5 OmegaSL kernel author
  pass — the Phase 2 algorithm is unchanged.
- **AABB fattening — no divergence.** The fattened-AABB + velocity-
  dilation policy (§11.4) is the same shape as Bullet's btDbvtBroadphase
  (DBVT) and PhysX's incremental SAP; no substantively-newer policy
  has emerged. The two-tree DBVT (separate static/dynamic) is an
  *architecture* choice that re-engages SAP/BVH heritage AQUA
  deliberately leaves behind. No change.
- **Karras 2012 LBVH — superseded by PLOC++** (see above) as the
  modern BVH-construction reference; the §11.1 decision lean is
  updated.

**Net for Phase 2:** the recency audit finds **no algorithmic
divergence to adopt now** for the chosen sort-based uniform grid lead —
Green 2010 + Karras 2012 (as the alternative) + fattened analytic AABBs
remain the right answer for AQUA's compute-first, all-three-backends,
particle-coexistent substrate. One **citation update** lands: the
§11.1 LBVH alternative is recharacterized as PLOC++ / PRBVH
(Meister & Bittner 2018/2022) so the future BVH path skips a
generation. Two future-work items recorded: **(a)** hardware RT-core
broadphase as a Phase 5.x acceleration path gated on
`GTEDEVICE_FEATURE_RAYTRACING` (Wang 2024, Mochi 2024); **(b)**
compact-hashing as a Phase 6 particle-pillar memory layout swap.

Re-audit due: 2028-06-06 (roadmap §4 two-year freshness rule) or sooner
if (a) PhysX 6 / Chaos roadmaps publicly commit to RT-core broadphase
shipping, or (b) the §11.1 escape hatch fires and we actually need to
build the BVH path.
