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

**Included clean-up.** Phase 1 also hardens the Phase 0 scaffold's GTE-math interop
so it actually compiles — the committed scaffold currently does not (see the
folded-in fix in §10). This lands first, before the integrator, since everything
else builds on it.

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

## 7. New math AQUA must add — `include/aqua/AQMath.h` (draft)

All `Ty`-generic, all built on the borrowed `OmegaGTE::Matrix`/`Quaternion`,
AQUA-owned. Proposed home: a new header `include/aqua/AQMath.h` that includes
`<omegaGTE/GTEMath.h>` (the deliberate math exception to the backend-hiding rule).
The public AQUA surface (§10) consumes the **`float`** instantiations
(`OmegaGTE::FVec<3>`, `OmegaGTE::FQuaternion`, `aqua::Transform<float>`); the solver
instantiates `double` too, as the parity oracle.

> **Namespace decision (minor, §11.6).** AQUA's existing public types are global and
> `AQ`-prefixed (`AQContext`, `AQSpace`, …). Free math templates read poorly with that
> prefix (`AQskew`), so this draft puts the owned math in `namespace aqua`. If the team
> prefers the global convention, these become `AQ`-prefixed free functions — mechanical
> to flip, flagged here rather than decided unilaterally.

```cpp
#ifndef AQUA_AQMATH_H
#define AQUA_AQMATH_H

#include <omegaGTE/GTEMath.h>
#include <cmath>

namespace aqua {

using OmegaGTE::Matrix;
using OmegaGTE::Quaternion;

// Convenience aliases (a "Vec3" in this engine *is* a GTE column vector).
template<class Ty> using Vec3 = Matrix<Ty,3,1>;
template<class Ty> using Mat3 = Matrix<Ty,3,3>;

// Ergonomic construction. GTE's Matrix has a *private* default ctor and no
// component constructor (named-ctor idiom — Create()/Identity()), so `{x,y,z}`
// init doesn't work; this gives it back without modifying GTE. Zero ⇒ Create().
template<class Ty>
Vec3<Ty> vec3(Ty x, Ty y, Ty z) {
    auto v = Vec3<Ty>::Create();
    v[0][0] = x; v[1][0] = y; v[2][0] = z;
    return v;
}

// --- cross-product (skew-symmetric) matrix:  skew(a) * b == cross(a, b) ---
// GTE indexes m[row][col]; FVec component i is v[i][0].
template<class Ty>
Mat3<Ty> skew(const Vec3<Ty>& a) {
    const Ty x = a[0][0], y = a[1][0], z = a[2][0];
    auto m = Mat3<Ty>::Create();        // Matrix() default ctor is private — use Create()
    m[0][1] = -z; m[0][2] =  y;
    m[1][0] =  z; m[1][2] = -x;
    m[2][0] = -y; m[2][1] =  x;
    return m;
}

// --- exponential map: quaternion from a half-angle rotation vector (½·φ) ---
// Uses the sinc Taylor series near 0 so it is finite at zero rotation and the
// C++ and OmegaSL paths agree (no raw libm sin/cos divergence). Unit to O(t^4).
template<class Ty>
Quaternion<Ty> quatExp(const Vec3<Ty>& halfAngle) {
    const Ty x = halfAngle[0][0], y = halfAngle[1][0], z = halfAngle[2][0];
    const Ty t2 = x*x + y*y + z*z;          // |½·φ|^2
    const Ty t  = std::sqrt(t2);
    const Ty s  = (t < Ty(1e-4)) ? (Ty(1) - t2 / Ty(6)) : (std::sin(t) / t);  // sinc
    const Ty w  = (t < Ty(1e-4)) ? (Ty(1) - t2 / Ty(2)) :  std::cos(t);
    return { x*s, y*s, z*s, w };
}

// --- inverse map: half-angle rotation vector from a (unit) quaternion ---
template<class Ty> Vec3<Ty> quatLog(const Quaternion<Ty>& q);  // small-angle stable

// --- integrate orientation by BODY-frame angular velocity over dt (step 4 of §6) ---
template<class Ty>
Quaternion<Ty> integrate(const Quaternion<Ty>& q, const Vec3<Ty>& omegaBody, Ty dt) {
    return (q * quatExp(omegaBody * (dt * Ty(0.5)))).normalized();  // right-multiply
}

// --- rotate a FREE vector (GTE only ships rotatePoint() for GPoint3D) ---
template<class Ty>
Vec3<Ty> rotate(const Quaternion<Ty>& q, const Vec3<Ty>& v) {
    auto u = Vec3<Ty>::Create();
    u[0][0] = q.x; u[1][0] = q.y; u[2][0] = q.z;
    const auto t = Ty(2) * OmegaGTE::cross(u, v);
    return v + q.w * t + OmegaGTE::cross(u, t);     // no quaternion inverse needed
}

// --- inertia: diagonal principal moments (Ib) for primitive shapes ---
template<class Ty> Vec3<Ty> inertiaSolidBox(Ty mass, Ty hx, Ty hy, Ty hz);  // half-extents
template<class Ty> Vec3<Ty> inertiaSolidSphere(Ty mass, Ty r);               // (2/5) m r^2
template<class Ty> Vec3<Ty> inertiaCapsule(Ty mass, Ty r, Ty h);

// Arbitrary symmetric tensor -> principal moments + principal-axis rotation
// (Jacobi eigendecomposition), folded into body orientation PhysX/Chaos-style.
template<class Ty>
void diagonalizeInertia(const Mat3<Ty>& I, Vec3<Ty>& outMoments,
                        Quaternion<Ty>& outPrincipalAxis);

// --- world-space inverse inertia:  R · diag(invMomentsBody) · Rᵀ ---
template<class Ty>
Mat3<Ty> worldInvInertia(const Quaternion<Ty>& q, const Vec3<Ty>& invMomentsBody);

// --- rigid transform (position + orientation) ---
template<class Ty>
struct Transform {
    Vec3<Ty>       p = Vec3<Ty>::Create();         // translation
    Quaternion<Ty> q = Quaternion<Ty>::Identity(); // rotation

    Matrix<Ty,4,4> toMatrix() const;               // q.toMatrix() with p in last column
    Transform      inverse() const;
    Transform      operator*(const Transform& child) const;  // compose: this ∘ child
    Vec3<Ty>       transformPoint(const Vec3<Ty>& v) const;   // rotate + translate
    Vec3<Ty>       transformVector(const Vec3<Ty>& v) const;  // rotate only
};

} // namespace aqua
#endif // AQUA_AQMATH_H
```

