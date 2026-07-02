# AQUA Phase 9 — Deformable Solids

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a subsystem is written: what PhysX 5/FleX
and Chaos do to make a solid squish and recover, which papers the algorithm
comes from, what we change for AQUA's substrate, and how we will measure
"better." It covers **Phase 9 — Deformable Solids [Soft body II]**: volumetric
soft bodies — squishy, recoverable solids that deform under load and spring
back to their rest shape — built on the Phase 7 XPBD core rather than as a new
solver. Where Phase 8 handled *codimensional* softness (cloth is a surface,
hair is a curve — a body with no interior volume), Phase 9 handles *volumetric*
softness: a tetrahedral mesh with an inside, where the physics you must get
right is **volume preservation** and **inversion recovery**, neither of which a
cloth solver ever has to think about.

This is a solver-*extension* phase, not a solver phase. The XPBD engine, the
substep loop, the constraint-batch coloring, and the particle pool it iterates
over all shipped earlier. Phase 9 adds two new `AQConstraintType`s (a
tetrahedral deviatoric/distortion constraint and a hydrostatic/volume
constraint — the constraint-based Neo-Hookean pair of Macklin & Müller 2021), a
cheap shape-matching fallback (Müller et al. 2005) for blobs that don't warrant
a tet mesh, and the two-way coupling that lets a deforming cube press on a
Phase 3 rigid stack and feel it press back.

---

## 1. Scope & deliverable

**Goal.** Simulate volumetric soft bodies — recoverable elastic solids with
preserved volume — as tetrahedral meshes whose nodes are particles in the Phase
6 pool and whose behavior is governed by XPBD constraints projected each
substep. A soft body must: deform under load, *recover* to (near) its rest
shape when the load is removed (elastic, no permanent set unless plasticity is
enabled), preserve its volume within tolerance under compression, survive a tet
flipping inside-out (recover, not explode), and exchange contact forces two-way
with rigid bodies and with cloth. Path selection is *data* (queried
`GTEDeviceFeatures`), never an `#ifdef` (`Physics-Roadmap.md` §3 principle 3).

**Runnable deliverable.** A **deformable cube dropped onto a stack of Phase 3
rigid boxes** — the headline scene, chosen because it exercises every new piece
at once and every assertion is a number we can derive by hand:

1. **The cube deforms and the boxes react (the headline — two-way coupling).**
   A tetrahedralized unit cube (a modest mesh, ~5×5×5 cells → ~750 tets, five
   tets per hex cell) is dropped from a small height onto a settled Phase 3
   three-box stack. On impact the cube visibly flattens at the contact face and
   the top rigid box registers the impulse (its contact normal force spikes,
   then settles). Both bodies are in the *same* solver step: the cube's XPBD
   node projections and the rigid stack's Phase 3 impulses are coupled at the
   contact (the HYBRID decision from Phase 7 §11). Asserted: the top box's
   accumulated normal impulse over the settle reflects the cube's added weight
   (`m_cube · g` within 5%), and neither the cube nor the stack gains energy.
2. **Elastic recovery (the oracle for "it's a solid, not putty").** With
   plasticity disabled, the cube is compressed to ~60% height under a static
   load for 1 s, the load removed, and after 2 s its per-node RMS displacement
   from the rest mesh is `< 2%` of the edge length — it *sprang back*. A putty
   model (pure shape-matching with no elastic recovery, or a mis-signed
   constraint) fails this immediately, which is the point.
