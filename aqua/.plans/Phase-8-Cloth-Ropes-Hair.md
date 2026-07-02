# AQUA Phase 8 — Cloth, Ropes & Hair

**Prior-art brief & proposal.**

---

## 1. Scope & deliverable

**Goal.** Ship AQUA's first deformables — 1D ropes, 2D cloth, and 1D **hair** (many rooted strands) — all riding the Phase 7 XPBD substrate. This is the phase where AQUA stops being a rigid-body-and-particles engine and starts being able to dress a character, hang a bridge, and grow a head of hair. Ropes and cloth are the obvious two; hair is deliberately the third, because a hair strand is nothing more than a rooted, inextensible, bend-stiff chain — the natural sibling of a rope on the same constraint core. Solving them together is cheaper and cleaner than solving them apart.

**Runnable deliverable.** Three scenes, one headless test binary (`aqua_deformables_test`), each with an obviously-correct oracle computed in `double`:

- **Cloth drape.** A 64×64 cloth sheet pinned at two adjacent corners, released over a Phase 2 `AQShape` sphere resting below it. Oracle: after settling, the sheet drapes into a plausible, left-right-**symmetric** fold about the sphere's vertical plane (symmetry error under 1e-3 m on mirror-paired particles, since the scene is symmetric); total mechanical energy is monotonically non-increasing after the first substep (no spurious energy gain, no explosion — `‖v‖∞` stays bounded); no cloth particle penetrates the sphere by more than the collision margin.
- **Arbitrary rope/chain.** The Phase 7 straight rope generalized to a branching chain: a Y-shaped rope pinned at one end, two free ends. Oracle: with `compliance = 0` every segment length stays within 1e-4·rest of its rest length (inextensibility), and the pinned particle sits **exactly** at its pin every frame (bit-exact, since pins are positional assignments, not constraints).
- **Head of hair.** Tens of thousands of rooted strands (target 32k guide strands × 16 segments) grown from a scalp collider, with bending/curl stiffness and strand–strand friction, settling under gravity, then swept by a moving capsule collider. Oracle: (a) each strand's total arc length stays within tolerance of rest (FTL inextensibility); (b) the hair **volume does not collapse to a line** — the covariance of particle positions retains meaningful spread orthogonal to gravity (loft preserved by strand friction/cohesion); (c) no strand particle passes through the scalp collider; (d) roots stay bit-exact on their kinematic binding.

**Included groundwork.** The new `AQConstraintType`s (Bending for cloth, curvature/curl for hair) on the Phase 7 core; the Follow-The-Leader (FTL) pass for hair inextensibility; strand–strand friction/cohesion via the Phase 6 spatial hash; pinning (attach particles/roots to kinematic/rigid bodies or world points); basic self-collision; the `AQClothDesc`/`AQStrandDesc`/`AQHairDesc` authoring structs and the `AQSpace::createCloth/createRope/createHair` factories; new debug bus bits.

**What earlier phases have already closed for us.**

- **Phase 7** gave us the XPBD core outright — `AQXPBDSolver`, `AQConstraint` (POD, with compliance and per-constraint lambda accumulator), `AQDistanceConstraint`, `AQConstraintBatch` (graph coloring), the substep loop (n substeps × 1 iteration, Macklin 2019), and the settled **hybrid** decision: Phase 3 impulse solver for rigid, XPBD for deformables/particles, coupled at contact. Phase 8 does not touch the loop; it *adds constraint types* and *authoring* on top.
- **Phase 6** gave us `AQParticlePool` (SoA positions/velocities/inverse-mass) and the sort-based grid neighbor search. Cloth, rope, and hair particles all live in this pool; self-collision reuses the grid.
- **Phase 5** gave us the compute substrate — `AQComputeBackend`, `AQExecPath`, pooled GTE buffers, `src/kernels/*.omegasl`, scan+radix sort, colored Gauss-Seidel, and stable-cross-path determinism.
- **Phase 3** gave us the impulse rigid solver (`AQContactManifold`, `AQConstraintRow`) that our deformable↔rigid contacts meet.
- **Phase 2** gave us `AQShape` (sphere/box/capsule/plane/hull), `AQshapeSupport`, `AQAABB<Ty>`/`FAABB`, `AQBroadphasePair`, and the uniform grid. Cloth and hair collide against these.

**Out of scope, by design.**