The inline bodies above (`skew`, `quatExp`, `integrate`, `rotate`) are the small,
determinism-critical pieces shown in full; the rest are declarations whose bodies
land with the integrator. The `quatExp` **small-angle Taylor** branch is the bit
that matters most for §6 parity — it removes the `0/0` at zero rotation and keeps
the CPU and GPU paths bit-closer than raw `sin`/`cos` (which is also why we do not
reuse `Quaternion::fromAxisAngle`, GTEMath.h:870).

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

Extends the existing surface (`AQRigidBody`, `AQBodyDesc` in `include/aqua/AQSpace.h`)
without breaking the pimpl discipline. New members marked `// new`.

**`AQBodyDesc`:**

```cpp
struct AQUA_EXPORT AQBodyDesc {
    AQBodyType type = AQBodyType::Dynamic;

    // --- pose & motion ---
    OmegaGTE::FVec<3>     position        = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FQuaternion orientation     = OmegaGTE::FQuaternion::Identity(); // new
    OmegaGTE::FVec<3>     linearVelocity  = OmegaGTE::FVec<3>::Create();        // new
    OmegaGTE::FVec<3>     angularVelocity = OmegaGTE::FVec<3>::Create();        // new

    // --- mass properties ---
    float mass = 1.f;                                              // ignored for Static
    // Diagonal principal moments of inertia, body frame. Zero ⇒ derive from the
    // body's collision shape (Phase 2); until shapes exist, fill via the AQMath.h
    // helpers (e.g. aqua::inertiaSolidBox).
    OmegaGTE::FVec<3> inertiaPrincipalMoments = OmegaGTE::FVec<3>::Create();    // new

    // --- material (consumed in Phase 3; reserved now to avoid descriptor churn) ---
    float restitution = 0.f;   // new
    float friction    = 0.5f;  // new
};
```