3. **Volume preservation under compression.** While compressed in (2), the
   summed signed tet volume stays within **±3%** of the rest volume (the
   hydrostatic constraint's whole job). A soft body that loses volume under
   load looks like a deflating balloon; this asserts it does not.
4. **Inversion recovery (the classic failure, tested on purpose).** The cube is
   squashed flat — driven past the point where interior tets invert (negative
   signed volume) — held, then released. Assertion: every tet's signed volume
   returns positive within N substeps and the mesh recovers to rest shape
   (deliverable 2's bound). This is the Irving-Teran-Fedkiw 2004 nightmare case;
   we test that the constraint formulation recovers from it rather than
   producing NaNs (§6, §9). A tet that inverts and *stays* inverted, or a NaN
   that spreads through the pool, is a hard failure, loud (§9).

Each deliverable is built around a slow, obviously-correct reference the fast
path must match: rest volume computed by summing tet determinants, the analytic
added-weight the rigid stack must feel, the RMS-to-rest recovery bound. As with
every prior phase, the `double` instantiation is the precision oracle and the
CPU path is held to these analytic numbers; the GPU path is then held to the
CPU path within the §8 tolerance band.

**Included groundwork (lands first in Phase 9).** Two things earlier phases
deferred to here, closed before any tet is solved:

- **The tet-mesh ingestion contract.** Phase 6's `AQParticlePool` stores nodes;
  Phase 9 must define how a *mesh* — node positions plus tet connectivity plus
  per-material parameters — becomes pool particles plus constraint records. That
  is `AQSoftBodyDesc` (§7) and `AQSpace::createSoftBody(...)`. The pool gains no
  new storage; the soft body is *indices into it* plus a constraint batch.
- **The rest-state precompute.** Each tet's rest-volume and its inverse
  reference shape matrix `Dm⁻¹` (the material-to-world map every FEM-flavored
  constraint needs) are computed once at create time, stored per-tet, and are
  the invariants the runtime constraint reads. A degenerate rest tet (zero or
  near-zero rest volume — a sliver in the input mesh) is rejected loudly at
  create time, not silently divided-by at step time (§6, §9).

**What earlier phases closed for us.** Do not re-derive these; they are the
substrate this phase consumes:

- **The XPBD core is shipped (Phase 7).** `include/aqua/AQXPBD.h`:
  `AQConstraintType` (Distance, Bending, **Volume**, …), `AQConstraint` (POD —
  indices, rest value, **compliance**, lambda), `AQConstraintBatch` (graph
  coloring), `AQXPBDSolver` with the substep loop `n × 1` (Macklin et al. 2019)
  and colored projection (Vivace-style). Phase 9 *adds constraint kinds*; it
  does not touch the solver loop.
- **The HYBRID coupling decision is made (Phase 7 §11).** Phase 3's impulse
  solver runs the rigid world; XPBD runs the deformables; they meet at the
  contact. Phase 9's two-way cube-on-stack coupling is that mechanism carrying
  its first *volumetric* load — cloth (Phase 8) already proved the codimensional
  case.
- **Particles + pool (Phase 6).** `AQParticlePool` (SoA) holds positions,
  predicted positions, inverse masses, velocities. Tet-mesh nodes *are*
  particles in this pool; a soft body owns a contiguous range.
- **Self-collision spatial hash (Phase 8).** The surface-triangle self-collision
  of a soft body reuses Phase 8's spatial-hash machinery unchanged; only the
  *proxy* (which triangles participate — the surface hull, §11) is new.
- **Collision against rigid shapes (Phase 2/3).** `AQShape`, `AQshapeSupport`,
  `AQAABB`, the sort-based grid. Soft-body surface nodes collide against rigid
  shapes through the existing broadphase/narrowphase; the response is the
  hybrid coupling.
- **The compute substrate (Phase 5).** `AQComputeBackend`, `AQExecPath`, SoA +
  pooled buffers, `src/kernels/*.omegasl`, scan + radix sort, colored
  Gauss-Seidel, stable-cross-path determinism. Phase 9's constraint kernels are
  new `.omegasl` entry points dispatched through the same machinery.

**Out of scope here, by design:** auto-tetrahedralization of an arbitrary
surface mesh (kREATE/tooling cooks the tet mesh; §11.2 — we require a tet mesh
in); a full corotated-FEM path (constraint Neo-Hookean is the lead — §6, §11.1);
fracture as arbitrary re-meshing (tearing in the first cut is constraint/element
*splitting* along existing faces, not surface reconstruction — §5, §11.4);
fluids (Phase 6/later — SPH/PBF is a different discipline); the **Vertex Block
Descent** upgrade (SIGGRAPH 2024 — genuinely better for FEM solids, flagged as
the strong behind-the-interface follow-up in §12, deliberately *not* the lead
because it is not the Phase 7 shared core). The types are authored so the VBD
path can slot behind `AQSoftBodyHandle` later without changing the public
surface.

---

## 2. Why volumetric deformation is its own problem

A soft solid is not a stiff cloth, and it is not a rigid body with a squishy
skin. It is a different discipline with a different unknown, a different failure
mode, and a different definition of "correct."

1. **Volume is a constraint, not a consequence.** A rigid body preserves its
   shape by definition; a cloth has no interior volume to preserve. A solid must
   *actively* preserve volume — real flesh, rubber, and gel are nearly
   incompressible, and a soft body that loses volume under load reads instantly
   as fake (a deflating balloon). Volume preservation is a per-tet constraint
   (the signed tet volume must return to its rest value), and it is stiff — near
   incompressibility means near-infinite stiffness, which in XPBD means
   near-zero *compliance*, which is exactly the regime where a naive solver
   locks up or explodes. Getting the hydrostatic term stable at low compliance
   is the central numerical problem of the phase.

2. **Inversion is possible, and it is catastrophic if unguarded.** A cloth
   triangle can fold, but a tetrahedron can turn *inside-out* — its signed
   volume goes negative — and a constraint that assumes positive volume (divides
   by it, takes its square root, orients a gradient by it) produces a NaN or a
   force that drives the tet *further* inverted. This is the classic
   large-deformation FEM failure (Irving-Teran-Fedkiw 2004). A soft-body solver
   that cannot recover from inversion is a toy; one that recovers is a tool. The
   deliverable's squash-flat test (§1.4) exists precisely to force this case.

3. **Deformation is the deviatoric/hydrostatic split.** A solid's response
   decomposes into two physically distinct parts: *distortion* (shape change at
   constant volume — the deviatoric part, resisted by shear stiffness) and
   *dilation* (volume change — the hydrostatic part, resisted by bulk
   stiffness). A single "keep the tet near its rest shape" constraint conflates
   them and cannot express a material that is floppy-but-incompressible (jelly)
   versus stiff-but-compressible (foam). The two-constraint formulation (§6) is
   what buys independent control of shear and bulk — which is what "material"
   means here.

4. **The rest state must be captured exactly, once.** Every FEM-flavored
   constraint measures the *current* deformation gradient against a *reference*
   shape — the inverse rest-shape matrix `Dm⁻¹` and the rest volume. These are
   invariants computed at create time, and if the input mesh has a degenerate
   (zero-volume, sliver) tet, `Dm⁻¹` doesn't exist and every downstream step
   divides by zero. This is a create-time validation problem masquerading as a
   runtime stability problem, and it must be caught at ingestion, loudly, not at
   step 40 when the NaN finally surfaces (§9).

A correct Phase 9 handles all four at once, on a path that runs as XPBD
constraint projections over the SoA particle pool — coupled, at the contact, to
the Phase 3 rigid solver.

---

## 3. Prior art — how the incumbents solve it

Studied for the shape of the terrain and the failure modes, **not** to
transcribe (`Physics-Roadmap.md` §4). Descriptions are drawn from published
talks, papers, and library structure and are representative.

**PhysX 5 / FleX.** NVIDIA's soft-body story runs through two lineages that met
in PhysX 5. FleX ("Unified Particle Physics," Macklin et al. 2014) modeled
*everything* — cloth, fluid, and deformables — as particles plus constraints,
solved with a GPU-native position-based projection using **graph coloring** to
batch constraint updates without write conflicts, all GPU-resident. Soft solids
in FleX were shape-matching clusters over particle groups (the Müller 2005
lineage) — cheap, robust, GPU-friendly, and deliberately *not* high-accuracy
FEM. PhysX 5 then added a proper **FEM soft-body** path: tetrahedral meshes with
a co-rotational or Neo-Hookean constitutive model, GPU-solved, with a separate
*collision* tet mesh (coarse) from the *simulation* tet mesh — the coarse-hull
idea we adopt in §11. The through-line PhysX 5/FleX teaches: soft bodies are a
tet mesh (for accuracy) or particle clusters (for cheap robustness), solved on
the GPU with colored constraint batches, and you want a coarse collision proxy
separate from the fine simulation mesh. That is the exact shape of our §6/§11.

**Epic Chaos (Unreal).** Chaos ships a deformable/flesh solver (the "Chaos
Flesh" / deformable path) built on **XPBD over tetrahedral meshes** with a
stable Neo-Hookean-family constitutive model, aimed at character flesh and
muscle. It is candid about the same concerns we face: inversion safety under
large deformation, the cost of near-incompressibility, and the separation of a
simulation mesh from a render/collision mesh. Chaos is the closest incumbent to
*our* chosen algorithm — XPBD tets with a Neo-Hookean-family energy — and it
validates that this is a production-viable path, not a research curiosity. We
read Chaos mainly to confirm the constraint-XPBD-tet lane is the right one and
to note its determinism posture (deterministic-on-a-path, consistent with our
§8).

What they have in common, and what we adopt: **tetrahedral mesh + a
Neo-Hookean-family constitutive model + XPBD/position-based projection + colored
constraint batches + a coarse collision proxy separate from the sim mesh**, with
shape-matching kept as the cheap-robust fallback. What they accept that we need
not: a `float4`/SIMD-GPU heritage baked into their math, and (for PhysX/FleX) no
requirement to keep a *parity* CPU path — their GPU soft-body solver has no
serial oracle the way ours does (§5).

---

## 4. The literature we build on

The incumbents' behavior is the baseline; the literature is where the algorithm
comes from (`Physics-Roadmap.md` §4, recency principle).

- **Shape matching — Müller, Heidelberger, Teschner, Gross, "Meshless
  Deformations Based on Shape Matching" (SIGGRAPH 2005).** The fallback. A
  cluster of particles is pulled toward the best-fit rigid (or linear)
  transform of its rest configuration each step: unconditionally stable, no
  mesh required, cheap, and robust to any deformation including inversion — at
  the cost of accuracy and true volumetric behavior. This is AQUA's
  low-tet-count / non-mesh path (`AQShapeMatchingCluster`, §7).
- **Position Based Dynamics — Müller, Heidelberger, Hennix, Ratcliff (JVCI
  2007).** The projection framework the whole solver family sits on: constraints
  are functions `C(x)=0` projected by moving particles along `∇C` scaled by
  inverse mass. Phase 7's core *is* this, extended by XPBD.
- **XPBD — Macklin, Müller, Chentanez, "XPBD: Position-Based Simulation of
  Compliant Constrained Dynamics" (MIG 2016).** Adds *compliance* (α) so a
  constraint's stiffness is a real physical parameter independent of iteration
  count and timestep — the `compliance` field on `AQConstraint` (Phase 7). This
  is what makes "material stiffness" a number the artist sets rather than a
  side-effect of solver tuning. Every Phase 9 constraint carries its α.
- **Strain-based continuum PBD — Bender, Koschier, Charrier, Weber,
  "Position-based Simulation of Continuous Materials" (Computers & Graphics
  2014).** Recasts continuum-mechanics strain (the deformation gradient `F`) as
  position-based constraints on tets — the bridge from FEM's `F` to PBD's `C(x)`.
  This is the intellectual predecessor of the constraint we adopt: it shows how
  to express a distortion-energy constraint per tet in the PBD projection form.
- **Constraint-based Neo-Hookean — Macklin & Müller, "A Constraint-based
  Formulation of Stable Neo-Hookean Materials" (MIG 2021). The lead.** Takes the
  stable Neo-Hookean energy and expresses it as *two* XPBD constraints per tet:
  a **deviatoric/distortion** constraint (resisting shape change, the `∥F∥`
  term) and a **hydrostatic/volume** constraint (resisting volume change, the
  `det F` term), each with its own compliance. Crucially it is **inversion-safe
  by construction** — the constraint gradients are well-defined through
  inversion and drive an inverted tet back to positive volume rather than
  toward a singularity — which is exactly why it answers §2.2 without a separate
  invertible-FEM apparatus. This is *the* algorithm, and it composes with the
  Phase 7 core because it *is* two more `AQConstraintType`s.
- **Stable Neo-Hookean energy — Smith, De Goes, Kim, "Stable Neo-Hookean Flesh
  Simulation" (TOG 2018).** The constitutive energy behind Macklin 2021: a
  Neo-Hookean model reformulated to be well-behaved (finite forces, correct
  rest state) under extreme compression and inversion. We read it for *why* the
  constraint pair is inversion-safe and for the deviatoric/hydrostatic split's
  physical meaning.
- **Invertible finite elements — Irving, Teran, Fedkiw, "Invertible Finite
  Elements For Robust Simulation of Large Deformation" (SCA 2004).** The classic
  inversion-handling reference — diagonalize `F`, clamp/reflect the degenerate
  singular value, reconstruct forces so an inverted element is driven back out.
  We cite it as the problem statement (and the alternative apparatus) that
  Macklin 2021's constraint formulation lets us *avoid* building explicitly; if
  the constraint's built-in robustness ever proves insufficient (§11.3), this is
  the fallback we implement.
- **Corotated FEM via operator splitting — Kugelstadt, Koschier, Bender, "Fast
  Corotated FEM using Operator Splitting" (CGF 2018).** The corotated
  alternative to Neo-Hookean: split the elastic solve into a volume-conserving
  and a shear part, each cheap. Considered as the constitutive-model alternative
  (§6, §11.1); not the lead because corotation needs an explicit
  polar-decomposition per tet per step and does not carry Neo-Hookean's
  inversion story as cleanly.
- **FEM background — Sifakis & Barbič, "FEM Simulation of 3D Deformable Solids"
  (SIGGRAPH 2012 course).** The reference for the deformation gradient `F`, the
  reference-shape matrix `Dm`, strain energy density, and the first
  Piola-Kirchhoff stress — the vocabulary every constraint above is written in.
  Read for grounding, not adopted as a solver (it is implicit/Newton FEM, a
  different integrator than our XPBD).

**Throughline.** Shape matching (2005) → PBD (2007) → XPBD (2016) →
strain-based continuum PBD (2014) → constraint-based stable Neo-Hookean (2021),
grounded in the stable Neo-Hookean energy (2018) and the FEM background (2012),
with invertible FEM (2004) as the named-and-avoided inversion apparatus. AQUA
takes the **2021 constraint-Neo-Hookean** as the lead (it *is* two Phase 7
constraints) and **2005 shape-matching** as the fallback.

**Recency audit (per the standing §4 principle).** Is there a *newer* deformable
solver that beats constraint-Neo-Hookean XPBD for AQUA's substrate? The audit
(full detail §12): **Vertex Block Descent (Chen, Macklin et al., SIGGRAPH
2024)** is genuinely newer, more stable, and more GPU-parallel *as an FEM
solver* — but it is a different integrator, not a Phase 7 constraint kind, and
adopting it now would fork the shared XPBD core and the HYBRID rigid coupling
before either has carried a volumetric load. So for *this* phase constraint-
Neo-Hookean XPBD is substrate-correct; VBD is flagged as the strong Phase 9.x
upgrade behind the `AQSoftBodyHandle` interface.

---

## 5. Where AQUA diverges — the openings

The incumbents' constraints are not ours; each divergence is an opening
(`Physics-Roadmap.md` §4 step 2).

- **We keep a CPU path at parity; PhysX/FleX/Chaos-GPU do not.** Their GPU
  soft-body solver has no serial oracle. AQUA's CPU path is the *reference the
  GPU is measured against* (§1), and because the CPU path is held to analytic
  numbers (rest volume by determinant sum, analytic added-weight, RMS-to-rest
  recovery), we get a two-level correctness ladder for free — GPU vs CPU vs
  math. A constraint sign error or a mis-scaled compliance shows up the instant
  the GPU diverges from a known-good serial answer.
- **The soft-body solver *is* the Phase 7 core — two more constraint kinds.**
  This is the biggest opening the phased architecture buys. Macklin 2021 is
  literally a deviatoric `AQConstraintType` plus a hydrostatic
  `AQConstraintType`; they slot into the existing `AQConstraintBatch`, get
  colored by the existing coloring, and are projected by the existing substep
  loop. There is no separate soft-body solver to write, verify, or keep in sync
  — which is exactly why Phase 7 was built before Phase 8/9 and why cloth and
  solids share a solver. The cost we pay for this is that the constraint must be
  expressible in the XPBD projection form (it is — that is Macklin 2021's whole
  contribution).
- **Single math source over `Ty` → CPU/GPU operation-order match is
  achievable.** The tet constraint's `F = Ds · Dm⁻¹`, its determinant, its
  gradient — all written once over `Ty`, instantiated at `float`, `double`
  (oracle), and as the OmegaSL kernel (a third instantiation of the same scalar
  recipe). The tolerance gap then comes only from the colored traversal and
  float reassociation, both bounded and measured (§8).
- **Borrowed `Matrix`/`Quaternion` fit soft-body math natively.** A deformation
  gradient `F` and the reference matrix `Dm⁻¹` are 3×3 matrices — a natural use
  of OmegaGTE's `OmegaGTE::Matrix` from `GTEMath.h`, and the shape-matching
  best-fit transform is a polar decomposition on the same type. We do not
  re-derive a matrix library for soft bodies; we borrow the graphics engine's,
  and own only `AQVec3<Ty>`/`AQVec3f` for node positions (per house convention).
- **Coupling is already hybrid, and coupling a *volume* is the new bit.** Phase
  8 coupled a codimensional surface to rigids; Phase 9 couples a solid that has
  interior mass. The opening: because the coupling meets at the *contact* (§6),
  a soft body pressing on a rigid stack is the same contact-impulse exchange the
  rigid solver already does — the soft node contributes its inverse mass to the
  contact, the rigid body its inverse mass + inertia, and the Phase 3 machinery
  resolves it. We are not inventing a soft-rigid coupling; we are feeding the
  soft body's surface nodes into the coupling Phase 7 §11 already defined.

---

## 6. Proposed algorithm — tetrahedral XPBD (constraint-based Neo-Hookean) with a shape-matching fallback

The lead is **constraint-based stable Neo-Hookean XPBD over tetrahedra**
(Macklin & Müller 2021), with **shape matching** (Müller et al. 2005) as the
fallback for low-tet-count or non-mesh blobs. Both run inside the Phase 7
substep loop; neither is a new solver.

**Precompute (create time, once per soft body).** For each tet with rest node
positions `X0..X3`:
```
Dm      = [X1-X0 | X2-X0 | X3-X0]        // 3x3 reference edge matrix (borrowed Matrix)
restVol = det(Dm) / 6
if restVol < ε_slivers: REJECT the mesh, loud (§9) — a degenerate rest tet has no Dm⁻¹
DmInv   = inverse(Dm)                     // the material→world map every constraint reads
store per-tet: DmInv, restVol
```

**Per-substep, per-tet, the two constraints (the heart of the phase).** For a
tet with current predicted positions `x0..x3`:
```
Ds = [x1-x0 | x2-x0 | x3-x0]              // current edge matrix
F  = Ds * DmInv                           // deformation gradient (3x3)

// (a) HYDROSTATIC / volume constraint — resists det(F) != 1 (volume change)
C_h      = det(F) - (1 + µ_h/λ_h)         // Macklin 2021 rest-offset; ≈ det(F)-1
gradients ∇C_h w.r.t. each node from d(det F)/dx  (cross-products of F columns)
project with compliance α_h = 1/(bulkStiffness · restVol)      // stiff → small α_h

// (b) DEVIATORIC / distortion constraint — resists shape change at fixed volume
C_d      = sqrt(trace(Fᵀ F))              // Frobenius norm of F (the shear term)
gradients ∇C_d = F * DmInvᵀ / C_d  scattered to nodes
project with compliance α_d = 1/(shearStiffness · restVol)
```
Both are ordinary XPBD constraint projections: compute `C` and its per-node
gradients, form `Δλ = (−C − α·λ) / (Σ wᵢ ∥∇Cᵢ∥² + α)`, move each node by
`Δxᵢ = wᵢ · ∇Cᵢ · Δλ`, accumulate `λ`. **Inversion safety is built in** — when a
tet inverts, `det(F)` goes negative and `C_h` becomes large-negative; its
gradient points *back toward positive volume*, so the hydrostatic projection
drives the tet back out rather than toward a singularity (the Smith-De Goes-Kim
2018 / Macklin 2021 property that makes §1.4 pass without an explicit
Irving-Teran-Fedkiw diagonalization). The one guard we still keep loud: if
`C_d`'s denominator `sqrt(trace(FᵀF))` underflows toward zero (a fully collapsed
tet), we clamp it and emit an `AQDebugSoftBodyStrain` warning rather than divide
by zero (§9).

**Batching & coloring (reuse Phase 7).** Each tet contributes two constraints
touching its four nodes; two constraints sharing a node cannot be in the same
color. The existing `AQConstraintBatch` greedy coloring partitions them exactly
as it does distance/bending constraints — no new coloring logic, and the
adjacency is the tet's four node indices. Colored dispatch → GPU parallel per
color → sequential over colors recovers Gauss-Seidel coupling (Phase 5 §6.F, the
same machinery).

**Substep loop (reuse Phase 7, unchanged).** For each of the `n` substeps:
predict positions, project all constraints once (Distance/Bending from cloth if
present, plus the new tet constraints, plus contacts), update velocities from
the position change. The soft body's node velocities fall out of the XPBD
position update; there is no separate velocity solve.

**Two-way coupling with rigid + cloth (the §1 deliverable).** The soft body's
*surface* nodes participate in collision:
- Against **rigids** (Phase 2/3): surface nodes generate contacts against
  `AQShape`s through the existing broadphase/narrowphase; the contact is a
  constraint that moves the soft node *and* applies the equal-opposite impulse
  to the rigid body (HYBRID coupling, Phase 7 §11) — this is how the cube's
  weight reaches the rigid box below and the box's reaction reaches the cube.
- Against **cloth** (Phase 8): soft-surface vs cloth self-collision reuses the
  Phase 8 spatial hash; the response is a mutual position projection. Two-way by
  construction.

**Shape-matching fallback (Müller et al. 2005).** For a soft body flagged
`ShapeMatching` (low tet count, non-mesh, or "cheap and unconditionally stable"
requested):
```
per cluster, per substep:
  cm      = Σ mᵢ xᵢ / Σ mᵢ                 // current center of mass
  Apq     = Σ mᵢ (xᵢ - cm)(x0ᵢ - cm0)ᵀ     // moment matrix vs rest
  R       = polar_decomp(Apq)              // best-fit rotation (borrowed Matrix)
  goalᵢ   = R (x0ᵢ - cm0) + cm             // where node i "should" be
  xᵢ     += β (goalᵢ - xᵢ)                  // pull toward goal, stiffness β
```
Unconditionally stable, inversion-proof, cheap, no `Dm⁻¹`, no per-tet work — the
robust-but-less-accurate path. Volume preservation is approximate (linear
allowed via the `Apq`-based variant), which is why it is the *fallback*, not the
lead.

**Optional plasticity (deferred by lean — §11.4, sketched).** Permanent set past
a yield threshold is a *rest-state update*: when a tet's strain exceeds
`yield`, blend its stored `Dm⁻¹` toward the current deformed shape (the classic
plastic-flow update), capped by `maxPlasticStrain`. The elastic constraint then
recovers to the *new* rest shape — permanent deformation. Because it only mutates
the per-tet precompute, it needs no solver change; it is deferred so the thin
slice (elastic only) lands first.

**Optional tearing (deferred by lean — §11.4, sketched).** Split a constraint
(or duplicate a node and split the incident tets) when strain exceeds a
`tearThreshold` along a face. In the first cut this is *element splitting along
existing faces*, not surface reconstruction — a topology edit to the constraint
batch and pool, gated behind a flag, deferred with plasticity.

**Why this combination.** The lead constraint *is* the Phase 7 core (§5) — the
cheapest possible thing that is also the best-available algorithm (Macklin
2021), inversion-safe for free, with independent shear/bulk control the
deliverable's material behavior needs. Shape matching covers the case where a
tet mesh is overkill or unavailable, at the cost of accuracy we accept for that
regime. Nothing here is a new solver; it is two constraint kinds and a fallback
projector, dispatched through machinery Phases 5–8 already built.

**Alternative considered — corotated FEM (Kugelstadt et al. 2018) as the lead.**
Split the solve into volume + shear, each cheap; well-understood, production-
proven. Rejected as the lead because it needs an explicit polar decomposition
per tet per step (extra work and an extra numerical seam), does not carry
Neo-Hookean's inversion-safety as cleanly (corotation's rotation extraction is
itself fragile under inversion), and — decisively — it is *not* a Phase 7
constraint kind, so it would not compose with the shared core the way the
constraint-Neo-Hookean pair does. Kept as the §11.1 constitutive alternative if
Neo-Hookean tuning proves unwieldy.

**Alternative considered — pure shape-matching as the lead (skip tets
entirely).** Simplest possible, unconditionally stable, and it *is* how FleX did
soft bodies. Rejected as the lead because it cannot preserve volume accurately
(§1.3 would fail) and cannot express distinct shear/bulk materials (§2.3) — it
is a blob model, not a solid model. It is exactly right as the *fallback*, which
is where it lives (§6, §11.1).

---

## 7. New types AQUA must add — `include/aqua/AQSoftBody.h` (draft)

All AQ-prefixed, no namespace (per `aqua/AGENTS.md`); POD / trivially-copyable /
standard-layout, GPU-uploadable, no virtuals on the kernel path. Backend/OmegaSL
types stay behind the pimpl (no OmegaSL types in `include/aqua/*`). 3×3 matrices
borrow `OmegaGTE::Matrix`; node positions use `AQVec3f`.

```cpp
#pragma once
#include <cstdint>
#include "AQVec3.h"          // AQUA-owned AQVec3<Ty> / AQVec3f
#include "AQXPBD.h"          // AQConstraintType, AQConstraint, AQConstraintBatch (Phase 7)
#include <omegaGTE/GTEMath.h> // OmegaGTE::Matrix, OmegaGTE::FVec<3> (borrowed 3x3)

/// How a soft body is solved. Tetrahedral is the lead (constraint Neo-Hookean);
/// ShapeMatching is the cheap, unconditionally-stable fallback (Müller 2005).
enum class AQSoftBodyModel : std::uint8_t {
    Tetrahedral,    ///< constraint-based Neo-Hookean XPBD over tets (lead, §6)
    ShapeMatching,  ///< best-fit-transform clusters — cheap, robust, less accurate
};

/// New XPBD constraint kinds this phase adds to AQConstraintType (Phase 7).
/// The deviatoric/hydrostatic pair of Macklin & Müller 2021. Each is an
/// ordinary AQConstraint carrying indices, rest value, compliance, lambda —
/// nothing about the solver loop changes.
enum class AQTetConstraintKind : std::uint8_t {
    Deviatoric,     ///< resists distortion (shape change) — shear stiffness → α_d
    Hydrostatic,    ///< resists dilation (volume change) — bulk stiffness → α_h
};

/// One tetrahedron: four node indices into the particle pool + rest invariants
/// computed once at create time. POD, GPU-uploadable.
struct AQTetConstraint {
    std::uint32_t node[4];          ///< indices into AQParticlePool (Phase 6)
    OmegaGTE::Matrix DmInv;         ///< inverse rest edge matrix (material→world), 3x3 borrowed
    float restVolume;               ///< det(Dm)/6, > ε (degenerate rejected at create)
    float lambdaDev;                ///< accumulated deviatoric multiplier (XPBD)
    float lambdaHyd;                ///< accumulated hydrostatic multiplier (XPBD)
};

/// A shape-matching cluster (fallback path, Müller 2005): particle group +
/// its rest configuration. POD; the runtime best-fit R is transient, not stored.
struct AQShapeMatchingCluster {
    std::uint32_t first;            ///< first node index in the pool
    std::uint32_t count;            ///< node count in this cluster
    AQVec3f restCom;                ///< rest center of mass (cm0)
    float stiffness;                ///< pull-to-goal β ∈ (0,1]
};

/// Per-material parameters. Deviatoric and volumetric are *separate* stiffnesses
/// (the §2.3 split); expressed as XPBD compliance internally (α = 1/(k·restVol)).
struct AQSoftMaterial {
    float shearStiffness;           ///< → deviatoric compliance α_d
    float bulkStiffness;            ///< → hydrostatic compliance α_h (stiff ⇒ near-incompressible)
    float damping;                  ///< XPBD velocity damping
    float yieldStrain;              ///< plasticity threshold; 0 ⇒ purely elastic (Phase 9.x)
    float maxPlasticStrain;         ///< cap on permanent set (ignored if yieldStrain == 0)
};

/// Ingestion contract: a cooked tet mesh + material. kREATE/tooling produces
/// this (auto-tetrahedralization is out of scope, §11.2). Node positions seed
/// the particle pool; tets become AQTetConstraints.
struct AQSoftBodyDesc {
    AQSoftBodyModel      model;
    const AQVec3f*       nodePositions;  ///< rest node positions
    std::uint32_t        nodeCount;
    const std::uint32_t* tetIndices;     ///< 4 · tetCount indices (Tetrahedral)
    std::uint32_t        tetCount;
    AQSoftMaterial       material;
    float                totalMass;       ///< distributed to nodes by incident rest volume
    // ShapeMatching path: clusters instead of tets (one or many).
    const AQShapeMatchingCluster* clusters;
    std::uint32_t                 clusterCount;
};

/// Opaque handle to a created soft body (indices into pool + a constraint batch).
struct AQSoftBodyHandle { std::uint32_t id; };
```

`AQSpace` gains the named-ctor factory (public API, §10):
```cpp
// Validates the mesh (rejects sliver tets, loud — §9), seeds the pool, builds
// the tet/hydrostatic constraints (or shape-matching clusters), colors them into
// the existing AQConstraintBatch. Returns an invalid handle on a degenerate mesh.
AQSoftBodyHandle AQSpace::createSoftBody(const AQSoftBodyDesc& desc);
```

The runtime tet-constraint projection and the shape-matching projector live in
`src/AQSoftBodySolver.cpp` (CPU reference) and `src/kernels/AQSoftBody.omegasl`
(GPU) — neither in `include/aqua/*`.

---

## 8. Data layout & GPU/numeric specialization

- **Nodes are pool particles; the soft body is indices + a constraint batch.**
  No new per-node storage — `AQParticlePool` (Phase 6, SoA) already holds
  position, predicted position, inverse mass, velocity. A soft body owns a
  contiguous node range and a set of `AQTetConstraint`s (or clusters). The
  constraints upload as one coalesced buffer (`AQTetConstraint` is POD;
  `DmInv`'s nine floats are contiguous — the same `OmegaGTE::Matrix` column-major
  storage the Phase 1.1 note flagged, written explicitly at gather to avoid
  relying on it).
- **Per-tet rest invariants are computed once, stored, never recomputed.**
  `DmInv` and `restVolume` are create-time constants read by every substep. This
  is the single most important data-layout decision for correctness: the
  constraint measures *against* these, so they must be exact and immutable
  (except when plasticity deliberately mutates `DmInv` — §6, §11.4).
- **Deviatoric/hydrostatic constraints color exactly like distance/bending.**
  The coloring adjacency is the tet's four node indices; the existing
  `AQConstraintBatch` handles it with no new logic. Within a color no two
  constraints share a node → the position writes are disjoint → **no
  order-dependent float atomics in the projection** → a single GPU path is
  run-to-run bitwise-deterministic (Phase 5 §8, inherited unchanged).
- **Determinism — stable across paths, bitwise within a path (Phase 5 §8,
  inherited).** Within one path on one device: bitwise-deterministic (colored
  projection, no atomics, fixed operation order for `F`, `det F`, and the
  gradient scatter, shared with the CPU recipe). Across CPU↔GPU: stable and
  tolerance-equivalent, not bitwise (reassociation/FMA + colored-traversal
  order), the same `1e-4`-class band the earlier phases use, relaxed by a
  *measured* factor for the accumulated-λ fields and pinned as a regression
  constant.
- **Numeric format — `float` on the step path, `double` oracle.** The port
  matches the CPU `float` path; the `double` instantiation is the precision
  oracle only. One soft-body-specific note: `det(F)` and the deviatoric
  denominator lose precision on near-collapsed tets, so those two quantities are
  computed in the constraint's working precision (the instantiation's `Ty`) and
  guarded (§9) rather than special-cased to `double` on the `float` path (which
  would widen the parity gap).