- **Twist.** No twist DOF this phase. Discrete Elastic Rods (Bergou 2008) is the model that captures twist correctly; it is flagged for **Phase 8.x** (ponytails, curled fur) and named explicitly in §5 and §12. Phase 8 hair holds *style* via a rest-curvature term, not via twist.
- **Volumetric soft bodies / FEM tetrahedra.** That is **Phase 9** (Soft body II). Phase 8's `AQConstraintType::Volume` slot exists on the Phase 7 enum but is not populated here.
- **Cloth tearing, plasticity, aerodynamics/wind fields.** Deferred; wind can be faked by the caller as a per-particle force, but there is no first-class wind system.
- **Render-time strand interpolation.** AQUA simulates *guide* strands only; multiplying guides into rendered hair is **kREATE's** job (§11).
- **Continuous collision (CCD) for cloth-vs-cloth.** Self-collision this phase is discrete and conservative (§6). Robust CCD is a later hardening pass.

---

## 2. Why cloth, ropes, and hair are one problem with three shapes

1. **They are all particle systems joined by constraints.** A rope is a 1D chain of distance constraints. A cloth is a 2D grid of the same distance constraints plus a bending term. A hair strand is a 1D chain — a rope with a root pin and a curvature term. The Phase 7 XPBD core already solves "particles connected by constraints"; Phase 8 is mostly a *constraint-authoring* problem, not a new-solver problem.

2. **Inextensibility is the shared hard part.** Cloth that stretches looks like rubber; rope that stretches looks like an elastic band; hair that stretches looks wrong instantly because the human eye knows hair length. All three want *near-inextensible* behavior, and stiff distance constraints under explicit integration are the classic instability. XPBD's compliance formulation (Macklin 2016) makes stiffness a physical, timestep-independent quantity; FTL (Müller 2012) sidesteps the stiffness problem entirely for 1D chains. Both tools serve all three shapes.

3. **They share a collision story.** Every deformable needs (a) collision against Phase 2 rigid shapes, (b) two-way coupling back onto rigid bodies (per the Phase 7 hybrid), and (c) self-collision via the Phase 6 spatial hash. Writing this machinery once for "deformable particles" covers cloth, rope, and hair. The only divergence is granularity: cloth self-collides particle-vs-grid; hair self-collides segment-vs-segment (§6).

4. **They share the data layout.** All three are SoA particle chains in `AQParticlePool` with an index topology (edges for rope/hair, an edge+diagonal mesh for cloth). One GPU-uploadable layout, one set of kernels, three authoring front-ends.

5. **Bundling them exposes the seams early.** If we shipped cloth alone, we'd design a bending model and a pin system in a vacuum and then find hair needs a *different* bending model (curvature, not dihedral) and a *root* pin. Doing all three now forces the constraint interface to be general on the first try — which is exactly the Phase 7 promise (new `AQConstraintType`s behind an unchanged solver interface).

---

## 3. Prior art — how the incumbents solve it

**PhysX 5 / FleX.** NVIDIA FleX is the position-based unified solver — cloth, ropes, inflatables, and fluids all as particles-plus-constraints, exactly the lineage AQUA sits in. Cloth is distance + tether + bending constraints on a triangle mesh; ropes are distance-constraint chains; everything solves with position-based Gauss-Seidel/Jacobi iterations. PhysX 5 folded FleX's deformables into the mainline SDK (FEM cloth and softbodies alongside the PBD path). The takeaway AQUA copies: *one particle substrate, many constraint authorings*. FleX's honesty about self-collision — it is expensive and approximate, done via the same spatial hash as fluid neighbor search — is the honesty AQUA adopts in §6.

**Epic Chaos.** Unreal's Chaos Cloth is XPBD on a triangle mesh with distance, bending (dihedral), and long-range-attachment (tether) constraints, plus air drag and a wind model — the production-proven "XPBD cloth done properly" reference. Chaos also ships a strand/flesh path. What we take from Chaos: the specific constraint menu (structural + bend + tether) and the discipline of *substep count as the stiffness knob* rather than iteration count, which matches the Phase 7 loop exactly.

**NVIDIA HairWorks / AMD TressFX heritage.** Hair has its own decade of production art. TressFX (AMD, from *Tomb Raider* 2013 onward) simulates guide strands as particle chains with edge (distance) constraints, a **global/local shape constraint** to hold hairstyle, and **FTL**-style length correction — then interpolates guides into rendered strands on the GPU. HairWorks (NVIDIA) is the same guide-strand-plus-interpolation shape with its own stiffness and collision model. Both establish the production truth AQUA follows: **simulate a few thousand guides, interpolate the rest, hold style with a rest-shape/curvature term, and lean on segment repulsion rather than exact strand-vs-strand intersection.** Han & Harada 2013 is the GPU crystallization of this style-preservation approach.

