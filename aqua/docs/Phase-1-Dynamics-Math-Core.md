# AQUA Phase 1 — Dynamics & Math Core

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a solver is written: what PhysX and Chaos do,
which papers we improve on, what we change for AQUA's substrate, and how we will
measure "better." It covers **Phase 1 — Dynamics & math core [Newtonian]** only:
promoting bodies from points to rigid bodies, and growing the math from "one
`Vec3`" into what dynamics needs. No collision (Phase 2+), no GPU port yet
(Phase 5) — but every choice here is made so those phases inherit it cleanly.

---

## 1. Scope & deliverable

**Goal.** Give bodies orientation and rotational dynamics, and add the math to
integrate them correctly under applied force, torque, and off-center impulse.

**Runnable deliverable.** A torque-free **asymmetric** body given an off-center
impulse that tumbles with the *tennis-racket flip* (the Dzhanibekov intermediate-
axis instability), while **conserving angular momentum** and bounding energy drift
over thousands of sub-steps — plus a body under constant torque that precesses
correctly. This is the scene that proves the integrator; it lives in `tests/`
next to this brief.

**Out of scope here, by design:** collision shapes/broadphase (Phase 2), contact
solving (Phase 3), the OmegaSL compute port (Phase 5). We design the data and the
math so those are drop-in, not rewrites.

---

## 2. Why rotation is the hard part

Linear motion is a flat-space problem; the current placeholder
(`AQSpace::stepInternal`, semi-implicit Euler) already does it correctly.
Rotation is where naive integration fails, for three coupled reasons:

1. **Orientation lives on a curved manifold (SO(3)).** A unit quaternion
   parameterizes it. The textbook update `q += dt·½·ω·q` leaves the unit sphere,
   so it must be renormalized every step — and renormalization is lossy, bleeding
   accuracy on fast spinners.
2. **Euler's equation is nonlinear.** In the body frame,
   `I_b·ω̇ + ω × (I_b·ω) = τ_b`. The **gyroscopic term** `ω × (I_b·ω)` is what
   makes a thrown phone flip. Integrated *explicitly*, it injects energy and
   diverges; asymmetric or fast bodies blow up within a second.
3. **World inertia rotates with the body.** `I_world⁻¹ = R · I_b⁻¹ · Rᵀ` must be
   recomputed as orientation changes; using a stale tensor corrupts the response.

A correct Phase 1 has to handle all three at once, cheaply, on a path that will
later run as a GPU kernel.

---

## 3. Prior art — how the incumbents solve it

Studied to understand the terrain and the failure modes, **not** to transcribe.
PhysX is BSD-licensed and readable; Chaos ships with Unreal's source. The
descriptions below are drawn from their published talks, docs, and source
structure and are representative, not a claim to quote current internals.

**NVIDIA PhysX (PhysX 5).**
- **Semi-implicit (symplectic) Euler** at the velocity level: integrate velocity
  from forces first, then advance position with the *new* velocity. Cheap and
  stable for linear motion; PhysX 5 sub-steps it (TGS).
- **Inertia stored diagonalized.** Mass-property cooking diagonalizes the inertia
  tensor to its principal moments and folds the principal-axis rotation into the
  body's orientation. `I_b` is therefore a 3-vector; `I_b⁻¹` is reciprocals; the
  gyroscopic term is a handful of FLOPs.
- **Optional implicit gyroscopic.** Behind `eENABLE_GYROSCOPIC_FORCES`, PhysX
  applies an implicit gyroscopic treatment rather than dropping the term — the
  same one-Newton-iteration-in-body-frame idea formalized below — because the
  explicit term is unstable.
- **Orientation** is integrated from angular velocity via an angle/axis
  (exponential-map-style) update and renormalized.

**Epic Chaos (Unreal Engine 5).**
- `FPBDRigidsEvolution`: **semi-implicit Euler** integration of linear and angular
  state, quaternion renormalized each step, inertia stored as diagonal principal
  moments — structurally similar to PhysX.
