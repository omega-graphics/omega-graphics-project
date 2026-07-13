# AQUA Phase 1.1 — Rigid-Body Completion & Observability

**Completion brief & proposal.** Phase 1 landed the dynamics & math core — the
body-frame symplectic Lie integrator with implicit gyroscopic, the `AQMath`
surface, and a passing Dzhanibekov / precession / parity test suite. This brief
covers the **bridge to Phase 2**: the rigid-body features Phase 1 *scoped but
deferred*, the robustness the guiding principles ask for, the one targeted fix the
Phase 1 *finding* earned, and the **neutral debug surface** two sibling documents
now depend on. It is deliberately a *finishing* phase, not a new subsystem — so
its prior-art sections are lighter than a from-scratch brief, and most of its math
already exists in the repo and only needs wiring.

It follows the conventions of `Phase-1-Dynamics-Math-Core.md` and
`Phase-2-Collision-Shapes-Broadphase.md` (AQ-prefixed, no namespace, header-only
`Ty`-generic where the code is kernel-bound, pimpl on the public surface, a
runnable `tests/` deliverable).

---

## 1. Scope & deliverable

**Goal.** Finish the rigid-body core so Phase 2 stands on a complete foundation:
the bounding-volume math the roadmap scoped to Phase 1, full mass-property input,
conserved-quantity observability, integrator robustness aimed at the Phase 1
drift finding, and AQUA's **drainable debug-primitive surface** (the dependency
the kREATE debug-draw plan surfaced).

**Runnable deliverable.** The Phase 1 Dzhanibekov scene, extended to prove the new
surface end to end:
- driven from a **full (non-diagonal) inertia tensor** diagonalized via the
  already-implemented `AQdiagonalizeInertia`, reproducing the same flip and
  conservation as the diagonal path;
- asserting **conserved-quantity accessors** (`angularMomentum`, `kineticEnergy`)
  rather than recomputing `L` in the test;
- showing **adaptive gyroscopic iteration** measurably cuts ‖L‖/energy drift at a
  *coarse* sub-step where Phase 1 only warned (the §6 fix);