**The shared shape.** Strip the branding and all four are the same pipeline: *particles in a pool → topology of distance constraints → a bending/curvature term → position-based iterations (colored Gauss-Seidel) → collision and self-collision through a spatial hash → guide-vs-render split for hair.* AQUA's Phase 5–7 substrate already provides the pool, the coloring, the hash, and the solver loop. Phase 8 provides the topologies, the bending/curvature terms, the FTL pass, and the authoring front-ends. We are not inventing; we are *specializing a substrate the incumbents validated*.

---

## 4. The literature we build on

- **Müller, Heidelberger, Hennix, Ratcliff 2007, "Position Based Dynamics" (JVCI).** The founding paper: integrate positions directly, satisfy constraints by projection, derive velocity after. Everything here is a PBD constraint.
- **Macklin, Müller, Chentanez 2016, "XPBD: Position-Based Simulation of Compliant Constrained Dynamics" (MIG).** Adds compliance so stiffness is a physical, timestep-independent material parameter and the lambda accumulator kills the iteration-count-dependence of plain PBD. This is the exact `AQConstraint.compliance` semantics from Phase 7. Cloth structural/shear/bend and rope/hair distance all get compliance from here.
- **Provot 1995, "Deformation Constraints in a Mass-Spring Model to Describe Rigid Cloth Behavior."** The stretch-limit lineage — clamp mass-spring cloth to near-inextensible by post-correcting over-stretched edges. XPBD compliance and FTL are the modern descendants; we cite it as the ancestor of the inextensibility discipline.
- **Bridson, Marino, Fedkiw 2003, "Simulation of Clothing with Folds and Wrinkles" (SCA).** Dihedral-angle bending and robust cloth collision/self-collision handling. Our dihedral bending option and the "start conservative on self-collision" posture trace here.
- **Bergou, Wardetzky, Harmon, Zorin, Grinspun 2006, "A Quadratic Bending Model for Inextensible Surfaces" (EG).** Isometric (quadratic) bending — a linear bending energy in positions, cheap and stable, decoupled from stretch. Our **leaned** cloth bending model.
- **Selle, Lentine, Fedkiw 2008, "A Mass Spring Model for Hair Simulation" (SIGGRAPH).** Altitude springs and edge/bend/torsion springs for hair as mass-spring chains; the reference for what a hair strand's stiffness terms *mean* physically.
- **Bergou, Wardetzky, Robinson, Audoly, Grinspun 2008, "Discrete Elastic Rods" (SIGGRAPH).** The canonical strand bend+twist model with a material frame. Higher fidelity than we ship now; the model behind the **Phase 8.x** twist option (§5, §12).
- **Müller, Kim, Chentanez 2012, "Fast Simulation of Inextensible Hair and Fur" (VRIPHYS).** PBD hair + **Follow-The-Leader**: propagate a length correction root-to-tip in one pass, getting inextensibility without stiff constraints and without the iteration cost. The real-time answer we build the head-of-hair on.
- **Han & Harada 2013, "Real-time Hair Simulation with Efficient Hair Style Preservation."** GPU guide-strand simulation with global/local shape constraints for style preservation — the practical crystallization of the TressFX approach; informs our rest-curvature term and the guide/render split.
- **Kaufman, Tamstorf, Smith, Aubry, Grinspun 2014, "Adaptive Nonlinearity for Collisions in Complex Rod Assemblies" (SIGGRAPH).** How to make dense hair contact converge without exploding; the high-end reference for *why* segment repulsion beats naive per-particle contacts in dense assemblies.
- **Daviet 2020, "Simple and Scalable Frictional Contacts for Thin Nodal Objects" (SIGGRAPH 2020).** Scalable frictional contact for hair-scale nodal assemblies — the recency lead for making tens of thousands of frictional strand contacts tractable, and the direction our friction/cohesion pass points toward hardening.

**The throughline.** PBD (2007) established position projection; XPBD (2016) made stiffness physical; FTL (2012) made 1D inextensibility cheap; quadratic bending (2006) made cloth bending cheap and stable; the hair line (Selle 2008 → Müller 2012 → Han 2013 → Kaufman 2014 → Daviet 2020) moves from mass-spring physics toward scalable frictional real-time strands. AQUA sits at the confluence: **XPBD cloth + FTL hair + spatial-hash self-collision on the Phase 7 core**, with Discrete Elastic Rods held in reserve for when twist matters.

---

## 5. Where AQUA diverges — the openings

1. **One pool, three shapes, zero fork.** The incumbents ship cloth and hair as separate subsystems that happen to share a math library. AQUA ships them as three *authorings* over one `AQParticlePool` and one `AQXPBDSolver`, differing only in topology and constraint mix. The opening: fewer moving parts, one determinism story, one place to optimize.