**`AQRigidBody`:**

```cpp
class AQUA_EXPORT AQRigidBody {
public:
    ~AQRigidBody();

    // --- linear state (existing) ---
    AQUA_NODISCARD OmegaGTE::FVec<3> position() const;
    void setPosition(const OmegaGTE::FVec<3> &p);
    AQUA_NODISCARD OmegaGTE::FVec<3> velocity() const;
    void setVelocity(const OmegaGTE::FVec<3> &v);

    // --- angular state (new) ---
    AQUA_NODISCARD OmegaGTE::FQuaternion orientation() const;
    void setOrientation(const OmegaGTE::FQuaternion &q);
    AQUA_NODISCARD OmegaGTE::FVec<3> angularVelocity() const;   // world frame
    void setAngularVelocity(const OmegaGTE::FVec<3> &w);

    // --- mass properties (new) ---
    AQUA_NODISCARD float mass() const;                          // 0 ⇒ static
    AQUA_NODISCARD OmegaGTE::FVec<3> inertiaPrincipalMoments() const;

    // --- force / torque / impulse API (new) ---
    // Accumulated in world space, consumed at the start of each sub-step.
    void applyForce(const OmegaGTE::FVec<3> &force);
    void applyForceAtPoint(const OmegaGTE::FVec<3> &force,
                           const OmegaGTE::FVec<3> &worldPoint);   // + torque (r × F)
    void applyTorque(const OmegaGTE::FVec<3> &torque);
    void applyImpulse(const OmegaGTE::FVec<3> &impulse);           // instantaneous Δp
    void applyImpulseAtPoint(const OmegaGTE::FVec<3> &impulse,
                             const OmegaGTE::FVec<3> &worldPoint);  // + angular impulse
    void applyAngularImpulse(const OmegaGTE::FVec<3> &angularImpulse);

    AQUA_NODISCARD AQBodyType type() const;

private:
    AQRigidBody();
    friend class AQSpace;
    struct Impl;
    std::unique_ptr<Impl> impl;
};
```

**Hidden state (illustrative, `src/AQSpace.cpp` — not public):**

```cpp
struct AQRigidBody::Impl {
    AQBodyType type = AQBodyType::Dynamic;
    // pose
    OmegaGTE::FVec<3>     position       = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FQuaternion orientation    = OmegaGTE::FQuaternion::Identity();
    // velocities — angular velocity stored in the BODY frame (§6); the public
    // angularVelocity() getter returns aqua::rotate(orientation, angularVelBody).
    OmegaGTE::FVec<3>     velocity       = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3>     angularVelBody = OmegaGTE::FVec<3>::Create();
    // mass properties
    float                invMass         = 1.f;                          // 0 ⇒ static
    OmegaGTE::FVec<3>     invInertiaBody  = OmegaGTE::FVec<3>::Create();  // 1 / moments
    // per-sub-step accumulators (world frame), cleared each step
    OmegaGTE::FVec<3>     forceAccum      = OmegaGTE::FVec<3>::Create();
    OmegaGTE::FVec<3>     torqueAccum     = OmegaGTE::FVec<3>::Create();
};
```