- **emitting drainable debug primitives** (COM, body axes, the `L` vector) that the
  test drains and checks — the exact surface kREATE's `DebugDraw-Implementation-
  Plan.md` §7 consumes.

Plus unit tests for `AQAABB` and the full-tensor → principal-moments path. It lives
in `tests/` next to `aqua_math_test` / `aqua_dynamics_test`.

**What this closes from Phase 1 / the roadmap.** `Physics-Roadmap.md` §5 Phase 1
lists, as Phase 1 work, *"bounding volumes"* and *"center of mass"* and *"inverse
inertia tensor (world-space)"*; Phase 1 shipped the integrator and deferred these
because the tumbling deliverable didn't exercise them. Phase 1.1 lands them, plus
the §9 *"metrics emitted as debug-draw"* promise Phase 1 only partially met (it
logged to stderr; it never emitted structured primitives).

**Out of scope, by design:** collision shapes, AABB-*of-a-shape*, and broadphase
(Phase 2 — Phase 1.1 ships the AABB *type and math*, not per-body bounds, since a
body has no geometry until Phase 2); contacts (Phase 3); the OmegaSL port
(Phase 5); kinematic bodies and CCD (Phase 4).

---

## 2. What "finishing" means here — the gap inventory

| Item | Phase 1 state | Phase 1.1 target |
|---|---|---|
| Bounding volumes (`AQAABB`) | none (roadmap scoped it to Phase 1) | `AQAABB<Ty>` + merge/overlap/fatten/surface-area/oriented-box bound, in `AQMath.h` |
| Full inertia tensor input | `AQdiagonalizeInertia` exists but **unwired**; only diagonal moments accepted | `AQBodyDesc` accepts a full 3×3 tensor; `addBody` diagonalizes + folds into orientation |
| Center of mass | implicit — `position` *is* the COM | COM concept reserved in the model so Phase 2 offset/compound shapes don't reshape state |
| World inverse inertia | `AQworldInvInertia` exists but **unexposed** | public accessor; reused by the momentum/energy accessors |
| Conserved-quantity accessors | tests recompute `L`/`E` by hand | `angularMomentum()`, `linearMomentum()`, `kineticEnergy()` on the body |
| Integrator drift | O(dt), secular; only a fast-spin *warning* | **adaptive gyroscopic iteration** mitigates it where the warning fired |
| Damping / clamp | none | optional linear/angular damping; opt-in max-angular-speed clamp (Chaos-style) |
| Gravity control | per-space, uniform | per-body `gravityScale` |
| NaN/inf robustness | none (only the spin warning) | debug-build loud guard when a body goes non-finite |
| Debug emission | stderr logs only | **neutral drainable `AQDebugLine` stream** (the kREATE-plan §7 dependency) |

The throughline: Phase 1.1 is mostly *wiring already-built math* (`AQdiagonalize-
Inertia`, `AQworldInvInertia`) into the public surface, plus three genuinely new
but small pieces — `AQAABB`, adaptive iteration, and the debug stream.

---

## 3. Prior art — only where it's load-bearing

This phase is completion, so it borrows narrowly:

- **Adaptive / multi-iteration implicit gyroscopic (Catto, GDC 2015).** Phase 1
  used Catto's *single* Newton iteration. Catto's method generalizes to *k*
  iterations of the same body-frame solve; each iteration drives the residual
  `f = dt·ω×(Ib·ω)` toward zero, and a few iterations sharply reduce the
  per-step conservation error when `‖ω‖·dt` is large. We already have the exact
  Jacobian and the GTE analytic 3×3 inverse — iterating is a loop, not new math.
- **Max-angular-velocity clamp (Epic Chaos).** Chaos clamps angular speed for
  stability on fast spinners. We adopt it as an **opt-in** safety valve (it
  changes the physics, so off by default), complementing the adaptive iteration.
- **Exponential / linear damping (every engine).** Per-sub-step velocity decay
  `v ← v / (1 + c·dt)` (exponential, unconditionally stable) is the standard drag
  model; it also bounds the slow energy *growth* the O(dt) scheme can exhibit.
- **Bounding volumes (Ericson, "Real-Time Collision Detection", 2004).** Standard
  AABB merge/overlap and the `|R|·h` oriented-box bound (§6.1 of the Phase 2
  brief). Reference, not novelty.

No deep survey here — the novel algorithmic work was Phase 1's, and the genuinely
new subsystem work is Phase 2's.

---

## 4. The Phase 1 finding, and the fix

Phase 1 measured the implicit-gyroscopic step's conservation error as **clean
O(dt) and secular** (‖L‖ ≈1.4%, energy ≈2.7% at dt=1/2000 on an 8 rad/s
asymmetric spinner; ≈20× worse at the `AQContext` default 1/120). We responded
with a *warning* (`AQSpace.cpp`, per-body, re-armed on timestep change) and a doc
note that small sub-steps are a correctness requirement (`AQContext.h`).

Phase 1.1 turns part of that warning into *mitigation*: when the per-step rotation
`‖ω‖·dt` exceeds a threshold, run **a few Newton iterations** of the gyroscopic
solve instead of one. The error is dominated by the single-iteration residual at
large per-step angles; 2–4 iterations cut it substantially at a cost of a handful
of FLOPs and one extra 3×3 inverse each — far cheaper than globally shrinking the
sub-step, and it keeps the existing warning as the "you're past where even this
helps, reduce the timestep" signal. The §11.1 decision (implicit gyroscopic over
momentum form) stands; this strengthens the chosen path rather than reopening it.

---

## 5. Where AQUA diverges — consistent with Phase 1

Nothing new in stance; Phase 1.1 inherits Phase 1's openings and extends them:
- **Same code, two precisions.** `AQAABB<Ty>` and the extended `AQStepBody<Ty>`
  stay generic, so the `double` oracle still validates the `float` path — the
  adaptive iteration and damping are exercised at both precisions by the parity
  test.
- **Reuse, don't re-derive.** `AQdiagonalizeInertia` (Jacobi) and
  `AQworldInvInertia` already exist in `AQMath.h`; Phase 1.1 *wires* them, it does
  not add competing implementations.
- **Debug as data, not rendering.** AQUA emits neutral `AQDebugLine` primitives
  and owns no renderer — kREATE renders (the boundary `Physics-Roadmap.md` §6 and
  the kREATE debug-draw plan §7 both assume). This keeps the sibling dependency
  one-directional: AQUA exposes a drainable buffer, kREATE pulls it.

---

## 6. Proposed work

### 6.1 Bounding-volume math — `AQAABB` (in `AQMath.h`)
The Phase 2 brief (§7) already drafted `AQAABB<Ty>`; Phase 1.1 lands it now as
foundational math so Phase 2 consumes rather than defines it (resolving the
overlap between the two briefs in favour of the earlier phase). Adds the
oriented-box bound `AQaabbOfOrientedBox(center, halfExtents, q)` using `|R|·h`
(Phase 2 §6.1), plus merge / overlaps / fattened / surfaceArea / center.

### 6.2 Mass-property completion
- `AQBodyDesc` gains an optional **full inertia tensor** (`AQMat3<float>`); when
  set, `addBody` calls `AQdiagonalizeInertia` to get principal moments + a
  principal-axis quaternion, and **folds that rotation into the body orientation**
  (PhysX/Chaos-style), storing diagonal `invInertiaBody` exactly as today. The
  diagonal `inertiaPrincipalMoments` path is unchanged and remains the fast default.
- A public **`worldInverseInertia()`** accessor (wrapping `AQworldInvInertia`),
  needed by Phase 3 contacts and by the momentum accessors below.
- **Center of mass:** reserve a COM offset in the body model (state + desc field,
  defaulted to zero = origin), but defer the offset *wiring* to Phase 2, where
  shape local-transforms create the first case of COM ≠ pose origin. Reserving it
  now means Phase 2 adds geometry, not a state refactor (§11.7).

### 6.3 Conserved-quantity accessors
`linearMomentum()` (`m·v`), `angularMomentum()` (world `L = R·Ib·ω_b`),
`kineticEnergy()` (`½m‖v‖² + ½ ω_b·Ib·ω_b`). These make the deliverable assert
against the engine's own numbers and give gameplay/debug a real handle.

### 6.4 Integrator robustness (`AQStepBody`, `AQIntegrator.h`)
- **Adaptive gyroscopic iteration** (§4): iterate the Newton step up to a small
  cap when `‖ω‖·dt` exceeds a threshold; deterministic and `Ty`-generic.
- **Linear/angular damping**: per-body coefficients, exponential per-sub-step.
- **Opt-in max-angular-speed clamp**: `0 ⇒ unlimited` (default); else clamp
  `‖ω_b‖`.
- **Per-body `gravityScale`** applied in the linear step.
- **NaN/inf guard** (debug builds): if a body's state goes non-finite after a
  step, emit one loud diagnostic and (debug) assert — the "fail loud, not silent
  NaN spew" principle (`Physics-Roadmap.md` §6).

### 6.5 The neutral debug surface
`AQDebugLine` value type + a per-space drainable buffer the space fills each step
according to a flag set (body axes, velocity/angular-velocity vectors, COM,
momentum). AQUA emits; the consumer drains and clears. This is the surface
kREATE's adapter (debug-draw plan §7) replays as `Kreate::DebugDraw::line` calls,
and the bus Phase 2 extends with AABBs + overlapping pairs.

---

## 7. New / changed types (draft)

`AQAABB` joins `AQMath.h`; the debug type and the body-state extensions are below.
All AQ-prefixed, no namespace.

```cpp
// --- AQMath.h: bounding volume (also drafted in the Phase 2 brief; lands here) ---
template<class Ty>
struct AQAABB {
    AQVec3<Ty> min, max;                       // (constructed via ::Create() — Matrix ctor is private)
    bool       overlaps(const AQAABB& o) const;
    AQAABB     merged(const AQAABB& o) const;
    AQAABB     fattened(Ty margin) const;
    Ty         surfaceArea() const;
    AQVec3<Ty> center() const;
};
using FAABB = AQAABB<float>;