2. **Compute-first at parity, never `#ifdef`.** Every kernel — distance projection, dihedral/quadratic bending, FTL, self-collision hashing — has a CPU-fallback path selected at runtime via `GTEDeviceFeatures`, at numeric parity, with a `double` oracle. No incumbent guarantees CPU/GPU parity as a first-class contract; AQUA does, inherited from Phase 5's stable-cross-path determinism.

3. **FTL as a *first-class* inextensibility mode, not a hack.** TressFX and HairWorks use FTL-ish correction internally; AQUA exposes it as an explicit, documented `AQStrandDesc` mode alongside stiff distance constraints, with the tradeoff stated (FTL for the head-of-hair, stiff distance for a handful of hero strands — §11). The opening is *honesty about the model choice* at the API surface.

4. **Bending model chosen per shape, not per engine.** Cloth gets quadratic isometric bending (Bergou 2006); hair gets a rest-curvature term; DER (Bergou 2008) is flagged for Phase 8.x. Because all three are `AQConstraintType`s behind the *unchanged* Phase 7 interface, swapping or adding a bending model later is additive, not a rewrite.

5. **Self-collision granularity is explicit and conservative-by-default.** Rather than pretend full self-collision is solved, AQUA ships cloth as particle-vs-grid and hair as segment-vs-segment repulsion, both on the Phase 6 hash, and *says so* — with a debug bit (`AQDebugSelfCollision`) so the 3am engineer can see exactly which pairs fired. The opening: no silent quality cliff, no mystery penetration.

6. **The DER door is left open behind the interface.** We are not shipping twist. But because the constraint interface is Phase 7's and unchanged, adding a material-frame DER strand in Phase 8.x does not disturb cloth or FTL hair. We name the seam now so the later phase is additive.

---

## 6. Proposed algorithm — XPBD strands & sheets + follow-the-leader hair

The Phase 8 step nests entirely inside the Phase 7 substep loop (n substeps × 1 iteration). Phase 8 contributes: constraint construction (once, at create time), and the per-substep projection passes for the new constraint types plus the FTL pass and self-collision.

```
# Per substep (inside AQXPBDSolver, Phase 7 loop). h = dt / nSubsteps.
# Positions x, predicted positions p, inverse mass w, all SoA in AQParticlePool.

for each substep:
    # 1. Predict (Phase 7, unchanged) — integrate velocity, apply gravity/external.
    for i in active_particles:
        if pinned(i): p[i] = pin_target(i)          # positional assignment, exact
        else:         p[i] = x[i] + h*v[i] + h*h*w[i]*f_ext[i]

    # 2. Distance constraints — structural+shear (cloth), chain (rope/hair).
    #    Colored Gauss-Seidel: one color per pass, parallel within a color.
    for color in distance_batches:                  # AQConstraintBatch, Phase 7 coloring
        parallel for c in color:                    # AQDistanceConstraint
            project_distance(p, w, c)               # XPBD w/ compliance + lambda accum (Macklin 2016)

    # 3. Bending.
    #    Cloth: quadratic isometric bending (Bergou 2006) over hinge quads.
    #    Hair:  rest-curvature term over consecutive segment triples.
    for color in bending_batches:
        parallel for c in color:
            project_bending(p, w, c)                # linear-in-position energy; compliance-scaled

    # 4. Hair inextensibility — FTL pass (Müller 2012), root->tip, per strand.
    #    Replaces stiff distance constraints on FTL strands; single sweep, no iteration.
    parallel for strand s in ftl_strands:
        d_prev = 0
        for k in 1..segments(s):                    # sequential along the strand only
            i, j = node(s,k-1), node(s,k)           # i is closer to root
            dir  = normalize(p[j] - p[i])
            corr = (p[j] - p[i]) - rest_len(s,k)*dir
            p[j] = p[j] - corr                       # move tip node to satisfy length
            # store correction for velocity damping (FTL "position based" velocity fix)
            ftl_correction[j] = corr
        # (roots are pinned in step 1; FTL never moves the root)

    # 5. Collision vs Phase 2 rigid shapes — two-way (Phase 7 hybrid).
    parallel for i in active_particles:
        for shape in broadphase_hits(AABB(p[i])):   # Phase 2 grid
            if penetrating(p[i], shape):
                push_out(p[i], shape, margin)        # move particle to surface + margin
                accumulate_rigid_impulse(shape, i)   # fed to Phase 3 impulse solver at contact

    # 6. Self-collision via Phase 6 spatial hash. Conservative.
    build_hash(p, active_particles)                 # reuse Phase 6 sort-based grid
    #   cloth: particle-vs-particle repulsion within cloth radius (grid cells)
    #   hair:  segment-vs-segment repulsion (Kaufman 2014 posture), skip same-strand neighbors
    for color in selfcoll_batches:
        parallel for pair in color:
            apply_repulsion(p, w, pair, thickness)  # + tangential friction/cohesion (Daviet 2020 direction)

    # 7. Update velocities (Phase 7, unchanged) — v = (p - x)/h, then damp.
    for i in active_particles:
        v[i] = (p[i] - x[i]) / h
        if ftl(i): v[i] += ftl_damp * ftl_correction[i] / h   # FTL velocity correction (Müller 2012)
        x[i] = p[i]

    # NaN / explosion guard — after every substep, before commit.
    assert_finite(p, v)                             # AQ_ASSERT_FINITE; loud, never silent
    clamp_or_flag(v, v_max)                         # flag runaway; never silently zero physics
```