- **Buffer residency & pooling (Phase 5, inherited).** Tet constraints, cluster
  records, and the per-tet scratch are `GPUOnly` pooled buffers grown
  geometrically by `ensureCapacity`; no per-substep allocation. The soft body's
  surface-node → contact handoff reuses the Phase 8 self-collision spatial hash
  buffers.
- **New debug bits.** The debug bus exists (Phase 1.1+); Phase 9 *adds*
  `AQDebugTetMesh` (draw the tet wireframe / surface) and `AQDebugSoftBodyStrain`
  (per-tet strain heatmap, and the loud marker for a tet that inverted or a
  collapsed-denominator clamp) into the existing `drainDebugLines` buffer — no
  new transport.

---

## 9. Validation — how we measure "better"

The reference is the CPU path, itself held to analytic oracles
(`Physics-Roadmap.md` §4). "Better" here means *a solid that squishes, preserves
volume, recovers, and survives inversion — proven against numbers we derive by
hand*.

- **The cube-on-stack headline.** §1 deliverable #1: the cube deforms, the boxes
  react, the top box's accumulated normal impulse reflects `m_cube · g` within
  5%, and total energy is non-increasing. The analytic added-weight is the
  hand-derivable number.
- **Elastic recovery.** §1 deliverable #2: compress to ~60% height, release,
  per-node RMS-to-rest `< 2%` of edge length after 2 s. A putty model or a
  mis-signed constraint fails this — the assertion *is* "it is a solid."