template<class Ty>                             // |R|·h oriented-box world bound (Phase 2 §6.1)
AQAABB<Ty> AQaabbOfOrientedBox(const AQVec3<Ty>& center, const AQVec3<Ty>& halfExtents,
                               const OmegaGTE::Quaternion<Ty>& q);

// --- AQIntegrator.h: AQBodyState<Ty> gains (defaults keep Phase 1 behaviour) ---
//   Ty         linearDamping   = 0;           // v /= (1 + c·dt)
//   Ty         angularDamping  = 0;
//   Ty         gravityScale    = 1;
//   Ty         maxAngularSpeed = 0;           // 0 ⇒ unlimited
//   AQVec3<Ty> comOffset       = 0;           // reserved; wired in Phase 2
// AQStepBody<Ty>: gravityScale in step 1; adaptive Newton iterations in step 3;
//   damping + optional clamp after step 3; NaN guard (debug) after step 5.

// --- AQCollision.h is Phase 2; the debug type lands now in a small AQDebug.h ---
struct AQDebugLine { OmegaGTE::FVec<3> a, b; float rgba[4]; };
enum AQDebugFlags : std::uint32_t {       // what a space emits each step
    AQDebugNone        = 0,
    AQDebugBodyAxes    = 1u << 0,         // RGB principal axes at the COM
    AQDebugVelocity    = 1u << 1,         // linear velocity vector
    AQDebugAngularVel  = 1u << 2,         // angular velocity vector
    AQDebugMomentum    = 1u << 3,         // world angular-momentum L vector
};
```

---

## 8. Data layout & GPU/numeric specialization

Consistent with Phase 1 / Phase 2: the new per-body scalars (`linearDamping`,
`angularDamping`, `gravityScale`, `maxAngularSpeed`, `comOffset`) extend the SoA
`AQBodyState`; they upload as plain columns and the adaptive-iteration loop is
still one-thread-per-body with no atomics (the GPU kernel shape Phase 1
established). The `AQDebugLine` buffer is an append-only SoA array drained per
frame — write-only on the sim side, so it never feeds back into the deterministic
step. Determinism: the adaptive iteration count is a pure function of `‖ω‖·dt`,
so it does not introduce path divergence between the CPU and (future) GPU runs.

---

## 9. Validation — how we measure "finished"

- **Full-tensor parity.** A body built from a full inertia tensor (a known
  rotation of a diagonal one) must reproduce — to oracle tolerance — the diagonal
  body's trajectory: same flip, same conserved `L`. Directly tests the
  `AQdiagonalizeInertia` wiring against the Phase 1 path.
- **Adaptive-iteration improvement (the headline robustness check).** At a coarse
  sub-step where Phase 1 warned, the adaptive path's ‖L‖/energy drift must be
  measurably below the single-iteration path's — asserted as a ratio, not an
  absolute (the honest, dt-independent claim, as in Phase 1).
- **Accessor correctness.** `angularMomentum`/`kineticEnergy` match the test's
  hand-computed oracle; `worldInverseInertia` at identity equals
  `diag(1/moments)`.
- **AABB math.** `overlaps`/`merged`/`fattened` against hand cases;
  `AQaabbOfOrientedBox` always contains the box's 8 rotated corners (the
  rotation-correctness check Phase 2 §9 will reuse).
- **Damping & clamp.** Damping monotonically reduces speed with no sign flip;
  the clamp holds `‖ω‖` at its cap; both are no-ops at their defaults (Phase 1
  trajectories unchanged — a regression guard).
- **Debug stream.** With `AQDebugBodyAxes|AQDebugMomentum` set, draining yields
  the expected line count and the momentum line points along the conserved `L`.
- **NaN guard.** A deliberately poisoned body trips the loud guard in a debug
  build (and the guard compiles out under `NDEBUG`).

Metrics continue to be emitted as debug-draw / logged series (principle 6); the
new debug stream *is* that emission, now structured.

---

## 10. Public API additions

Extends the Phase 1 surface (`AQBodyDesc`, `AQRigidBody`, `AQSpace`) without
breaking pimpl. New members marked `// new`.