Notes for the 3am engineer:

- **Colors are computed once at create time** (Phase 7 `AQConstraintBatch`), not per frame. Distance, bending, and self-collision each get their own coloring; self-collision recolors only when the hash topology changes materially.
- **FTL is sequential *along a strand* but parallel *across strands*.** That is the whole point — 32k strands run in parallel, each strand's root-to-tip sweep is cheap and stable, no iteration count to tune. Roots are pinned in step 1 and FTL never touches them, so root binding stays exact.
- **The FTL velocity correction (step 7) is not optional.** Without it, FTL removes stretch energy from positions but leaves it in velocities and hair jitters. Müller 2012 §4 is explicit about this; we implement the damping term.
- **Pinning is a positional assignment, not a constraint.** A pinned particle's predicted position is *set* to its target every substep (step 1), so it is bit-exact on its pin regardless of constraint order — which is what the rope/hair oracles check.
- **Two-way coupling accumulates rigid impulses** into the Phase 3 contact solver rather than shoving rigids directly, honoring the Phase 7 hybrid boundary. Coupling strength is a §11 open decision.
- **Guards are loud.** `AQ_ASSERT_FINITE` fires on the first non-finite position or velocity with the particle index and the last constraint that touched it; velocity clamp *flags* (debug bus) rather than silently zeroing, so a runaway is visible, not swallowed.

**Alternative considered — stiff distance constraints for hair instead of FTL.** We could drop FTL and make hair just be ropes: chains of high-stiffness (low-compliance) distance constraints solved by the same colored Gauss-Seidel as cloth. This is *simpler* (one code path, no separate sweep) and it is what a few hero strands should use for maximum fidelity. But for a full head, stiff distance constraints need many substeps/iterations to look inextensible and they fight the collision solver, and the cost scales badly with strand count. FTL gets visually-inextensible hair in a single root-to-tip sweep at a fraction of the cost — which is why TressFX/HairWorks and Müller 2012 all use it for real-time heads. **Decision: FTL is the default for `createHair` guides; stiff distance is available per-strand for hero strands (§11).** DER (Bergou 2008) is the *other* alternative — strictly higher fidelity, adds twist — and is deferred to Phase 8.x precisely because it does not fit inside a single cheap sweep and we don't need twist yet.

---

## 7. New types AQUA must add — `include/aqua/AQCloth.h`, `AQStrand.h` (drafts)

All AQUA-owned, AQ-prefixed, no namespace, POD / trivially-copyable / standard-layout, GPU-uploadable, no virtuals on the kernel path. Backend/OmegaSL types stay out of the public headers (pimpl). Math borrows `OmegaGTE::FVec<3>` / `OmegaGTE::Matrix` from `GTEMath.h`; positions live in the Phase 6 pool as `AQVec3f`.

```cpp
// include/aqua/AQCloth.h  (draft)
#pragma once
#include "AQXPBD.h"        // Phase 7: AQConstraintType, AQConstraint, AQConstraintBatch
#include "AQMath.h"        // AQVec3f

// How a cloth edge/quad resists deformation. Compliance is XPBD compliance
// (Macklin 2016): 0 == rigid, larger == softer. Units: m / N.
struct AQClothCompliance {
    float structural;   // axis-aligned edges (stretch)
    float shear;        // diagonal edges (shear)
    float bending;      // hinge/dihedral (bend). See AQBendingModel.
};

enum AQBendingModel : uint32_t {
    AQBendDihedral = 0,   // Bridson 2003 dihedral angle
    AQBendQuadratic = 1,  // Bergou 2006 isometric quadratic (cloth default)
};

// A pinned cloth particle: grid index -> world/kinematic target.
struct AQClothPin {
    uint32_t gridIndex;   // row*cols + col
    AQVec3f  target;      // world point OR local offset if boundBody != 0
    uint32_t boundBody;   // 0 == world-space pin; else rigid/kinematic body id
};

// Authoring descriptor consumed by AQSpace::createCloth.
struct AQClothDesc {
    uint32_t          rows, cols;     // grid resolution
    float             cellSize;       // rest edge length (m)
    float             particleMass;   // per-particle mass (kg); inv-mass in pool
    AQClothCompliance compliance;
    AQBendingModel    bending;
    float             thickness;      // self-collision radius (m)
    float             friction;       // cloth-vs-rigid & self friction [0,1]
    const AQClothPin* pins;           // may be null
    uint32_t          pinCount;
    AQVec3f           origin;         // world placement of grid (0,0)
    OmegaGTE::Matrix  orientation;    // grid basis
};
```