> **Folded-in fix — Phase 0 math interop (lands first in Phase 1).** Drafting this
> surface turned up that the **committed Phase 0 scaffold does not compile** against
> GTE's math types, for three related reasons:
>
> 1. **Undefined type.** `AQSpace.cpp` uses a bare `Vec3` that is defined *nowhere*
>    in the repo or build (no alias; `CMakeLists.txt` defines only `AQUA__BUILD__`),
>    while `AQSpace.h` declares the same members as `OmegaGTE::FVec<3>` — a
>    header/impl signature mismatch.
> 2. **Private default ctor.** `OmegaGTE::Matrix`'s default constructor is private
>    (GTEMath.h:301; the named-ctor idiom routes through `Create()`/`Identity()`),
>    so `Vec3 position{};` (AQSpace.cpp:7-8) cannot default-construct.
> 3. **No component ctor.** `Matrix` has no `{x,y,z}` constructor, so
>    `Vec3 gravity{0.f,-9.81f,0.f};` (AQSpace.cpp:23) does not compile.
>
> **Decision (lean): fix entirely on the AQUA side; do not modify GTE.** GTE is a
> shared dependency and the private default ctor is a deliberate idiom, not an
> oversight — exposing it has broad blast radius for no real gain. Phase 1, before
> the integrator:
> - replaces the undefined bare `Vec3` in `AQSpace.cpp` with `OmegaGTE::FVec<3>` to
>   match the header. (Note: AQMath.h's `aqua::Vec3<Ty>` is *templated*, so a
>   non-template `Vec3` alias is intentionally **not** introduced — it would clash.)
> - defaults every vector member with `::Create()` and every quaternion with
>   `::Identity()` (as the §10 drafts already show);
> - rewrites the scaffold initializers: `Vec3 position{}` → `…FVec<3>::Create()`,
>   `Vec3 gravity{0.f,-9.81f,0.f}` → `aqua::vec3(0.f, -9.81f, 0.f)` (§7 factory).

No OmegaSL or backend types cross into `include/aqua/*`; only AQUA types and the
borrowed `FVec`/`FQuaternion` appear, per the roadmap's boundary rule.

---

## 11. Open decisions for this phase

1. **Angular state — body-frame `ω` + implicit gyroscopic vs. world `L` (momentum
   form).** *DECIDED: `ω` + implicit gyroscopic.* The implementation A/B'd both on
   the torque-free asymmetric scene (ω≈8 rad/s, 20 s): the implicit-gyroscopic step
   drifts ‖L‖ ≈1.4% and energy ≈2.7% at dt=1/2000 (both clean O(dt)); the momentum
   form conserves ‖L‖ *exactly* but its energy drift is ≈9% — ~3× worse — at the
   same dt. We are after a *better, balanced* solution, not exact conservation
   (which this scheme will never give), so the implicit-gyroscopic lead stands: its
   energy behaviour is the more balanced of the two. Revisit only if Phase 3 contact
   coupling makes exact ‖L‖ conservation worth the energy cost.
2. **Inertia representation — diagonal principal moments (`FVec<3>`) vs. full
   `Matrix<Ty,3,3>`.** *Lean: diagonal + orientation* (as PhysX/Chaos), with
   `diagonalizeInertia` (Jacobi) for arbitrary cooked tensors. Keeps the gyroscopic
   Jacobian cheap.
3. **Orientation update — exp-map vs. linearized + renormalize.** *Lean: exp-map*,
   small-angle-Taylor stabilized; renormalize only as a guard.