- **Volume preservation.** §1 deliverable #3: summed signed tet volume within
  ±3% of rest volume under compression. Rest volume computed the slow, obvious
  way (sum of `det/6`), and the constraint held to it.
- **Inversion recovery (the classic failure, tested on purpose).** §1
  deliverable #4: squash flat past inversion, hold, release; every tet's signed
  volume returns positive within N substeps and the mesh recovers to rest. This
  is the assertion that proves the constraint's inversion-safety is real, not
  claimed. A tet that stays inverted, or a NaN that reaches the pool, is a hard
  failure.
- **Create-time mesh validation.** A `AQSoftBodyDesc` with a sliver (near-zero
  rest-volume) tet is *rejected at create*, loud, returning an invalid handle —
  asserted directly, so a degenerate mesh never reaches the step loop to
  produce a NaN there (§2.4). This is the 3am-engineer guard: fail at ingestion
  with "tet 417 has rest volume 3e-9, mesh rejected," not at step 40 with a NaN
  of unknown origin.
- **CPU↔GPU parity.** The same soft-body scenes on both paths agree per-substep
  within the §8 tolerance band (tight on node state, measured-relaxed on
  accumulated λ), pinned as regression constants. The stage-isolation posture of
  Phase 5 §9 carries over: a tet-constraint-only parity test (CPU vs GPU on a
  fixed mesh, one substep) localizes a failure to the constraint kernel, not
  "the soft body is wrong."