```cpp
// include/aqua/AQStrand.h  (draft) — ropes AND hair
#pragma once
#include "AQXPBD.h"
#include "AQMath.h"

// Inextensibility model for a strand chain.
enum AQStrandInextModel : uint32_t {
    AQInextFTL = 0,        // Follow-The-Leader (Müller 2012). Hair default.
    AQInextDistance = 1,   // stiff XPBD distance constraints. Rope / hero strands.
};

// Bending/curl of a strand. restCurvature holds "style".
struct AQStrandBend {
    float compliance;      // XPBD bend compliance (m/N)
    float restCurvature;   // 0 == straight; >0 == holds a curl/wave (per segment)
};

// A single rope or a rooted hair guide strand.
struct AQStrandDesc {
    uint32_t          segments;      // node count = segments + 1
    float             segmentLength;  // rest length per segment (m)
    float             particleMass;
    AQStrandInextModel inext;
    AQStrandBend      bend;
    float             friction;      // strand-strand friction/cohesion [0,1]
    float             thickness;     // self-collision radius (m)
    AQVec3f           rootTarget;    // root world point OR local offset
    uint32_t          rootBody;      // 0 == world pin; else kinematic/rigid body id
    AQVec3f           growDirection; // initial strand direction from root
};

// A whole head of hair: many strands sharing a model, rooted on a collider.
struct AQHairDesc {
    const AQVec3f*    roots;         // guide-strand root positions (world)
    const AQVec3f*    rootNormals;   // grow direction per root (e.g. scalp normal)
    uint32_t          rootCount;     // == number of simulated GUIDE strands
    AQStrandDesc      strandProto;   // shared per-strand model (segments override rootTarget/growDir)
    uint32_t          scalpBody;     // collider the roots are bound to
    // Rendered strands are interpolated from guides by the CALLER (kREATE), not here.
};
```

Debug bus additions (`AQDebug.h`, extend `AQDebugFlags`, drain via `AQSpace::drainDebugLines`):

```cpp
// New AQDebugFlags bits (added, not renumbered):
//   AQDebugClothEdge      — draw structural/shear/bend edges (color by strain)
//   AQDebugHairStrand     — draw guide-strand segments (color by strand id)
//   AQDebugSelfCollision  — draw the self-collision pairs that fired this frame
```

---

## 8. Data layout & GPU/numeric specialization

- **Particles stay in `AQParticlePool` (Phase 6).** Cloth/rope/hair add *no new particle storage* — positions, velocities, inverse-mass are the existing SoA arrays. Phase 8 adds *topology* arrays (constraints, strand offsets) alongside, not new particle SoA.
- **Constraints are POD SoA, GPU-uploadable.** `AQConstraint` (Phase 7) already carries particle indices, rest value, compliance, lambda accumulator — all trivially copyable, no pointers, no virtuals. Bending and hair-curvature constraints reuse the same struct with a `type` tag; the kernel dispatches on the tag. Raw floats inside any union; no `std::variant`, no vtables on the kernel path.
- **Strand topology is offset-based.** A hair block stores `strandOffset[]` (prefix-sum of node counts) so strand `s` owns pool indices `[strandOffset[s], strandOffset[s+1])`. This is the exact scan/prefix-sum layout Phase 5 already provides, and it makes the FTL pass a clean "parallel over strands, sequential within" GPU dispatch — one thread (or warp) per strand.
- **Coloring is precomputed and stored.** `AQConstraintBatch` colors are computed once at create time on the CPU and uploaded; the per-substep kernel just iterates colors. Self-collision coloring is rebuilt only when the hash's occupied-cell topology changes materially (dirty flag), not every frame.
- **FTL wants strand-major locality.** Because FTL walks root-to-tip, nodes of one strand should be contiguous in the pool. `createHair` lays guide strands out strand-major so a warp streams one strand's nodes; this is a placement choice at create time, transparent to the solver.
- **Compute-first, CPU-fallback at parity.** Every pass (distance, bending, FTL, self-collision) ships an `.omegasl` kernel and a CPU path selected at runtime by `GTEDeviceFeatures` — never `#ifdef`. The CPU path is the parity reference; the `double` oracle in the tests is a *third* path used only for validation, not shipped.
- **Determinism.** Stable ordering everywhere: colors iterate in fixed order, within a color the reduction order is fixed (Phase 5 stable-cross-path), FTL is sequential-within-strand (inherently ordered), self-collision pairs are sorted by (min,max) index before projection. Fixed sub-step `h = dt/nSubsteps`. Same scene, same result, CPU or GPU, run to run.
- **Numeric range guards live on the kernel path.** `AQ_ASSERT_FINITE` after each substep; a per-particle velocity clamp that *flags* on the debug bus rather than silently clamping physics away. A strand whose segment blows past `k·rest` is flagged, not hidden.

