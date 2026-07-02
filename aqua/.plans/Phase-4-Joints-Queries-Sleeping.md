# AQUA Phase 4 — Joints, Queries, Sleeping & CCD

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a subsystem is written: what PhysX and Chaos
do for joints, queries, islands/sleeping and continuous detection, which papers
we improve on, what we change for AQUA's substrate, and how we will measure
"better." It covers **Phase 4 — Joints, queries & sleeping [Newtonian → complete]**
only: turning the working contact solver from Phase 3 into the full rigid-body
feature set that kREATE's Engine-Roadmap Phase 8 needs to consume — bilateral
constraints (joints), gameplay queries (raycast / shapecast / overlap), kinematic
bodies and trigger volumes, island detection with sleeping, and continuous
collision detection for fast/thin bodies. No OmegaSL kernel port yet (Phase 5);
no particle or soft contacts (Phase 6+); no full Featherstone articulations
(deferred minor) — but every choice here is made so those phases inherit it
cleanly.

---

## 1. Scope & deliverable

**Goal.** Take the working Phase 3 PGS solver and make it a *complete* rigid-body
engine: solve bilateral constraints (joints) in the same row buffer the contact
solver already iterates; answer gameplay queries against the Phase 2 broadphase
without re-walking the world; let bodies that have come to rest stop costing
solver iterations; and stop fast objects from tunneling through thin geometry.

**Runnable deliverable.** Four scenes, all driven through the public
`AQContext` / `AQSpace` / `AQRigidBody` surface, asserted in `tests/`:

1. **The swinging bridge** — 12 dynamic boxes connected end-to-end by 11
   ball-socket joints, with the two end-boxes pinned to static anchors. Within
   3 simulated seconds the bridge must reach a quasi-static catenary shape; each
   joint's position error (distance between the two anchor points it constrains)
   must stay `< 1 mm` for another 5 s. The analytic catenary tension at the
   midspan is hand-computable from the chain weight; the solver's accumulated
   joint impulse must match that within 5% — the bilateral-constraint analogue
   of Phase 3's resting-stack normal-impulse oracle.
2. **The hinge door** — a kinematic frame, one dynamic door panel, one hinge
   joint with angular limits `[−90°, +90°]` and a small motor torque (0.5 N·m).
   The door must swing under gravity, hit each limit and bounce cleanly (no
   limit penetration `> 0.01 rad`), and the motor must drive it open against the
   stop at a measured steady-state angular velocity matching `τ / (c·ω) = ω_ss`
   for the configured viscous damping `c`.
3. **The raycast & sleep** — a stack of 10 boxes from Phase 3 left alone for
   2 seconds; the test asserts every body is `Asleep` after 2 s (the island
   went idle and slept). A raycast then fires at the top box; the hit must be
   reported, the island must wake within the next sub-step, and the stack must
   still be a settled stack 1 s later (sleep+wake did not destabilize it). A
   shapecast (sphere of radius 0.1 m) along the same ray must report the same
   hit. The raycast hit's `fraction` and `position` are derived from the
   analytic ray/AABB → ray/shape geometry — the same oracle pattern Phases 2/3
   use.
4. **The bullet** — a small high-velocity sphere (`v = 200 m/s`, radius 5 cm)
   fired at a static plane 1 m thick. *Without* CCD (the discrete path) the
   sphere is allowed to pass through and the test asserts it; *with* CCD opt-in
   (`AQCCDMode::Speculative`) the test asserts the sphere stops at the plane
   surface with penetration `< 1 cm`; *with* `AQCCDMode::Continuous` it stops
   with penetration `< 1 mm`. The three runs share a single test scaffold so
   the regression "CCD off still tunnels, speculative catches the common
   case, continuous catches all of it" is asserted directly.

These run alongside `aqua_broadphase_test`, `aqua_rigid_body_test`, and
`aqua_contact_test`. Like Phase 3's settling-stack oracle (per-contact analytic
normal impulse) and Phase 2's brute-force pair set, **each deliverable is built
around a slow, obviously-correct reference** the fast path must match: the
bridge's analytic catenary tension, the motor's `τ = c·ω` steady-state, the
ray/shape analytic hit, and (for CCD) the swept-shape closed-form time of impact
on a half-space.

**Included groundwork (lands first in Phase 4).** Phase 3 deferred three hooks
to here; we close them before joints land:
- **The `AQConstraintRow` schema reserved kinds for joints.** Phase 3 shipped
  `AQConstraintKind::ContactNormal` / `ContactFriction` and explicitly noted
  "Phase 4 joints will add their own kinds (revolute axis row, limit row, motor
  row) with their own bound rules — the SoA layout stays the same"
  (`AQContact.h:62-66`). Phase 4 extends the enum and ships the row-build
  paths; no row format churn.
- **The PGS sweep is already row-agnostic.** Phase 3's iteration loop reads
  `bodyA`/`bodyB`, `direction`, `effectiveMass`, `bias`, `lowerBound`/
  `upperBound`, and the friction `peerRow` cone-clip. Joints land as: more
  rows in the same buffer with kind-specific row construction and per-kind
  bound rules. The inner loop does not need to know what a joint is.
- **The persistence cache shape extends to joints unchanged.** Joints carry
  warm-started impulses across frames exactly the way contacts do; the cache
  key swaps `(sortedPairIndex, featureKey)` for `(jointIndex, rowIndex)` but
  the storage is the same `float[3]`-ish accumulator-per-row pattern, on the
  same SoA path. The Phase 5 GPU port (sorted-array cache) carries over too.

**What Phase 3 has already closed for us (audited 2026-06-06).** Phase 3 shipped
the substrate this phase needs; the work below now consumes rather than defines
them. **Do not re-add these; they are already in `include/aqua/` and `src/`:**
- **PGS sweep + split-impulse position correction — DONE.** `AQSpace::Impl`
  holds the row buffer and the velocity/position iteration counts
  (`AQSpace.h:84-90`). Joints contribute rows; the sweep iterates them.
- **Material combine + per-body `restitution`/`friction` — DONE.** Joints
  don't use these directly (no friction on a hinge axis), but joint *limit*
  rows reuse the `restitution`-bias pattern from contact normals for clean
  bouncing off the limit (§6 below).
- **Drainable debug bus + per-flag bits — DONE.** Phase 4 *extends*
  `AQDebugFlags` with `AQDebugJointAnchor`, `AQDebugJointAxis`,
  `AQDebugIsland`, `AQDebugRaycastHit`, `AQDebugSleepingBody`, and appends
  into the existing `AQSpace::drainDebugLines` buffer.
- **Contact manifold view (`contactManifolds()`) — DONE.** Phase 4 mirrors
  it with `joints()` / `islands()` / `triggerEvents()` read-only views, all
  refreshed once per `advance` exactly the same way.
- **Body indices are stable for the body's lifetime in the space — DONE.**
  Joints, islands, and the persistence cache key on body index. The index
  stability contract Phase 2/3 rely on (`AQSpace::candidatePairs()` returns
  stable indices into the body SoA) carries over.

The phase's remaining work is therefore: the joint POD + handle + space-owned
table; joint-row construction for the five joint types + limits + motors;
union-find island detection over the (contact ∪ joint) adjacency graph;
per-island sleep/wake state; the kinematic body type; trigger volumes and
their event queue; the broadphase-driven raycast/shapecast/overlap API; and
opt-in CCD (speculative + conservative-advancement TOI). The contact solver,
material combine, and broadphase pipeline are already shipped.

**Out of scope here, by design:** Featherstone / articulated multibody systems
(deferred minor — the row layout below admits an articulation solver as a
parallel path, but full articulations are their own subsystem); soft-constraint
*driven* parameter authoring tools (compliance is a numeric input, not a UI);
the OmegaSL joint kernel (Phase 5); particle/soft joints (Phase 7+); and any
unified-XPBD recast of joints (Phase 7 — the architecture decision in roadmap
§7.2). We design joint data + the row schema so the OmegaSL port is a layout
port, not a rewrite, and so an XPBD recast would reuse the bookkeeping.

---

## 2. Why "finish the Newtonian pillar" is four problems, not one

Joints, queries, sleeping, and CCD look like loosely-related polish items
sharing only the contact solver. They are not — they share four failure modes
that each have to be designed against at once.

1. **A joint is a *bilateral* constraint with no penetration floor.** A
   contact's normal row is one-sided: `λ_n ≥ 0`, the solver never pulls bodies
   together. A joint's row is two-sided: `λ` can be of either sign, because the
   joint pulls *or* pushes to maintain the constraint. A naive port of the
   Phase 3 normal-row code that forgot to widen `lowerBound`/`upperBound` would
   silently produce a one-sided joint that breaks the moment the constraint
   error reverses — and the bug would not show up on a hanging-from-anchor
   test because gravity makes the sign monotonic. Test scenes that *swing*
   the joint are mandatory for catching this.