```cpp
struct AQUA_EXPORT AQBodyDesc {
    // ... Phase 1 fields ...
    AQMat3F  inertiaTensor;        // new — optional full tensor; diagonalized if set
    OmegaGTE::FVec<3> centerOfMass = OmegaGTE::FVec<3>::Create(); // new — reserved (Phase 2 wiring)
    float    linearDamping  = 0.f; // new
    float    angularDamping = 0.f; // new
    float    gravityScale   = 1.f; // new
    float    maxAngularSpeed = 0.f;// new — 0 ⇒ unlimited
};

class AQUA_EXPORT AQRigidBody {
public:
    // ... Phase 1 API ...
    OMEGA_NODISCARD OmegaGTE::FVec<3> linearMomentum()  const;   // new
    OMEGA_NODISCARD OmegaGTE::FVec<3> angularMomentum() const;   // new (world)
    OMEGA_NODISCARD float            kineticEnergy()    const;   // new
    OMEGA_NODISCARD OmegaGTE::FMatrix<3,3> worldInverseInertia() const; // new

    void  setLinearDamping(float c);   OMEGA_NODISCARD float linearDamping()  const;  // new
    void  setAngularDamping(float c);  OMEGA_NODISCARD float angularDamping() const;  // new
    void  setGravityScale(float s);    OMEGA_NODISCARD float gravityScale()   const;  // new
    void  setMaxAngularSpeed(float s); OMEGA_NODISCARD float maxAngularSpeed()const;  // new
};

class AQUA_EXPORT AQSpace {
public:
    // ... Phase 1 API ...
    void setDebugFlags(std::uint32_t flags);                         // new (AQDebugFlags)
    OMEGA_NODISCARD std::vector<AQDebugLine> drainDebugLines();        // new — read & clear
};
```