---

## 9. Validation — how we measure "better"

Every check runs headless in `aqua_deformables_test`, CPU and GPU paths, against a `double` oracle where one exists.

- **Inextensibility (rope/hair, `compliance = 0` / FTL).** Every segment length within 1e-4·rest of rest length across the whole sim; report max strain. FTL strands: total strand arc length within tolerance of rest.
- **Pin/root exactness.** Pinned cloth corners and hair roots are bit-exact on their targets every frame (positional assignment, §6 step 1). Any drift is a bug, not a tolerance.
- **Energy sanity (cloth drape).** Total mechanical energy monotonically non-increasing after the first substep; `‖v‖∞` bounded. No spurious energy gain (the classic explicit-integration failure), no explosion. The `double` oracle integrates the same scene at a smaller `h` and the two agree to a stated tolerance on final rest shape.
- **Drape symmetry (cloth).** The two-corner-pinned sheet over a centered sphere is a symmetric scene; mirror-paired particles agree within 1e-3 m at rest. Asymmetry indicates order-dependent projection (a determinism/coloring bug).
- **No penetration.** No cloth or hair particle inside a Phase 2 collider beyond the collision margin; measured as max penetration depth per frame.
- **Hair loft (does-not-collapse).** After settling, the covariance of hair-particle positions retains meaningful spread orthogonal to gravity — the volume must not collapse to a line. Turning strand friction/cohesion to zero should *measurably reduce* loft (a control test that the friction term is actually doing work).
- **Sweep response (hair).** A moving capsule swept through the settled hair displaces it and it recovers toward rest without explosion; roots stay pinned throughout; no strand passes through the scalp.
- **CPU/GPU parity.** Final rest positions agree between paths within the Phase 5 stable-cross-path tolerance. Divergence is a substrate regression.
- **Determinism.** Byte-stable (or within stated tolerance) across two runs of the same scene on the same path.
- **Perf floor (informational, not pass/fail).** Record substep time for the 64×64 cloth and the 32k×16 hair on the reference GPU, so later hardening (Daviet-style contacts, DER) has a baseline to beat.

"Better" for this phase = *cloth that drapes symmetrically without gaining energy, ropes/hair that don't stretch, hair that keeps its loft and stays out of the scalp — reproducibly, on CPU and GPU, with every self-collision pair visible on the debug bus.*

---

## 10. Public API additions

```cpp
// AQSpace factories (declarations; return opaque handles, pimpl backend).
class AQSpace {
public:
    // Cloth: returns a handle; particles are appended to the shared pool.
    AQClothHandle  createCloth(const AQClothDesc& desc);

    // Rope / arbitrary chain: a single strand with AQInextDistance by default.
    // Branching chains are authored by pinning one strand's root to another's node.
    AQStrandHandle createRope(const AQStrandDesc& desc);

    // Hair: many rooted guide strands sharing a model. FTL by default.
    AQHairHandle   createHair(const AQHairDesc& desc);

    // Repin / release at runtime (attach to body, move target, or free).
    void setPinTarget(AQClothHandle, uint32_t gridIndex, const AQVec3f& target, uint32_t body);
    void releasePin (AQClothHandle, uint32_t gridIndex);
    void setRootTarget(AQHairHandle, uint32_t strandIndex, const AQVec3f& target, uint32_t body);

    // Read-back (positions live in the pool; these are convenience views).
    AQParticleView clothParticles(AQClothHandle) const;
    AQParticleView strandNodes(AQStrandHandle) const;
    AQParticleView hairGuideNodes(AQHairHandle) const;   // guide strands only; kREATE interpolates

    // Debug bus (existing; new bits AQDebugClothEdge/AQDebugHairStrand/AQDebugSelfCollision).
    void setDebugFlags(AQDebugFlags);
    AQDebugFlags debugFlags() const;
    uint32_t drainDebugLines(AQDebugLine* out, uint32_t maxLines);
    // ...
};
```