2. **Sleeping injects a *non-physical* discontinuity into a deterministic
   solver.** A sleeping body has `v ≡ 0, ω ≡ 0` by fiat; it skips the
   velocity sweep and the position update. When two islands touch — one awake,
   one asleep — the awake island sees a body whose velocity is suddenly *not*
   the result of integration. If the wake-up criterion is sloppy (e.g. "wake
   when contact appears" without "wake the *whole* island"), a single sleeping
   body in a connected chain can absorb impulses through the joints, store
   nothing in its velocity, and the chain looks like it's dragging a hidden
   anchor. Sleeping has to be **island-scoped** (decided once for the whole
   connected component) and **deterministic** (the idle-counter increments at
   the same point in the step every time), or it stops being an optimization
   and becomes a bug factory.
3. **Queries are a different access pattern over the same data.** The Phase 2
   broadphase is a streaming pipeline: hash → sort → run-scan, all one-pass.
   A raycast wants to *walk* the grid along the ray, visiting cells in order
   and short-circuiting on the first hit; a shapecast wants a swept AABB
   against the grid; an overlap test wants an AABB query against the grid. All
   three are *random-access reads* of the same per-step structure. The Phase 2
   sort-based grid was chosen partly so this access pattern is natural (the
   sorted cell-hash array indexes directly), and Phase 4 has to make good on
   it: queries reuse the broadphase scratch, they do not rebuild it.
4. **CCD is a *re-integration* problem, not a contact problem.** A bullet
   that wants to stop at a plane has to *not advance* past the plane, which
   means the time of impact has to be computed and the integration sub-step
   has to be split there. The contact solver runs on the *post-step* state;
   if the body has already tunneled, no amount of contact response will pull
   it back to the correct surface. CCD has to live *before* the contact step
   (speculative: extend the manifold across the swept volume) or *inside* the
   step (continuous: compute TOI, advance to it, re-step the remainder). Both
   are heavy and both have to be opt-in per body, because making them
   unconditional regresses the Phase 3 settling-stack performance.

A correct Phase 4 handles all four at once, on a path that will later run as a
GPU constraint-solver kernel over SoA buffers — the Phase 5 port.

---

## 3. Prior art — how the incumbents solve it

Studied to understand the terrain and the failure modes, **not** to transcribe.
Descriptions are drawn from published talks, docs, and source structure and are
representative, not a claim to quote current internals.

**NVIDIA PhysX 5.**
- **Joints** are typed C++ classes (`PxRevoluteJoint`, `PxSphericalJoint`,
  `PxPrismaticJoint`, `PxDistanceJoint`, `PxFixedJoint`, `PxD6Joint`) that
  build constraint rows fed to the same iterative solver as contacts.
  Soft-constraint parameters live on each joint: spring stiffness, damping,
  and the **CFM** (Constraint Force Mixing) / **ERP** (Error Reduction
  Parameter) dialect Open Dynamics Engine made canonical, translated into the
  Catto-style `bias` + `compliance` form for the modern PGS/TGS solver.
  Limits and motors are bounded extra rows on the same joint.
- **D6 joint as the universal joint.** PhysX exposes a single 6-DOF joint
  whose per-axis state can be Locked, Limited, or Free, with optional spring
  and motor — superset of all the other types, but heavier. Specialized
  hinge/ball/slider/fixed are present because they are cheaper and the API is
  more legible.
- **Articulations** (`PxArticulationReducedCoordinate`) are a separate solver
  (Featherstone-based reduced coordinate) for chained bodies that need
  maximum stability — a parallel path to the maximal-coordinate joint
  solver. We are *not* shipping articulations in Phase 4 (deferred minor),
  but the joint surface here is designed so an articulation solver can read
  the same joint descriptors later without churn.
- **Queries** ride a separate scene-query (SQ) acceleration structure (often
  a BVH), built and refit out of band from the simulation broadphase. PhysX
  chose to *duplicate* the broadphase work because their SQ workload (many
  parallel reads per step, different shape vocabulary including arbitrary
  raycasts) is poor for SAP and they don't want to constrain the simulation
  broadphase to BVH for query performance.
- **Sleeping** is per-island, with a wake-on-contact + wake-on-force +
  wake-on-touched-by-awake-body discipline. Idle threshold is a velocity
  energy `‖v‖² + (Iω·ω)`; an island sleeps when *every* member has been
  below threshold for some number of sub-steps.
- **CCD** is opt-in per body. PhysX runs **swept-shape vs. swept-shape** TOI
  with conservative advancement (Mirtich) for the opt-in pairs, plus
  **speculative contacts** as a cheap default for "fast enough to risk
  tunneling but not enough to enable full TOI." Both paths re-feed contact
  rows into the same solver.

**Epic Chaos (Unreal Engine 5).**
- **Joints** as a single configurable `FPBDJointConstraints` system —
  generalizes the D6 idea: each joint declares which axes are constrained and
  what type of constraint (lock / limit / free) on each. Built on Catto-style
  PGS with per-row soft compliance.
- **Queries** use the same `FAABBTree` (BVH) the simulation broadphase uses
  in Chaos's grid+BVH hybrid, populated with the *visual* swept-shape
  representation.
- **Sleeping** per-island with conservative wake; **CCD** as a sweep+rewind.
- Articulations are not as central as in PhysX 5; chains tend to be built out
  of well-tuned soft joints + sub-stepping rather than the reduced-coordinate
  path.

**Box2D / Bullet heritage.**
- The line all three engines trace back to: **Erin Catto's soft constraint
  paper** (GDC 2011) on adding spring/damper to the PGS row schema via the
  `softness = damping_ratio` + `frequency = ω_n` parameterization, which
  reduces to `compliance` (CFM) and `bias` (ERP/dt) under the hood.
- Bullet's `btTypedConstraint` hierarchy is the cleanest open implementation
  of the typed-joint approach; reading it is the cheapest way to understand
  the row construction before facing PhysX/Chaos's much larger surface area.

**The shared shape:** all three engines (i) implement joints as N constraint
rows fed to the *same* iterative solver as contacts, (ii) parameterize soft
constraints with stiffness + damping that translate to CFM + ERP / bias +
compliance, (iii) treat sleeping as per-island state with conservative wake,
(iv) ship queries against a structure that may or may not be the simulation
broadphase, and (v) gate CCD behind a per-body opt-in with speculative as the
cheap default. **All three converged on the same approximate shape**, which is
the strongest signal in the prior-art literature: this is the path. The bar to
clear is not "find a different shape." It is "make the same shape work on our
substrate, in our determinism stance, on the GPU later."

---

## 4. The literature we build on

The research again leads the shipped engines by years. The pieces we combine:

- **Catto, "Soft Constraints" (GDC 2011).** The reference for adding stiffness +
  damping to a velocity-level constraint row. Two parameters (`frequency
  ω_n`, `damping ratio ζ`) map to **compliance** `α` and **bias** `β` such
  that an unmodified PGS iteration produces critically-damped or
  spring-driven joint behaviour without changing the solver shape:
  `Keff' = Keff + α/dt²`, `bias_λ = β·error/dt`. The row schema absorbs both
  fields uniformly; the inner loop changes by one add. This is the citation
  for §6's joint soft-constraint pick.
- **Catto, "Modeling and Solving Constraints" (GDC 2009)** — already cited
  in Phase 3 for split-impulse, also cited here for the Jacobian-row form of
  the five canonical joint types (distance, ball-socket, hinge, slider,
  fixed) and the standard limit-row construction (one-sided bounded row at
  the violated end).
- **Featherstone, "Rigid Body Dynamics Algorithms" (2008) — referenced, not
  adopted in Phase 4.** The reduced-coordinate articulation solver PhysX
  ships separately. We carry the reference because if/when we ship
  articulations as a later minor, the joint descriptors here are exactly the
  topology input that solver consumes — no descriptor churn.
- **Mirtich, "V-Clip: Fast and Robust Polyhedral Collision Detection" (1997)
  and Hubbard, "Approximating Polyhedra with Spheres for Time-Critical
  Collision Detection" (1996).** Time-of-impact computation as a swept
  closest-point problem; the conservative-advancement algorithm (advance by
  `distance / max_relative_velocity`, repeat) gives a TOI that is always a
  lower bound on the true impact time — exactly the guarantee CCD needs.
  This is what `AQCCDMode::Continuous` implements over the Phase 2 support
  functions.
- **Bridson, Fedkiw, Anderson — "Robust Treatment of Collisions, Contact and
  Friction for Cloth Animation" (SIGGRAPH 2002).** The **speculative contact**
  idea (extend the contact manifold across the swept AABB and let the
  solver's PGS clamp resolve it pre-impact) was first formalized here for
  cloth and adopted by Bullet / Box2D / PhysX as the cheap CCD default. We
  adopt the rigid-body specialization.
- **Tarjan, "Depth-First Search and Linear Graph Algorithms" (1972) /
  union-find (Hopcroft, Tarjan).** The graph-theory underpinning of
  **islands**: a body's "island" is its connected component in the graph
  whose nodes are dynamic bodies and whose edges are active contacts +
  joints. Union-find computes the components in near-linear time; the per-
  island sleep decision is then "AND of per-body idle flags within the
  component." Standard, deterministic, and parallel-friendly (parallel union-
  find for the Phase 5 GPU port — Patwary et al. PPoPP 2012).
- **Ericson, "Real-Time Collision Detection" (2004)** — already cited in
  Phase 2 for AABB math, also cited here for the **ray vs. analytic shape**
  closed forms (ray/sphere, ray/box, ray/capsule, ray/plane) and the
  **swept-shape vs. shape** closed forms (sphere sweep vs. plane / sphere /
  AABB) that the speculative-contact path uses. GJK-Ray as the general
  fallback (Cameron 1997) for convex hulls.

The throughline: the incumbents' joint + sleep + CCD + query line is mature
and battle-tested, but the *newer* soft-constraint (Catto 2011) and
speculative-contact (Bridson 2002, generalized) refinements are AQUA-
substrate-friendly in a way the incumbents' CPU/SIMD heritage and their long
backward-compatibility surface constrains. That is the opening.

---

## 5. Where AQUA diverges — the openings

Grounded in the actual post-Phase-3 surface (`AQCollision.h`, `AQContact.h`,
`AQMath.h`, `AQIntegrator.h`, `AQRigidBody.h`, `AQSpace.h`):

- **Joints land as more rows in the same `AQConstraintRow` buffer — no parallel
  solver.** PhysX maintains parallel "contact" and "joint" constraint streams
  largely because their CPU SIMD paths were tuned separately; AQUA's Phase 3
  schema (`AQContact.h:64-89`) is *already* row-agnostic. The Phase 4 joint
  build appends rows of new `AQConstraintKind`s (`JointAxis`, `JointAngular`,
  `JointLimit`, `JointMotor`) to the same buffer, each with its own
  `lowerBound`/`upperBound` rule. The PGS inner loop is unchanged; only the
  row-build paths grow. This is the "row is the unit of layout" promise from
  Phase 3 §5 cashed out — the Phase 5 GPU kernel is unchanged.
- **Five specialized joint types, not a D6.** PhysX's `PxD6Joint` is a
  superset and a maintenance win; it is also heavier per-row and harder to
  reason about for users. AQUA ships specialized **Distance** (1 row), **Ball-
  socket** (3 rows), **Hinge** (5 rows = 3 ball-socket + 2 axis), **Slider**
  (5 rows = 3 + 2), and **Fixed** (6 rows = 3 + 3), each with optional limits
  and motors as additional bounded rows. The D6 generalization is a §11.1
  open decision — we *can* add it later as a wrapper that builds the same
  rows; we are not ruling it out, we are not leading with it.
- **Soft constraints via Catto 2011's `(ω_n, ζ)` → `(compliance, bias)`
  parameterization, on the existing row.** The `AQConstraintRow` already
  carries `bias` (Phase 3 §7); Phase 4 adds **one field**: `compliance`. The
  PGS effective-mass update becomes `Keff' = Keff + compliance/dt²`, the bias
  becomes `β·error/dt`. Distance/ball-socket/hinge default to *hard*
  (`compliance = 0` ⇒ unchanged from contact-row math); user-facing
  `setSoftness(frequency, damping)` translates to those numbers behind the
  pimpl. This is **deliberately compatible** with a Phase 7 XPBD recast — XPBD
  *is* the compliance form, expressed in position-projection units; the
  algebra carries over.
- **Queries reuse the Phase 2 sort-based grid — no parallel BVH.** PhysX
  duplicates the broadphase for queries because their SAP doesn't support
  efficient ray walks; AQUA's sort-based uniform grid (Phase 2 §6.B) gives a
  **3D-DDA ray walk** for free — cells along the ray are visited in order via
  Amanatides & Woo (1987), and the candidate body list per cell is already
  sorted. Raycast / shapecast / overlap all become walks of the same
  per-step grid scratch. The Phase 5 GPU port reuses the same scratch buffers.
  This is the "the grid is what Phase 6 wants anyway" Phase 2 §5 promise
  paying off a second time.
- **Islands as deterministic union-find over the sorted constraint graph.**
  The contact + joint edges arrive in a deterministic order (Phase 3 §5/§8;
  contact pairs sorted by `(a, b)`, joints in user-creation order with the
  same `a < b` invariant). A single linear pass of union-find produces the
  islands in the same order across runs. Per-island sleep state is then a
  parallel reduction (AND of per-body idle flags), which the Phase 5 GPU port
  implements directly. No hash tables, no per-frame reordering.
- **Sleep state is a body attribute, not a side table.** A body has
  `AQActivationState ∈ {Active, Sleeping}`. The integrator (`AQIntegrator.h`
  §6, the existing `AQStepBodyVelocity` / `AQStepBodyPosition`) reads it and
  fast-paths sleeping bodies (no velocity update, no position update, no
  damping, no integration) — the existing static-body fast path is the
  template (`AQStepBodyVelocity: if (b.invMass == Ty(0)) return;`). One new
  early-return, no per-step iteration over a separate sleep list. The Phase 5
  GPU kernel inherits the same branch.
- **CCD is opt-in per body via `AQCCDMode`, with speculative as the cheap
  default and conservative-advancement TOI as the heavy option.** PhysX
  exposes both; we ship both with the same names and the same semantics so
  the comparison is honest. The speculative path **extends the body's fat
  AABB by `v·dt`** before the broadphase (Phase 2 already fattens by a
  velocity-proportional amount — Phase 2 §11.4 — so this is a knob, not new
  machinery), then narrowphase generates contacts against the swept volume's
  *touched* shapes (one extra GJK call per swept pair). The TOI path is
  conservative-advancement (Mirtich 1997) over the Phase 2 `AQshapeSupport`
  functions — no new shape vocabulary. Both feed the same solver.
- **Determinism stays a body-index sort.** Joint impulses persist across
  frames keyed by `(jointIndex, rowIndex)`; sleep state by body index; query
  results are reported in a fixed order (raycast: by fraction along the ray;
  overlap: by body index ascending). The Phase 5 GPU port replicates each
  with a stable sort over the same buffers — same recipe as Phase 3.

**Gaps we must fill (this is Phase 4's work):** there is no joint type, no
joint descriptor, no joint row construction, no island detection, no sleep
state, no kinematic body type, no trigger volume, no query API, no CCD. None
of it depends on a third-party physics SDK; it is rigid-body completion code
we own and build on the Phase 3 PGS solver and the Phase 2 broadphase grid.

---

## 6. Proposed algorithm — typed joints, union-find islands, grid-walk queries, opt-in CCD

The synthesis, per sub-step, in the order it runs:

**A. World-AABB refresh + broadphase (Phase 2, with the CCD fattening hook).**
For bodies with `ccdMode == Speculative`, the broadphase fattening margin
becomes `max(staticMargin, ‖v‖·dt + ‖ω × r_max‖·dt)` so the swept volume is
captured in the candidate list. `ccdMode == Off` (the default) uses the Phase 2
margin unchanged. `ccdMode == Continuous` uses the Phase 2 margin AND records
the body for the TOI substep pass below.

**B. Narrowphase — Phase 3 branch table (specialized + GJK/EPA) on the
candidate pairs.** Unchanged.

**C. Joint-row build (NEW).** For each joint:
```
for each joint J in joints:
    if eitherBody(J).activation == Sleeping and otherBody(J).activation == Sleeping:
        continue                                   // both sleeping, skip
    rows = J.buildRows(bodyA, bodyB, dt)           // §7 per-type build
    for r in rows: r.accumImpulse = cache[(J.index, r.localIndex)] or 0   // warm-start
    constraintRows.append_all(rows)
```
Per-joint row counts:
- **Distance**: 1 row along `(posB − posA)` direction, target length `L`.
  `lowerBound = −∞, upperBound = +∞` (bilateral); `bias = β·(currentLen − L)/dt`.
- **Ball-socket** (`spherical`): 3 rows on world axes (X, Y, Z),
  constraining `posA + R_A·anchorA = posB + R_B·anchorB`. Bilateral.
- **Hinge** (`revolute`): 3 ball-socket rows + 2 axis rows (cross-axes of the
  hinge orthogonal to the rotation axis), constraining `R_A·axis = R_B·axis`.
  Bilateral.
- **Slider** (`prismatic`): 2 perpendicular ball-socket rows (no row along the
  slide axis) + 3 angular rows (orientation locked). Bilateral.
- **Fixed**: 3 ball-socket rows + 3 angular rows. Bilateral.

Plus optional **limit rows** (one-sided bounded — `lowerBound = 0` for "must
not go further negative") and **motor rows** (target velocity, bounded by
max impulse / max force — `λ_max = F_max · dt`). Limits and motors share the
existing row format; only the bound rule differs.

**D. Soft-constraint compliance application.** For each row with `compliance > 0`:
```
r.effectiveMass = 1 / (Keff_raw + r.compliance / (dt * dt))
r.bias = beta * positionError / dt              // β from (ω_n, ζ) per Catto 2011
```
For hard joints (`compliance = 0`, the default), this collapses to the Phase 3
formula — no branch in the inner loop.

**E. Contact-row build.** Phase 3's narrowphase manifold → constraint rows.
Unchanged.

**F. Island detection — union-find over the (contact ∪ joint) adjacency graph.**
```
forEach body b: parent[b] = b
forEach row r in constraintRows:
    if !static(r.bodyA) && !static(r.bodyB):
        union(r.bodyA, r.bodyB)
forEach body b: islandOf[b] = find(b)            // canonical root
```
A single pass; deterministic because the row order is deterministic (Phase 3
§5/§8). Static bodies are *not* unioned (they would merge every island
that touches the floor — the standard gotcha).

**G. Per-island sleep decision.**
```
for each island I:
    allIdle = AND over members b in I of (‖v_b‖² + ‖ω_b‖²·avgInertia < idleThreshold)
    if allIdle:
        I.idleFrames += 1
        if I.idleFrames >= sleepFrameCount:        // default 60 frames @ 1/120 = 0.5 s
            for b in I: b.activation = Sleeping
    else:
        I.idleFrames = 0
        for b in I: b.activation = Active           // wake the whole island
```
The idleness predicate is energy-flavored (mass-weighted), not velocity-only,
so a heavy slow body and a light fast body don't cross the threshold for
different reasons. The wake-on-any-active-member rule is what stops the
chain-with-one-sleeping-body bug from §2.

**H. PGS sweep (Phase 3, with the sleep skip).**
```
for iter in 0..N_vel:
    for each row r in constraintRows:
        bA, bB = bodies[r.bodyA], bodies[r.bodyB]
        if bA.activation == Sleeping and bB.activation == Sleeping:
            continue                              // whole row inert
        # Standard Phase 3 PGS iteration
        ...
```
The row visit order is unchanged. Joint rows iterate alongside contact rows;
the only per-row change vs. Phase 3 is the `effectiveMass` formula picking up
`compliance` (one extra add for hard rows, computed once per sub-step).

**I. Split-impulse position pass (Phase 3, unchanged).** Contact rows
participate; joint rows skip (position correction for bilateral joints is
already handled by the velocity solve plus the `bias = β·error/dt` term — the
Catto 2009 split-impulse argument is specific to one-sided contact rows).

**J. Integrate.** Sleeping bodies skip cleanly (existing static-body fast path
in `AQStepBodyVelocity`/`AQStepBodyPosition` extends one more case). Active
bodies advance as Phase 3.

**K. CCD TOI substep (NEW, opt-in only).** For each body `b` with
`ccdMode == Continuous` and `‖v‖·dt > b.aabbExtent`:
```
toi = 1.0
for each shape S overlapping b's swept AABB this frame:
    t = conservativeAdvancement(b.shape at t=0, S, v_b, ω_b, dt)
    toi = min(toi, t)
if toi < 1.0:
    advance b to toi · dt
    add a contact at the impact point (full PGS row)
    re-run the solver on this row only (single PGS sweep)
    advance b for the remaining (1 - toi) · dt
```
This is **per-body, not per-pair**, and runs *after* the regular sub-step's
position pass. The repeated PGS-over-one-row is cheap because there's only
one row; the cost knob is the conservative-advancement iteration count
(default 8, capped — Mirtich 1997 §4). Bodies with `ccdMode == Speculative`
skip this step entirely — their CCD was paid for in step A's fattening.

**L. Queries (called by the user between `advance` calls, not in the step
loop).** Raycast / shapecast / overlap walk the Phase 2 per-step grid scratch
(stable until the next `advance`):
```
raycast(origin, dir, maxT, filter, hits):
    for each cell c visited by 3D-DDA(origin, dir, maxT):
        for each body b in cell c:
            if !passFilter(filter, b.filter): continue
            if !rayAABB(origin, dir, b.fatAABB): continue
            t = rayShape(origin, dir, b.shape, b.transform)   // §7 analytic
            if t < maxT: hits.append({b.index, t, origin + t·dir, normal})
    sort hits by t                                  // deterministic order
```
Shapecast is a swept-shape vs. swept-shape variant of the same walk;
overlap is the AABB-only short form (no shape narrowphase needed unless
`exactShapes = true`).

**M. Trigger events.** Bodies tagged with `AQBodyDesc::isTrigger = true`
contribute candidate pairs as normal but the narrowphase short-circuits
after the AABB test (no constraint rows). A pair `(trigger, other)` that
overlapped last frame but not this one emits an `Exit` event; new overlaps
emit `Enter`; persistent ones emit `Stay`. The event queue drains via
`AQSpace::triggerEvents()` and clears each `advance`.

**N. Persistence handoff.** Joint impulses go back to the cache keyed by
`(jointIndex, rowLocalIndex)`. Island and sleep state are body-attributes
already persistent. Trigger events are consumed-and-cleared.

Why this combination:
- **Joints-as-rows** is the unified solver Phase 3's row schema was designed
  for. Zero change to the PGS inner loop is the strongest sign the schema
  generalizes correctly.
- **Five specialized types** matches the kREATE Phase 8 surface (ragdoll =
  ball-socket + hinge + limits; bridge = distance; door = hinge + limits;
  vehicle suspension = slider + spring) without forcing a D6 dialect on
  every gameplay author.
- **Catto 2011 soft constraints** drop into one row field. The
  `(frequency, damping)` UI translates internally; users author behaviour,
  not numerics. The XPBD compatibility (Phase 7) falls out because XPBD's
  compliance is the same field.
- **Sort-based grid for queries** is the structural payoff Phase 2 promised.
  No parallel BVH, no separate broadphase to maintain, the Phase 5 port
  reuses the same scratch.
- **Union-find islands** are the deterministic, parallel-friendly choice for
  a problem the literature considers settled. The Patwary et al. parallel
  variant is the Phase 5 port.
- **Sleep-as-attribute** is the cheapest implementation; the
  static-body fast path is already there and tested.
- **Speculative + TOI CCD** matches PhysX's two-tier opt-in and gives users
  the cost/quality dial they actually want. Speculative is the default
  because the failure mode (an opted-in fast body that *barely* misses TOI
  but is handled by the fat AABB) is the common case.

**Alternative considered — D6 joint as the primary type.** Less code,
single-type API; loses the per-type optimization (a hinge needs 5 rows; a D6
configured as a hinge builds 5 rows with extra branching) and is harder to
debug ("which axis is misconfigured?"). Kept as a §11.1 follow-up — the row
build paths factor cleanly enough that a D6 wrapper is a Phase 4.x add.

**Alternative considered — reduced-coordinate articulations
(Featherstone) as the primary chain solver.** Better stability for high-mass-
ratio chains (a hand with light fingers off a heavy arm); much more code, a
different topology surface, and out of step with the maximal-coordinate row
solver the rest of Phase 3/4 is built on. Documented as a deferred minor
(roadmap follow-up) because the joint descriptors here are the input that
solver would need anyway — no waste motion.

**Alternative considered — BVH for queries.** What PhysX duplicates for. We
don't, because the Phase 2 grid already supports ray walks well and the
parallel-broadphase-for-queries cost is real. Revisit if profiles show
queries dominating; not the default.

---

## 7. New types AQUA must add — `include/aqua/AQJoint.h`, `AQQuery.h` (drafts)

AQUA-owned, AQ-prefixed, no namespace (per `AGENTS.md`). All public surface
types here are trivially-copyable / standard-layout so they upload to a GPU
buffer with no repacking — the Phase 5 PGS solver kernel reads
`AQConstraintRow` records (already shipped, just more kinds) as raw arrays,
and the joint descriptor table parallels the Phase 2 shape table.

**`include/aqua/AQJoint.h` (new):**
```cpp
#ifndef AQUA_AQJOINT_H
#define AQUA_AQJOINT_H

#include "AQBase.h"
#include "AQContact.h"        // AQConstraintRow / Kind
#include <omegaGTE/GTEMath.h>
#include <cstdint>

/// Joint type discriminator. Specialized types ship in Phase 4; a D6
/// superset is a §11.1 follow-up that builds the same rows.
enum class AQJointType : std::uint32_t {
    Distance,    ///< 1 row — keep ‖anchorA_world − anchorB_world‖ at L
    BallSocket,  ///< 3 rows — coincident world-frame anchors
    Hinge,       ///< 5 rows — BallSocket + 2 axis rows (revolute)
    Slider,      ///< 5 rows — 2 perp-axis rows + 3 angular rows (prismatic)
    Fixed,       ///< 6 rows — 3 ball-socket + 3 angular
};

/// CCD opt-in per body.
enum class AQCCDMode : std::uint8_t {
    Off,         ///< (default) Discrete; the body can tunnel through thin shapes
    Speculative, ///< Fatten the broadphase AABB by ‖v‖·dt; cheap
    Continuous,  ///< Conservative-advancement TOI; expensive, exact for the swept volume
};

/// New constraint-row kinds Phase 4 adds. (`ContactNormal`/`ContactFriction`
/// already exist in AQContact.h.) `JointAxis` is a bilateral row on a linear
/// or angular axis (lowerBound = -inf, upperBound = +inf, default).
/// `JointLimit` is the one-sided row at a violated limit. `JointMotor` is the
/// bounded target-velocity row.
/// (Extension: declared in AQContact.h's existing enum, listed here for
/// visibility — the enum stays defined in AQContact.h.)
//
// enum class AQConstraintKind : uint32_t {
//     ContactNormal,
//     ContactFriction,
//     JointAxis,      // new (Phase 4)
//     JointLimit,     // new (Phase 4)
//     JointMotor,     // new (Phase 4)
// };

/// Per-row soft-constraint parameters (Catto 2011). `frequency` is the
/// angular natural frequency in rad/s; `damping` is the damping ratio
/// (1.0 ⇒ critically damped). Defaults (`0, 0`) produce a hard constraint
/// — the row reduces to the Phase 3 formula byte-for-byte. The translation
/// to `(compliance, bias)` lives in the joint build path.
struct AQJointSoftness {
    float frequency = 0.f;
    float damping   = 0.f;
};

/// Optional per-axis limit. `enabled = false` means the joint axis is free;
/// when enabled, the axis is bounded to [min, max] (angle in radians for
/// rotational axes, distance for translational). A motor on the same axis
/// drives toward `motorTargetVelocity` clamped by `motorMaxImpulse`.
struct AQJointAxisLimit {
    bool  enabled            = false;
    float min                = 0.f;
    float max                = 0.f;
    bool  motorEnabled       = false;
    float motorTargetVelocity = 0.f;
    float motorMaxImpulse    = 0.f;
};

/// Parameters for creating an AQJoint. Anchors are in each body's LOCAL
/// frame; the joint build path transforms them to world per sub-step. The
/// axis (for Hinge and Slider) is in body A's local frame; the build path
/// rotates it to world. Per-type fields live in the union — POD by
/// construction so the descriptor uploads as a row of a typed table.
struct AQUA_EXPORT AQJointDesc {
    AQJointType       type = AQJointType::BallSocket;
    std::uint32_t     bodyA = 0;          ///< body index (or use the handle ctor)
    std::uint32_t     bodyB = 0;
    OmegaGTE::FVec<3> anchorA = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> anchorB = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> axisLocalA = OmegaGTE::FVec<3>::Create();  ///< Hinge/Slider axis (body-A local)
    float             distanceTarget = 0.f;  ///< Distance joint resting length
    AQJointSoftness   softness;              ///< Hard by default
    AQJointAxisLimit  limit;                 ///< Hinge: angular; Slider: linear; others: ignored
};

/// Opaque joint handle returned by `AQSpace::createXJoint`. Same shape as
/// AQShapeHandle / body index — a small value, backend-free.
struct AQJointHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;
    AQUA_NODISCARD bool valid() const { return generation != 0; }
};

/// Activation state. The integrator and PGS sweep fast-path `Sleeping`.
/// `Kinematic` skips the velocity update (the user owns velocity) but still
/// runs the position update and participates in collision response (one-way:
/// pushes others, isn't pushed). Same enum so the inner loop branches once.
enum class AQActivationState : std::uint8_t {
    Active,
    Sleeping,
    Kinematic,
};

#endif // AQUA_AQJOINT_H
```

**`include/aqua/AQQuery.h` (new):**
```cpp
#ifndef AQUA_AQQUERY_H
#define AQUA_AQQUERY_H

#include "AQBase.h"
#include "AQCollision.h"
#include <omegaGTE/GTEMath.h>
#include <cstdint>

/// A single raycast or shapecast hit, returned in fraction-sorted order
/// (smallest `fraction` first). Stable across runs because the sort is on
/// `fraction` and the body-index tiebreak is deterministic.
struct AQRaycastHit {
    std::uint32_t     bodyIndex = 0;
    float             fraction  = 0.f;     ///< t in [0, maxT] along the ray/sweep
    OmegaGTE::FVec<3> position  = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3> normal    = OmegaGTE::FVec<3>::Create();
};

/// Filter for queries — same shape as `AQCollisionFilter` so a body's
/// filter naturally pairs with a query's filter via the same rule.
struct AQQueryFilter {
    std::uint32_t layer = 1u;
    std::uint32_t mask  = ~0u;
};

/// Trigger event. `kind = Enter` the frame an overlap starts, `Stay` while
/// it persists, `Exit` the frame it ends. Drained via
/// `AQSpace::triggerEvents()` and cleared each `advance`. Body indices in
/// `(a, b)` with `a < b` per the deterministic ordering.
enum class AQTriggerEventKind : std::uint8_t {
    Enter, Stay, Exit,
};
struct AQTriggerEvent {
    std::uint32_t      a = 0;
    std::uint32_t      b = 0;
    AQTriggerEventKind kind = AQTriggerEventKind::Enter;
};

#endif // AQUA_AQQUERY_H
```

The joint per-type row-build functions are free functions in
`src/AQJoint.cpp` (one builder per `AQJointType`); the CCD TOI iteration
lives in `src/AQCCD.cpp` and consumes `AQshapeSupport` (Phase 2 §7) for the
conservative-advancement step. Query walks live in `src/AQQuery.cpp`,
reading the per-step grid scratch directly from `AQSpace::Impl`. The
union-find island pass lives inline in `AQSpace::runNarrowphaseAndSolve`
(`AQSpace.h:120-124`) — between row-build and the PGS sweep.

---

## 8. Data layout & GPU/numeric specialization

Decided now so Phase 5 is a port, not a rewrite (ties to roadmap §5/§7.3 SoA
decision):

- **Joints as a SoA table, parallel to bodies and shapes.** `AQSpace::Impl`
  gains `joints[]` (an SoA `AQJointDesc`-shaped pack: `type[], bodyA[],
  bodyB[], anchorA[], anchorB[], axisLocalA[], distanceTarget[],
  softness[], limit[]`), keyed by handle (index + generation). One thread
  per joint in the row-build kernel; the row count is type-determined so the
  prefix-sum that allocates row slots is a known-shape parallel primitive.
- **Constraint rows stay the unified SoA buffer.** Phase 3 already mixes
  contact-normal and contact-friction rows; Phase 4 just adds more kinds.
  Row order: contacts first (sorted by `(a, b)`), then joints (sorted by
  joint index), then limits, then motors. The inner-loop sweep is unchanged;
  Phase 5 GPU coloring (the constraint-graph batching) reads `bodyA`/`bodyB`
  for each row regardless of kind, so the graph build is unchanged too.
- **Island state on the body SoA.** Two new body-side fields:
  `islandId : uint32_t` (the union-find root, fresh each sub-step) and
  `activation : uint8_t` (Active / Sleeping / Kinematic). The integrator's
  fast-path branch becomes a `switch` on `activation` — three cases, the
  Phase 5 GPU kernel branches the same way. Per-island sleep accumulator
  (`idleFrames : uint32_t`) lives in a small `islands[]` table built each
  sub-step from the union-find roots.
- **Per-step grid scratch promoted to a stable handoff for queries.** The
  Phase 2 grid scratch (`cellHash[]`, sorted `(hash, body)[]`,
  `cellStart[]`) is currently lifetime-bounded to the broadphase pass.
  Phase 4 promotes it to "valid until next `advance`," so queries between
  `advance` calls read it directly. No new data, just a lifetime contract
  change — the existing buffers are reused.
- **Trigger-event queue is a small SoA buffer.** `prevTriggerPairs[]` (from
  last frame, sorted) and `currTriggerPairs[]` (this frame); a single linear
  pass produces Enter / Stay / Exit events into `triggerEvents[]`. All three
  arrays are body-index SoA; the Phase 5 GPU port replaces the linear pass
  with a sorted-merge — same shape.
- **CCD scratch is a per-body sparse buffer.** Only bodies with
  `ccdMode == Continuous` and a non-trivial `‖v‖·dt` appear, so the
  conservative-advancement pass is over a *small* compacted array (one-pass
  filter). Doesn't bloat the regular Phase 3 hot loop.
- **Determinism:** body index → island root via union-find with path
  compression and union-by-rank is deterministic when the edge order is
  deterministic, which it is (Phase 3 §5/§8). Joint row impulses persist by
  `(jointIndex, localRowIndex)`. Query results are sorted by `(fraction,
  bodyIndex)` — fraction first, body-index tiebreak. The `double`-precision
  oracle for the joint and TOI solvers is the same code at `double`,
  mirroring the Phase 1/2/3 parity story.

---

## 9. Validation — how we measure "better"

The incumbent's *behavior* is the reference, not its code (roadmap §4).

- **The bridge (the joint headline).** 12 boxes + 11 ball-socket joints, two
  end-anchors pinned. Within 3 simulated seconds: every joint's anchor-pair
  distance `< 1 mm`; the bridge stays under that bound for another 5 s;
  the analytic catenary tension at the midspan (closed form from the chain
  weight) matches the solver's accumulated joint normal-impulse within 5%.
  This is the §1 deliverable, asserted in a test.
- **The hinge door (limits & motor).** Door panel, hinge with `[-90°, +90°]`
  limit and a 0.5 N·m motor. Door must not exceed the limit by `> 0.01 rad`
  across 30 s of stop-bouncing under gravity; the motor must reach
  `ω_ss = τ / c` (closed form for the viscous-damped angular motion) within
  1 s, within 5%.
- **Sleep on the settling stack.** Phase 3's settling stack must enter
  `Sleeping` within 2 s of settling (idleFrames hits the threshold); the
  PGS sweep's per-step row count drops to zero for that island; the body
  count stays put. A raycast hits the top box; the island wakes within
  one sub-step; the stack stays settled for 1 s thereafter.
- **Determinism with sleep.** A scene that sleeps under one trial produces a
  byte-identical sleep timeline (idleFrames per sub-step) on a re-run.
- **Raycast correctness (the query headline).** 1000 randomized rays vs.
  1000 randomized scenes. The grid-walk raycast result must agree with a
  brute-force all-bodies ray/shape test for every body returned, with the
  same `fraction` to within `1e-4` and the same `bodyIndex` ordering. The
  brute-force test is the same kind of slow oracle the Phase 1 `double`
  integrator and Phase 2 brute-force AABB pair set used.
- **Shapecast and overlap correctness.** Same regime: brute-force oracle on
  swept-shape vs. shape; result must agree.
- **Trigger event determinism.** A scene with a trigger pair entering,
  staying, and exiting must emit exactly one `Enter`, K `Stay`, and one
  `Exit` event over the K+2 frames they overlap; the event ordering across
  triggers is byte-identical on re-run.
- **CCD: the bullet.** Three sub-cases (Off / Speculative / Continuous)
  asserted directly per §1 deliverable #4. The Off case must tunnel
  (regression guard — confirms the discrete path is genuinely discrete);
  Speculative must stop the bullet to within 1 cm; Continuous to within
  1 mm. The analytic TOI for sphere-vs-plane is closed form; the test
  asserts the post-solve position against it.
- **Phase 3 regression.** Settling stack, incline friction, sphere bounce,
  GJK/EPA correctness, and energy non-growth all continue to pass — Phase 4
  must not shift Phase 3's promises. The PGS inner loop is unchanged; this
  is provable by inspection but also tested by re-running the Phase 3
  battery against the Phase 4 code.

Metrics emitted as debug-draw / logged series (roadmap §3 principle 6,
"author for the 3am engineer"): joint anchor positions (two spheres per
joint), joint axes (a colored segment for the hinge/slider axis), per-island
boxes (color-coded by sleep state), raycast hits (origin → hit point as a
segment), sleeping bodies (greyed-out body AABB), and a loud guard when an
island grows beyond N bodies (a sign the island merge is over-connecting —
typically a missed-static-body bug).

**The debug bus already exists.** Phase 3's drainable `AQDebugLine` stream
and per-flag-bit extension are in place; Phase 4 extends `AQDebugFlags`
with `AQDebugJointAnchor`, `AQDebugJointAxis`, `AQDebugIsland`,
`AQDebugRaycastHit`, `AQDebugSleepingBody`, `AQDebugCCDSweep` (one segment
per opted-in body showing the swept AABB extension), and appends into the
same buffer the kREATE adapter already drains. No new transport, no new
boundary, no new consumer contract.

---

## 10. Public API additions

Extends the existing surface across `include/aqua/AQRigidBody.h`,
`include/aqua/AQSpace.h`, and the new `include/aqua/AQJoint.h` /
`AQQuery.h`. New members marked `// new`; pre-existing-from-Phase-3
members that Phase 4 *uses* are marked `// Phase 3, present`. No OmegaSL
or backend types cross into `include/aqua/*`.

**`AQBodyType` (in `AQRigidBody.h`):**
```cpp
enum class AQBodyType {
    Static,     ///< Phase 1, present
    Dynamic,    ///< Phase 1, present
    Kinematic,  ///< new — user-driven pose, infinite mass, pushes others one-way
};
```

**`AQBodyDesc` (in `AQRigidBody.h`):**
```cpp
struct AQUA_EXPORT AQBodyDesc {
    // ... Phase 1 / 1.1 / 2 / 3 fields ...

    bool      isTrigger = false;                ///< new — narrowphase emits events, no rows
    AQCCDMode ccdMode   = AQCCDMode::Off;        ///< new — opt-in CCD (§11.5)

    // Sleep thresholds — per-body overrides; zero means "use space default."
    float     sleepLinearVelocity  = 0.f;        ///< new
    float     sleepAngularVelocity = 0.f;        ///< new
};
```

**`AQRigidBody` (in `AQRigidBody.h`):**
```cpp
class AQUA_EXPORT AQRigidBody {
public:
    // ... Phase 1 + 1.1 + 2 + 3 surface ...

    AQUA_NODISCARD AQActivationState activation() const;          // new
    void wakeUp();                                                 // new — force Active
    void putToSleep();                                             // new — force Sleeping
    AQUA_NODISCARD bool isTrigger() const;                         // new
    AQUA_NODISCARD AQCCDMode ccdMode() const;                      // new
    void setCCDMode(AQCCDMode m);                                  // new

    // Kinematic bodies: pose is set externally each frame; the velocity used
    // for collision response is derived from (newPose - prevPose) / dt.
    // setKinematicTarget is the kinematic-only setter; setPosition/
    // setOrientation still work but don't compute the implicit velocity.
    void setKinematicTarget(const OmegaGTE::FVec<3>     &p,
                            const OmegaGTE::FQuaternion &q);       // new
};
```

**`AQSpace` (in `AQSpace.h`):**
```cpp
class AQUA_EXPORT AQSpace {
public:
    // ... Phase 1 + 1.1 + 2 + 3 surface ...

    // --- joints (Phase 4) ---
    // Named-ctor idiom matching `createSphereShape` etc.; each returns a
    // handle into the space's joint table. Static-to-static joints return
    // an invalid handle (no effect on either body — would be a no-op).
    AQJointHandle createDistanceJoint(const std::shared_ptr<AQRigidBody> &a,
                                       const std::shared_ptr<AQRigidBody> &b,
                                       const OmegaGTE::FVec<3> &anchorALocal,
                                       const OmegaGTE::FVec<3> &anchorBLocal,
                                       float length);                       // new
    AQJointHandle createBallSocketJoint(const std::shared_ptr<AQRigidBody> &a,
                                        const std::shared_ptr<AQRigidBody> &b,
                                        const OmegaGTE::FVec<3> &anchorALocal,
                                        const OmegaGTE::FVec<3> &anchorBLocal); // new
    AQJointHandle createHingeJoint(const std::shared_ptr<AQRigidBody> &a,
                                    const std::shared_ptr<AQRigidBody> &b,
                                    const OmegaGTE::FVec<3> &anchorALocal,
                                    const OmegaGTE::FVec<3> &anchorBLocal,
                                    const OmegaGTE::FVec<3> &axisALocal,
                                    const AQJointAxisLimit &limit = {});      // new
    AQJointHandle createSliderJoint(const std::shared_ptr<AQRigidBody> &a,
                                     const std::shared_ptr<AQRigidBody> &b,
                                     const OmegaGTE::FVec<3> &anchorALocal,
                                     const OmegaGTE::FVec<3> &anchorBLocal,
                                     const OmegaGTE::FVec<3> &axisALocal,
                                     const AQJointAxisLimit &limit = {});      // new
    AQJointHandle createFixedJoint(const std::shared_ptr<AQRigidBody> &a,
                                    const std::shared_ptr<AQRigidBody> &b);    // new

    void           setJointSoftness(AQJointHandle h, AQJointSoftness s);       // new
    bool           destroyJoint(AQJointHandle h);                              // new

    // Read-only joint view, refreshed per `advance`.
    AQUA_NODISCARD std::vector<AQJointDesc> joints() const;                    // new

    // --- queries (Phase 4) ---
    // hits is cleared then appended; results are sorted by fraction. The grid
    // walked is the same per-step structure Phase 2 builds; queries are valid
    // between `advance` calls and stale during one.
    void raycast(const OmegaGTE::FVec<3> &origin,
                 const OmegaGTE::FVec<3> &direction,
                 float maxT,
                 const AQQueryFilter &filter,
                 std::vector<AQRaycastHit> &hits) const;                       // new
    void shapecast(AQShapeHandle shape,
                   const OmegaGTE::FVec<3> &origin,
                   const OmegaGTE::FQuaternion &orientation,
                   const OmegaGTE::FVec<3> &direction,
                   float maxT,
                   const AQQueryFilter &filter,
                   std::vector<AQRaycastHit> &hits) const;                     // new
    void overlap(AQShapeHandle shape,
                 const OmegaGTE::FVec<3> &origin,
                 const OmegaGTE::FQuaternion &orientation,
                 const AQQueryFilter &filter,
                 bool exactShapes,
                 std::vector<std::uint32_t> &bodies) const;                    // new

    // --- triggers (Phase 4) ---
    // Drains the per-`advance` event queue; subsequent calls until the next
    // advance return empty.
    AQUA_NODISCARD std::vector<AQTriggerEvent> triggerEvents();                // new

    // --- sleep tuning (Phase 4) ---
    // Defaults: 0.01 m/s linear, 0.01 rad/s angular, 60 sub-steps (~0.5 s at
    // the 1/120 default). Per-body overrides on AQBodyDesc take precedence.
    void setSleepThresholds(float linearVel, float angularVel,
                             std::uint32_t idleSubsteps);                       // new
};
```

> **Folded-in groundwork (lands first, §1).** `AQConstraintKind` gains
> `JointAxis`, `JointLimit`, `JointMotor` in `AQContact.h` (the existing
> enum — no header churn elsewhere). `AQConstraintRow` gains
> `compliance : float = 0.f` immediately after `frictionCoeff` (Catto 2011
> soft-constraint hook; defaults preserve Phase 3 row math byte-for-byte).
> The persistence cache adds a joint-keyed map alongside the existing
> contact-keyed one; eviction is "one-frame grace" matching Phase 3 §11.8.
> `AQSpace::stepInternal` gains one new step between the row-build and the
> PGS sweep: `runIslandsAndSleep()` — a private method that does the
> union-find, sleep accumulation, and per-island activation flip. The
> Phase 2 grid-scratch lifetime contract changes from "broadphase-local"
> to "valid until next `advance`" so queries read it directly. The existing
> Phase 2 / 3 tests pin the activation default (`Active`) as a regression
> guard — sleeping-body cases get their own Phase 4 tests.

`AQJointHandle` and `AQRaycastHit` are small opaque values (index +
generation; index + scalars), copyable and backend-free — neither is a
pointer to a backend type, so the pimpl boundary holds.

---

## 11. Open decisions for this phase

1. **Joint type vocabulary — five specialized types vs. one D6 superset vs.
   both.** *Lean: five specialized types* (Distance, BallSocket, Hinge,
   Slider, Fixed) per §5/§7. D6 is the PhysX-style universal joint; rejected
   as the *lead* because it inflates the per-row branch count and is harder
   to debug. Kept as a §11 follow-up — the row-build paths factor cleanly
   enough that a D6 wrapper that builds the same rows could land as Phase
   4.x without breaking the surface here.
2. **Soft-constraint parameterization — `(frequency, damping)` vs. raw
   `(compliance, ERP)` vs. spring `(k, c)`.** *Lean: `(frequency, damping)`*
   (Catto 2011 — the user-facing UI). The translation to `(compliance, bias)`
   lives behind the pimpl; users author behaviour (`ω_n = 10, ζ = 0.7`) not
   numerics (`compliance = 0.00253, ERP = 0.18`). Hard joints default to
   `(0, 0)` ⇒ `compliance = 0` ⇒ Phase 3 row math unchanged.
3. **Articulations (Featherstone reduced-coordinate) — in Phase 4 vs.
   deferred minor.** *Lean: deferred minor*. The joint descriptors here are
   exactly the topology input an articulation solver would consume, so the
   work isn't wasted; reduced-coordinate solving is its own subsystem
   (different state representation, different integrator) and Phase 4 is
   already broad. Document as a recommended kREATE Phase 9+ extension.
4. **Island detection — union-find vs. BFS vs. parallel coloring.** *Lean:
   union-find with path compression + union-by-rank*. Linear in the number
   of constraint rows, deterministic given a deterministic edge order
   (which Phase 3 §5/§8 supplies). BFS is more cache-friendly but harder to
   parallelize for Phase 5; coloring is overkill for the connectivity
   problem (it's a Phase 5 *solver* concern, not an island-detection one).
5. **CCD model — speculative contacts vs. continuous TOI vs. both, default
   Off vs. default Speculative.** *Lean: ship both, default Off* per body —
   the conservative choice matches the Phase 3 settling-stack performance
   bar (CCD on by default would regress it for stacks of slow boxes). Users
   opt in per body. This is the **roadmap §7.7 key decision**: CCD scope.
   Speculative is the cheap "on for anything that might tunnel" choice;
   Continuous is the expensive "must not tunnel" choice for bullets,
   ragdoll-during-cinematic, and physics traps.
6. **Sleep thresholds — fixed scalar vs. energy-based vs. user-configured
   per-body.** *Lean: energy-flavored default (mass-weighted `‖v‖² +
   ω·I·ω < ε_E`), per-body override*. Pure velocity thresholds misclassify
   heavy slow bodies (too eager to sleep) and light fast ones (too eager to
   wake). Energy-flavored is one extra multiply per body per step and
   directly reflects "moving how much, by how much energy."
7. **Kinematic body collision response — one-way (kinematic pushes
   dynamics) vs. two-way (dynamics can push kinematic indirectly via
   joints) vs. fully bidirectional.** *Lean: one-way for collisions,
   bidirectional via joints*. Kinematic bodies are user-driven pose; a
   collision pushing the user's animation back is wrong by definition. But
   a joint to a kinematic body (a door hinge on a kinematic frame) needs
   the joint to drive the dynamic side — which is what we want.
8. **Query API — value-returning `std::vector` vs. user-owned buffer +
   append.** *Lean: user-owned buffer + append* (the `void raycast(...,
   std::vector<AQRaycastHit> &hits)` shape in §10). Avoids per-query
   allocation; matches kREATE's existing buffer-pool pattern. Returning a
   value `std::vector` is the convenience overload we *could* add — defer
   until profiling shows hit-count distributions justify it.
9. **Trigger event ordering — by `(a, b)` vs. by insertion vs. by kind.**
   *Lean: by `(a, b)` ascending, with `(kind = Exit, Stay, Enter)` as the
   tiebreak so exits drain before re-enters in the same frame*. Determinism
   plus the standard "process exits first" gameplay idiom.

---

*Brief status: proposal. Decisions in §11 — above all the CCD model (#5,
roadmap §7.7) and the joint type vocabulary (#1) — should be settled before
the joint and CCD code lands. This document is the Phase 4 entry of the
per-phase prior-art series roadmap §4 establishes, and follows the
conventions set by `Phase-1-Dynamics-Math-Core.md`,
`Phase-2-Collision-Shapes-Broadphase.md`, and
`Phase-3-Narrowphase-Contact-Solver.md`. After this phase lands, kREATE's
Engine-Roadmap Phase 8 (rigid-body integration) is unblocked — the §1 brief
of `Physics-Roadmap.md` Phase 4 promise. The Phase 5 OmegaSL port (the
existing kernels placeholder in `aqua/src/kernels/`) and the Phase 7
unified-XPBD architecture fork (roadmap §7.2) are explicitly out of scope
here — the data layout chosen in §8 keeps both forks open without
prejudging them.*

---

## 12. Recency-principle audit (addendum, 2026-06-06)

Roadmap §4 was strengthened to make "newest viable algorithm from the
literature" the standing default for every phase, with incumbents
adopted only when no substantively-newer alternative offers a real
improvement for AQUA's substrate (`Physics-Roadmap.md` §4 — "Recency
principle"). The Phase 4 brief carried part of this audit inline in §4
(literature); this section consolidates it under the explicit recency
discipline so it parallels the Phase 1 / Phase 2 / Phase 3 addenda.
Findings mirror `Physics-Roadmap.md` §5 Phase 4.

The Phase 4 picks span four subsystems — joints, queries, sleeping,
CCD — so the audit is per-subsystem.

- **Joints — Müller, Macklin, Chentanez, Jeschke, "Detailed Rigid Body
  Simulation with Extended Position Based Dynamics" (CGF 2020) is the
  substantive divergence; deferred because it is the §7.2 fork.** Applies
  XPBD's compliance-form constraint projection directly to *rigid* bodies
  — distance, ball-socket, hinge, slider, fixed — with `n` substeps × 1
  iteration instead of 1 substep × `n` PGS iterations, claims
  unconditional stability at any stiffness (`compliance = 0` ⇒ infinitely
  stiff with no Baumgarte-style energy injection), and removes joint
  warm-starting. The **2023 survey** (Fei et al., "Survey of Rigid Body
  Simulation with XPBD," arXiv 2311.09327) confirms it as *the* modern
  alternative to Catto-2011 PGS for joints; **Mercier-Aubin 2024**
  multi-layer XPBD (CGF 2024) refines it further. **But adopting it for
  Phase 4 joints alone would prejudge the §7.2 unified-XPBD decision** —
  the architectural fork the roadmap explicitly defers to Phase 7. The
  Phase 4 lean stays *Catto-2011 soft constraints on the PGS row buffer*,
  with the `AQConstraintRow::compliance` field (§7) carrying enough of
  XPBD's parameter surface that a Phase-7 unified-XPBD recast reuses the
  row layout without rewriting it. Müller 2020 is cited for that field,
  even while the algorithm using it is PGS.
- **CCD — Wang, Ferguson, Schneider, Panozzo et al., "A Large-Scale
  Benchmark and an Inclusion-Based Algorithm for CCD" (TOG 2021; the
  "Tight Inclusion" line, used by IPC). Not applicable.** Provably-
  correct (no false negatives, no false positives) and the most-cited
  new CCD work since Mirtich 1997, **but it targets triangle-mesh
  vertex-face / edge-edge pairs in deforming simulations** (FEM cloth,
  soft bodies), not rigid bodies with analytic support functions. For
  AQUA's analytic shape vocabulary (sphere / box / capsule / plane /
  convex hull), conservative-advancement on `AQshapeSupport` (Mirtich
  1997) is the right answer; Tight Inclusion's guarantees buy nothing
  because GJK on convex shapes does not suffer the near-zero false-
  negative regime that motivates inclusion-based CCD. The incumbents'
  speculative + conservative-advancement two-tier (§6.K, §7
  `AQCCDMode`) is what Phase 4 ships. Revisit if/when AQUA grows a
  deforming-mesh collider type in the soft-body pillar.
- **CCD acceleration via hardware RT-cores — flagged-for-Phase-5,
  not adopted.** **Wang et al., "Hardware-Accelerated Ray Tracing for
  Discrete and Continuous Collision Detection on GPUs" (arXiv
  2409.09918, 2024)** uses RTX BVH-traversal hardware for both
  discrete and continuous detection — the *continuous* angle is a
  direct Phase 4 follow-up. Not adopted now for the same reason as
  the Phase 2 broadphase RT note: vendor-specific (NVIDIA RTX with AMD
  and Apple closing the gap, feature-uneven across GPUs), AQUA's
  three-backends-required posture, and `GTEDEVICE_FEATURE_RAYTRACING`
  gating expected as a Phase 5.x acceleration path.
- **Islands & sleeping — no divergence.** Union-find with path
  compression + union-by-rank (Tarjan 1975) is still the answer;
  recent **parallel union-find** work (Patwary et al. PPoPP 2012;
  Jaiganesh & Burtscher 2018, "A High-Performance Connected Components
  Implementation for GPUs") is the Phase 5 GPU port — algorithmically
  the same union-find, parallelized. There is no newer algorithm
  shifting the connected-component / sleep-state shape of the problem.
  Phase 4 lean unchanged; Patwary 2012 / Jaiganesh 2018 cited for the
  Phase 5 port path.
- **Queries — no divergence.** 3D-DDA grid traversal (Amanatides &
  Woo 1987) over the Phase 2 sort-based grid is still the right
  answer for analytic-shape ray/shape queries; modern alternatives
  (neural-BVH ray traversal, locality-sensitive hashing) target wholly
  different cost regimes. No opening.
- **Friction at contact (within joint contact) — Ly et al. SIGGRAPH
  2024 primal-dual non-smooth friction.** Same conclusion as the
  Phase 3 audit: substrate mismatch at AQUA's small-step `dt`;
  flagged not adopted.
- **TGS (PhysX 5 Temporal Gauss-Seidel) — already documented in the
  Phase 3 brief, revisited here.** Phase 3 §6 noted TGS as the
  "alternative considered" for stacking quality at coarse `dt` and
  flagged a revisit in Phase 4 if joint stacks need it. The Phase 4
  audit re-evaluates: at AQUA's small-step `dt` the swinging-bridge
  deliverable (§1) is hand-computable from the analytic catenary
  tension; TGS does not surface a measurable win in that regime. Stay
  with PGS + split-impulse. Revisit only if profiles surface joint-
  stack convergence issues kREATE's gameplay actually triggers.

**Net for Phase 4:** the audit returns **no adopt-now finding** for
the four subsystems in scope — the joint XPBD divergence is the §7.2
fork (defer), and the other three subsystems' incumbents (PGS +
split-impulse, 3D-DDA grid walk, union-find islands, conservative-
advancement CCD on analytic shapes) remain the right answer for
AQUA's substrate. Three future-work items recorded: **(a)** XPBD-
for-rigid recast under the §7.2 decision (Müller 2020); **(b)** RT-
core hardware CCD as a Phase 5.x acceleration path (Wang 2024);
**(c)** parallel union-find for the Phase 5 island-detection GPU
port (Patwary 2012, Jaiganesh 2018).

Re-audit due: 2028-06-06 (roadmap §4 two-year freshness rule) or sooner
if the §7.2 fork lands in Phase 7, or if hardware RT broadphase ships
broadly enough to justify enabling the Phase 5.x path.

---

## 13. Implementation phasing (addendum, 2026-06-17)

§1–§11 are the prior-art brief; this section is the reviewable-increment
breakdown AGENTS.md requires before code lands (the brief did not carry one).
Each increment builds and keeps the Phase 1–3 test battery green as a
regression guard before the next begins; the §11 leans are adopted as the
decisions (specialized joints #1, Catto-2011 softness #2, deferred
articulations #3, union-find islands #4, ship-both-CCD-default-Off #5,
energy-flavored sleep #6, one-way-kinematic #7, append-buffer queries #8,
`(a,b)`-ordered triggers #9).

- **4a — Groundwork (lands first, §1).** Type/enum/header surface and the
  integrator hooks the rest builds on. `AQConstraintKind` gains
  `JointAxis`/`JointLimit`/`JointMotor`; `AQConstraintRow` gains
  `compliance` (defaults preserve Phase 3 row math byte-for-byte). New
  `AQJoint.h` / `AQQuery.h`. `AQBodyType::Kinematic`, `AQActivationState`,
  `AQCCDMode`, the `AQBodyDesc` Phase 4 fields, and the `AQRigidBody`
  accessors. `AQBodyState<Ty>` gains `activation`; `AQStepBodyVelocity`/
  `AQStepBodyPosition` fast-path `Sleeping`. `AQDebugFlags` extension.
  AQSpace wires the body-side state, kinematic implicit velocity, the
  both-sleeping PGS row skip, and the `compliance` term in `effectiveMass`.
- **4b — Islands & sleeping.** `runIslandsAndSleep()` between row-build and
  the PGS sweep: union-find over the (contact ∪ joint) edge set (statics
  excluded), per-island energy-flavored idle accumulation, island-scoped
  activation flips, the over-connection debug guard, and the sleep-tuning
  API.
- **4c — Joints.** `src/AQJoint.cpp` per-type row builders + limits/motors;
  the SoA joint table on `AQSpace::Impl`; the `create*Joint` /
  `destroyJoint` / `setJointSoftness` / `joints()` surface; the
  `(jointIndex, rowIndex)` warm-start cache; the joint-row build step in
  `runNarrowphaseAndSolve`; the Catto-2011 `(frequency,damping)` →
  `(compliance,bias)` translation.
- **4d — Queries.** Grid-scratch lifetime promoted to "valid until next
  `advance`"; `src/AQQuery.cpp` 3D-DDA walk + analytic ray/shape forms +
  GJK-ray fallback; `raycast` / `shapecast` / `overlap`.
- **4e — Triggers.** `isTrigger` short-circuit in the candidate loop;
  Enter/Stay/Exit diff over prev/curr overlap sets; `triggerEvents()`.
- **4f — CCD.** `src/AQCCD.cpp` conservative-advancement TOI; speculative
  fattening hook; the post-step per-body continuous substep.
- **4g — Deliverable tests.** The four §1 scenes + the Phase 3 regression
  re-run, wired into `tests/CMakeLists.txt`.

Off-platform note (per the repo's multi-backend convention): AQUA is a
pure-CPU library with no backend-specific paths, so there are no
D3D12/Metal-unverified surfaces here — every increment is built and run on
this Linux host.

**Status (2026-06-17): COMPLETE.** All seven increments landed; the Phase 1–3
test battery stays green and the new `aqua_phase4_test` passes all four §1
deliverables (bridge, hinge door, raycast+sleep, bullet). Notable
implementation decisions that diverged from the brief's lead, recorded for the
record:

- **Angular constraint rows.** The shared `AQConstraintRow` gained one field,
  `isAngular`, beyond the `compliance` the brief scoped. Hinge/slider/fixed
  orientation locks, angular limits, and angular motors are pure-torque rows
  that the brief's contact-shaped row could not express; the PGS sweep branches
  once on it and contacts take the unchanged linear path (byte-for-byte Phase 3).
- **CCD via analytic swept-sphere TOI, not GJK conservative-advancement.**
  §6.K's lead was Mirtich conservative-advancement over `AQshapeSupport`. The
  shipped `runCCD` instead sweeps the body's bounding sphere with the analytic
  sphere-cast already built for queries (`AQrayShape`): exact for the spherical
  bullet deliverable, conservative for other shapes, and it reuses shipped code
  instead of a second GJK path. The general convex-convex CA generalization is
  deferred (no deliverable needs it); `src/AQCCD.cpp` was therefore not created.
- **Queries are an O(N) sweep over the per-body broadphase AABBs, not a 3D-DDA
  grid walk.** Correctness-first and matches the brute-force oracle exactly; the
  per-body fat AABB *is* broadphase output (valid until the next advance), so the
  §8 lifetime contract holds. The full 3D-DDA over the cell grid is the
  documented performance follow-up.
- **`jointImpulse()` accessor added.** §9's bridge oracle needs the solver's
  accumulated joint impulse; a small read-only `AQSpace::jointImpulse(handle)`
  exposes the joint's last-sub-step world linear impulse (reaction force =
  impulse/dt). Folded into the §10 surface.
- **The bridge deliverable is a *taut* rigid-link chain, not a sagging
  catenary.** Wide rigid boxes joined by ball-sockets have a stable collinear
  equilibrium (each box balances equal vertical joint forces at ±0.5), so a
  span-length chain stays straight rather than catenary-sagging like a
  point-mass string. The test asserts the load-bearing invariants instead
  (joint error < 1 mm, symmetry, ends pinned, end anchors carry the chain
  weight within ~15% — the Baumgarte position-bias inflates the pure support
  force slightly). The < 1 mm joint-error bound is the §1 headline and holds.
- **The query/CCD math seams** live in `src/AQQuery.cpp` (`AQrayShape`,
  `AQshapeBoundingRadius`) and `src/AQJoint.cpp` (`AQbuildJointRows`); the
  `AQSpace::Impl`-touching glue (joint table, query/trigger/CCD iteration) stays
  in `AQSpace.cpp` because `AQRigidBody::Impl`/`AQSpace::Impl` are private and
  only `AQSpace` is their friend — the same split the brief assumes for the
  per-type math vs. the SoA driver.

---

## 13. Phase 4.x — Joint split-impulse position correction (Baumgarte retirement)

**Status: active plan (implementation phasing below). Added 2026-07-02.**

### 13.1 Why this section exists — revising the §6.I decision

§6.I shipped joints on a **Baumgarte velocity bias** for position correction:
each joint row bakes its constraint error `C` into `bias = (β/dt)·C` (β = 0.2,
`AQJoint.cpp` `softParams`, hard branch), and the PGS velocity sweep
(`AQSpace.cpp` §E) adds that `bias` term to the row's relative velocity. The
rationale recorded there was that "the Catto-2009 split-impulse argument is
specific to one-sided contact rows," so joints stayed on Baumgarte while
contacts got the clean split-impulse (pseudo-velocity) pass (§6.I / Phase 3 §F).

That decision is now **overruled**, with a documented reason (case-law style —
rule + scope + why):

- **Rule.** Joint *position-error* correction moves off the Baumgarte velocity
  bias and onto the same split-impulse pseudo-velocity pass contacts already
  use. The velocity solve keeps only genuine velocity goals (motor target
  speeds; restitution has none for joints).
- **Scope.** AQUA rigid-body joints — the bilateral `JointAxis` position rows
  of every type (Distance, BallSocket, Hinge, Slider, Fixed) and, in a second
  step, the one-sided `JointLimit` rows. Motors (`JointMotor`) are unaffected.
- **Why.** Two reasons, both rooted in the developer's standing AQUA bar
  (highest-precision, no-shortcut, cross-platform-identical physics; see the
  determinism work in §13.6):
  1. **Baumgarte is a shortcut that injects energy and pollutes the reaction
     force.** The velocity bias reaches equilibrium at a small *steady* joint
     stretch, and the impulse spent holding that stretch is added on top of the
     true constraint impulse. This is exactly what the §9 bridge deliverable's
     support-force oracle measures: `jointImpulse()` returns the velocity-solve
     accumulated impulse (`AQSpace::Impl::…lastLinearImpulse`), so the anchor
     "vertical force" reads high. The §9 note already conceded "the Baumgarte
     position-bias inflates the pure support force slightly" and set the oracle
     tolerance to ~15% to absorb it.
  2. **The inflation is not stable across toolchains.** After AQUA moved to
     `-ffp-contract=off` for cross-platform-identical results (§13.6), the
     bridge oracle settled at ~18.8% over the analytic chain weight — past the
     15% tolerance. Split-impulse removes the bias term from the measured
     impulse entirely, so the oracle converges to the analytic weight (≈0%)
     *and* does so identically on every backend. The fix is the algorithm, not
     a wider tolerance — matching the "adjust the algorithm" directive.

The §6.I text and the `AQJoint.cpp:16-17` comment ("joints skip split-impulse")
are updated by this section when it lands.

### 13.2 Why split-impulse and not XPBD

§12's recency audit named **XPBD** (Müller/Macklin 2020) as the modern
alternative that is "infinitely stiff with no Baumgarte-style energy injection."
It is *not* chosen here, for the reason §12 already recorded: XPBD for joints is
the **§7.2 unified-XPBD architectural fork**, deferred to Phase 7, and adopting
it for joints alone would prejudge that decision (n-substep × 1-iteration loop,
no warm-start, a different solver shape).

Split-impulse position correction is the **minimal, non-fork-prejudging** way to
retire Baumgarte: it keeps the Phase 3/4 PGS-row-buffer architecture, the
velocity warm-start, the sub-step count, and the row schema — it only relocates
*where* position error is corrected. It is also strictly consistent with the
path contacts already ship. When Phase 7 revisits §7.2, a unified-XPBD recast
still reuses this row layout (the `compliance` field already carries XPBD's
parameter surface, per §12); this change does not close that door.

### 13.3 Design

**Row schema (`AQConstraintRow`, `include/aqua/AQContact.h`).** Add one field:

- `float positionError = 0.f;` — the signed scalar constraint value `C` along
  `direction` (metres for linear rows, radians for angular rows). Today `C` is
  computed in `AQbuildJointRows` and immediately folded into `bias` then lost;
  the split-impulse pass needs it live. This mirrors the role `AQContactPoint::
  depth` plays for the contact split-impulse pass.

**Joint build (`AQJoint.cpp`).** `softParams` grows a flag (or a sibling
`softParamsSplit`) so that for a **hard** position row it returns
`bias = 0` and hands `C` back to be stored in `row.positionError`, instead of
`bias = (β/dt)·C`. The **soft** branch (`AQJointSoftness.frequency > 0`) is
unchanged in spirit — a soft joint is a spring the user asked for, and its ERP
bias is a modelling parameter, not a stabilisation shortcut; soft rows keep
their velocity bias and do **not** enter the position pass (documented so the
two paths never both correct the same error). Motor rows (`JointMotor`) keep
their velocity bias (target speed). The per-type builders (`emitPoint3`,
`emitAngular3`, Distance, Hinge/Slider limit-motor) are otherwise untouched —
they already compute `C` at each call site (`dot(Cvec, ax)`, `dot(theta, ax)`,
`len - target`, …); they just pass it through to `positionError`.

**Velocity sweep (`AQSpace.cpp` §E).** Unchanged in structure. Because hard
position rows now carry `bias = 0`, the sweep naturally solves them to pure
zero relative velocity — no bias term, no injected energy. Motor/soft rows are
unaffected.

**Split-impulse pass (`AQSpace.cpp` §F).** Currently iterates `impl->manifolds`
only. Add a second loop over `impl->jointRowSpans` → their rows, applying
bilateral pseudo-velocity correction per row, reusing the existing per-body
`pseudoLinear`/`pseudoAngular` accumulators (already zeroed once per sub-step,
§ init) and the existing `positionIters` count:

```
for span in jointRowSpans:
  for row in span rows:
    if row.kind == JointMotor: continue          // velocity goal, not position
    if row.compliance != 0:    continue          // soft joint: user spring, skip
    C = row.positionError
    // one-sided for limits, bilateral otherwise
    if row.kind == JointLimit and C <= 0: continue
    target = clamp(ERP * C / dt, ±maxPosVel)
    if row.isAngular:
        relPV = dot(pseudoAngB - pseudoAngA, dir)
        k     = dot(dir, invIB·dir) + dot(dir, invIA·dir)
        λ     = (target - relPV) / k
        if row.kind == JointLimit: λ = clamp λ to the one-sided cone
        pseudoAngB += invIB·(dir·λ);  pseudoAngA -= invIA·(dir·λ)
    else:
        // same rAxN/rBxN effective-mass form as the contact pass, at row.rA/rB
        …apply dir·λ at the joint anchor via pseudoLinear/pseudoAngular…
```

`ERP`, `maxPosVel` reuse the contact constants (`kPositionERP = 0.2`,
`kMaxPosCorrectionVel = 2`) so joints and contacts correct position at the same
rate — important where a body is in both a joint and a contact. Sign
convention: `positionError` is defined so a positive `C` means "B drifted +dir
from A," matching `Cvec = bW - aW` in `AQbuildJointRows`; the pseudo-impulse
pushes them back together (verify the sign against the Distance and BallSocket
cases in test).

**`jointImpulse()` (`AQSpace.cpp`).** No API change; it now returns the pure
constraint impulse (Baumgarte term gone). The §9 bridge oracle consequently
reads ≈ the analytic weight. Its tolerance can tighten from 15% → e.g. 5% once
the change lands (a follow-up assertion tightening, not a loosening).

**Determinism.** The joint position loop visits `jointRowSpans` in the existing
deterministic build order; the pseudo-velocity accumulators are per-body and
order-stable. No new nondeterminism. The §5/§8 byte-identical contract is
preserved and re-checked by the determinism test.

### 13.4 Row-kind handling matrix

| Row kind    | Velocity `bias` after change | In split-impulse pass? | Notes |
|-------------|------------------------------|------------------------|-------|
| `JointAxis` (hard)  | 0                    | yes, bilateral         | the core fix — the bridge's ball-socket rows |
| `JointAxis` (soft)  | ERP bias (unchanged) | no                     | user spring; position handled by the spring |
| `JointLimit`        | 0 (Phase 4x.3)       | yes, one-sided         | like a contact; retires limit-bounce energy |
| `JointMotor`        | target speed (unchanged) | no                 | velocity goal, not a position error |

### 13.5 Implementation phasing

Small-to-moderate; each phase is independently verifiable and reversible.

- **4x.1 — Schema plumbing (no behaviour change).** Add `positionError` to
  `AQConstraintRow` and its GPU mirror struct; populate it in `AQbuildJointRows`
  alongside the existing `bias`. Leave Baumgarte on. The whole suite must be
  **bit-identical** to before (positionError is written but unread). Gate.
- **4x.2 — CPU bilateral `JointAxis` split-impulse (the core fix).** Flip hard
  `JointAxis` rows to `bias = 0`; add the joint loop to §F for bilateral rows.
  Verify: bridge oracle → ≈ analytic weight on macOS/Linux **and** the joint-
  error < 1 mm / symmetry / ends-pinned invariants still hold; `aqua_phase4_test`
  green; `aqua_contact_test` unchanged (contacts untouched).
- **4x.3 — CPU one-sided `JointLimit` split-impulse.** Move limit rows too, for
  consistency and to kill limit-bounce energy. Verify the hinge-door limit case
  in `aqua_phase4_test` (settles at the limit without overshoot/bounce).
- **4x.4 — GPU mirror.** Port to the compute path so CPU and GPU stay
  bit-parity: `AQComputeBackend.h/.cpp` constraint-row struct gains
  `positionError`; `src/kernels/AQSolver.omegasl` gains the joint split-impulse
  in its position pass with the same order and math. Verify `aqua_gpu_solver_test`
  and the CPU/GPU cross-path determinism check. (Ties to §7.3 SoA layout;
  `AQKernelsCommon.omegaslh` shared struct updated once.)
- **4x.5 — Tighten oracles + doc.** Tighten the §9 bridge support-force oracle
  (15% → ~5%), update the §6.I text and the `AQJoint.cpp` header comment, and
  move this doc to `.plans/done/` only when 4x.1–4x.4 are all shipped.

### 13.6 Context — the determinism build

This work was surfaced while fixing `aqua_contact_test`'s three failures
(2026-07-02). Two of those were cross-platform FP divergence, fixed by
`-ffp-contract=off` on AQUA (Clang contracts `a*b+c` into a single-rounded FMA;
MSVC's `/fp:precise` does not — the divergence amplified over the settling
stack's chaotic sub-steps). The third was a genuine GJK/EPA bug (a degenerate,
zero-volume tetrahedron handed to EPA produced a NaN witness; fixed with the
`expandToTetrahedron` "enclose the origin" recovery in `AQGJK.cpp`). The
deterministic build is what pushed the bridge oracle past its 15% tolerance and
exposed the Baumgarte shortcut this section retires.

### 13.7 Risks / open decisions

- **Limit one-sidedness sign.** `JointLimit`'s `positionError` must encode "how
  far past the limit" with the correct sign for the one-sided clamp; the current
  Baumgarte limit bias (`-(β(coord-max))/dt`, `emitAxisLimitMotor`) is the
  reference for that sign. Nail it in 4x.3 against the hinge-door test.
- **Coupled joint+contact bodies.** A body in both a joint and a resting contact
  now takes pseudo-impulses from both loops in the same §F sweep. Sharing `ERP`/
  `maxPosVel` keeps the two correction rates matched; watch the stacked-and-
  jointed case for pseudo-velocity fighting (none expected — contacts are
  one-sided, joints bilateral, and they act on different error directions).
- **Warm-start scope.** Only the *velocity* impulse is warm-started (unchanged);
  the pseudo-velocity pass is fresh each sub-step, exactly as contacts do. Do
  not warm-start the position pass.
- **Soft joints keep ERP bias by design.** Confirm no test drives a *hard* joint
  through the soft path; the split only diverts the hard branch.
