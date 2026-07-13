# AQUA Phase 10 — Fluids: Liquids & Gases

**Prior-art brief & proposal.** This is the §4 research artifact for **Phase 10 —
Fluids: liquids & gases [Particle + soft] (optional / advanced)**: the capstone of
the roadmap, and the one phase gated on a project-level question — does kREATE need
fluids at all (roadmap §7.8)? The central claim of this brief is one the incumbents
obscure by shipping two products: **fluid is not just liquid.** Water sloshing in a
tank and smoke rising off a fire are *the same smoothed-particle machinery* — the
same neighbor search, the same density estimate, the same per-particle kernel — run
with two **equations of state** and two **boundary regimes**. An incompressible
liquid holds a rest density near-exactly; a compressible gas lets density fall as it
heats and rises. That is a change of parameters and one force term, not a change of
engine. AQUA is uniquely positioned to make that claim *true in code* because both
sit on the same substrate: Phase 6's particle pool, Phase 7's XPBD constraint solve,
Phase 2's grid, Phase 5's compute path. One solver, two phases of matter. This brief
argues that unification is the whole point of Phase 10, and that anything else is a
second engine wearing a fluid hat.

---

## 1. Scope & deliverable

**Goal.** Simulate fluids on the particle substrate — **both** an incompressible
**liquid** (water: sloshing, splashes, a settling pool) **and** a compressible
**gas** (smoke: a buoyant, expanding, curling plume) — with one shared neighbor
search, one density estimator, and two equations of state. Prove the unification by
shipping *both* on the same solver, not two solvers that happen to live in the same
repo.

**Runnable deliverable (two scenes, one solver).**
- A **dam-break**: a rectangular volume of liquid particles released to collapse and
  slosh inside Phase 2 static geometry (a box tank with a step). The reference test
  for the liquid path.
- A **rising smoke plume**: a buoyant gas emitter (Phase 6 `AQEmitter`) whose
  particles expand, advect upward, cool with height, and curl around a Phase 2
  obstacle. The reference test for the gas path.

Both run through the *same* `stepInternal` fluid pass — the same neighbor build, the
same density solve loop — differing only in the `AQFluidPhase` branch that selects
equation of state and boundary handling. If the two scenes require two code paths
that don't share the density/neighbor core, the phase has failed its own thesis.

**Oracles (how each scene is held honest).**
- **Liquid (dam-break).** The collapse **front advances at roughly the analytic
  shallow-water rate** (the Ritter / Stoker dam-break solution gives a front celerity
  of `2·sqrt(g·h0)` for the idealized case — we match the *trend and order*, not a
  tight number, because SPH is viscous and the tank is finite). **Volume is conserved**
  (particle count × per-particle rest volume stays flat — no particles created or
  destroyed, no drift). The pool **settles flat and level** with **no persistent
  compression** — density relaxes back to rest (`ρ ≈ ρ0`, no standing over-pressure
  band at the bottom of the column, the classic PBF failure).
- **Gas (plume).** The plume **rises** — buoyancy sign correct, hot particles go up
  not down (the single most common smoke-sim sign bug). It **cools and slows with
  height** (temperature and vertical velocity both decay as particles climb and mix).
  **Particle count is conserved** (a gas particle doesn't vanish when its density
  drops).
- **Both.** **No particle tunnels through the Phase 2 boundary** — the tank walls and
  the obstacle hold. **Density never blows up** — a runaway-pressure particle trips a
  **loud guard**, never a silent NaN that poisons the buffer (§9).

The dam-break analytic front and the settled-flat-pool are to fluids what the `double`
oracle was to Phase 1 and the brute-force pair set was to Phase 2: slow, physically
obvious references the fast path is held to.

**Included groundwork (lands first in Phase 10).** Two seams the earlier phases left
open specifically for fluids:
- Phase 6's `AQParticlePool` carries positions / velocities / inverse-mass / lifetime
  but **no density or pressure state**. Phase 10 adds the parallel `density[]` /
  `lambda[]` SoA arrays (the PBF Lagrange multiplier) and, for gas, the optional
  `temperature[]` side-array — as *additions* to the existing pool, not a new pool.
- Phase 7's `AQConstraintType` enumerates the XPBD constraints but has **no `Density`
  case**. Phase 10 adds it: Position-Based Fluids is literally a density constraint on
  the Phase 7 solver (Macklin & Müller 2013 built PBF on PBD). The substep loop and
  the colored solve are Phase 7's; the density constraint is the new resident.

**What earlier phases closed for us (do not re-add).**
- **Phase 2:** `AQShape` + `AQshapeSupport` (signed-distance / support — the boundary
  pushout leans on this), `AQAABB`, and the **sort-based uniform grid**. Fluid neighbor
  search *reuses this grid* — the Phase 2 recency audit already flagged **compact
  hashing (Teschner 2003; Ihmsen 2011)** as the sparse-particle layout swap, and
  fluids are exactly that workload.
- **Phase 5:** the compute substrate — `AQComputeBackend`, `AQExecPath`, SoA + pooled
  GTE buffers, `src/kernels/*.omegasl`, the AQUA-owned Blelloch **scan** and
  Merrill–Grimshaw **radix sort** (the density and neighbor passes lean directly on
  these), colored Gauss-Seidel, and the **stable-cross-path** determinism stance.
- **Phase 6:** `AQParticlePool` (SoA), emitters (`AQEmitter`), force fields, and the
  sort-based grid neighbor search over particles. **Fluid particles *are* Phase 6
  particles** with density/pressure state added; the gas emitter *is* an `AQEmitter`.
- **Phase 7:** the XPBD core (`include/aqua/AQXPBD.h`), the `n×1` substep loop (Macklin
  2019), the colored parallel solve (Vivace), and the hybrid decision (Phase 3 impulse
  rigid + XPBD particles/fluids, coupled at boundaries).

**Out of scope, by design:** the **rendered surface** and **rendered volume** are
kREATE's job. AQUA hands back particle + density state; kREATE runs marching-cubes /
screen-space fluid for the liquid surface and builds the density field for the gas
volume. AQUA does *not* ship a surface mesher or a volume renderer. Also out of scope:
phase transition (boiling/condensation), multi-fluid mixing beyond one liquid + one
gas, and any Eulerian grid solver (rejected in §11).