4. **Scalar/parity policy — which `Ty` is the oracle.** *Lean: `double` CPU
   reference, `float` GPU/production*, feeding the §6 harness. (Connects to roadmap
   §7 decision #4, determinism guarantee.)
5. **Symplectic ordering & sub-step count** inside `AQContext`'s fixed step —
   *DECIDED, and now a REQUIREMENT:* adopt the small-steps posture (more sub-steps,
   one solve) from Müller 2020. Because decision #1's conservation error is O(dt)
   and secular, the sub-step size is a *correctness* knob for fast rotational
   bodies, not just a quality dial — a fast spinner drifts ≈20× more at the default
   1/120 s than at 1/2000 s. `AQContext` is built on a fixed sub-step accumulator
   precisely so callers can (must) shrink `setFixedTimestep` to the scene's angular
   rates. Marked necessary in `AQContext.h`.
6. **Math placement & namespace.** New owned math in `namespace aqua`
   (`include/aqua/AQMath.h`) vs. global `AQ`-prefixed free functions, matching the
   existing public types. *Lean: `namespace aqua`* — but mechanical to flip; the
   team's house style decides. (See the note in §7.)

---

## 12. Research notes — post-implementation findings

### 12.1 Catto multi-iteration vs splitting error (Phase 1.1, 2026-06-04)

Phase 1.1's §4 mitigation — adaptive Catto-style multi-iteration of the implicit-
gyroscopic Newton step — was scoped on the belief that the secular drift §11.1
measured would shrink as the per-step Newton residual shrank. The implementation
A/B'd Phase 1 (one Newton iteration) against Phase 1.1's adaptive 4-iteration
cap at the targeted regime (`‖ω‖·dt ≈ 0.08 rad`, just past the §5 fast-spin
warning) on the same torque-free asymmetric scene. The per-step Catto residual
`‖Ib·(ω' − ω₀) + dt·ω' × Ib·ω'‖` collapsed from ≈5×10⁻¹⁰ to ≈7×10⁻¹⁶ — machine
precision — exactly as Newton's quadratic convergence predicts. **Cumulative
‖L‖ and energy drift over 4 s of sim time were unchanged**: ≈4.85% L drift,
≈8.64% energy drift, with the adaptive and single-iteration paths within 1‰
of each other.

The finding: this scheme's cumulative drift is dominated by **splitting error**
between the implicit-gyroscopic step and the orientation / position updates,
not by the Newton residual once Newton has converged. A single iteration
already converges Newton to within ~10⁻¹⁰ at any per-step angle where the
integrator itself is usable; multi-iteration buys nothing additional for
cumulative metrics. Adaptive iteration is still load-bearing as a *robustness
margin* — it keeps the Newton residual buried in roundoff as `‖ω‖·dt` climbs
toward the §5 / Phase 1.1 §4 warning band (0.01–0.05 rad), where a single
iteration would start leaving a residual large enough to be visible. Phase
1.1's §9 "adaptive drift measurably below single-iter" claim was reframed in
the shipped test as "adaptive drives the per-step Catto residual ≥3 orders
below single-iter at the same dt and ω, while cumulative drift is bounded and
matches single-iter to working precision" (`aqua_rigid_body_test`).

Operationally this means §11.5 still holds — the sub-step size remains a
*correctness* knob for fast rotational bodies, and the Phase 1 finding that
drift is "O(dt) and secular" describes the *splitting*, not the Newton
residual. The Phase 1.1 §5 fast-spin warning is what catches the dt the
user must shrink; adaptive iteration is the silent safety margin between
"single iteration is enough" and "the warning fires". Closing the underlying
O(dt) splitting drift would require a higher-order integrator (e.g., a Lie-
symplectic Strang split, or the momentum form §11.1 weighed and rejected on
balance grounds) — out of scope for Phase 1.1, flagged here for future work.

### 12.2 GTE Matrix storage convention — a latent bug discovered (Phase 1.1, 2026-06-04)

`AQdiagonalizeInertia` (Phase 1, §7) carried a transposition bug from Phase 1
that the Phase 1 math test could not detect: the Jacobi loop accumulates the
eigenvector matrix `v` in row-major math convention (`v[row][col]`), but the
output 4×4 was filled with `m[i][j] = v[i][j]`, which under GTE's column-major
`m[col][row]` storage convention stores `Vᵀ` rather than `V`. The recovered
eigenvalues are correct under transpose, so the math test passed; only Phase
1.1's `addBody` full-tensor path — which composes the diagonalize quaternion
with `desc.orientation` and inspects the resulting world inertia tensor —
surfaced it. Fix landed as `m[i][j] = v[j][i]` with a comment block; a
column-major storage note was added on `OmegaGTE::Matrix` itself (`gte/include/
omegaGTE/GTEMath.h`) so future code interfacing GTE matrices via raw indexing
doesn't repeat the assumption.

---

*Brief status: proposal. Decisions in §11 should be settled before the integrator
lands. This document is the Phase 1 entry of the per-phase prior-art series §4 of
`Physics-Roadmap.md` establishes.*