- Heavy **sub-stepping** plus a **max-angular-velocity clamp** for stability.
- **Gyroscopic torque** support exists as a per-particle option and is historically
  conservative/off-by-default; the energy the explicit term adds is a known pain
  point they damp around.

**The shared shape:** both are `float`-committed, SIMD-hand-tuned, diagonal-inertia,
semi-implicit-Euler engines that treat the gyroscopic term gingerly because it is
the thing most likely to explode. That is the bar to clear — and the set of
constraints we do *not* share.

---

## 4. The literature we build on

The research leads the shipped engines by years. The pieces we combine:

- **Baraff & Witkin, "Physically Based Modeling" (SIGGRAPH course notes).** The
  canonical rigid-body formulation; the **momentum form** (state = linear &
  angular momentum, recover `ω = I_world⁻¹·L`) conserves angular momentum by
  construction under zero torque.
- **Grassia, "Practical Parameterization of Rotations Using the Exponential Map"
  (Journal of Graphics Tools, 1998).** Integrating orientation by the quaternion
  **exponential map** of the rotation vector `φ = ω·dt` keeps the quaternion on
  the manifold *exactly* (no renormalization drift) and is exact for constant `ω`.
- **Catto, "Numerical Methods" (GDC 2015).** The **implicit gyroscopic** solve: one
  Newton–Raphson iteration of Euler's equation in the body frame, stable where the
  explicit term diverges. Reduces to a single 3×3 linear solve.
- **Kharevych et al., "Geometric, Variational Integrators for Computer Animation"
  (SCA 2006)**; **Lee/Leok/McClamroch, Lie-group variational integrators (2007)**;
  **Munthe-Kaas, RK methods on Lie groups (1998).** The Lie-group/variational
  family: momentum-preserving, near-exact energy behavior over long runs — the
  theory underwriting "exp-map + momentum/implicit-gyro" as a principled scheme,
  not a hack.
- **Müller et al., "Detailed Rigid Body Simulation with Extended Position Based
  Dynamics" (SCA 2020).** The **small-steps** insight: many small sub-steps with
  one solve each beat few big steps with many iterations, at equal cost. AQUA's
  `AQContext` already owns a fixed-sub-step accumulator, so we are positioned to
  exploit this for free in the integrator — well before the XPBD work in Phase 7.

---

## 5. Where AQUA diverges — the openings

This is the part the incumbents' architectures can't easily copy back, grounded in
the actual GTE math layer (`gte/include/omegaGTE/GTEMath.h`):

- **One integrator, generic over the scalar `Ty`.** GTE's math is a single
  template — `Matrix<Ty,column,row>` with the aliases `FMatrix`/`DMatrix`,
  `FVec<n>=FMatrix<n,1>`, `DVec<n>`, and `Quaternion<Ty>` (GTEMath.h:534-558,
  :858). PhysX and Chaos are hard-committed to `float` with SSE/NEON intrinsics.
  AQUA writes the integrator **once over `Ty`** and instantiates `double` as a
  CPU **reference oracle** and `float` for the GPU/production path — so the §6
  CPU/GPU parity harness gets a high-precision ground truth *from the same code*.
  Their architecture can't hand us that; ours does by construction.
- **The 3×3 solve Catto needs is already in GTE.** `inverse(Matrix<Ty,3,3>)`
  (GTEMath.h:633) is **branchless analytic** — no pivoting, no divergence — which
  is exactly what a GPU thread wants for the implicit-gyroscopic Newton step.
  `cross`, `dot`, `normalize` on `Matrix<Ty,3,1>` exist too (GTEMath.h:816-830).
- **The state type is GPU-upload-ready.** `Matrix` stores
  `std::array<std::array<Ty,row>,column>` — fixed-size, heap-free, register-
  resident — and `data()` is contiguous **column-major** (GTEMath.h:530), matching
  Metal/HLSL. Per-body state packs into GTE buffers with no repacking for Phase 5.