- **Shape-matching fallback.** The fallback path runs the same cube (as clusters)
  and must be *stable* under the same squash-flat load (it cannot fail
  inversion — that is its selling point) and recover approximately, at a
  documented lower accuracy than the tet path.
- **Determinism.** Two GPU runs → byte-identical node/λ buffers each substep
  (Phase 5 §8, inherited). CPU re-run determinism continues to pass.
- **Coupling regression.** The Phase 3 rigid battery and Phase 8 cloth battery
  stay green — the soft body must not perturb the rigid stack's settling or the
  cloth's drape when no soft body is present, and the cube-on-stack must not
  inject energy into the rigid world.

Metrics emitted as debug-draw / logged series (roadmap §3 principle 6, "author
for the 3am engineer"): per-tet strain (heatmap via `AQDebugSoftBodyStrain`), a
loud marker each substep a tet inverts or a deviatoric denominator is clamped
(a shower of these means the mesh or the timestep is wrong — surface it, don't
swallow it), summed-volume-vs-rest per step, per-node RMS-to-rest during a
recovery test, and a post-substep NaN/inf readback guard on the soft body's node
range (a soft node that goes non-finite must fail loud, not spread NaN through
the shared pool — the same discipline Phase 5 §9 applies to rigid state).

---

## 10. Public API additions

Minimal, matching the phased convention that new physics is new *types + a
factory*, not a new solver surface. All additive; nothing existing changes
shape.

- **`AQSpace::createSoftBody(const AQSoftBodyDesc&)`** — the named-ctor factory
  (§7). Validates the mesh (rejects slivers, loud), seeds the pool, builds and
  colors the constraints, returns an `AQSoftBodyHandle` (invalid on a degenerate
  mesh). This is the whole ingestion surface.
- **`AQSoftBodyDesc` / `AQSoftMaterial` / `AQSoftBodyModel` /
  `AQTetConstraint` / `AQShapeMatchingCluster` / `AQSoftBodyHandle`** — the new
  public POD types (§7). `AQSoftMaterial` is where an artist sets shear vs bulk
  stiffness, damping, and (later) yield.
- **New `AQConstraintType` values (Deviatoric, Hydrostatic)** on the Phase 7
  enum — additive; existing constraint kinds and the solver loop are untouched.
- **New debug bits** `AQDebugTetMesh`, `AQDebugSoftBodyStrain` on the existing
  `AQDebugFlags` — drained through the existing `drainDebugLines`.
- **No change to `AQRigidBody` / rigid shapes / joints / queries / cloth / the
  `advance` loop / execution-path control.** A soft body is added and stepped
  through the *same* `advance`; the tet constraints ride the *same* XPBD substep
  loop; the GPU path is selected by the *same* `AQExecPath` (Phase 5). The
  solver, coloring, and coupling are entirely behind the pimpl.

A short Sphinx note documents that AQUA now simulates volumetric soft bodies
from a cooked tet mesh, that the material is a shear/bulk stiffness pair, that
the tet path is the accurate lead and shape-matching the cheap-robust fallback,
and that the caller cooks the tet mesh (AQUA does not tetrahedralize) — the
user-facing version of §7's ingestion contract.

---

## 11. Open decisions for this phase

1. **Constitutive model — constraint-based Neo-Hookean (Macklin 2021) vs.
   corotated FEM (Kugelstadt 2018) vs. pure shape-matching (Müller 2005).**
   *Lean: constraint Neo-Hookean as the lead, shape-matching as the fallback,
   corotated FEM not built.* Neo-Hookean *is* a Phase 7 constraint kind
   (composes with the shared core), is inversion-safe by construction (answers
   §2.2 without extra apparatus), and gives the shear/bulk split the deliverable
   needs (§2.3). Corotated FEM is the §6 alternative if Neo-Hookean tuning
   proves unwieldy; pure shape-matching is the fallback, not the lead, because
   it cannot preserve volume accurately.
2. **Mesh source — require a cooked tet mesh in vs. auto-tetrahedralize a
   surface mesh.** *Lean: require a tet mesh in; kREATE/tooling cooks it;
   auto-tet deferred.* Tetrahedralization is a meshing problem (quality
   guarantees, sliver avoidance, feature preservation) that belongs in the asset
   pipeline, run offline with human-inspectable output, not at runtime in the
   physics engine. `AQSoftBodyDesc` takes nodes + tet indices; if runtime
   auto-tet is ever needed it is an additive helper, not a change to this
   contract.
3. **Inversion handling — rely on the constraint's built-in robustness vs.
   explicit invertible FEM (Irving-Teran-Fedkiw 2004).** *Lean: rely on the
   constraint formulation's built-in robustness, and guard loudly.* Macklin
   2021 / Smith 2018 make the hydrostatic gradient well-defined through
   inversion, which the §1.4 squash-flat test verifies. If profiles ever show
   the constraint alone does not recover a specific pathological mesh, the
   named-and-avoided ITF diagonalization (§4) is the fallback — implemented then,
   not speculatively now. Either way the collapsed-denominator clamp stays a
   loud debug marker, never a silent default.
4. **Plasticity & tearing — first cut or deferred.** *Lean: deferred; elastic
   first (the thin slice).* Both are additive (plasticity mutates the per-tet
   `Dm⁻¹`; tearing edits the constraint batch/pool topology) and neither changes
   the solver loop, so both can land as Phase 9.x behind flags once the elastic
   deliverable is proven. Shipping elastic-only first keeps the deliverable
   honest and the first cut small.
5. **Collision proxy — full tet-surface vs. a coarse collision hull.** *Lean:
   coarse hull for broadphase + surface for narrowphase.* PhysX 5's separation
   of a coarse *collision* mesh from the fine *simulation* mesh (§3) is the
   right pattern: broadphase against a coarse hull (cheap, few pairs), narrowphase
   against the actual surface triangles for accurate contact. The first cut may
   use the surface tris directly for a small mesh; the coarse-hull optimization
   is the follow-up when tet counts grow (logged, tuned on real scenes rather
   than guessed).

---

## 12. Recency-principle audit (addendum, 2026-07-01)

Roadmap §4 makes "newest viable algorithm from the literature" the standing
default for every phase, with incumbents/older work adopted only when no
substantively-newer alternative offers a real improvement *for AQUA's substrate*
(`Physics-Roadmap.md` §4 — "Recency principle"). The Phase 9 picks —
constraint-based Neo-Hookean XPBD (Macklin & Müller 2021, on Smith-De Goes-Kim
2018), shape-matching fallback (Müller 2005), with corotated FEM (Kugelstadt
2018) and invertible FEM (Irving-Teran-Fedkiw 2004) as named alternatives —
span a 2004–2021 line. The lead is already the newest *constraint-form*
deformable model. What do the last five years add on top?

- **Vertex Block Descent — Chen, Macklin, Kim, Erleben, et al. (SIGGRAPH 2024).
  Genuinely newer, more stable, more GPU-parallel. The strong upgrade — flagged,
  deferred to Phase 9.x, NOT the lead.** VBD is a block-coordinate-descent
  integrator for FEM solids (and more): it minimizes the implicit-integration
  energy vertex-block by vertex-block, is unconditionally stable at large
  timesteps, converges faster than PBD/XPBD on stiff materials, and is
  massively GPU-parallel (vertex blocks colored like our constraints). On the
  merits it is the best-available deformable solver in 2026 and a real
  improvement over constraint-XPBD for high-stiffness flesh. It is deferred *for
  this phase* for one decisive, substrate-specific reason: **it is a different
  integrator, not a Phase 7 constraint kind.** Adopting VBD now forks the shared
  XPBD core (which cloth, hair, and — via HYBRID — rigid coupling all sit on)
  and re-litigates the roadmap's solver architecture before soft bodies have
  carried a single volumetric load. Constraint-Neo-Hookean XPBD ships *inside*
  the existing solver, proves the deliverable, and — because the public surface
  is `AQSoftBodyHandle` + `AQSoftBodyDesc`, not a solver — VBD can slot behind
  that interface as a `AQSoftBodyModel::VertexBlockDescent` path in Phase 9.x
  without changing a line of caller code. This is the recency principle working
  as intended: *name the newer-and-better thing, adopt it behind the interface
  when the substrate is ready, don't fork the core to chase it early.*
- **Complementarity / offset constraints — Macklin et al. (2020–2021, the same
  line as the lead). Surveyed, adopted implicitly.** The rest-offset term in the
  hydrostatic constraint (`1 + µ_h/λ_h`, §6) and the general XPBD compliance
  treatment come from exactly this thread; the lead is already downstream of it.
  No separate adoption — it *is* Macklin 2021.
- **FEM-on-GPU + XPBD unification (the broad 2020–2024 trend).** The field has
  been converging FEM soft-body solvers and position-based methods onto the GPU
  with colored/block-parallel projection — PhysX 5's FEM path, Chaos Flesh, and
  VBD are all instances. AQUA is *already* on the unified side of this trend:
  Phase 7 made cloth, hair, and now solids share one XPBD solver over one
  particle pool with one coloring. The trend confirms the architecture; it does
  not demand a new algorithm this phase.
- **Mesh-barrier / IPC contact (Li et al. 2020 and successors).** Barrier-
  potential contact for deforming meshes is the accurate-but-heavy end of
  soft-body *contact*. Not adopted: AQUA's coupling meets at the HYBRID contact
  (§6), and the deliverable's cube-on-stack closes on the analytic added-weight
  without a barrier model. Same conclusion the Phase 3/4 audits reached on IPC —
  flagged, revisit only if self-intersection artifacts surface in production
  soft-vs-soft contact.

**Net for Phase 9:** the audit returns **no adopt-now finding** — the lead
(constraint-Neo-Hookean XPBD) is already the newest constraint-form model and
the correct one for the shared Phase 7 core — plus **one strong flagged upgrade
(Vertex Block Descent, Chen et al. 2024)** deferred to Phase 9.x behind the
`AQSoftBodyHandle` interface, because adopting it now would fork the shared
solver before the deliverable is proven. Constraint-Neo-Hookean tet-XPBD +
shape-matching fallback is **substrate-correct**; VBD is the named,
interface-ready successor.

Re-audit due: 2028-07-01 (roadmap §4 two-year freshness rule) or sooner if the
VBD path is scheduled or a production flesh scene surfaces a stiffness regime
constraint-XPBD cannot reach.

---

*Brief status: proposal. Decisions in §11 — above all the constitutive model
(#1, the constraint-Neo-Hookean lead + shape-matching fallback) and the
inversion-handling stance (#3, rely on the constraint's built-in robustness,
guard loudly) — should be settled before the solver constraints land. This
document is the Phase 9 entry of the per-phase prior-art series roadmap §4
establishes, and follows the conventions set by `Phase-1-Dynamics-Math-Core.md`
through `Phase-8`. The Vertex Block Descent upgrade (§12) is explicitly out of
scope here — the `AQSoftBodyHandle` / `AQSoftBodyDesc` surface in §7 keeps that
fork open behind the interface without prejudging it.*