Contract notes: factories only *append* to the pool and *build* constraints/coloring; they never run a step. Handles are stable across steps. `hairGuideNodes` returns *guide* strands only — the render multiplication is the caller's (kREATE's) job, stated at the API surface so nobody expects AQUA to return 500k strands. All descriptors are POD and copyable; the caller owns the `pins`/`roots` arrays only for the duration of the create call.

---

## 11. Open decisions for this phase

1. **Hair inextensibility model — FTL vs stiff distance constraints.** *Lean: FTL (Müller 2012) as the `createHair` default for the real-time head; stiff XPBD distance constraints available per-strand for a handful of hero strands where maximum fidelity matters.* FTL is cheaper, single-sweep, and visually inextensible; stiff distance is simpler code but scales badly across a full head. Both ship; the descriptor (`AQStrandInextModel`) chooses.
2. **Bending model — dihedral (Bridson 2003) vs quadratic isometric (Bergou 2006) vs Discrete Elastic Rods (Bergou 2008).** *Lean: quadratic isometric for cloth (cheap, stable, decoupled from stretch); a rest-curvature term for hair; DER flagged for Phase 8.x when twist matters (ponytails, curled fur).* Dihedral stays available as `AQBendDihedral` for parity with Chaos-style cloth.
3. **Self-collision granularity — particle-particle vs segment-segment.** *Lean: particle-vs-grid for cloth, segment-segment repulsion for hair (Kaufman 2014 posture); start conservative and discrete, harden later toward Daviet 2020 frictional contacts.* Robust CCD is explicitly out of scope this phase.
4. **How many strands are actually simulated vs interpolated for rendering.** *Lean: AQUA simulates guide strands only; interpolation into rendered hair is kREATE's job (the TressFX/HairWorks split).* The API returns `hairGuideNodes` and says so. Open sub-question: recommended guide density per scalp area — punt to kREATE authoring guidance.
5. **Two-way cloth↔rigid coupling strength.** *Lean: full two-way via accumulated impulses into the Phase 3 solver (Phase 7 hybrid), but expose a coupling-strength scalar so a heavy character isn't visibly shoved by a light cloth.* Open: default value and whether it is per-cloth or global. Needs a scene with a light cloth on a light rigid to calibrate.
6. **FTL velocity-damping coefficient.** *Lean: expose `ftl_damp` (Müller 2012 §4) as a per-hair parameter with a sane default; too low and hair jitters, too high and it looks dead.* Needs the sweep test to tune.

---

## 12. Recency-principle audit (addendum, 2026-07-01)

Last-5-years scan (2020–2026) for work that would change the *substrate* choice, not just add polish:

- **Daviet 2020, "Simple and Scalable Frictional Contacts for Thin Nodal Objects" (SIGGRAPH 2020).** The recency lead for dense frictional hair. It does not change the substrate — it is a *contact-solver* improvement that sits behind the same particle/constraint model. We adopt its *direction* for the friction/cohesion pass (§6 step 6) and flag full adoption as a hardening pass, not a Phase 8 rewrite.
- **Andrews & Erleben, contact/constraint survey work (recent).** Confirms position-based and compliant-constraint methods as the mainstream real-time deformable substrate; nothing here argues against XPBD + colored Gauss-Seidel.
- **VBD — Vertex Block Descent (Chen, Macklin, et al. 2024).** A newer solver that improves stability/convergence for deformables and would be a genuine upgrade *behind* the Phase 7 constraint interface. Crucially: it changes the *inner solve*, not the constraint authoring or the public types. The Phase 7 `AQConstraint`/`AQConstraintBatch` interface is unchanged, so VBD is a future drop-in under the same interface — noted, not adopted this phase.
- **Discrete Elastic Rods (Bergou 2008) remains the higher-fidelity strand option.** It is not recent, but it is the correct *next* fidelity step (twist, material frame) and is explicitly flagged for **Phase 8.x** — fur curl, ponytails, hero strands where twist reads on screen. It does not belong in the first deformables phase because we don't need twist and it doesn't fit the cheap FTL sweep.

**Net for Phase 8.** The 2020–2026 literature *validates* the plan rather than upending it: XPBD cloth + FTL hair + spatial-hash self-collision on the Phase 7 core is substrate-correct. The recency improvements (Daviet frictional contacts, VBD inner solve) are additive *behind unchanged interfaces* — friction hardening and a possible future inner-solver swap — and DER is the fidelity door left open for Phase 8.x. Nothing in the recent work says "don't build it this way."

**Re-audit due.** Phase 8.x kickoff (twist/DER), or first sign that dense-hair frictional contacts (Daviet direction) are the bottleneck in the head-of-hair perf floor — whichever comes first.

---

*Brief status: proposal. Leans are stated, not settled; §11 decisions are the developer's call. No code shipped, no build touched — this is the plan AQUA implements once Phase 7's XPBD core lands.*