- **We own the determinism stance.** Because the math is one path we control, we
  route `sin`/`cos`/`sinc` through a **shared polynomial** used by both the C++ and
  OmegaSL paths, so CPU and GPU agree. (GTE's `Quaternion::fromAxisAngle` calls
  `std::sin`/`std::cos` directly — GTEMath.h:870 — which is CPU-libm-only and not
  small-angle stable; our exp-map helper replaces it on the step path.)

**Gaps in GTE we must fill (this is Phase 1's math work):** `Quaternion<Ty>` has no
**exp/log map** and no "integrate from angular velocity"; `fromAxisAngle` is raw
`sin/cos`; there is a `rotatePoint` for `GPoint3D` but **no rotate-vector for
`FVec<3>`**; there is **no skew/cross-product matrix**, **no inertia-tensor
builder**, and **no `Transform`**. None of these are numerically sensitive linear
algebra (the roadmap's reason for borrowing `Matrix`/`Quaternion`) — they are
physics types we own.

---

## 6. Proposed algorithm — body-frame symplectic Lie integrator with implicit gyroscopic

The synthesis: **symplectic Euler** for linear motion, **implicit gyroscopic**
(Catto) for the nonlinear angular term, and the **exponential map** (Grassia /
Lie-group) for the orientation update — all in the **body frame** so the world
inertia never appears in the hot path, and all per-body independent so it is
embarrassingly parallel (one GPU thread per body, no atomics in Phase 1).

**State, per body** (`Ty`-generic):

| Quantity | Type | Notes |
|---|---|---|
| position `x` | `FVec<3>` | center of mass |
| linear velocity `v` | `FVec<3>` | |
| orientation `q` | `Quaternion<Ty>` | unit |
| angular velocity `ω_b` | `FVec<3>` | **stored in body frame** |
| inverse mass `m⁻¹` | `Ty` | 0 ⇒ static (matches existing convention) |
| inverse inertia `Ib⁻¹` | `FVec<3>` | diagonal principal moments |
| force / torque accum | `FVec<3>` × 2 | world frame, consumed each sub-step |

**Per sub-step `dt`** (driven by `AQContext`; one thread per body):

```
1. v   += dt · m⁻¹ · F_accum                 // symplectic: velocity first
2. τ_b  = Rᵀ(q) · τ_accum                     // torque into body frame
   ω_b += dt · Ib⁻¹ ⊙ τ_b                     // diagonal Ib⁻¹ ⇒ componentwise
3. implicit gyroscopic (one Newton iteration, body frame):
       Ib   = diag(1/Ib⁻¹)
       f    = dt · ( ω_b × (Ib · ω_b) )       // gyroscopic residual
       J    = Ib + dt · ( skew(ω_b)·Ib − skew(Ib·ω_b) )
       ω_b -= inverse(J) · f                  // GTE inverse(Matrix<Ty,3,3>)
4. φ    = dt · ω_b                            // body-frame rotation vector
   Δq   = quatExp(½ φ)                         // exp map; unit by construction
   q    = (q ⊗ Δq).normalized()               // right-multiply (body frame)
5. x   += dt · v                              // symplectic position update
6. clear F_accum, τ_accum
```

Why this specific combination:
- **Implicit gyroscopic** is the one place we refuse the incumbents' "damp it and
  hope" default — it is stable where the explicit term diverges, and with diagonal
  `Ib` the Jacobian is tiny.
- **Exp-map** removes the renormalization drift that semi-implicit-Euler+normalize
  carries; the `.normalized()` in step 4 is belt-and-suspenders, not load-bearing.
- **Body frame** means `I_world⁻¹ = R·diag(Ib⁻¹)·Rᵀ` is only materialized when a
  caller asks for world-space angular response (Phase 3 contacts) — the integrator
  never pays for it.

**Alternative considered — momentum form** (`L += dt·τ; ω = I_world⁻¹·L`):
conserves angular momentum exactly and is elegant, but pushes the nonlinearity into
a per-step `I_world⁻¹` rebuild and is slightly more drift-prone for fast spinners
than the implicit-gyroscopic solve. Kept as the fallback and as an open decision
(§11.1), not the lead.

---

## 7. New math AQUA must add (API sketch, real GTE types)

All `Ty`-generic, all built on the borrowed `Matrix`/`Quaternion`; AQUA-owned,
living in AQUA's math headers (not GTE):

```cpp
// skew / cross-product matrix:  skew(a)·b == cross(a, b)
template<class Ty>
Matrix<Ty,3,3> skew(const Matrix<Ty,3,1>& a);

// quaternion exponential / logarithm maps, small-angle-Taylor stable
template<class Ty>
Quaternion<Ty> quatExp(const Matrix<Ty,3,1>& halfAngleVec);   // shared sinc poly
template<class Ty>
Matrix<Ty,3,1> quatLog(const Quaternion<Ty>& q);

// integrate orientation by body-frame angular velocity over dt
template<class Ty>
Quaternion<Ty> integrate(const Quaternion<Ty>& q,
                         const Matrix<Ty,3,1>& omegaBody, Ty dt);

// rotate a free vector (FVec<3>) — GTE only ships rotatePoint(GPoint3D)
template<class Ty>
Matrix<Ty,3,1> rotate(const Quaternion<Ty>& q, const Matrix<Ty,3,1>& v);

// inertia: principal moments for primitive shapes (diagonal Ib)
template<class Ty> Matrix<Ty,3,1> inertiaSolidBox(Ty mass, Ty hx, Ty hy, Ty hz);
template<class Ty> Matrix<Ty,3,1> inertiaSolidSphere(Ty mass, Ty r);
template<class Ty> Matrix<Ty,3,1> inertiaCapsule(Ty mass, Ty r, Ty h);
// arbitrary tensor -> principal moments + axis (Jacobi eigen), for cooked meshes
template<class Ty> void diagonalizeInertia(const Matrix<Ty,3,3>& I,
                                           Matrix<Ty,3,1>& moments,
                                           Quaternion<Ty>& principalAxis);

// world-space inverse inertia from body diagonal + orientation
template<class Ty>
Matrix<Ty,3,3> worldInvInertia(const Quaternion<Ty>& q,
                               const Matrix<Ty,3,1>& invMomentsBody);

// position + orientation; toMatrix()/inverse() via GTE FMatrix<4,4>
template<class Ty> struct Transform { Matrix<Ty,3,1> p; Quaternion<Ty> q; /* ... */ };
```

The **small-angle Taylor** branch in `quatExp`/`quatLog` (use the `sinc` series as
`|φ|→0`) is the determinism-critical bit: it avoids the `0/0` at zero rotation and
keeps the CPU and GPU paths bit-closer than a raw `sin`/`cos` would.

---

## 8. Data layout & GPU/numeric specialization

Decided now so Phase 5 is a port, not a rewrite (ties to §7 decision #3 of the
roadmap):

- **SoA, not AoS.** Separate pooled GTE buffers for `x`, `v`, `q`, `ω_b`, `m⁻¹`,
  `Ib⁻¹`. `Matrix::data()` is contiguous column-major, so each array uploads
  directly and an OmegaSL kernel reads it coalesced.
- **One thread per body, no atomics.** Phase 1 bodies don't interact, so the step
  is pure map — ideal GPU occupancy. The only per-thread cost is one 3×3 inverse
  and one quaternion exp.
- **Determinism:** shared `sin/cos/sinc` polynomial across C++ and OmegaSL; fixed
  operation order in the Newton step and the cross products; no fast-math
  reassociation on the step path. The `double` instantiation is the oracle.
- **Diagonal inertia** keeps the gyroscopic Jacobian `J` well-conditioned and
  cheap, and halves the state footprint vs. a full tensor.

---

## 9. Validation — how we measure "better"

The incumbent's *behavior* is the reference, not its code (§4 of the roadmap).

- **Conservation tests (the headline).** Torque-free asymmetric body: angular
  momentum `‖L‖` constant to tolerance and energy drift bounded over 10k+
  sub-steps; the tennis-racket flip must appear (it's real physics, not a bug).
- **Precession test.** Constant torque on a spinning body precesses at the
  analytic rate.
- **Oracle & parity.** The `double` instantiation (and a high-sub-step reference)
  is ground truth; the `float` path must match within tolerance — this *is* the
  seed of the §6 parity harness, brought online a phase early.
- **Baseline comparison.** Plot energy/momentum drift and quaternion non-unit drift
  for **(a)** the current placeholder (linearized quaternion + semi-implicit
  Euler), **(b)** explicit gyroscopic, **(c)** this proposal. "Better" = (c) stays
  bounded on scenes where (a)/(b) diverge, at comparable cost.

Metrics emitted as debug-draw / logged series (§3 principle 6, "author for the 3am
engineer"): momentum drift, energy drift, ‖q‖−1, CPU↔GPU max divergence.

---

## 10. Public API additions

Extends the existing surface (`AQRigidBody`, `AQBodyDesc`) without breaking the
pimpl discipline:

- `AQBodyDesc`: add `orientation` (`FQuaternion`), `angularVelocity` (`FVec<3>`),
  and inertia input — either explicit principal moments or a shape handle
  (the shape handle is *defined* in Phase 2; Phase 1 accepts explicit moments or a
  primitive helper from §7).
- `AQRigidBody`: `orientation()/setOrientation()`,
  `angularVelocity()/setAngularVelocity()`, and the force/torque/impulse API the
  roadmap names — `applyForce(F)`, `applyForceAtPoint(F, p)`,
  `applyTorque(τ)`, `applyImpulse(J)`, `applyAngularImpulse(L)` — accumulated and
  consumed each sub-step.
- `AQBodyDesc`: also reserve `restitution`/`friction` material params (used in
  Phase 3) so the descriptor doesn't churn again.

No OmegaSL or backend types cross into `include/aqua/*`; only AQUA types and the
borrowed `FVec`/`Quaternion` appear, per the roadmap's boundary rule.

---

## 11. Open decisions for this phase

1. **Angular state — body-frame `ω` + implicit gyroscopic vs. world `L` (momentum
   form).** *Lean: `ω` + implicit gyroscopic* — best stability per FLOP and it maps
   onto GTE's analytic 3×3 inverse. Revisit if Phase 3 contact coupling makes the
   momentum form's exact conservation more valuable.
2. **Inertia representation — diagonal principal moments (`FVec<3>`) vs. full
   `Matrix<Ty,3,3>`.** *Lean: diagonal + orientation* (as PhysX/Chaos), with
   `diagonalizeInertia` (Jacobi) for arbitrary cooked tensors. Keeps the gyroscopic
   Jacobian cheap.
3. **Orientation update — exp-map vs. linearized + renormalize.** *Lean: exp-map*,
   small-angle-Taylor stabilized; renormalize only as a guard.
4. **Scalar/parity policy — which `Ty` is the oracle.** *Lean: `double` CPU
   reference, `float` GPU/production*, feeding the §6 harness. (Connects to roadmap
   §7 decision #4, determinism guarantee.)
5. **Symplectic ordering & sub-step count** inside `AQContext`'s fixed step — adopt
   the small-steps posture (more sub-steps, one solve) from Müller 2020.

---

*Brief status: proposal. Decisions in §11 should be settled before the integrator
lands. This document is the Phase 1 entry of the per-phase prior-art series §4 of
`Physics-Roadmap.md` establishes.*