---

## 2. Why "fluid" is two phases of matter on one solver

The incumbents ship a "liquid" product and a "smoke/gas" product as if they were
different disciplines. At the level of the *renderer* they are — a water surface and a
smoke volume look nothing alike. At the level of the *solver* they are not, and
conflating "how it renders" with "how it simulates" is the mistake this phase exists
to avoid.

1. **The machinery is identical up to the equation of state.** Both a liquid and a gas
   are, in a particle solver: (a) find each particle's neighbors within the smoothing
   radius `h`; (b) estimate density by summing a smoothing kernel `W` over those
   neighbors, `ρ_i = Σ_j m_j W(x_i − x_j, h)`; (c) turn the density error into a force
   or a positional correction; (d) advect. Steps (a), (b), and (d) are **byte-for-byte
   the same** for water and smoke. Only step (c) — the equation of state relating
   density to pressure — and the boundary regime differ. That is the whole divergence,
   and it is small.

2. **The divergence is a sign and a stiffness, not an architecture.** A liquid is
   **incompressible**: it resists *any* deviation from rest density, in both directions,
   very stiffly — you push it back to `ρ0` hard (§6, the PBF density constraint). A gas
   is **compressible and buoyant**: density is *allowed* to fall (that is what "hot air
   is less dense" means), pressure follows an ideal-gas law `p = k(ρ − ρ0)`, and the
   *low-density regions get pushed up* by buoyancy `∝ (T − T_ambient)`. Same density
   estimate; opposite attitude toward density deviation. A liquid fights it; a gas
   rides it.

3. **The failure mode is shared and it is loud by nature.** Both regimes fail the same
   way — a particle whose neighbor count spikes gets a runaway density, its pressure
   (or its density constraint's `λ`) explodes, it shoots off, and one NaN propagates
   through the neighbor sum into every particle it touches next step. Water that
   "explodes" and smoke that "explodes" are the *same bug*. That is why the density-
   blowup guard (§9) is a single guard serving both scenes — the failure has one
   mechanism.

4. **Boundaries are where the two genuinely diverge — and even that is one mechanism.**
   A liquid must not leak through a wall and should form a meniscus against it; a gas
   should flow *around* an obstacle and vent past its edges. Both are handled by the
   *same* boundary primitive — density contribution from the wall plus a signed-distance
   pushout against Phase 2 shapes (§6) — tuned differently: the liquid pushout is stiff
   and no-penetration; the gas pushout is soft and lets tangential flow slip. One
   mechanism, two tunings.

A correct Phase 10 writes the neighbor search, the density estimate, and the advect
**once**, and branches on `AQFluidPhase` only at the equation-of-state and boundary
step. If the two scenes need two solvers, we have proven the opposite of the thesis and
should reconsider whether Phase 10 is coherent at all.

---

## 3. Prior art — how the incumbents solve it

Studied for the terrain and the failure modes, not to transcribe. Drawn from published
talks, docs, and source structure — representative, not a claim to current internals.

**NVIDIA PhysX 5 / FleX / Flow.**
- **FleX** is the unified particle solver: rigids, cloth, and **fluids** are all
  particles under a **position-based** constraint solve — and this is the strongest
  external evidence for AQUA's thesis. FleX does liquids as a **PBF density constraint**
  (Macklin & Müller, both NVIDIA) on the same solver that does everything else. It is
  the substrate-unification argument made by the people who wrote PBF.
- **Flow** is the *separate* product for gaseous **combustible fluids** (fire/smoke) —
  and tellingly, it is a **sparse-grid Eulerian** solver, not FleX particles. NVIDIA
  split liquid (particle, FleX) from gas (grid, Flow). That split is exactly the one
  AQUA declines to make: we keep gas on the particle substrate (§5, §11).
- **PhysX 5** integrates the FleX particle system into the mainline SDK — fluids share
  the SDK's GPU pipeline, contact model, and rigid coupling. Position-based fluid is
  the shipped liquid path.

**Epic Chaos / Niagara Fluids (Unreal Engine 5).**
- **Niagara Fluids** ships *both* a **grid-based (Eulerian) gas** solver (smoke, fire)
  **and** an **FLIP / particle liquid** solver (splashes, pools) — and, again, they are
  **two different simulation cores** exposed through one authoring surface. Gas is
  grid; liquid is particle. The unification is at the *tooling* layer, not the *solver*
  layer.
- The liquid path is **FLIP** (Fluid-Implicit-Particle — particles carry velocity, a
  grid does the pressure projection), a hybrid that leans on a background grid for
  incompressibility. That grid is precisely the second engine AQUA avoids (§11).
- Chaos rigid bodies couple to the fluids at the authoring layer; the coupling is
  product-integration, not a shared constraint solve.

**The shared shape — and the opening.** *Both* incumbents ship liquid-as-particle and
gas-as-grid, unified only in tooling. That is a defensible engineering choice for a
shipping product with a huge feature surface — but it means **neither of them makes the
claim this brief makes**: that one particle solver does both. FleX comes closest (its
liquids *are* PBF particles), but even NVIDIA sends gas to Flow's grid. The bar to
clear is FleX's PBF liquid quality; the opening is to *not* fork the gas off to a grid,
and to prove buoyant compressible SPH gas rides the same substrate well enough that the
fork isn't needed for AQUA's scale.

---

## 4. The literature we build on

The research is unambiguous and deep — SPH is thirty years mature. The pieces we
combine, oldest foundation first:

- **Monaghan, "Smoothed Particle Hydrodynamics" (Annu. Rev. Astron. Astrophys., 1992).**
  The method itself: represent a continuum by particles carrying mass, interpolate any
  field by summing a smoothing kernel `W(r, h)` over neighbors. Every fluid below is a
  choice of kernel and equation of state on top of this. The density estimate
  `ρ_i = Σ_j m_j W_ij` is Monaghan's; we compute it once and both phases read it.
- **Müller, Charypar, Gross, "Particle-Based Fluid Simulation for Interactive
  Applications" (SCA 2003).** SPH brought to graphics at interactive rates: the
  poly6 / spiky / viscosity kernel triad, pressure via a simple state equation, and the
  **surface-tension and viscosity** models we use for the liquid. This is the paper that
  made SPH a real-time graphics tool.
- **Becker & Teschner, "Weakly Compressible SPH for Free Surface Flows" (SCA 2007).**
  **WCSPH / Tait equation of state** — enforce near-incompressibility by a *stiff* state
  equation `p = B((ρ/ρ0)^γ − 1)` rather than a hard projection. The reference for the
  weakly-compressible attitude, and the conceptual bridge to the compressible **gas**
  EOS (same machinery, softer stiffness, buoyancy added).
- **Solenthaler & Pajarola, "Predictive-Corrective Incompressible SPH" (PCISPH,
  SIGGRAPH 2009).** Iterate pressure to drive density error toward zero without the
  crippling timestep of stiff WCSPH — the first of the "predict, measure density error,
  correct" family that PBF later expresses as a constraint.
- **Ihmsen, Cornelis, Solenthaler, Horvath, Teschner, "Implicit Incompressible SPH"
  (IISPH, IEEE TVCG 2014).** Solve the pressure Poisson problem implicitly for large
  stable timesteps — the high-accuracy incompressible-SPH line, and part of why DFSPH
  (below) is the accuracy upgrade rather than PBF.
- **Bender & Koschier, "Divergence-Free Smoothed Particle Hydrodynamics" (DFSPH,
  SCA 2015).** The **modern incompressibility lead for pure SPH**: enforce *both* a
  constant-density condition *and* a divergence-free velocity field, for large timesteps
  and low volume drift. This is the accuracy target — and the flagged Phase 10.x upgrade
  (§11, §12) — but it is a *separate solver*, not an XPBD constraint, which is why PBF
  wins the first cut.
- **Macklin & Müller, "Position Based Fluids" (PBF, SIGGRAPH / ACM TOG 2013).** The
  substrate-unifying choice **we adopt for liquids.** Incompressibility becomes a
  per-particle **density constraint** `C_i(x) = ρ_i/ρ0 − 1 = 0` solved in the PBD
  iteration — the exact same solver Phase 7 already ships. Plus the **artificial
  pressure** term (surface-tension-like clustering fix) and **XSPH viscosity** and
  **vorticity confinement** that keep PBF water from looking dead. This paper *is* why
  Phase 10 sits on Phase 7.
- **Akinci, Ihmsen, Akinci, Solenthaler, Teschner, "Versatile Rigid-Fluid Coupling for
  Incompressible SPH" (ACM TOG 2012).** **Boundary handling** done right: sample the
  boundary with particles that contribute to density and carry pressure forces back to
  the rigid — the two-way coupling reference. The follow-up path once the first-cut
  signed-distance pushout isn't enough (§11).
- **Akinci, Akinci, Teschner, "Versatile Surface Tension and Adhesion for SPH Fluids"
  (ACM TOG 2013).** Cohesion + surface-area-minimization surface tension and wall
  adhesion — the model behind a liquid that beads and clings rather than smearing.
- **Fedkiw, Stam, Jensen, "Visual Simulation of Smoke" (SIGGRAPH 2001).** The **gas**
  reference for *behavior*: **buoyancy** driven by temperature (`f_buoy ∝ (T − T_amb)`,
  Boussinesq) and **vorticity confinement** to restore the small-scale rolls a coarse
  solver damps out. Eulerian in the original — but the buoyancy and vorticity-confinement
  *models* transfer directly onto SPH particles, and that transfer is the gas half of
  this phase.
- **Stam & Fiume, "Depicting Fire and Other Gaseous Phenomena Using Diffusion Processes"
  (SIGGRAPH 1995).** The earlier particle/diffusion treatment of gaseous phenomena — the
  lineage that says gases *can* be particles, before the Eulerian era took over.
- **Bender, Koschier, Kugelstadt, Weiler, "Turbulent Micropolar SPH Fluids with Foam"
  (SCA 2017).** **SPH vorticity / micropolar turbulence** — particles carry angular
  momentum, restoring the fine rotational detail that plain SPH gas loses. The gas-detail
  upgrade (§11): a smoke plume that keeps its curls without a grid.
- **Ihmsen, Akinci, Becker, Teschner, "A Parallel SPH Implementation on Multi-core CPUs"
  (2011).** **Compact hashing** for the neighbor search — the sparse-cell hash the Phase
  2 audit already flagged for the particle pillar. Directly reused here.
- **Koschier, Bender, Solenthaler, Teschner, "SPH Techniques for the Physics-Based
  Simulation of Fluids and Solids" (Eurographics 2019 tutorial / survey).** The modern
  survey — the map of the whole field, and the one document to hand a new engineer
  before they touch the fluid path.

The throughline: **one estimator, two attitudes.** Monaghan's density sum is the shared
spine. Incompressible liquids (Müller 2003 → WCSPH → PCISPH → IISPH → DFSPH, expressed
here as PBF) fight density deviation; compressible buoyant gases (Fedkiw 2001 + micropolar
2017) ride it upward. AQUA picks **PBF for liquids** because it *is* a Phase 7 constraint,
and **compressible buoyant SPH for gas** because it keeps gas on the *same* substrate — the
unification the incumbents decline to attempt.

---

## 5. Where AQUA diverges — the openings

Grounded in the actual shipped surface (`AQParticlePool` from Phase 6, `AQXPBD.h` from
Phase 7, the Phase 2 grid, the Phase 5 compute path):

- **Liquid *and* gas on one particle solver — the divergence that defines the phase.**
  PhysX sends gas to Flow's grid; Niagara sends gas to a Eulerian core. AQUA keeps both
  on the particle substrate and branches only at the equation of state. The payoff is
  concrete: **one** neighbor build, **one** density kernel, **one** SoA layout, **one**
  compute pipeline serve both — and rigid↔fluid coupling (Phase 2 shapes) is written once
  for both. The cost is that SPH gas is a coarser smoke than a dedicated sparse-grid
  solver at the same particle budget; we accept that trade because AQUA's remit is a
  *unified real-time substrate*, not a film smoke solver (§11 rejects the grid explicitly).
- **PBF liquids *are* a Phase 7 constraint — no new solver.** PhysX/FleX also do PBF, but
  for AQUA the point is structural: the density constraint `C_i = ρ_i/ρ0 − 1` drops into
  the existing XPBD substep loop as a new `AQConstraintType::Density`, solved by the same
  colored Gauss-Seidel Phase 7 already ships. We do not add a fluid solver; we add a
  constraint. That is the smallest possible liquid path, and it is the one Macklin & Müller
  designed.
- **Neighbor search is the Phase 2 grid, in its compact-hashing form — already flagged.**
  The Phase 2 recency audit recorded compact hashing (Teschner 2003; Ihmsen 2011) as the
  particle-pillar layout swap. Fluids are hundreds of thousands of uniform-radius particles
  spread sparsely — exactly the workload compact hashing was designed for. Every step is a
  Phase 5 primitive already: hash → radix-sort → scan → read runs. No new broadphase; a
  layout swap on an existing one.
- **Gas rides the SoA the pool already owns, plus one side-array.** Temperature is the only
  new *state* a gas needs. It lives as an **optional `temperature[]` SoA side-array** on the
  particle pool — present for gas systems, absent for liquid systems — so a liquid pays zero
  bytes for a field it never reads, and the gas buoyancy/cooling kernels read one extra
  coalesced stream. The pool's positions/velocities/inverse-mass are shared verbatim.
- **We own the determinism stance (again).** A fluid step is a sum over neighbors, and
  floating-point summation is order-dependent — an unordered neighbor list is a determinism
  leak. We accumulate density and constraint corrections in a **fixed neighbor order** (the
  sorted `(cellHash, particleIndex)` order the grid already produces), on both CPU and GPU,
  so the `double` oracle and the stable-cross-path guarantee (Phase 5 §7.4) carry into
  fluids. The substep count is fixed (Phase 7's `n×1`); the sub-step is deterministic.
- **The density-blowup guard is first-class, not an afterthought.** A runaway-pressure
  particle is *the* canonical SPH failure (§2.3). AQUA treats it like the Phase 1 fast-spin
  guard and the Phase 2 O(n²) guard: a **loud, named** trip (`AQDebugFluidDensity` at a
  hard ceiling), never a silent clamp that hides the mistuned kernel. A 3am engineer sees
  "particle 40213 density 8.4× rest, guard tripped" in the debug stream, not a screen full
  of NaN.

**Gaps we must fill (this is Phase 10's work):** there is no `AQFluidPhase`, no
`AQFluidDesc`, no density/pressure state on the pool, no `Density` constraint in Phase 7's
enum, no gas temperature side-array, no fluid boundary handling, no `AQSpace::createFluid`
/ `createGasEmitter`. All of it is fluid machinery we own, built on the Phase 6 pool and the
Phase 7 solver — none of it is the numerically-sensitive core linear algebra we borrow from
GTE (`Matrix`/`Quaternion`); `AQVec3f` and the fluid types are AQUA's.

---

## 6. Proposed algorithm — position-based fluids (liquid) + compressible buoyant SPH (gas) on one neighbor search

The synthesis, per sub-step. **Steps 1–2 and 6 are shared verbatim; only steps 3–5 branch
on `AQFluidPhase`.** All per-particle passes are one-thread-per-particle over the Phase 6
SoA; the neighbor build is the Phase 2 / Phase 5 hash-sort-scan.

```
FLUID SUBSTEP (per Phase 7 n×1 iteration):

--- SHARED: neighbor build + density estimate (identical for liquid and gas) ---
1. NEIGHBOR BUILD (Phase 2 grid, compact-hashing form; Phase 5 primitives):
     for each particle i (one thread):  cellHash[i] = hash(cellOf(x_i), h)
     radix-sort (cellHash, i) by cellHash          // Merrill–Grimshaw, Phase 5
     scan to cell-start offsets                     // Blelloch, Phase 5
     // neighbors of i = particles in i's cell + the 26 (3D) neighbor cells within h

2. DENSITY ESTIMATE (Monaghan):
     for each particle i (one thread):
         rho[i] = sum over neighbors j of  m_j * W_poly6(x_i - x_j, h)
         rho[i] += boundary density contribution from Phase 2 shapes within h   // §6.boundary
         if (rho[i] > DENSITY_CEILING * rho0) trip AQDebugFluidDensity, clamp-and-log  // §9 loud guard

--- BRANCH on AQFluidPhase ---

3a. LIQUID  — PBF density constraint (Macklin & Müller 2013), solved in the Phase 7 loop:
        C_i     = rho[i]/rho0 - 1
        lambda_i = -C_i / ( sum_j |grad_j C_i|^2 + epsilon )     // XPBD-style, epsilon-regularized
        dx_i    = (1/rho0) * sum_j (lambda_i + lambda_j + s_corr) * gradW_spiky(x_i - x_j, h)
                  // s_corr = artificial pressure (anti-clustering, surface-tension-like)
        x_i    += dx_i                                            // POSITIONAL correction — this IS the XPBD constraint

3b. GAS  — compressible EOS + buoyancy (Becker–Teschner EOS + Fedkiw buoyancy):
        p_i     = k * (rho[i] - rho0)                            // ideal-gas / state-equation pressure, k = gasStiffness
        f_press_i = - sum_j m_j * (p_i + p_j)/(2 rho_j) * gradW_spiky(x_i - x_j, h)
        f_buoy_i  = buoyancy * (T[i] - T_ambient) * up            // Boussinesq — hot rises, sign is the #1 bug (§1 oracle)
        a_i     = (f_press_i + f_buoy_i) / m_i                    // FORCE path — gas is not incompressible
        v_i    += a_i * dt                                        // velocity update (contrast liquid's positional dx)

--- BRANCH: post-density velocity terms ---

4a. LIQUID  — XSPH viscosity + vorticity confinement (Macklin & Müller 2013):
        v_i += xsphViscosity * sum_j (m_j/rho_j)(v_j - v_i) W_poly6(x_i - x_j, h)
        v_i += vorticityConfinement * (curl-restoring term)      // keeps splashes lively

4b. GAS  — thermal advection + cooling + vorticity confinement (Fedkiw 2001 / micropolar 2017):
        T[i] += (thermal diffusion from neighbors) * dt
        T[i] -= coolingRate * (T[i] - T_ambient) * dt            // cools with height/time → plume slows (§1 oracle)
        v_i  += vorticityConfinement * (curl-restoring term)     // keeps the plume's rolls (micropolar, §11)

--- BRANCH: boundary (same primitive, two tunings) ---
5.  BOUNDARY vs Phase 2 shapes:
        d = AQshapeSignedDistance(shape, x_i)                    // reuse Phase 2 support/SDF
        if d < contactRadius:
            LIQUID: stiff no-penetration pushout along +normal + boundary-density (no leak, meniscus)
            GAS:    soft pushout, allow tangential slip (flow AROUND the obstacle, vent past edges)

--- SHARED: advect ---
6. ADVECT:
     for each particle i (one thread):
         x_i += v_i * dt            // liquid: v recovered from positional dx as (x_i - x_i_prev)/dt (PBD velocity update)
                                    // gas:    v already updated in 3b/4b
     enforce lifetime / emission (Phase 6 emitter for gas)
```

**Why this combination.**
- **PBF for the liquid** because incompressibility becomes a *constraint on the solver we
  already have* — the density constraint enters Phase 7's colored substep loop as one more
  `AQConstraintType`, and the positional correction *is* the XPBD update. No second solver,
  no pressure-Poisson machinery, and it is the exact method FleX ships (§3).
- **Compressible buoyant SPH for the gas** because it keeps gas on the *same particles, same
  neighbor build, same density estimate* — the branch is one EOS line plus a buoyancy term
  plus a thermal update. The alternative (a Eulerian grid) is a whole second engine (§11).
- **One neighbor build, one density estimate** (steps 1–2) shared verbatim is the phase's
  thesis expressed as code: the expensive, parallel, GPU-primitive part is *identical* for
  water and smoke; only the cheap per-particle EOS branch differs.
- **Ordered accumulation** (§5) keeps the sums deterministic for the `double` oracle and the
  stable-cross-path target; the loud density guard (step 2) makes the canonical blowup a
  named event, not a NaN cascade.

**Alternative considered — DFSPH liquids (Bender & Koschier 2015).** DFSPH holds
incompressibility more accurately (constant-density *and* divergence-free, larger stable
timesteps, less volume drift) and is the modern pure-SPH lead. It is **not adopted for the
first cut** because it is a *separate implicit solver*, not an XPBD constraint — adopting it
means Phase 10 stops sharing Phase 7's substrate, which forfeits the unification this phase
is *for*. **DFSPH is the flagged Phase 10.x accuracy upgrade** (§11.1, §12): the day the
dam-break's volume drift or the pool's residual compression is measured as unacceptable, DFSPH
is what we build — but only then, and as an opt-in alternative EOS, not a replacement of the
substrate. **Eulerian gas** was also considered and is *rejected outright* (§11.2): it would
be a second engine with a second data model, defeating the whole brief.

---

## 7. New types AQUA must add — `include/aqua/AQFluid.h` (draft)

AQUA-owned, AQ-prefixed, no namespace (per `AGENTS.md`). POD / trivially-copyable /
standard-layout, GPU-uploadable, no virtuals on the kernel path; primitive params are raw
floats so the descriptor uploads to a GTE buffer with no repacking. Backend / OmegaSL stays
out of the header (pimpl). The header consumes the Phase 6 pool and Phase 7 solver; it does
not redefine them.

```cpp
#ifndef AQUA_AQFLUID_H
#define AQUA_AQFLUID_H

#include "AQMath.h"          // AQVec3f (AQUA-owned); Matrix/Quaternion borrowed from GTEMath.h
#include "AQParticle.h"      // Phase 6 AQParticlePool — fluid particles ARE these
#include "AQXPBD.h"          // Phase 7 solver — PBF density constraint lives here
#include <cstdint>

// --- Which phase of matter this fluid system simulates. The single branch that
// selects equation of state and boundary regime (§6). Liquid = incompressible
// (PBF density constraint); Gas = compressible + buoyant (EOS + buoyancy). ---
enum class AQFluidPhase : std::uint32_t { Liquid, Gas };

// --- Fluid material description. POD, GPU-uploadable. Fields common to both
// phases sit first; the `gas` sub-struct is read only when phase == Gas (the
// AQVec/temperature side-array it implies is allocated only then, §8). ---
struct AQFluidDesc {
    // --- shared (both phases) ---
    float restDensity      = 1000.0f;   // rho0 — water ~1000 kg/m^3; gas ambient set low
    float particleRadius   = 0.025f;    // per-particle radius; rest volume = f(radius)
    float smoothingLength  = 0.10f;     // h — kernel support radius (typ. 2-4x particleRadius)
    float viscosity        = 0.01f;     // XSPH (liquid) / thermal-transport scale (gas)
    float surfaceTension   = 0.0728f;   // cohesion + curvature (liquid); ~0 for gas
    float vorticityConfine = 0.10f;     // restore small-scale rolls (both; matters most for gas)

    // --- gas-only (ignored when phase == Liquid) ---
    struct {
        float gasStiffness      = 1.0f;    // k in p = k(rho - rho0) — compressible EOS (§6.3b)
        float buoyancy          = 1.0f;    // Boussinesq coeff: f_buoy ∝ buoyancy*(T - T_ambient) (§6.3b)
        float thermalExpansion  = 1.0f;    // how strongly temperature lowers effective density
        float ambientTemp       = 293.0f;  // T_ambient — the rest temperature the plume cools toward
        float coolingRate       = 0.5f;    // per-second exponential cooling toward ambient (§6.4b)
    } gas;
};

// --- Opaque handle to a fluid system owned by an AQSpace. Small value (index +
// generation into the space's fluid table), copyable, backend-free — crosses the
// pimpl boundary without exposing the pool or any backend type. ---
struct AQFluidSystemHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;       // guards stale handles after teardown
    OMEGA_NODISCARD bool valid() const { return generation != 0; }
};

// --- Per-particle fluid state added to the Phase 6 pool as parallel SoA arrays
// (§8). density[] / lambda[] are present for every fluid system; temperature[]
// is an OPTIONAL side-array allocated only for AQFluidPhase::Gas — a liquid pays
// zero bytes for a field it never reads. These are declarations of the layout;
// the arrays live in the pool, not in this struct (which is documentation of the
// SoA contract, never instantiated per-particle). ---
struct AQFluidParticleFields {
    // parallel to AQParticlePool's position/velocity/invMass/lifetime SoA:
    float* density;      // rho_i — Monaghan estimate, recomputed each substep (§6.2)
    float* lambda;       // PBF Lagrange multiplier (liquid); unused for gas (§6.3a)
    float* temperature;  // OPTIONAL — non-null only for gas systems (§6.3b/4b)
};

#endif // AQUA_AQFLUID_H
```

`AQFluidDesc` is a POD so a Phase 5 kernel reads it (or an array of them) coalesced.
`AQFluidParticleFields` is **not** a per-particle struct — it documents the SoA arrays the
pool grows; the kernel reads `density[i]`, never a struct-of-arrays gather. The `gas`
sub-struct is inert when `phase == Liquid`, and the `temperature[]` array it implies is
simply not allocated (§8) — the one place the two phases diverge in *storage*, and it
diverges by *absence*, not by a second layout.

---

## 8. Data layout & GPU/numeric specialization

Decided now so the compute path (Phase 5) is a parameter change, not a rewrite.

- **SoA, extending the Phase 6 pool.** `AQParticlePool` already owns pooled
  `position[]` / `velocity[]` / `invMass[]` / `lifetime[]` GTE buffers. Phase 10 adds
  parallel `density[]` and `lambda[]` buffers for every fluid system, and — **only for
  gas** — a `temperature[]` side-array. Each is one contiguous buffer read coalesced by
  the density / EOS / buoyancy kernels. No struct-of-arrays gather on the kernel path.
- **Gas pays for exactly one extra stream; liquid pays for none.** The `temperature[]`
  buffer is allocated iff `AQFluidPhase::Gas`. A liquid system's kernels never reference
  it and never allocate it — the divergence between the two phases is a single optional
  buffer, which is the storage expression of the §2 thesis.
- **Neighbor structure is the Phase 2 grid, compact-hashing variant (Phase 2 §12 flag).**
  `cellHash[]`, the sorted `(hash, particleIndex)[]`, and the cell-start scan output are
  transient per-step buffers, built by the Phase 5 radix-sort + Blelloch scan. Fluids do
  **not** introduce a new broadphase; they select the sparse-hash layout the Phase 2
  audit already reserved for the particle pillar.
- **Compute-first, CPU-fallback at parity — never `#ifdef`.** Fluids are massively
  parallel (one thread per particle for density, EOS, boundary, advect); GPU is the
  primary path, selected by `GTEDeviceFeatures` exactly as Phase 5 does. The CPU path runs
  the *same* algorithm at parity for the `double` oracle and for machines without a
  compute device — chosen at runtime, never compiled out.
- **Determinism.** Neighbor sums accumulate in the fixed sorted `(cellHash, index)` order
  (§5); the substep count is Phase 7's fixed `n×1`; the density guard clamps deterministically.
  A `double`-precision reference path exists for the density estimate and the PBF correction —
  the fluid analogue of Phase 1's `double` integrator — so "the water settled differently on
  the GPU" is a measurable parity failure, not a mystery.
- **The blowup guard is in the hot loop, cheap, and loud.** A single compare
  `rho[i] > DENSITY_CEILING * rho0` per particle (step 2 of §6) trips `AQDebugFluidDensity`
  and clamps-with-log. It costs one comparison on the kernel's hottest pass and converts the
  canonical NaN cascade into a named, drainable event — the 3am-engineer contract (roadmap §3
  principle 6).

---

## 9. Validation — how we measure "better"

The incumbent's *behavior* is the reference, not its code (roadmap §4). Each scene has a
physically-obvious oracle.

- **Liquid — dam-break front (the headline).** Release a column of liquid particles; the
  collapse front's leading edge must advance at **roughly the analytic shallow-water rate**
  (order and trend of `~2·sqrt(g·h0)`, allowing for SPH viscosity and tank finiteness). This
  is the fluid analogue of Phase 1's `double` oracle and Phase 2's brute-force set: a slow,
  physically-obvious reference the fast path is held to.
- **Liquid — volume conservation & settled flatness.** Particle count × per-particle rest
  volume stays flat across the whole sim (no drift, no created/destroyed particles). After
  the slosh damps, the pool is **flat and level** with **`ρ ≈ ρ0` everywhere** — no standing
  over-pressure band at the column base (the PBF residual-compression failure). Measured as a
  logged density histogram converging on `ρ0`.
- **Gas — buoyancy sign & rise.** The plume **rises** — the single most-tested fact, because
  a flipped buoyancy sign sends smoke to the floor. Vertical velocity is positive for
  above-ambient particles at emission.
- **Gas — cool-and-slow with height.** Temperature and vertical velocity both **decay with
  height** as particles climb and mix toward ambient (§6.4b cooling) — a logged
  height-vs-temperature and height-vs-speed series, both monotone-ish downward.
- **Gas — particle-count conservation.** A gas particle does **not** vanish when its density
  falls (the compressible-EOS trap of treating low density as "gone"). Count is conserved
  modulo explicit emitter lifetime.
- **Both — boundary integrity.** **Zero particles tunnel** through the Phase 2 tank walls or
  the obstacle across the whole sim (a per-step signed-distance check against Phase 2 shapes,
  asserted).
- **Both — density never blows up.** The loud guard (§8) never *should* fire in a tuned
  scene; a test deliberately mistunes `h` and asserts the guard fires **loudly and named**
  rather than the sim NaN-ing — proving the 3am contract holds.
- **Determinism & parity.** The same scene yields a byte-identical trajectory within a path,
  and CPU vs GPU agree within the Phase 5 stable-cross-path tolerance.

**Debug bus — ADD bits (the bus already exists, Phase 1.1 / Phase 2).** Phase 10 extends
`AQDebugFlags` with:
- **`AQDebugFluidDensity`** — per-particle density heat coloring, and the **loud red trip**
  when a particle exceeds the density ceiling (the blowup guard's visible face).
- **`AQDebugFluidNeighbors`** — the neighbor set of a probed particle drawn as segments to
  each neighbor within `h` (the "why is this particle's density wrong" tool).
- **`AQDebugGasTemperature`** — per-gas-particle temperature coloring (hot→cold), so the
  plume's cooling is visible and the buoyancy sign is checkable at a glance.

All append into the existing drainable `AQDebugLine` stream the kREATE adapter already
consumes — no new transport, no new boundary.

---

## 10. Public API additions

Extends the existing surface — `include/aqua/AQSpace.h` (`AQSpace`) plus the new
`include/aqua/AQFluid.h` (§7) — without breaking pimpl discipline. No OmegaSL or backend
types cross into `include/aqua/*`; only AQUA types and the borrowed `FVec`/`FQuaternion`
appear. New members marked `// new`.

**`AQSpace` (in `AQSpace.h`):**
```cpp
class AQUA_EXPORT AQSpace {
public:
    // ... Phase 1–4 rigid API; Phase 6 particle/emitter API; Phase 7 XPBD API ...

    // Create a fluid system. Phase selects EOS + boundary regime (§6); desc carries
    // the material. The space owns the pool + fluid state; the handle is a backend-free
    // value across the pimpl boundary. `initialParticles` seeds a liquid volume (e.g.
    // the dam-break column); gases are usually seeded empty and fed by createGasEmitter.
    AQFluidSystemHandle createFluid(const AQFluidDesc &desc,
                                    AQFluidPhase phase,
                                    const OmegaGTE::FVec<3> *initialParticles = nullptr,
                                    std::size_t initialCount = 0);              // new

    // A buoyant gas source: an AQEmitter (Phase 6) bound to a Gas fluid system, emitting
    // particles at an above-ambient temperature. The smoke-plume deliverable's source.
    // Emission rate / spread / initial velocity reuse the Phase 6 AQEmitterDesc surface.
    AQEmitterHandle createGasEmitter(AQFluidSystemHandle gas,
                                     const AQEmitterDesc &emit,
                                     float emitTemperature);                    // new

    // State handoff to kREATE (surface/volume extraction is kREATE's render job, §1):
    // AQUA returns particle positions + per-particle density. kREATE meshes the liquid
    // surface (marching cubes / screen-space fluid) and builds the gas density field.
    OMEGA_NODISCARD std::size_t fluidParticleCount(AQFluidSystemHandle f) const; // new
    OMEGA_NODISCARD const OmegaGTE::FVec<3>* fluidPositions(AQFluidSystemHandle f) const; // new
    OMEGA_NODISCARD const float* fluidDensities(AQFluidSystemHandle f) const;    // new
    // gas only — null for liquid systems:
    OMEGA_NODISCARD const float* fluidTemperatures(AQFluidSystemHandle f) const; // new

    void destroyFluid(AQFluidSystemHandle f);                                   // new
};
```

> **Folded-in groundwork (lands first, §1).** `AQParticlePool` gains the `density[]` /
> `lambda[]` arrays (and, for gas systems, `temperature[]`); `AQConstraintType` (Phase 7)
> gains **`Density`**, and the PBF constraint solve (§6.3a) is registered in the XPBD
> substep loop as a colored constraint like the others. `stepInternal` gains the fluid pass
> (§6) between the Phase 7 constraint solve and the Phase 6 advect — a liquid's PBF density
> correction *is* an XPBD constraint and rides the existing loop; a gas's EOS/buoyancy is a
> force pass before advect. The Phase 6 emitter path is reused verbatim for gas sources;
> `createGasEmitter` is a thin binding of an existing `AQEmitter` to a Gas fluid system with
> an emission temperature.

`AQFluidSystemHandle` is a small opaque value (index + generation) — not a pointer to a
backend type — so the pimpl boundary holds and the returned `fluidPositions` / `fluidDensities`
pointers are read-only views into the space's SoA buffers (kREATE reads in place, no copy).

---

## 11. Open decisions for this phase

1. **Liquid incompressibility — PBF density constraint vs. DFSPH.** *Lean: PBF* (Macklin &
   Müller 2013), because it *is* a Phase 7 `AQConstraintType` and forfeits no substrate — the
   liquid path is a constraint, not a new solver. **DFSPH (Bender & Koschier 2015) is flagged
   as the Phase 10.x accuracy upgrade**: more accurate incompressibility (constant-density +
   divergence-free), larger stable steps, less volume drift — adopted *if and when* the
   dam-break's measured volume drift or the pool's residual compression proves unacceptable,
   and even then as an opt-in alternative, not a substrate change. This is the biggest fork in
   the phase.
2. **Gas model — compressible buoyant SPH vs. a separate Eulerian/grid smoke solver.**
   *Lean: particle SPH gas* — compressible EOS + Boussinesq buoyancy + vorticity confinement,
   on the *same* particle substrate, no grid. **The Eulerian/grid path is explicitly rejected**:
   it would be a second simulation engine with a second data model (the exact split PhysX/Flow
   and Niagara make, §3), defeating the unification that is the phase's entire justification.
   The cost — coarser smoke at a given particle budget than a dedicated sparse-grid solver — is
   accepted; AQUA is a unified real-time substrate, not a film smoke solver.
3. **Neighbor structure — sort-based uniform grid vs. compact hash.** *Lean: compact hash*
   (Teschner 2003; Ihmsen 2011) — the Phase 2 §12 audit already reserved this as the
   particle-pillar layout swap, and fluids are hundreds of thousands of uniform-radius
   particles spread sparsely, exactly its target workload. A layout swap on the Phase 2 grid,
   not a new broadphase.
4. **Boundary handling — Akinci boundary particles vs. signed-distance pushout.** *Lean:
   signed-distance pushout against Phase 2 shapes first* (reusing `AQshapeSupport` / an SDF
   query) — simple, no boundary-particle sampling, good enough for the dam-break tank and the
   plume obstacle. **Akinci boundary particles (2012)** are the flagged follow-up for
   *accurate* coupling (proper boundary density, no near-wall density deficit, correct forces
   back to the rigid).
5. **Two-way fluid↔rigid coupling in the first cut, or one-way.** *Lean: one-way first* — the
   fluid feels the rigid (Phase 2 boundary), the rigid does not yet feel the fluid. **Two-way
   coupling (Akinci 2012)** — the fluid's pressure forces pushing the rigid back — is the
   follow-up, and it pairs naturally with decision #4's boundary-particle upgrade (the same
   boundary particles carry the reaction forces).
6. **Surface / volume extraction ownership.** *Confirm: kREATE's job.* AQUA hands back particle
   positions + per-particle density (+ gas temperature); kREATE meshes the liquid surface and
   builds the gas density field for rendering. AQUA ships **no** surface mesher and **no**
   volume renderer — the §1 out-of-scope boundary, restated as a decision so it does not creep.
7. **Is Phase 10 even in scope?** The roadmap gates this phase on whether the project needs
   fluids at all (roadmap §7.8). *This brief is the research artifact that makes the decision
   informed, not the decision itself.* If kREATE never asks for water or smoke, Phase 10 stays
   a proposal — which is a perfectly good outcome for an optional capstone.

---

*Brief status: proposal. Phase 10 is optional and gated (roadmap §7.8, decision §11.7);
this document is the prior-art artifact that the gate decision should be made against, not a
commitment to build. The two-scene deliverable (dam-break liquid + smoke plume) and the
unification thesis — one neighbor search, one density estimate, two equations of state — are
the shape the phase would take if the gate opens. Follows the conventions set by
`Phase-1-Dynamics-Math-Core.md` and the per-phase prior-art series roadmap §4 establishes.*

---

## 12. Recency-principle audit (addendum, 2026-07-01)

Roadmap §4's recency principle makes "newest viable algorithm from the literature" the
standing default for every phase, incumbents adopted only when no substantively-newer
alternative offers a real improvement for AQUA's substrate. This audit runs the last five
years (≈2020–2025) against the Phase 10 picks: **PBF liquids** (Macklin & Müller 2013),
**compressible buoyant SPH gas** (Becker–Teschner EOS 2007 + Fedkiw buoyancy 2001 + micropolar
turbulence 2017), and the **compact-hash neighbor search** (Ihmsen 2011). The core methods date
to a 2001–2017 line; what does 2020-onward add?

- **DFSPH (Bender & Koschier 2015) as the higher-accuracy pure-SPH incompressibility lead —
  flagged, not adopted now.** DFSPH remains the modern reference for accurate incompressible
  SPH (constant-density + divergence-free), and the 2019 Koschier/Bender survey confirms it as
  the field's default. **Not adopted for the first cut** because it is a *separate implicit
  solver*, not an XPBD constraint — adopting it forfeits the Phase 7 substrate sharing that is
  the whole point of Phase 10. **Recorded as the §11.1 Phase 10.x accuracy upgrade**: the day
  measured volume drift or residual pool compression is unacceptable, DFSPH is what we build, as
  an opt-in alternative EOS. This is the one substantive newer-method flag from the fluid
  literature — and it is a deliberate *defer*, not a miss.
- **Micropolar / turbulent SPH (Bender, Koschier, Kugelstadt, Weiler 2017) for gas detail —
  adopted in spirit as the gas-detail path.** Particles carrying angular momentum restore the
  fine rotational structure plain SPH gas loses — the difference between a smoke plume that
  keeps its curls and one that smears. Folded into the §6.4b vorticity-confinement term as the
  gas-quality upgrade; the base plume uses Fedkiw-style confinement, the micropolar model is the
  detail lift when the plume looks too smooth.
- **ML / neural fluid surrogates (2020–2024: learned SPH, graph-network fluid simulators,
  neural closures) — not a fit, explicitly.** A substantial recent literature learns fluid
  dynamics with graph networks or neural closures, and is genuinely faster for *offline*
  visual plausibility. **Not adopted:** they are non-deterministic (weights + inference
  numerics vary the trajectory), they do not offer the `double` oracle / stable-cross-path
  guarantee AQUA's substrate is built on (roadmap §7.4), and their speedups are at film-scale
  offline budgets, not a real-time deterministic step. A learned surrogate is the antithesis of
  the guarantees Phase 5 shipped. Recorded as considered-and-rejected for a deterministic
  real-time substrate.
- **Vertex Block Descent (Chen et al., SIGGRAPH 2024) — out of scope, not a fluid method.**
  VBD is a fast, robust *solids / constraint* solver (a block-coordinate-descent alternative to
  XPBD for cloth and deformables) — relevant to Phases 7–9, **not** a fluid method. It does not
  change the Phase 10 EOS choice or the density estimate. Noted here only to record that it was
  checked and is the wrong layer for this phase; its natural home is a Phase 7/8/9 audit.
- **GPU neighbor-search refinements (2020+ cooperative-group / cache-friendly SPH hashing) —
  adopted in spirit at the kernel level.** As with the Phase 2 audit, recent GPU-spatial-hashing
  work is *kernel-level optimization* of the same compact-hash sort-scan-read structure Phase 10
  picks, not algorithmic divergence. Rolled into the Phase 5 OmegaSL kernel author pass; the §6
  algorithm is unchanged.
- **PBF itself (Macklin & Müller 2013) — no successor supersedes it for substrate-unified
  liquids.** The unique property AQUA needs — incompressibility *as an XPBD constraint on the
  existing solver* — is still exactly PBF. DFSPH is more accurate but not a constraint; the ML
  line is not deterministic. For "the liquid path that *is* a Phase 7 constraint," PBF has no
  2020+ replacement. No change.

**Net for Phase 10:** the recency audit finds **no algorithmic divergence to adopt now** over
the chosen PBF-liquid + compressible-buoyant-SPH-gas + compact-hash-neighbor lead — it remains
the substrate-correct answer, and the liquid/gas *unification* is the phase's whole point and
the thing the incumbents (PhysX/Flow, Niagara) decline to attempt. **One accuracy upgrade is
flagged for later:** DFSPH (Bender & Koschier 2015) as the Phase 10.x opt-in for measured
incompressibility failures — deferred, not missed, because adopting it early forfeits the Phase
7 substrate sharing. **One gas-detail path is folded in:** micropolar turbulence (2017) as the
vorticity-confinement upgrade. **Two lines are checked and rejected/deferred:** ML/neural
surrogates (rejected — non-deterministic, wrong for a real-time deterministic substrate) and
Vertex Block Descent (out of scope — a solids solver, belongs to a Phase 7–9 audit).

Re-audit due: 2028-07-01 (roadmap §4 two-year freshness rule) or sooner if (a) the §11.1 DFSPH
upgrade fires because measured liquid volume drift is unacceptable, (b) PhysX 6 / Niagara
publicly ship a *particle-unified* gas path (which would validate the thesis and warrant a
comparison pass), or (c) a deterministic, oracle-compatible neural fluid method appears — none
exists today.
