# AQUA Phase 5 — Compute Execution Substrate (OmegaSL Kernel Port)

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a subsystem is written: what PhysX 5,
Chaos, and FleX do to move a rigid-body step onto the GPU, which papers we
improve on, what we change for AQUA's substrate, and how we will measure
"better." It covers **Phase 5 — Compute execution substrate [execution]**:
making good on the roadmap's compute-first promise (`Physics-Roadmap.md` §3
principle 3) by porting the Phase 1–4 hot loops — integration, broadphase,
narrowphase, and the contact/constraint solve — to OmegaSL compute kernels
dispatched through `AQContext`'s GTE command queue, with the existing CPU path
retained at parity as the runtime-selected fallback. This is a **port, not a
new simulation**: every prior phase chose its data layout and iteration order
"so Phase 5 is a port, not a rewrite" (the standing note in each phase's §8),
and this phase cashes that promise. No new physics, no new shapes, no new
constraint kinds — the *same* simulation on whichever path the device's
`GTEDeviceFeatures` selects.

This phase is also where two of the roadmap's far-reaching key decisions
(§7.3 data layout, §7.4 determinism) finally bind, because they govern how the
kernels are written. They were *leaned* in the earlier phases (SoA, stable-not-
bitwise); here they are *settled* with the kernel code in front of us.

---

## 1. Scope & deliverable

**Goal.** Take the working, CPU-only Phase 1–4 step pipeline and run its three
hottest stages — per-body integration, the sort-based-grid broadphase, and the
sequential-impulse contact/constraint solve — as OmegaSL compute kernels on the
GPU, producing results equivalent to the CPU path within a stated tolerance,
and falling back to the CPU path automatically on a device that reports no
usable compute capability. The public `AQContext` / `AQSpace` / `AQRigidBody`
surface does not change shape; path selection is *data* (queried device
features), never an `#ifdef` (`Physics-Roadmap.md` §3 principle 3).

**Runnable deliverable.** The Phase 3 settling stack and the Phase 4 swinging
bridge, driven unchanged through the public surface, run end-to-end on the GPU
path and are asserted in `tests/` to match the CPU path:

1. **The GPU settling stack (the headline).** Phase 3's ten unit-cube stack
   dropped onto a static plane, stepped on the **GPU path**. Within 3 simulated
   seconds every body has `‖v‖ < 0.01 m/s` and `‖ω‖ < 0.01 rad/s`, max contact
   penetration `< 1 mm`, and the stack holds for another 2 s — the *same*
   assertions Phase 3 makes on the CPU, now proving the kernel pipeline settles
   the stack as cleanly. The per-contact accumulated normal impulse must still
   agree with the analytic `(11 − i)·m·g` resting force within 5%.
2. **CPU↔GPU parity (the substrate headline).** The same stack and the same
   bridge run on both paths from identical initial state; at every sub-step the
   GPU body state (position, velocity, orientation, ω) must match the CPU body
   state within the §8 tolerance band (the same `1e-4`-class band Phase 1's
   `float`/`double` oracle uses, relaxed once for the colored-solve traversal-
   order divergence quantified in §8). This is the parity harness §6 of the
   roadmap calls for, brought online as a hard test.
3. **The CPU fallback.** With the device's compute capability masked off
   (a test hook that forces `AQExecPath::CPU`, see §10), the *same* scene runs
   and passes the *same* settling assertions — proving the fallback is the
   identical simulation, not a degraded one. This is asserted directly so
   "compute-first with a CPU fallback at parity" is a test, not a slogan.
4. **Within-path determinism.** Two runs of the GPU stack on the same device
   produce **byte-identical** body-state buffers at every sub-step (§8: the
   colored solve uses no order-dependent float atomics, and the broadphase sort
   is stable, so a single GPU path is run-to-run deterministic). The CPU path's
   existing byte-identical-on-re-run guarantee (Phase 2/3/4) is unchanged.

Each deliverable keeps the same slow, obviously-correct reference the earlier
phases established — here the reference is **the CPU path itself**, already
held to its own analytic oracles in Phases 1–4. The GPU is measured against the
CPU; the CPU is measured against the math. That two-level oracle is the whole
point of having kept a CPU path "at parity."

**Included groundwork (lands first in Phase 5).** Three things the earlier
phases deferred to here, closed before any kernel runs:

- **`AQContext` can reach the graphics engine, not just the queue.** Today
  `AQContext` holds only `SharedHandle<OmegaGTE::GECommandQueue> commandQueue`
  ([AQContext.h:35](aqua/include/aqua/AQContext.h:35)) and is created from a
  queue alone ([AQContext.h:43](aqua/include/aqua/AQContext.h:43)). But
  *creating* a compute pipeline and GPU buffers needs the
  `OmegaGraphicsEngine` (`makeComputePipelineState`, `makeBuffer` —
  [gte/tests/metal/ComputeTest/main.mm:43,50](gte/tests/metal/ComputeTest/main.mm:43)),
  and *gating* GPU-vs-CPU needs `GTEDevice::features`
  ([GTEDevice.h:162](gte/include/omegaGTE/GTEDevice.h:162)). The queue exposes
  neither (`GECommandQueue` has no engine/device accessor — confirmed against
  its 16-method surface). So Phase 5's first increment makes the
  `OmegaGraphicsEngine` a **required** constructor argument on
  `AQContext::Create`, threaded down to `AQSpace::Impl` behind the pimpl. The
  engine is mandatory, not optional: kREATE is a GPU-first 3D engine and always
  has a live engine to hand AQUA, so there is no real "physics without a
  graphics engine" configuration to keep a queue-only overload alive for. The
  one existing queue-only call site ([AQContext.cpp](aqua/src/AQContext.cpp))
  is migrated; the old overload is removed rather than deprecated (a clean
  break is cheaper than a permanently-CPU-only escape hatch nobody should use).
  Forcing the CPU path stays available — but through `setExecutionPath(CPU)`
  and automatic fallback on a no-compute device (§7.1), not through a
  degenerate engine-less context.
- **The SoA body buffers the earlier phases promised actually get built.** The
  per-body state is still **AoS** today — `AQBodyState` is an aggregate
  ([AQIntegrator.h:29-57](aqua/include/aqua/AQIntegrator.h:29)) explicitly
  marked "SoA-friendly at the buffer level later; AoS here for the CPU
  reference path" ([AQIntegrator.h:26-28](aqua/include/aqua/AQIntegrator.h:26)),
  and bodies live in a pointer-chased `std::vector<std::shared_ptr<AQRigidBody>>`
  ([AQSpace.cpp:296](aqua/src/AQSpace.cpp:296)). Phase 5 builds the parallel
  SoA arrays + their pooled GTE buffers (`x[]`, `v[]`, `q[]`, `ω_b[]`,
  `m⁻¹[]`, `Ib⁻¹[]`, plus the Phase 2/3/4 side-arrays) and keeps them in sync.
  This is the "per-body parallel arrays land in Phase 5" the Phase 3 §8 note
  refers to.
- **`src/kernels/` gets its build wiring.** The directory is empty today. Phase
  5 establishes how an `.omegasl` source becomes a runtime-loadable shader
  library (the `omegaSlCompiler->compile` → `loadShaderLibraryRuntime`
  sequence from the worked example,
  [main.mm:34-35](gte/tests/metal/ComputeTest/main.mm:34)), how the CMake build
  carries the kernel sources, and the cross-backend story (omegaslc targets
  HLSL/MSL/GLSL from one source).

**What the earlier phases have already closed for us.** Do not re-derive these;
they are the substrate this port consumes:

- **The algorithms are all chosen and CPU-proven.** Sort-based uniform-grid
  broadphase (Phase 2 §6 — hash → radix sort → cell-run pair scan → sort+unique),
  hybrid narrowphase (Phase 3 §6 — specialized per-pair + GJK/EPA fallback), and
  sequential-impulse PGS with split-impulse (Phase 3 §6). Each was selected
  *because* "every step is already a GPU primitive" (Phase 2 §6) or "maps
  cleanly to a per-row sweep, parallelizes via constraint-graph coloring (Phase
  5)" (Phase 3 §6). Phase 5 implements those already-named GPU forms.
- **The row/manifold/pair PODs are already trivially-copyable, GPU-shaped.**
  `AQConstraintRow` ([AQContact.h:85](aqua/include/aqua/AQContact.h:85)) and
  `AQContactManifold` ([AQContact.h:48](aqua/include/aqua/AQContact.h:48)) were
  authored "so the row buffer uploads to a GPU constraint-solver kernel with no
  repacking" ([AQContact.h:6-9](aqua/include/aqua/AQContact.h:6)). Each row
  carries `bodyA`/`bodyB` — "exactly the adjacency the coloring needs" (Phase 3
  §8).
- **The integrator is already split for solver interleave and written once over
  `Ty`.** `AQStepBodyVelocity` / `AQStepBodyPosition`
  ([AQIntegrator.h:79,173](aqua/include/aqua/AQIntegrator.h:79)) are the two
  half-steps the kernels mirror; the `double` instantiation remains the
  precision oracle.
- **The debug bus, material combine, sleeping/islands, joints, queries, CCD —
  DONE.** Phase 5 ports the three *hot* stages; the cold stages (debug
  emission, query API, trigger diff, CCD swept-sphere, island union-find) stay
  on the CPU for now (§5 — they are not the throughput bottleneck and several
  are inherently serial), reading the GPU's downloaded body state.

**Out of scope here, by design:** porting the cold stages above to the GPU
(a Phase 5.x follow-up once the hot path pays off); GPU graph coloring (the
first cut colors on the CPU — §6.F, §11.3); a fully GPU-*resident* loop with no
per-step readback (§11.5 — the first cut tolerates a readback so the CPU cold
stages keep working); bitwise CPU↔GPU determinism (§8, §11.2 — leaned out);
particle kernels (Phase 6 reuses this substrate); and any unified-XPBD solver
(Phase 7, roadmap §7.2 — that changes the *algorithm*, which a faithful port
must not). The kernels are written so Phase 6's particles dispatch through the
same buffer-pool + command-encode machinery without a rewrite.

---

## 2. Why "move the step to the GPU" is several problems, not one

The CPU step is a single function that runs stages back-to-back over `std::vector`s
([`stepInternal`, AQSpace.cpp:599](aqua/src/AQSpace.cpp:599) →
[`runNarrowphaseAndSolve(dt)`, AQSpace.cpp:670](aqua/src/AQSpace.cpp:670)).
Re-expressing it as GPU kernels is four distinct problems with four different
shapes, and conflating them is how a GPU port turns into a GPU rewrite.

1. **Integration is trivially parallel — and a trap.** The two half-steps are
   pure per-body maps (Phase 1 bodies don't interact; the velocity half-step
   touches only body `i`'s own state). One thread per body, no atomics, ideal
   occupancy — exactly what Phase 1 §8 promised. The trap is *numerics*: the
   implicit-gyroscopic Newton step ([AQIntegrator.h:119-143](aqua/include/aqua/AQIntegrator.h:119))
   does a 3×3 inverse and a quaternion exponential per body, and the whole point
   of Phase 1 was that the `double` oracle and the `float` path agree. The GPU
   `float` path must reproduce the *same* operation order, or it drifts away
   from the CPU `float` path faster than either drifts from the oracle.

2. **Broadphase is parallel primitives — but the primitives aren't free.** The
   sort-based grid (Phase 2 §6) is a chain of GPU-standard primitives: a hash
   map, a **radix sort**, a cell-start scan, a neighbor-cell pair scan, and a
   **sort+unique** dedup. GTE gives us the *dispatch substrate* — pipelines,
   buffers, command encoding — but not the *primitives*: there is no GTE radix
   sort or prefix scan to call. AQUA must ship its own sort/scan kernels. And
   the pair scan *appends* a variable number of pairs per cell-run — the first
   place atomics enter (an atomic bump-allocator into the pair buffer).

3. **Narrowphase is a parallel map with variable, data-dependent output.** One
   thread per candidate pair branches over `(typeA, typeB)` and emits 0..4
   contact points → 0..12 constraint rows (Phase 3 §6.A). Variable output means
   a thread can't know where to write without a **prefix sum** over per-pair row
   counts to allocate each pair's slice of the row buffer. And the branch is a
   real divergence problem: a warp with a sphere/sphere pair next to a
   hull/hull GJK pair runs both paths. This is the classic narrowphase-on-GPU
   tension; we manage it (§6.D) but it doesn't vanish.

4. **The solver is Gauss-Seidel — and Gauss-Seidel is sequential by
   construction.** The PGS sweep ([Phase 3 §6.D](aqua/.plans/Phase-3-Narrowphase-Contact-Solver.md))
   applies an impulse for contact `c`, *and the very next contact reads the
   velocity that impulse just changed*. That read-after-write is what makes it
   converge fast — and what makes it not parallel. Two rows that share a body
   cannot run concurrently without racing on that body's velocity. This is the
   hard problem of the phase, and the entire reason Phase 3 §8 said "we keep the
   row layout coloring-friendly": the GPU solver is a **graph-colored
   Gauss-Seidel** — partition rows into colors such that no two rows in a color
   share a body, then dispatch one conflict-free parallel kernel per color
   (§6.F). Coloring changes the traversal order, which is why CPU↔GPU is
   tolerance-equivalent, not bit-identical (§8).

On top of these four sits a fifth, cross-cutting problem — **where does the data
live, and when does it move?** A naive port that uploads buffers, dispatches one
kernel, and reads back per stage drowns in PCIe traffic and sync stalls. The
real design question (§6, §11.5) is how much of the step stays resident on the
GPU between dispatches and how the per-step CPU↔GPU handoff is structured so the
*cold* CPU stages (debug, queries, CCD) still see correct state.

And a sixth — **determinism across two very different float machines.** The CPU
path is deterministic run-to-run (Phase 2/3/4). The GPU path can be made
deterministic run-to-run *on one device*. Making the two agree *bit-for-bit* is
a different and much harder promise (reassociation, FMA contraction, atomic
reduction order, vendor math-library differences), and whether kREATE needs it
(lockstep netcode) is the §7.4 roadmap decision this phase settles (§8, §11.2).

---

## 3. Prior art — how the incumbents solve it

The mature engines have all crossed this bridge; we read them for the shape of
the terrain and the failure modes, not to transcribe (`Physics-Roadmap.md` §4).

- **NVIDIA PhysX 5 (GPU rigid-body pipeline).** PhysX runs the full rigid
  pipeline on the GPU: a GPU broadphase (incremental SAP on CPU historically,
  now a GPU grid/ABP), GPU narrowphase contact generation, and a GPU
  **constraint solver built on batched, graph-colored constraints** plus their
  **TGS** (Temporal Gauss-Seidel) substepping. The constraints are partitioned
  into batches where no two constraints in a batch touch the same body; each
  batch is a parallel dispatch; batches run in sequence to recover the
  Gauss-Seidel coupling. Data is SoA and **GPU-resident** across the whole step
  — bodies, contacts, and constraints live in device buffers and only the
  results the CPU needs (transforms, events) are read back. This is the model we
  follow; the divergences are §5.
- **Epic Chaos (Unreal).** Chaos's solver is CPU-first with an evolving GPU
  path; its design center is determinism for networked gameplay and its island/
  constraint-graph machinery is mature. We read Chaos mainly for its
  determinism stance (it works hard to be deterministic *on a path*, and is
  candid that cross-platform bitwise is not a default promise) — which directly
  informs §8.
- **NVIDIA FleX / "Unified Particle Physics" (Macklin et al. 2014).** The
  GPU-native unified solver: everything is particles + constraints, solved with
  a Jacobi/Gauss-Seidel hybrid using **graph coloring** to batch constraint
  projection without write conflicts, all GPU-resident. FleX is the proof that
  colored-batch constraint solving is the GPU-correct shape — but FleX is the
  *unified XPBD* world that is AQUA's Phase 7 fork, not Phase 5. We take its
  coloring/batching mechanics and leave its solver philosophy for Phase 7.
- **The common GPU broadphase.** All of them (and every GPU-physics tutorial
  since) build the uniform grid the same way: hash cells, radix-sort by cell,
  find cell starts by scan, gather neighbors. This is exactly Phase 2 §6, which
  is not a coincidence — Phase 2 chose the grid *because* it is this canonical
  GPU pipeline.

What they have in common, and what we adopt: **SoA + GPU-resident + colored
constraint batches + a sort/scan-based broadphase.** What they accept that we
need not: a `float4`/SIMD-CPU heritage in their math, a fixed determinism stance
baked in years ago, and (for PhysX/FleX) no requirement to keep a *parity* CPU
path — their CPU and GPU solvers are different code with different results,
where ours must be the same simulation on both.

---

## 4. The literature we build on

The incumbents' behavior is the baseline to beat; the literature is where the
algorithm comes from (`Physics-Roadmap.md` §4, recency principle).

- **Graph-colored / batched constraint solving on the GPU.** Tonge, Benson,
  Erez, *"Mass Splitting for Jitter-Free Parallel Rigid Body Simulation"*
  (SIGGRAPH 2012) — the NVIDIA parallel rigid solver; and the constraint-
  batching/coloring line that PhysX-GPU and FleX (Macklin et al. 2014) ship.
  The core idea AQUA uses: partition the constraint graph by a greedy coloring
  so each color is an independent set (no shared bodies), then each color is a
  data-parallel dispatch and the sequence of colors recovers Gauss-Seidel
  coupling. Mass-splitting is the alternative that trades a Jacobi-style update
  (fully parallel, no coloring) for split masses + averaging; we keep it as the
  §11.3 fallback if coloring quality is poor on real scenes.
- **Parallel greedy graph coloring.** Jones & Plassmann (1993) and the
  Gebremedhin–Manne parallel-coloring line — the algorithm behind a GPU-side
  coloring kernel (the §11.3 follow-up; the first cut colors on the CPU, which
  is cheap because the adjacency is already the row buffer).
- **GPU sort & scan primitives.** Blelloch (1990) work-efficient prefix sum;
  Merrill & Grimshaw (2011) high-radix GPU radix sort; Harris/Sengupta scan.
  These are the broadphase's hash-sort-scan and the narrowphase's row-allocation
  prefix sum. AQUA authors them in OmegaSL because GTE exposes the dispatch
  substrate, not algorithmic primitives.
- **GPU uniform-grid broadphase.** Green, *"Particle Simulation using CUDA"*
  (NVIDIA 2010) — the canonical hash→sort→cell-start→neighbor-gather pipeline
  Phase 2 §6 already adopted; this phase is its OmegaSL realization.
- **Floating-point (non-)determinism.** The well-known result that IEEE-754
  results depend on operation order, FMA contraction, and reduction order, so a
  GPU's parallel reductions and a CPU's serial loop diverge in the last bits
  even when both are "correct." This is why §8 leans cross-path *stable, not
  bitwise*, and why within-path determinism is reachable only by removing
  order-dependent float atomics from the solve (which coloring does for free).
- **Small steps (Macklin et al. 2019).** Already in Phase 1's posture; relevant
  here because small `dt` keeps the colored-GS convergence gap from sequential-GS
  small per step — the regime where the port stays faithful to the CPU oracle.

**Recency audit (per the standing §4 principle).** Is there a *newer* parallel
rigid solver that beats colored-Gauss-Seidel for a faithful GPU port of AQUA's
shipped Phase 3 PGS? The audit (full detail §12): the newest threads —
**Vertex/Block Descent (Chen et al., SIGGRAPH 2024)** and **GPU-XPBD / unified
solvers (Müller 2020 + FleX line)** — are genuinely newer and genuinely better
*at being GPU solvers*, but they are **different algorithms**, not parallel
forms of sequential-impulse PGS. Adopting either would mean the GPU path no
longer matches the CPU oracle, which defeats the parity contract that is the
whole deliverable, and it would be making the roadmap §7.2 unified-solver
decision early. So for the **port**, colored-GS (Tonge 2012 lineage) is the
substrate-correct choice; VBD and GPU-XPBD are flagged as Phase 7 candidates
where the *algorithm itself* is on the table.

---

## 5. Where AQUA diverges — the openings

The incumbents' constraints are not ours; each divergence is an opening
(`Physics-Roadmap.md` §4 step 2).

- **We must keep a CPU path at parity; they don't.** PhysX/FleX have a GPU
  solver and a (different) CPU solver with different results. AQUA's CPU path is
  the *oracle the GPU is measured against* (§1 deliverable #2). This is a
  constraint, but it is also the opening: because the CPU path is already held
  to analytic oracles (Phases 1–4), we get a *two-level* correctness ladder for
  free — GPU vs CPU vs math — and a regression that catches a kernel bug the
  instant it diverges from a known-good serial answer. The incumbents have no
  such ladder.
- **Single math source over `Ty` → exact CPU/GPU operation-order match is
  achievable.** Phase 1's integrator is one template instantiated at `float`
  and `double`; the OmegaSL kernel is a third instantiation of the *same
  scalar recipe*. Where PhysX's CPU SIMD and GPU code are separately written
  (and separately drift), AQUA can write the velocity-half-step kernel as a
  line-by-line transliteration of [AQIntegrator.h:80-163](aqua/include/aqua/AQIntegrator.h:80),
  same Newton iteration count (the adaptive cap is a pure function of `‖ω‖·dt`,
  deterministic and warp-uniform — [AQIntegrator.h:59-66](aqua/include/aqua/AQIntegrator.h:59)),
  same cross-product order. The tolerance gap then comes *only* from the solver's
  colored traversal and from float reassociation — both bounded and measured.
- **We target all three backends from one kernel source.** omegaslc emits
  HLSL/MSL/GLSL from a single `.omegasl` (the compiler's whole reason for
  existing). AQUA writes each kernel once; the determinism story is then
  "within-path on a given backend" plus "tolerance-equivalent across backends,"
  not "rewrite per API." (The cost: only one backend is *runtime-verifiable* on
  any given host — see the off-platform note in §13.)
- **We can choose the numeric format per buffer.** GTE buffers are typed and
  the device advertises 16/64-bit support (`GTEDEVICE_FEATURE_SHADER_FLOAT16` /
  `_FLOAT64`, [GTEDevice.h:122-129](gte/include/omegaGTE/GTEDevice.h:122)). The
  solver accumulators that need range can stay `float`; positions that want
  precision over a large world can be a §11.6 follow-up to double-or-offset.
  The incumbents' `float4`-everywhere is not forced on us. (We do *not* cash
  this in Phase 5 — it's an opening noted, not taken; the port stays `float` to
  match the CPU `float` path.)
- **OmegaSL atomics have landed (§5.6 complete).** `atomic_int`/`atomic_uint`
  plus `atomic_add/min/max/and/or/xor/exchange`, `atomic_load`/`atomic_store`,
  and strong `atomic_compare_exchange` are available now (GPU-verified on
  Metal). So the pair-append bump allocator (§2 point 2) is a straight
  `atomic_add(pairCount, 1)` and reductions/prefix-scatter can use real
  intrinsics — no fallback needed. The one subtlety to state, not fight:
  **atomic-append *order* is non-deterministic** (which slot a pair lands in
  depends on race order), but it is **immaterial** here because the downstream
  `sort+unique` (§6.C step 5) re-establishes the canonical `(a, b)` ordering —
  the final pair list is deterministic regardless of append order. The
  count-then-scatter form Phase 2 §6 notes is kept only as an optional
  deterministic cross-check for the atomic path, not as a required fallback.

---

## 6. Proposed algorithm — the kernel pipeline

The CPU `stepInternal` becomes a sequence of compute dispatches over the SoA
buffers, encoded on `AQContext`'s command queue. The mechanical shape of every
dispatch is the worked GTE example
([main.mm:76-85](gte/tests/metal/ComputeTest/main.mm:76)): `startComputePass`
→ `setComputePipelineState` → `bindResourceAtComputeShader(buffer, slot)` per
buffer → `setComputeConstants(&uniforms, size)` for the per-dispatch scalars
(`dt`, `gravity`, body/row/color counts, color index) → `dispatchThreads` →
`finishComputePass`, then `submitCommandBuffer` + a fence. The per-sub-step pass
order, mirroring `stepInternal`:

**A. Kinematic apply + velocity half-step (one thread per body).** Direct
transliteration of the per-body loops at
[AQSpace.cpp:621-667](aqua/src/AQSpace.cpp:621). Two kernels (or one with a
branch on `activation`/`type` — [AQIntegrator.h:56](aqua/include/aqua/AQIntegrator.h:56),
the `switch` Phase 4 §8 anticipated):
```
kernel integrateVelocity(tid):           // one thread per body
    b = bodyState[tid]
    if b.invMass == 0 or b.activation == Sleeping: return
    // gravity + accumulated force; linear damping
    // torque → body frame; explicit Ib⁻¹ kick
    // implicit-gyroscopic Newton, iters = (‖ω‖²·dt² > thr²) ? cap : 1   // warp-uniform
    // angular damping; opt-in max-ω clamp
    bodyState[tid] = b
```
No atomics, no cross-thread reads — the embarrassingly-parallel map Phase 1 §8
promised. The fast-spin debug warning ([AQSpace.cpp:652](aqua/src/AQSpace.cpp:652))
stays on the CPU (it's a one-time `cerr`, not hot).

**B. World-AABB refresh + fatten (one thread per body).** Transliteration of
[AQSpace.cpp:705-718](aqua/src/AQSpace.cpp:705) and Phase 2 §6.A. Reads
`shapeIndex[]` + the shared shape table + hull-vertex pool, writes `worldAABB[]`
and re-fattens `fatAABB[]` (rotation-correct `|R|·h` bound, Phase 2 §6.1). One
thread per body, no interaction.

**C. Broadphase — the sort-based grid (Phase 2 §6.B), the primitive chain.**
```
1. gridHash(tid):     cellHash[tid] = hash(cellOf(center(fatAABB[tid])));  key[tid] = (cellHash[tid], tid)
2. radixSort(key[]):  stable LSD radix sort by cellHash      // AQUA-owned multi-pass kernel
3. cellStart(tid):    if key[tid].hash != key[tid-1].hash: cellStart[key[tid].hash] = tid   // run boundaries
4. pairScan(run):     for each body i in cell-run, for each j in the 27-cell neighborhood:
                          if i<j and fatAABB[i]∩fatAABB[j] and passFilter(i,j):
                              slot = atomicAdd(pairCount, 1);  pairOut[slot] = (i,j)
5. sortUnique(pair[]): radix sort pairs by (a<<32|b), then stream-compact duplicates
```
Steps 2 and 5 are AQUA-owned radix-sort kernels (no GTE primitive exists);
step 4's `atomicAdd` is a straight `atomic_add` (OmegaSL §5.6 atomics are
landed, §5). Its append order is non-deterministic but immaterial — step 5's
`sort+unique` canonicalizes the list. The output is Phase 2's ordered,
de-duplicated pair list — byte-identical ordering to the CPU broadphase because
the sort is stable and the hash is the shared function (§8 determinism).

**D. Narrowphase — per-pair manifold + row build, with prefix-sum allocation.**
Two-pass to handle the variable output (§2 point 3):
```
pass 1  countRows(pid):   one thread per candidate pair; run the (typeA,typeB)
                          branch (Phase 3 §6.A) far enough to know the contact
                          count; rowCount[pid] = 3 * contactCount   // or joint rows
pass 1b prefixSum(rowCount[]) → rowOffset[]                          // AQUA-owned scan
pass 2  buildRows(pid):   re-run the (typeA,typeB) branch fully; write the
                          manifold and its 3·k constraint rows into
                          rows[rowOffset[pid] ..], precomputing effectiveMass,
                          rA/rB, direction, bias exactly as the CPU does
```
The `(typeA,typeB)` branch is the divergence cost; we mitigate by **sorting
candidate pairs by shape-type-pair before pass 1** so a warp tends to run one
narrowphase path (a cheap extra radix key, reusing the step-5 sort). GJK/EPA for
hull pairs is a bounded-iteration loop (Phase 3 §6) — warp-uniform iteration cap,
same determinism discipline as the Newton step. Warm-start (Phase 3 §6.C) reads
the persistence cache, which on the GPU is the **sorted-array + binary-search**
form Phase 3 §8 specified (the cache key parallel-sorts the same way the pairs
do).

**E. Constraint-graph coloring (the enabling step for F).** Build, from the row
buffer's `bodyA`/`bodyB` adjacency, a partition of rows into colors such that no
two rows in a color share a (dynamic) body. First cut: **color on the CPU**
(§11.3) — the adjacency is already the row buffer, greedy coloring is linear and
cheap, and it is trivially deterministic. Output is a `colorOffset[]` /
`rowByColor[]` index list uploaded once per sub-step. (GPU coloring is the
§11.3 follow-up.)

**F. Colored Gauss-Seidel velocity solve (the heart of the port).** For each
color, in sequence, one parallel dispatch over that color's rows:
```
for iter in 0..N:                          // N = solverIterations (Phase 3, lean 8)
    for color in 0..numColors:             // sequential — recovers GS coupling
        solveColor(k):                     // one thread per row in this color
            r = rows[rowByColor[colorOffset[color] + k]]
            // compute rel-vel along r.direction from bodyState[r.bodyA], [r.bodyB]
            // λ = -(rel_v + r.bias) / r.effectiveMass  (+ CFM·accum for compliance)
            // clamp by kind: normal λ≥0; friction |λ|≤μ·peer.accum; joint per-kind
            // apply ±λ·direction (linear) or ±λ torque (isAngular) to the two bodies
            // r.accumImpulse += Δλ
```
Within a color no two rows share a body, so the two `bodyState` writes never
race — **no atomics in the inner solve**, which is exactly why a single GPU path
is run-to-run deterministic (§8). Friction rows read their normal row's
`accumImpulse` via `peerRow` ([AQContact.h:96](aqua/include/aqua/AQContact.h:96));
the coloring keeps a friction row and its normal peer in compatible colors (they
share both bodies, so they are never in the *same* color — the peer's accum from
the previous color/iteration is what the cone clip reads, matching the CPU's
within-sweep ordering up to the colored-traversal gap quantified in §8).

**G. Colored split-impulse position solve.** The same colored dispatch over the
same colors, with the pseudo-velocity rows (Phase 3 §6.E) — depth-only RHS, no
restitution — accumulating `pseudoLinear`/`pseudoAngular` per body. Separate
accumulators so it never touches real velocity (Catto 2009).

**H. Position half-step + split-impulse apply (one thread per body).**
Transliteration of [AQSpace.cpp:691-698](aqua/src/AQSpace.cpp:691): exp-map
orientation update, symplectic position update, then `+= pseudoLinear·dt`.
Clears force/torque accumulators. The debug NaN guard
([AQIntegrator.h:196-203](aqua/include/aqua/AQIntegrator.h:196)) becomes an
optional debug-build readback check (a kernel can't `assert`; §9 emits a loud
guard instead).

**I. Readback handoff.** After H, the body-state buffer is read back so the CPU
*cold* stages run on correct state: CCD swept-sphere ([AQSpace.cpp:672-687](aqua/src/AQSpace.cpp:672)),
AABB-refresh-driven debug emission, the per-`advance` query/trigger/island
refresh. The first cut tolerates this readback (§11.5); keeping the loop
GPU-resident across sub-steps (reading back only once per `advance`, not per
sub-step) is the §11.5 optimization once correctness is proven.

**Why this combination.** Each stage is the GPU form the earlier phases *named*
when they chose the algorithm: the grid because it is the canonical GPU
primitive chain, the row layout because it is coloring-ready, the integrator
split because the half-steps map to per-body maps. The only genuinely new design
is the **coloring + colored-GS solve** (E/F/G), and that is the one piece every
GPU rigid solver converges on (§3, §4). Nothing here changes the simulation; it
changes only *where and in what order the same arithmetic runs* — which is the
definition of a port.

**Alternative considered — Jacobi solve + mass-splitting (Tonge 2012), no
coloring.** Fully parallel (every row every iteration, no color sequencing),
no coloring pass. Rejected as the *lead* because pure-Jacobi converges slower
and is more jittery than colored-GS at the same iteration count, and
mass-splitting changes the per-row effective mass — a divergence from the CPU
oracle the parity test would flag. Kept as the §11.3 fallback for scenes where
the constraint graph colors badly (one giant island → few huge colors →
poor occupancy is not the failure; *many* colors → many tiny serial dispatches
is).

**Alternative considered — one mega-kernel per sub-step.** Encode the whole
step in a single dispatch with device-wide barriers between stages. Rejected:
OmegaSL/GTE expose pass-level dispatch, not device-wide barriers inside a
kernel; the sort/scan primitives need multiple passes anyway; and per-stage
dispatch is what lets the cold CPU stages interleave during bring-up. Revisit
if the per-dispatch overhead dominates on small scenes (§11.5).

---

## 7. New types AQUA must add (drafts)

All AQ-prefixed, no namespace (per `aqua/AGENTS.md`); GTE/OmegaSL backend types
stay behind the pimpl (no OmegaSL types in `include/aqua/*`).

**7.1 Execution-path selection — `include/aqua/AQContext.h` (public).**
```cpp
/// Which substrate AQUA runs the hot stages on. Selected from device features
/// (Physics-Roadmap §3 principle 3): never an #ifdef.
enum class AQExecPath : std::uint8_t {
    Auto,   ///< query GTEDeviceFeatures; GPU if usable compute, else CPU
    CPU,    ///< force the CPU reference path (the parity oracle / fallback test)
    GPU,    ///< force the GPU path (diagnostic; falls back + warns if unusable)
};
```
`AQContext::Create` **requires** an `OmegaGraphicsEngine` handle (the
groundwork from §1) so the context can always make pipelines/buffers and query
device features. The engine is mandatory — kREATE is GPU-first and always
supplies one — so the old queue-only factory is **removed**, not kept as a
CPU-only overload. Selecting the CPU path is `setExecutionPath(CPU)` or
automatic fallback (§1), never an engine-less context:
```cpp
// The ONLY factory. Engine is required; queue-only Create is removed.
static SharedHandle<AQContext> Create(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                                      SharedHandle<OmegaGTE::GECommandQueue> commandQueue);
void        setExecutionPath(AQExecPath path);   // default Auto
AQExecPath  executionPath() const;               // the *resolved* path (never Auto)
```

**7.2 SoA body store — `src/AQBodySoA.h` (internal).** The parallel arrays the
kernels read, mirroring `AQBodyState` field-for-field but flattened to scalar
arrays so each uploads as one coalesced GTE buffer. Kept in sync with the
authoritative `std::vector<std::shared_ptr<AQRigidBody>>`; the AoS `AQBodyState`
remains the `double` oracle's home.
```cpp
struct AQBodySoA {                         // host-side staging; mirrors into GEBuffers
    std::vector<float>    posX, posY, posZ;            // x[]
    std::vector<float>    velX, velY, velZ;            // v[]
    std::vector<float>    quatX, quatY, quatZ, quatW;  // q[]
    std::vector<float>    wbX, wbY, wbZ;               // ω_body[]
    std::vector<float>    invMass;                     // m⁻¹[]  (0 ⇒ static/kinematic)
    std::vector<float>    invInertiaX, invInertiaY, invInertiaZ;  // Ib⁻¹ principal
    std::vector<float>    forceX, forceY, forceZ, torqueX, torqueY, torqueZ;
    std::vector<float>    linDamp, angDamp, gravScale, maxAngSpeed;
    std::vector<float>    pseudoLinX, pseudoLinY, pseudoLinZ;     // split-impulse accum
    std::vector<std::uint8_t> activation;              // Active/Sleeping/Kinematic
    void resize(std::size_t n);
    void gatherFrom(const std::vector<std::shared_ptr<AQRigidBody>>& bodies);  // AoS→SoA
    void scatterTo  (std::vector<std::shared_ptr<AQRigidBody>>& bodies) const; // SoA→AoS (readback)
};
```
The matching OmegaSL `struct` for each buffer is one component group; the
worked example's `buffer<Data> name : slot` + `[in/out]` annotation
([main.mm:19-22](gte/tests/metal/ComputeTest/main.mm:19)) is the binding shape.

**7.3 The GPU pipeline owner — `src/AQComputeBackend.h` (internal, pimpl-only).**
Holds the engine handle, the compiled kernel library, the pooled buffers, and
the per-sub-step encode. This is the *only* file that includes OmegaGTE pipeline
headers; everything public stays clean.
```cpp
struct AQComputeBackend {
    static std::unique_ptr<AQComputeBackend> TryCreate(
        SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
        SharedHandle<OmegaGTE::GECommandQueue> queue);   // nullptr ⇒ no usable compute

    bool usable() const;                                 // device features gate
    void ensureCapacity(std::size_t bodies, std::size_t rows, std::size_t pairs);
    void uploadBodies(const AQBodySoA& soa);
    void encodeStep(const AQStepParams& p);              // dispatches A–H, fence
    void downloadBodies(AQBodySoA& soa);                 // the §6.I readback
};
```
`AQStepParams` is the push-constant payload (`dt`, `gravity`, counts, iteration
counts, color table offsets) set via `setComputeConstants`
([GERenderTarget.h:325](gte/include/omegaGTE/GERenderTarget.h:325)) — the
`constant<T>` path is landed and Metal-verified (push-constant work, repo
memory), so the per-dispatch scalars cost no buffer.

**7.4 The kernels — `src/kernels/*.omegasl`.** One source per stage (§6):
`AQIntegrateVelocity`, `AQIntegratePosition`, `AQRefreshAABB`, `AQGridHash`,
`AQRadixSort` (shared by broadphase pair sort, type sort, cache sort),
`AQPrefixScan` (shared by cell-start and row-allocation), `AQBroadphasePairs`,
`AQNarrowphase` (count + build), `AQSolveVelocityColor`, `AQSolvePositionColor`.
Each declares its buffers with explicit slots and `[in …, out …]` annotations
matching the bind order in `encodeStep`.

**7.5 Build wiring.** `aqua/CMakeLists.txt` gains the kernel sources; they are
compiled with the same `OmegaSLCompiler` the runtime example uses — the first
cut **embeds the `.omegasl` source and compiles at load** (`compile` →
`loadShaderLibraryRuntime`, [main.mm:34-35](gte/tests/metal/ComputeTest/main.mm:34)),
matching the worked example and avoiding a build-step dependency; a build-time
precompile (omegaslc → serialized lib) is the §11.4 optimization.

---

## 8. Data layout & GPU/numeric specialization

This phase **settles** roadmap §7.3 (data layout) and §7.4 (determinism) — the
two decisions the earlier phases leaned and deferred here.

- **Data layout — SoA, settled and now built.** Every prior §8 promised it;
  §7.2 is the realization. The flattened scalar arrays upload coalesced; a
  kernel thread for body `i` reads `posX[i]`, `velX[i]`, … at stride-1 offsets.
  `AQConstraintRow` ([AQContact.h:85](aqua/include/aqua/AQContact.h:85)) and
  `AQContactManifold` are already trivially-copyable and upload without
  repacking (their `OmegaGTE::FVec<3>` members are column-major contiguous — the
  GTE storage convention the Phase 1.1 note flagged; the SoA gather writes the
  three scalars explicitly to avoid relying on it). The CPU production path is
  migrated to read the same SoA store so the parity test compares like with
  like; the AoS `AQBodyState<double>` instantiation stays as the precision
  oracle only.
- **Determinism — stable across paths, bitwise within a path. Settled.** This
  is the §7.4 decision:
  - **Within one path on one device: bitwise-deterministic, asserted (§1
    deliverable #4).** Achieved by (a) the broadphase using a **stable** radix
    sort and the **shared** hash function, so the pair list and its order are
    reproducible; (b) the colored solve using **no order-dependent float
    atomics** — within a color no two rows touch the same body, so the velocity
    writes are disjoint and order-independent; (c) **no fast-math
    reassociation** on the step path and a fixed cross-product / Newton operation
    order shared with the CPU recipe.
  - **Across CPU↔GPU (and across GPU vendors/backends): stable and
    tolerance-equivalent, NOT bitwise.** Two reasons, both fundamental and
    documented rather than fought: float **reassociation/FMA contraction**
    differs between a serial CPU loop and a GPU's codegen, and the **colored
    traversal order** of the solve differs from the CPU's strict row order
    (colored-GS visits rows grouped by color, sequential-GS visits them in
    buffer order). The parity test (§9) therefore asserts agreement within a
    tolerance band — the `1e-4`-class band Phase 1's `float`/`double` oracle
    uses for state, relaxed by a measured factor for the accumulated-impulse
    fields where the traversal-order gap concentrates. The factor is *measured*
    on the settling stack and pinned as a regression constant, not guessed.
  - **The kREATE consequence (roadmap §6, cross-cutting determinism).** Lockstep
    netcode / replay that needs bitwise agreement across machines must pin a
    **single path** (run the CPU path on all peers, or the same GPU backend on
    identical hardware). AQUA exposes the resolved path (`executionPath()`,
    §7.1) so kREATE can enforce that. A bitwise-cross-path mode (integer/
    fixed-point accumulation, fixed reduction trees) is a real but expensive
    future option flagged in §11.2 — explicitly *not* built here.
- **Numeric format — `float` everywhere on the step path, to match the CPU
  `float` path.** The §5 opening (per-buffer 16/64-bit) is noted but **not
  taken** in Phase 5: the port's job is to match the CPU `float` oracle, and
  mixing formats would widen the parity gap. Large-world position precision
  (double-or-offset) is the §11.6 follow-up.
- **Buffer residency & pooling.** Bodies, rows, manifolds, pairs, and the
  sort/scan scratch are `GPUOnly` pooled buffers ([BufferDescriptor::GPUOnly](gte/include/omegaGTE/GE.h:197));
  the upload staging and the §6.I readback use `Upload`/`Readback` buffers via
  `GEBufferWriter`/`GEBufferReader` ([main.mm:55,88](gte/tests/metal/ComputeTest/main.mm:55)).
  `ensureCapacity` grows pools geometrically; no per-sub-step allocation
  (roadmap §6, "avoid per-frame allocation in the step path").

---

## 9. Validation — how we measure "better"

The reference is the CPU path, itself held to the Phase 1–4 analytic oracles
(`Physics-Roadmap.md` §4). "Better" here means *equivalent, faster, and as
deterministic as promised* — a faithful port, proven.

- **GPU settling stack (the headline).** The §1 deliverable #1 assertions,
  run on the GPU path: settle bounds, `< 1 mm` penetration, and the analytic
  per-contact `(11 − i)·m·g` normal impulse within 5%. If the GPU path settles
  the stack to the same analytic answer the CPU does, the colored solve is
  faithful.
- **CPU↔GPU parity (the substrate headline).** §1 deliverable #2: identical
  initial state, both paths, per-sub-step state agreement within the §8
  tolerance band over the full simulation, for both the stack and the bridge.
  The per-field band (tight on state, measured-relaxed on accumulated impulse)
  is pinned as regression constants.
- **The fallback is the same simulation.** §1 deliverable #3: feature-masked
  device forces CPU; same scene passes the same settling assertions. Guards
  against a fallback that silently degrades.
- **Within-path determinism.** §1 deliverable #4: two GPU runs → byte-identical
  state buffers each sub-step. The CPU path's existing re-run determinism
  (Phase 2/3/4) continues to pass.
- **Stage-isolation parity (bring-up regression).** Each increment (§13) lands
  with its *own* stage-only parity test before the next: integration-only
  (Phase 1 spinning body, GPU vs CPU within tolerance), broadphase-only (GPU
  ordered pair list byte-identical to CPU's), narrowphase-only (GPU
  manifolds/rows match CPU), solver-only (GPU colored solve on a fixed row
  buffer matches CPU PGS within tolerance). A failure localizes to one kernel,
  not "the GPU path is wrong."
- **Scaling (the reason the phase exists).** Step time vs body count on a
  uniformly-distributed scene, GPU vs CPU, logged as a series (not asserted as a
  wall-clock bound — machine-dependent). The GPU should cross over the CPU above
  some body count; below it, the readback/dispatch overhead dominates and the
  Auto path may legitimately prefer CPU (a §11.5 heuristic, logged).
- **Phase 1–4 regression.** The entire existing CPU battery
  (`aqua_rigid_body_test`, `aqua_broadphase_test`, `aqua_contact_test`,
  `aqua_phase4_test`) stays green — Phase 5 must not shift any earlier promise;
  the CPU path is byte-for-byte unchanged where the SoA migration is a pure
  data-layout refactor (proven by re-running the battery).

Metrics emitted as debug-draw / logged series (roadmap §3 principle 6, "author
for the 3am engineer"): per-stage GPU dispatch timing (via
`GTEDEVICE_FEATURE_TIMESTAMP_QUERIES` where available,
[GTEDevice.h:41](gte/include/omegaGTE/GTEDevice.h:41)), CPU↔GPU max per-field
divergence per sub-step, colors-per-step and rows-per-color (a degenerate
coloring is a perf cliff — loud-guard when color count approaches row count),
and a post-step NaN/inf readback guard replacing the CPU integrator's
debug-build `assert` (§6.H) — a body that goes non-finite on the GPU must fail
loud, not silently spread NaN. The debug bus already exists (Phase 1.1+); Phase
5 adds `AQDebugExecPath` (which path ran) and `AQDebugSolverColor` (color-coded
rows) bits to the existing `AQDebugFlags`, appended into the same
`drainDebugLines` buffer the kREATE adapter already drains — no new transport.

---

## 10. Public API additions

Deliberately minimal — the whole design goal is that the GPU path is invisible
to existing callers (`Physics-Roadmap.md` §3 principle 3). The additions, all on
`AQContext` unless noted:

- **`AQContext::Create(engine, queue)`** — now the **only** factory (§7.1). The
  `OmegaGraphicsEngine` is a required argument; the old queue-only factory is
  removed. This is a deliberate breaking change, justified by kREATE being a
  GPU-first engine that always has a live `OmegaGraphicsEngine` — there is no
  supported "AQUA without a graphics engine" configuration. CPU execution is
  still fully reachable (forced via `setExecutionPath(CPU)`, or automatic on a
  no-compute device), just not via an engine-less context.
- **`AQExecPath` enum + `setExecutionPath(AQExecPath)` / `executionPath()`** —
  the path control (§7.1). `Auto` is the default; `executionPath()` returns the
  *resolved* path so kREATE can pin it for lockstep (§8).
- **A test/diagnostic hook to force the fallback.** `setExecutionPath(CPU)` *is*
  that hook for the §1 deliverable #3 fallback test; the feature-mask override
  (simulating a no-compute device) is an internal test seam on
  `AQComputeBackend::TryCreate`, not public API.
- **No change to `AQSpace` / `AQRigidBody` / shapes / joints / queries / debug
  shape.** Bodies are still added, joints still created, `advance` still
  stepped, `drainDebugLines` still drained — identically. The SoA store and the
  kernels are entirely behind the pimpl.

A short Sphinx note documents that AQUA now selects GPU compute automatically
when the device supports it, that the result is equivalent (not bitwise
identical) to the CPU path across devices, and that lockstep consumers must pin
a path — the user-facing version of the §8 determinism contract.

---

## 11. Open decisions for this phase

1. **Execution-path selection threshold.** *Lean: Auto = "GPU if the device
   advertises usable compute (`maxComputeWorkGroupInvocations > 0` and shader
   model ≥ baseline) AND the scene exceeds a body/row count where the port pays
   off; else CPU."* The capability gate is unambiguous; the *count* threshold is
   a tunable (a small scene is faster on the CPU because of dispatch+readback
   overhead, §9 scaling). First cut: capability-only (GPU whenever usable),
   with the count heuristic as a logged §11.5 follow-up so we tune it on real
   data rather than guessing.
2. **Determinism guarantee — bitwise cross-path vs. stable cross-path (roadmap
   §7.4).** *Lean: stable cross-path, bitwise within-path* (§8). Bitwise
   cross-path costs integer/fixed-point accumulation and fixed reduction trees
   throughout the solver and broadphase — a large, perpetual tax on every
   kernel — and kREATE can get lockstep more cheaply by pinning one path. Build
   the cheap, strong promise now; leave the expensive, total promise as a
   documented future mode gated on a concrete kREATE netcode requirement.
3. **Where graph coloring runs — CPU vs. GPU.** *Lean: CPU first, GPU as a
   follow-up.* The adjacency *is* the row buffer; greedy coloring is linear,
   trivially deterministic, and the row count per sub-step is modest. A GPU
   coloring kernel (Jones–Plassmann, §4) removes a CPU-side serial step and a
   small upload, and matters once the loop is GPU-resident (§11.5) — until then
   CPU coloring is correct and not the bottleneck. The §6.E fallback if coloring
   quality is poor is **Jacobi + mass-splitting** (Tonge 2012), kept as the
   §6 documented alternative.
4. **Kernel compilation — runtime-compile vs. build-time precompile.** *Lean:
   runtime-compile the embedded sources first* (matches the worked example,
   [main.mm:34](gte/tests/metal/ComputeTest/main.mm:34); no build-step
   dependency; one code path). Build-time precompile (omegaslc → serialized lib,
   loaded once) cuts startup cost and is the obvious optimization, but it
   couples AQUA's build to the omegaslc toolchain invocation; defer until
   startup cost is shown to matter.
5. **GPU residency — per-sub-step readback vs. once-per-`advance` resident
   loop.** *Lean: per-sub-step readback first, resident loop second.* The cold
   CPU stages (CCD, query refresh, debug emission) read body state; the simplest
   correct thing is to read back after every sub-step (§6.I). The optimization
   is to keep the loop resident across the sub-steps of one `advance` and read
   back once at the end — but that requires the cold stages either to move onto
   the GPU or to run only at `advance` granularity (most already do). This is
   the single biggest perf lever and the natural Phase 5.x.
6. **Position precision for large worlds — `float` vs. double-or-offset.**
   *Lean: `float`, deferred.* The port matches the CPU `float` path; large-world
   precision (camera-relative offset or a `double` position buffer where the
   device supports `GTEDEVICE_FEATURE_SHADER_FLOAT64`) is a real need for big
   kREATE levels but is a separable change that would widen the Phase 5 parity
   gap if done now.
7. **Pair-append — atomic append vs. count-then-scatter.** *Lean: atomic
   append.* OmegaSL §5.6 atomics have landed (§5), so this is settled rather
   than hedged: the pair scan uses `atomic_add` for the bump allocator. The
   append order is non-deterministic but immaterial because `sort+unique`
   (§6.C) canonicalizes the list, so within-path determinism (§8) holds anyway.
   Count-then-scatter is retained only as an optional deterministic cross-check
   of the atomic path, not as a required fallback.

---

## 12. Recency-principle audit (addendum)

Roadmap §4's standing rule: the default answer is the newest viable algorithm
from the literature, with incumbents adopted only when nothing newer offers a
real improvement *for AQUA's substrate*. Phase 5 spans four portable subsystems —
integration, broadphase, narrowphase, solver — plus the cross-cutting determinism
and data-movement design. The audit, per subsystem:

- **Solver — colored Gauss-Seidel (Tonge 2012 lineage). Adopted; the newer
  alternatives are the Phase 7 fork, not a port.** The newest parallel-solver
  threads are **Vertex/Block Descent (Chen, Macklin et al., SIGGRAPH 2024)** —
  a block-coordinate-descent solver that is exceptionally GPU-parallel and more
  stable than PGS — and the **GPU-XPBD / unified-particle** line (Müller 2020;
  Macklin 2014 FleX). Both are genuinely newer and genuinely better *as GPU
  solvers*. Neither is adopted here, for a reason specific to this phase's
  contract: Phase 5 is a **faithful port of the shipped Phase 3
  sequential-impulse PGS**, measured against the CPU PGS as its oracle (§9).
  VBD and XPBD are *different algorithms* — porting to them would mean the GPU
  path no longer matches the CPU oracle, breaking the parity deliverable, and
  would be making the roadmap §7.2 unified-solver decision a phase early. They
  are flagged as the Phase 7 candidates where the algorithm itself is on the
  table. Colored-GS remains the substrate-correct choice *for a port*: it is the
  parallel form of the exact iteration the CPU runs.
- **Broadphase — sort-based grid (Green 2010). No divergence beyond Phase 2's
  audit.** Phase 2's recency audit already recharacterized the future BVH path
  as **PLOC++/PRBVH (Meister & Bittner 2018/2022)** and flagged **RT-core
  broadphase (Wang 2024)** as a hardware-gated Phase 5.x acceleration. That
  flag lands *here*: an RT-core broadphase is gated on
  `GTEDEVICE_FEATURE_RAYTRACING` ([GTEDevice.h](gte/include/omegaGTE/GTEDevice.h)),
  which is vendor-specific today, so AQUA's all-three-backends posture keeps the
  sort-based grid as the unconditional lead and the RT path as an opt-in
  acceleration — exactly as Phase 2 §12 predicted. No change to the lead.
- **Narrowphase — GJK/EPA + specialized, with accelerated GJK pending. No new
  divergence.** Phase 3's audit found **Nesterov-accelerated GJK (Montaut et al.,
  RSS 2024)** as a drop-in iteration swap; that is a CPU-side `AQGJK.cpp`
  maintenance patch independent of the port. On the GPU the same accelerated
  iteration applies unchanged (same support-function interface); the port simply
  carries whichever GJK iteration the CPU ships. No divergence created by
  going to the GPU.
- **Integration — single-source over `Ty`, no algorithmic change.** The kernel
  is a third instantiation of the Phase 1 recipe (§5); there is no "newer
  integrator" question here because the port deliberately does not change the
  integrator (the Phase 1 §13 audit covers the integrator's own recency — Lie-
  group variational integrators, deferred). Faithful transliteration is the
  requirement.
- **Determinism & sort/scan primitives — classical, by necessity.** Blelloch
  scan, Merrill–Grimshaw radix sort, and the IEEE-754 reassociation result are
  decades old and remain exactly right; "newer" here would be vendor-specific
  cooperative-matrix or subgroup tricks that trade portability (and
  determinism) for speed — the wrong trade for a parity-constrained port.

**Net conclusion.** The Phase 5 port adopts colored-Gauss-Seidel and the
canonical GPU broadphase/primitives — the substrate-correct choices *because the
phase is a port held to the CPU oracle*, not because nothing newer exists. The
newer solvers (VBD 2024, GPU-XPBD) are explicitly the Phase 7 architecture fork
(roadmap §7.2); the data layout chosen in §8 and the row schema inherited from
Phase 3/4 keep that fork open without prejudging it. RT-core broadphase is the
recorded hardware-gated Phase 5.x acceleration.

---

## 13. Implementation phasing (addendum)

§1–§12 are the prior-art brief; this section is the reviewable-increment
breakdown AGENTS.md requires before code lands. Each increment builds, keeps the
**entire Phase 1–4 CPU battery green** as a regression guard, and lands its own
stage-isolation parity test (§9) before the next begins. The §11 leans are
adopted as the decisions (capability-gated Auto #1, stable-cross-path/bitwise-
within-path #2, CPU-coloring-first #3, runtime-compile-first #4, per-sub-step-
readback-first #5, `float` #6, atomic-append-with-scatter-fallback #7).

- **5a — Engine/device plumbing + path selection (groundwork, lands first).**
  Make `OmegaGraphicsEngine` a **required** argument on `AQContext::Create`,
  threaded down to `AQSpace::Impl` behind the pimpl; **remove** the queue-only
  factory and migrate its one call site ([AQContext.cpp](aqua/src/AQContext.cpp));
  add `AQExecPath` + `setExecutionPath` / `executionPath`;
  `AQComputeBackend::TryCreate` queries `GTEDevice::features` and returns null
  on no-usable-compute. **No kernels yet** — every device resolves to
  `AQExecPath::CPU` and behavior is byte-for-byte Phase 4. Proves the plumbing
  and the fallback resolution in isolation. (This is the phase's one breaking
  API change; landing it first means the rest of Phase 5 builds on the final
  constructor shape.)
- **5b — SoA store + buffer pool + kernel build wiring.** `AQBodySoA`
  (§7.2) with `gatherFrom`/`scatterTo`; migrate the CPU production step to read
  the SoA store (the AoS `double` instantiation stays the oracle); pooled GTE
  buffers + `ensureCapacity`; the `src/kernels/` runtime-compile path
  (`compile` → `loadShaderLibraryRuntime`) wired with a trivial no-op kernel to
  prove the toolchain end-to-end. Regression: the full battery on the SoA-backed
  CPU path is byte-identical to the AoS path.
- **5c — Integration kernels.** `AQIntegrateVelocity` + `AQIntegratePosition`
  (§6.A, §6.H), line-by-line transliterations of the half-steps. Parity test:
  Phase 1 spinning-body + a free-fall scene, GPU vs CPU within tolerance;
  within-path bitwise re-run. First real dispatch through `encodeStep`.
- **5d — Broadphase kernels.** `AQRefreshAABB`, `AQGridHash`, `AQRadixSort`,
  `AQPrefixScan`, `AQBroadphasePairs` (atomic append + scatter fallback), pair
  sort+unique (§6.B, §6.C). Parity: GPU ordered pair list byte-identical to the
  CPU broadphase oracle on randomized moving scenes (Phase 2's own oracle, now
  cross-path).
- **5e — Narrowphase kernels.** `AQNarrowphase` count+build with the prefix-sum
  row allocation and the type-pair pre-sort (§6.D); the sorted-array warm-start
  cache. Parity: GPU manifolds + rows match CPU narrowphase (depth/normal/row
  fields) within `1e-4`.
- **5f — Colored solver kernels.** CPU-side greedy coloring producing
  `colorOffset[]`/`rowByColor[]`; `AQSolveVelocityColor` + `AQSolvePositionColor`
  dispatched per color per iteration (§6.E–G); the degenerate-coloring loud
  guard. Parity: GPU colored solve on the Phase 3 settling stack matches CPU PGS
  within the measured-relaxed impulse band; the §8 cross-path tolerance constants
  are pinned here from the measured divergence.
- **5g — Deliverable tests + Auto wiring + docs.** The four §1 deliverables
  (GPU stack, CPU↔GPU parity on stack + bridge, forced-CPU fallback, within-path
  determinism) wired into `tests/CMakeLists.txt`; `AQExecPath::Auto` resolution
  turned on (capability-gated); the Sphinx determinism note (§10). The Phase 3/4
  regression battery re-run on both paths.

**Off-platform note (per the repo's multi-backend convention).** Phase 5 is the
first AQUA work with backend-specific surfaces: the OmegaSL kernels compile to
HLSL/MSL/GLSL from one source, but only **one backend is runtime-verifiable on
any given host**. On this macOS host that is **Metal** (repo memory: only Metal
compiles+runs here; D3D12/Vulkan are write-only and need CI). So each kernel
increment is runtime-verified on Metal here; the D3D12 and Vulkan emissions are
**source-verified** via `omegaslc -S` (which emits the target shader source —
repo memory) and must be **built and run by the user on Windows/Linux CI**
before the increment is considered cross-backend-complete. Do not assume a
D3D12/Vulkan kernel ran; work only from CI/user-reported compiler+run output,
the same WSL/Windows handoff discipline AGENTS.md mandates for the Windows
build.

---

*Brief status: proposal. The two decisions this phase settles for the whole
roadmap — data layout (§7.3 → SoA, built in 5b) and determinism (§7.4 →
stable-cross-path / bitwise-within-path, §8) — should be confirmed before the
solver kernel (5f) lands, because they govern how every kernel is written. This
document is the Phase 5 entry of the per-phase prior-art series roadmap §4
establishes, and follows the conventions of `Phase-1-Dynamics-Math-Core.md`
through `Phase-4-Joints-Queries-Sleeping.md`. Phase 5 makes good on the
compute-first promise (`Physics-Roadmap.md` §3 principle 3) without changing the
simulation; Phase 6 (particles) reuses this buffer-pool + dispatch substrate
wholesale, and Phase 7's unified-XPBD fork (roadmap §7.2) is kept open — not
prejudged — by porting the existing PGS faithfully rather than replacing it.*