> **Folded-in groundwork.** `AQBodyDesc::inertiaTensor` defaults to zero/unset, so
> existing diagonal-moment descriptors are untouched; `addBody` only diagonalizes
> when a non-zero tensor is supplied. The new `AQRigidBody::Impl` scalars default
> to Phase 1 behaviour (no damping, unlimited spin, gravityScale 1), so every
> Phase 1 test passes unchanged — the regression guard in §9.

No OmegaSL or backend types cross into `include/aqua/*`; `AQDebugLine` carries the
borrowed `FVec<3>` only, per the boundary rule.

---

## 11. Open decisions for this phase

1. **Adaptive iteration policy — fixed cap gated on `‖ω‖·dt` vs. iterate-to-
   residual-tolerance.** *Lean: small fixed cap (≤4) gated on the per-step angle*
   — deterministic, bounded cost, GPU-uniform (no per-thread variable loop
   length). Iterate-to-tolerance is more accurate but introduces path divergence
   the determinism target (roadmap §7.4) disfavours.
2. **Damping model — exponential `v/(1+c·dt)` vs. linear `v·(1−c·dt)`.** *Lean:
   exponential* — unconditionally stable, no sign flip at large `c·dt`.
3. **Max-angular-speed clamp default — off vs. on.** *Lean: off (opt-in)* — it
   changes the physics; the adaptive iteration is the default stability path, the
   clamp is the explicit safety valve.
4. **`AQAABB` home — `AQMath.h` vs. a new `AQBounds.h`.** *Lean: `AQMath.h`* — it
   is foundational math and Phase 2's `AQCollision.h` already includes `AQMath.h`.
   (This supersedes the Phase 2 brief's tentative placement; resolved in favour of
   the earlier phase.)
5. **Debug surface shape — drainable buffer vs. callback/visitor.** *Lean:
   drainable buffer* (`drainDebugLines`) — matches the *pull* model the kREATE
   adapter uses (debug-draw plan §7), is deterministic, and ports to a GPU
   append-buffer later. A callback would couple AQUA to the consumer's threading.
6. **Mass-property input — keep both diagonal moments and full tensor.** *Lean:
   both* — diagonal is the fast default; the full tensor is diagonalized via the
   already-built Jacobi path for arbitrary (Phase 2 hull) shapes.
7. **Center of mass — introduce the offset now vs. reserve and wire in Phase 2.**
   *Lean: reserve now, wire in Phase 2* — the field enters the model so Phase 2's
   offset/compound shapes add geometry rather than refactor body state, but the
   offset has no effect until a shape needs it. Flagged because a non-zero COM
   touches every accessor (`applyForceAtPoint`'s torque arm, `angularMomentum`),
   so the wiring is deliberately Phase 2's, with the shapes that motivate it.

---

*Brief status: proposal. A finishing/bridge phase between Phase 1 (done) and
Phase 2 (next). The §11 decisions are lighter-weight than a new subsystem's;
the one worth settling first is the adaptive-iteration policy (#1), since it
touches the integrator hot path the rest of the engine runs on.*
