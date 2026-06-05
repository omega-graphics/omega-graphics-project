#include <aqua/AQSpace.h>
#include <aqua/AQCollision.h>
#include <aqua/AQIntegrator.h>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>
#ifndef NDEBUG
#include <cmath>
#include <iostream>
namespace {
// Debug guard threshold for the per-sub-step rotation angle ‖ω‖·dt (radians).
// The Phase 1 integrator's conservation error is O(‖ω‖·dt) and secular, so this
// angle — not dt alone — is what determines rotational accuracy. ~0.05 rad
// (≈2.9°) per sub-step corresponds to roughly 15-20% momentum/energy drift over
// a long run; below it the scene is in the well-behaved regime. A fast spinner
// at AQContext's default 1/120 s sits well above this, which is the misconfig
// this warning exists to make loud. Phase 1.1's adaptive Newton iteration cuts
// drift sharply for `‖ω‖·dt` in the 0.01–0.05 rad band, so the warning fires
// only when the user is past where even the adaptive path can save them.
// (See AQContext.h and Phase-1 doc §11.5, Phase-1.1 doc §4.)
constexpr float kMaxStepAngle = 0.05f;
}
#endif

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;
using OmegaGTE::FMatrix;

// Hidden state. Dynamics live in an AQBodyState<float> so the exact same step
// code (AQStepBody, AQIntegrator.h) runs here in production-float and in the
// parity test in double — the §5/§6 reference-oracle design. Angular velocity
// is stored in the BODY frame; the public angularVelocity() getter rotates it
// back to world.
struct AQRigidBody::Impl {
    AQBodyType type = AQBodyType::Dynamic;
    AQBodyState<float> s;
    bool warnedFastSpin = false;   ///< debug fast-spin warning latch (per body)

    // --- Phase 2 collision state (per-body) ---
    AQShapeHandle      shape;             ///< invalid handle ⇒ no shape
    AQCollisionFilter  filter;            ///< layer + mask
    FAABB              worldAABB =        ///< tight bound, refreshed each sub-step
        FAABB::fromMinMax(AQvec3(0.f,0.f,0.f), AQvec3(0.f,0.f,0.f));
    FAABB              fatAABB   =        ///< worldAABB grown by §11.4 fattening
        FAABB::fromMinMax(AQvec3(0.f,0.f,0.f), AQvec3(0.f,0.f,0.f));
    bool               fatValid  = false; ///< first refresh seeds fatAABB
};

AQRigidBody::AQRigidBody() : impl(std::make_unique<Impl>()) {}
AQRigidBody::~AQRigidBody() = default;

// --- linear state ---
FVec<3> AQRigidBody::position() const { return impl->s.position; }
void AQRigidBody::setPosition(const FVec<3> &p) { impl->s.position = p; }
FVec<3> AQRigidBody::velocity() const { return impl->s.velocity; }
void AQRigidBody::setVelocity(const FVec<3> &v) { impl->s.velocity = v; }

// --- angular state ---
FQuaternion AQRigidBody::orientation() const { return impl->s.orientation; }
void AQRigidBody::setOrientation(const FQuaternion &q) {
    impl->s.orientation = q.normalized();
}
FVec<3> AQRigidBody::angularVelocity() const {
    // Stored body-frame; report world frame.
    return AQrotate(impl->s.orientation, impl->s.angularVelBody);
}
void AQRigidBody::setAngularVelocity(const FVec<3> &w) {
    // World -> body (orientation is unit, so conjugate == inverse).
    impl->s.angularVelBody = AQrotate(impl->s.orientation.conjugate(), w);
}

// --- mass properties ---
float AQRigidBody::mass() const {
    return impl->s.invMass == 0.f ? 0.f : 1.f / impl->s.invMass;
}
FVec<3> AQRigidBody::inertiaPrincipalMoments() const {
    // Invert the stored inverse moments; 0 (infinite inertia) reports 0.
    auto &inv = impl->s.invInertiaBody;
    return AQvec3(inv[0][0] == 0.f ? 0.f : 1.f / inv[0][0],
                  inv[1][0] == 0.f ? 0.f : 1.f / inv[1][0],
                  inv[2][0] == 0.f ? 0.f : 1.f / inv[2][0]);
}
FMatrix<3,3> AQRigidBody::worldInverseInertia() const {
    // R · diag(invMomentsBody) · Rᵀ via the AQMath outer-product builder.
    return AQworldInvInertia(impl->s.orientation, impl->s.invInertiaBody);
}

// --- conserved-quantity accessors (Phase-1.1 §6.3) ---
FVec<3> AQRigidBody::linearMomentum() const {
    return mass() * impl->s.velocity;
}
FVec<3> AQRigidBody::angularMomentum() const {
    // L_world = R · (Ib ⊙ ω_body). Body-stored ω is already body-frame.
    const auto &w = impl->s.angularVelBody;
    const auto I = inertiaPrincipalMoments();
    const FVec<3> Lb = AQvec3(I[0][0] * w[0][0], I[1][0] * w[1][0], I[2][0] * w[2][0]);
    return AQrotate(impl->s.orientation, Lb);
}
float AQRigidBody::kineticEnergy() const {
    // ½ m ‖v‖² + ½ ω_b · (Ib ⊙ ω_b). Convention-safe regardless of orientation.
    const auto &v = impl->s.velocity;
    const auto &w = impl->s.angularVelBody;
    const auto I = inertiaPrincipalMoments();
    const float linear  = 0.5f * mass() * OmegaGTE::dot(v, v);
    const float angular = 0.5f * (I[0][0] * w[0][0] * w[0][0] +
                                  I[1][0] * w[1][0] * w[1][0] +
                                  I[2][0] * w[2][0] * w[2][0]);
    return linear + angular;
}

// --- robustness controls (Phase-1.1 §6.4) ---
void  AQRigidBody::setLinearDamping(float c)    { impl->s.linearDamping   = c < 0.f ? 0.f : c; }
float AQRigidBody::linearDamping()  const       { return impl->s.linearDamping; }
void  AQRigidBody::setAngularDamping(float c)   { impl->s.angularDamping  = c < 0.f ? 0.f : c; }
float AQRigidBody::angularDamping() const       { return impl->s.angularDamping; }
void  AQRigidBody::setGravityScale(float s)     { impl->s.gravityScale    = s; }
float AQRigidBody::gravityScale()   const       { return impl->s.gravityScale; }
void  AQRigidBody::setMaxAngularSpeed(float s)  { impl->s.maxAngularSpeed = s < 0.f ? 0.f : s; }
float AQRigidBody::maxAngularSpeed()const       { return impl->s.maxAngularSpeed; }

// --- force / torque / impulse API (world frame) ---
void AQRigidBody::applyForce(const FVec<3> &force) {
    impl->s.forceAccum += force;
}
void AQRigidBody::applyForceAtPoint(const FVec<3> &force, const FVec<3> &worldPoint) {
    impl->s.forceAccum += force;
    // Phase 2 COM-offset wiring: torque arm is (worldPoint − comWorld), where
    // comWorld = position + R · comOffset. With a zero comOffset (the Phase 1
    // default — guarded by the existing zero-COM accessor tests) this
    // collapses to the previous behaviour.
    const FVec<3> comWorld = impl->s.position + AQrotate(impl->s.orientation, impl->s.comOffset);
    impl->s.torqueAccum += OmegaGTE::cross(worldPoint - comWorld, force);
}
void AQRigidBody::applyTorque(const FVec<3> &torque) {
    impl->s.torqueAccum += torque;
}
void AQRigidBody::applyImpulse(const FVec<3> &impulse) {
    // Instantaneous Δp: Δv = m⁻¹ · J.
    impl->s.velocity += impl->s.invMass * impulse;
}
void AQRigidBody::applyImpulseAtPoint(const FVec<3> &impulse, const FVec<3> &worldPoint) {
    impl->s.velocity += impl->s.invMass * impulse;
    // Phase 2 COM-offset wiring — same as applyForceAtPoint.
    const FVec<3> comWorld = impl->s.position + AQrotate(impl->s.orientation, impl->s.comOffset);
    applyAngularImpulse(OmegaGTE::cross(worldPoint - comWorld, impulse));
}
void AQRigidBody::applyAngularImpulse(const FVec<3> &angularImpulse) {
    // World angular impulse -> body frame, then Δω = Ib⁻¹ · (Rᵀ · L) (diagonal).
    const FVec<3> lBody = AQrotate(impl->s.orientation.conjugate(), angularImpulse);
    impl->s.angularVelBody[0][0] += impl->s.invInertiaBody[0][0] * lBody[0][0];
    impl->s.angularVelBody[1][0] += impl->s.invInertiaBody[1][0] * lBody[1][0];
    impl->s.angularVelBody[2][0] += impl->s.invInertiaBody[2][0] * lBody[2][0];
}

AQBodyType AQRigidBody::type() const { return impl->type; }

// --- Phase 2 collision accessors (per-body) ---
AQShapeHandle AQRigidBody::shape() const                       { return impl->shape; }
void          AQRigidBody::setShape(const AQShapeHandle &s)    { impl->shape = s; }
FVec<3>       AQRigidBody::aabbMin() const                     { return impl->fatAABB.min; }
FVec<3>       AQRigidBody::aabbMax() const                     { return impl->fatAABB.max; }
AQCollisionFilter AQRigidBody::collisionFilter() const         { return impl->filter; }
void          AQRigidBody::setCollisionFilter(const AQCollisionFilter &f) { impl->filter = f; }

struct AQSpace::Impl {
    FVec<3> gravity = AQvec3(0.f, -9.81f, 0.f);
    std::vector<std::shared_ptr<AQRigidBody>> bodies;

    // Drainable debug surface (Phase-1.1 §6.5). `flags == AQDebugNone` keeps
    // the buffer empty — the per-step emission early-outs and there is nothing
    // to drain. Pull model, owned by the space.
    std::uint32_t                 debugFlags = AQDebugNone;
    std::vector<AQDebugLine>      debugLines;

    // --- Phase 2: shape table + vertex pool (§8 shapes pooled and shared) ---
    // Index 0 is a sentinel "invalid" slot — handles default to {0, 0} and
    // `valid()` returns false on generation 0. Generations start at 1 and tick
    // monotonically on remove; the simple pool never shrinks.
    std::vector<AQShape>        shapes      = std::vector<AQShape>(1);
    std::vector<std::uint32_t>  generations = std::vector<std::uint32_t>(1, 0);
    std::vector<FVec<3>>        hullVerts;     ///< vertex pool referenced by shapes

    // --- Phase 2: broadphase output (§5/§8 ordered + de-duplicated) ---
    std::vector<AQBroadphasePair> pairs;
    float fattenMargin = 0.02f;                 ///< §11.4 fixed margin (≈2cm world units)

    // Look up a shape by handle. Returns nullptr if the handle is invalid or
    // stale (generation mismatch). Caller checks the pointer.
    const AQShape *shapeAt(const AQShapeHandle &h) const {
        if (!h.valid()) return nullptr;
        if (h.index >= shapes.size()) return nullptr;
        if (generations[h.index] != h.generation) return nullptr;
        return &shapes[h.index];
    }
    AQShapeHandle pushShape(const AQShape &shape) {
        const std::uint32_t idx = static_cast<std::uint32_t>(shapes.size());
        shapes.push_back(shape);
        generations.push_back(1);          // first generation for a fresh slot
        return AQShapeHandle{idx, 1};
    }
};

AQSpace::AQSpace() : impl(std::make_unique<Impl>()) {}
AQSpace::~AQSpace() = default;

void AQSpace::setGravity(const FVec<3> &g) { impl->gravity = g; }
FVec<3> AQSpace::gravity() const { return impl->gravity; }

namespace {
// True iff any entry of the supplied tensor is non-zero. Zero (the default)
// means "use the diagonal `inertiaPrincipalMoments` path", which is the Phase 1
// fast default. Distinguishing here lets `addBody` only run Jacobi when needed.
bool anyNonZero(const AQMat3F &M) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (M[i][j] != 0.f) return true;
    return false;
}
}

std::shared_ptr<AQRigidBody> AQSpace::addBody(const AQBodyDesc &desc) {
    struct Ctor : AQRigidBody { Ctor() : AQRigidBody() {} };
    auto body = std::shared_ptr<AQRigidBody>(new Ctor());
    auto &s = body->impl->s;

    body->impl->type = desc.type;
    s.position    = desc.position;
    s.orientation = desc.orientation.normalized();
    s.velocity    = desc.linearVelocity;

    const bool isStatic = (desc.type == AQBodyType::Static || desc.mass <= 0.f);
    s.invMass = isStatic ? 0.f : 1.f / desc.mass;

    // Resolve principal moments. If the desc supplies a full inertia tensor we
    // diagonalize it (the Phase 1 `AQdiagonalizeInertia` Jacobi path, now wired
    // — Phase-1.1 §6.2) and fold the principal-axis rotation into the body
    // orientation, PhysX/Chaos-style: the stored "body frame" becomes the
    // principal frame, so the integrator keeps its diagonal-Ib fast path.
    // Otherwise the desc's diagonal moments are used unchanged.
    //
    // Phase 2 inertia-from-shape closure: if the desc supplies a valid shape
    // handle, has no full tensor, and leaves the diagonal moments at zero,
    // derive them from the shape via the AQMath.h helpers. This closes the
    // documented hook on AQBodyDesc::inertiaPrincipalMoments. Static bodies
    // and zero-mass descriptors skip — they have infinite inertia anyway.
    FVec<3> moments = desc.inertiaPrincipalMoments;
    const auto isVec3Zero = [](const FVec<3>& v) {
        return v[0][0] == 0.f && v[1][0] == 0.f && v[2][0] == 0.f;
    };
    if (!isStatic && !anyNonZero(desc.inertiaTensor) && isVec3Zero(moments)) {
        if (const AQShape *sp = impl->shapeAt(desc.shape)) {
            moments = AQshapeInertiaMoments(*sp, desc.mass,
                                            impl->hullVerts.data(),
                                            impl->hullVerts.size());
        }
    }
    if (!isStatic && anyNonZero(desc.inertiaTensor)) {
        FVec<3>      diag = FVec<3>::Create();
        FQuaternion  Qprincipal = FQuaternion::Identity();
        AQdiagonalizeInertia(desc.inertiaTensor, diag, Qprincipal);
        moments = diag;
        // world ← body_old via desc.orientation; body_old ← principal via Qprincipal,
        // so world ← principal via (desc.orientation * Qprincipal). The angular
        // velocity rotation below uses the new orientation, so it carries the
        // change without an extra basis change.
        s.orientation = (s.orientation * Qprincipal).normalized();
    }

    // Inverse principal moments; a zero moment means infinite inertia on that
    // axis (no angular response), which a static body has on every axis.
    s.invInertiaBody = AQvec3(
        (isStatic || moments[0][0] <= 0.f) ? 0.f : 1.f / moments[0][0],
        (isStatic || moments[1][0] <= 0.f) ? 0.f : 1.f / moments[1][0],
        (isStatic || moments[2][0] <= 0.f) ? 0.f : 1.f / moments[2][0]);

    // Desc gives world-frame angular velocity; store it body-frame relative to
    // the (possibly folded) orientation set above.
    s.angularVelBody = AQrotate(s.orientation.conjugate(), desc.angularVelocity);

    // Robustness controls (Phase-1.1 §6.4) — defaults match Phase 1 behaviour.
    s.linearDamping   = desc.linearDamping   < 0.f ? 0.f : desc.linearDamping;
    s.angularDamping  = desc.angularDamping  < 0.f ? 0.f : desc.angularDamping;
    s.gravityScale    = desc.gravityScale;
    s.maxAngularSpeed = desc.maxAngularSpeed < 0.f ? 0.f : desc.maxAngularSpeed;

    // COM-offset wiring (Phase-1.1 reserved field; Phase 2 makes it load-
    // bearing — applyForceAtPoint torque arms and momentum debug-draw use
    // comWorld = position + R · comOffset).
    s.comOffset       = desc.centerOfMass;

    // Phase 2 collision attachments. Shape may be invalid (the body is then
    // broadphase-invisible). Filter defaults are "everyone collides".
    body->impl->shape  = desc.shape;
    body->impl->filter = desc.filter;
    body->impl->fatValid = false;        // refresh on the next runBroadphase

    impl->bodies.push_back(body);
    return body;
}

bool AQSpace::removeBody(const std::shared_ptr<AQRigidBody> &body) {
    auto &v = impl->bodies;
    auto it = std::find(v.begin(), v.end(), body);
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

std::size_t AQSpace::bodyCount() const { return impl->bodies.size(); }

// --- debug surface (Phase-1.1 §6.5) ---
void AQSpace::setDebugFlags(std::uint32_t flags) { impl->debugFlags = flags; }
std::uint32_t AQSpace::debugFlags() const { return impl->debugFlags; }
std::vector<AQDebugLine> AQSpace::drainDebugLines() {
    std::vector<AQDebugLine> out;
    out.swap(impl->debugLines);
    return out;
}

namespace {
inline AQDebugLine makeLine(const FVec<3>& a, const FVec<3>& b,
                            float r, float g, float bl, float al = 1.f) {
    AQDebugLine L;
    L.a = a; L.b = b;
    L.rgba[0] = r; L.rgba[1] = g; L.rgba[2] = bl; L.rgba[3] = al;
    return L;
}

// Emit primitives for `body` according to `flags` into `out`. Run after the
// sub-step so the visualization reflects the just-simulated state.
//
// Phase 2 COM-offset wiring: axes / velocity / angular-velocity / momentum
// lines anchor at `anchor` (= position + R · comOffset, supplied by the
// caller — the AQSpace member that is friend to AQRigidBody::Impl), NOT
// at the pose origin. With a zero comOffset (the Phase 1 default — guarded
// by the existing zero-COM tests) `anchor` collapses to `body.position()`.
void emitBodyDebug(const AQRigidBody &body, const FVec<3>& anchor,
                   std::uint32_t flags, std::vector<AQDebugLine> &out) {
    if (flags == AQDebugNone) return;
    const FQuaternion q = body.orientation();

    if (flags & AQDebugBodyAxes) {
        // RGB principal axes at the COM, length 1 (consumer scales). The "body
        // frame" stored on impl is the principal frame (full-tensor descs are
        // folded), so these axes are the principal axes — what the user expects
        // to see for an asymmetric body.
        const auto ex = AQrotate(q, AQvec3(1.f, 0.f, 0.f));
        const auto ey = AQrotate(q, AQvec3(0.f, 1.f, 0.f));
        const auto ez = AQrotate(q, AQvec3(0.f, 0.f, 1.f));
        out.push_back(makeLine(anchor, anchor + ex, 1.f, 0.f, 0.f));
        out.push_back(makeLine(anchor, anchor + ey, 0.f, 1.f, 0.f));
        out.push_back(makeLine(anchor, anchor + ez, 0.f, 0.f, 1.f));
    }
    if (flags & AQDebugVelocity) {
        out.push_back(makeLine(anchor, anchor + body.velocity(), 1.f, 1.f, 0.f));
    }
    if (flags & AQDebugAngularVel) {
        out.push_back(makeLine(anchor, anchor + body.angularVelocity(), 0.f, 1.f, 1.f));
    }
    if (flags & AQDebugMomentum) {
        out.push_back(makeLine(anchor, anchor + body.angularMomentum(), 1.f, 0.f, 1.f));
    }
}

// Emit the 12 line segments of an AABB outline (Phase 2 §9 AQDebugAABB).
void emitAABBDebug(const FAABB& bb, std::vector<AQDebugLine>& out) {
    const float xs[2] = {bb.min[0][0], bb.max[0][0]};
    const float ys[2] = {bb.min[1][0], bb.max[1][0]};
    const float zs[2] = {bb.min[2][0], bb.max[2][0]};
    auto corner = [&](int i, int j, int k) {
        return AQvec3(xs[i], ys[j], zs[k]);
    };
    // 4 edges parallel to X
    for (int j = 0; j < 2; ++j)
      for (int k = 0; k < 2; ++k)
        out.push_back(makeLine(corner(0,j,k), corner(1,j,k), 0.f, 0.6f, 1.f));
    // 4 edges parallel to Y
    for (int i = 0; i < 2; ++i)
      for (int k = 0; k < 2; ++k)
        out.push_back(makeLine(corner(i,0,k), corner(i,1,k), 0.f, 0.6f, 1.f));
    // 4 edges parallel to Z
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 2; ++j)
        out.push_back(makeLine(corner(i,j,0), corner(i,j,1), 0.f, 0.6f, 1.f));
}
}

void AQSpace::stepInternal(float dt) {
    // Phase 1 integrator: the body-frame symplectic Lie scheme with implicit
    // gyroscopic (AQStepBody, AQIntegrator.h). No collision yet (Phase 2+). The
    // step is per-body independent — the shape a Phase 5 GPU kernel inherits.
    //
    // Phase-1.1 additions are folded into AQStepBody itself (per-body gravity
    // scale, adaptive Newton, damping, clamp, debug NaN guard); the per-space
    // loop only adds the optional debug emission below.
    for (auto &body : impl->bodies) {
        if (body->impl->type == AQBodyType::Static) continue;
        auto &s = body->impl->s;
#ifndef NDEBUG
        // Make a too-coarse sub-step for the scene's angular rates loud rather
        // than silently inaccurate. Latched per body — each body warns at most
        // once, so a newly added fast body still warns even if another already
        // did, and changing the sub-step (AQContext::setFixedTimestep) re-arms
        // every body. ‖ω‖ is frame-invariant, so the body-frame norm is the
        // world angular speed.
        if (!body->impl->warnedFastSpin) {
            const float angle = std::sqrt(OmegaGTE::dot(s.angularVelBody, s.angularVelBody)) * dt;
            if (angle > kMaxStepAngle) {
                std::cerr << "AQUA::AQSpace: a body rotates " << angle
                          << " rad/sub-step (‖ω‖·dt, dt=" << dt
                          << " s) — above " << kMaxStepAngle
                          << " rad the Phase 1 integrator's O(dt) drift grows large "
                          << "even with adaptive Newton iteration. Reduce "
                          << "AQContext::setFixedTimestep for this scene's "
                          << "angular rates. (further warnings for this body suppressed)\n";
                body->impl->warnedFastSpin = true;
            }
        }
#endif
        AQStepBody(s, impl->gravity, dt);

        // Debug emission reflects the post-step state. Phase 2 anchors at the
        // world COM (position + R·comOffset); zero offset = pose origin.
        const FVec<3> anchor = s.position + AQrotate(s.orientation, s.comOffset);
        emitBodyDebug(*body, anchor, impl->debugFlags, impl->debugLines);

        // Phase 2 §6.A — refresh the world AABB if the body has a shape, and
        // re-fatten on first refresh or when it has wandered out of fatAABB.
        // Static bodies skip the integrator above but still need their AABB
        // built (once) so they participate in pair generation. Static bodies
        // never move, so this loop reaches them via the static-AABB pass that
        // runBroadphase runs below; here we handle the dynamic case only.
        if (const AQShape *sp = impl->shapeAt(body->impl->shape)) {
            AQTransform<float> bodyXform;
            bodyXform.p = s.position; bodyXform.q = s.orientation;
            body->impl->worldAABB = AQshapeAABB(*sp, bodyXform,
                                                impl->hullVerts.data(),
                                                impl->hullVerts.size());
            if (!body->impl->fatValid || !body->impl->fatAABB.contains(body->impl->worldAABB.min)
                                      || !body->impl->fatAABB.contains(body->impl->worldAABB.max)) {
                // Re-fatten: margin + velocity-proportional dilation per §11.4.
                // dt here is the sub-step; runBroadphase tops up the velocity
                // dilation again against the frame dt before pair generation.
                const float vmag = std::sqrt(OmegaGTE::dot(s.velocity, s.velocity));
                const float margin = impl->fattenMargin + vmag * dt;
                body->impl->fatAABB = body->impl->worldAABB.fattened(margin);
                body->impl->fatValid = true;
            }
        }
    }
}

void AQSpace::resetStepWarnings() {
    // Re-arm the per-body debug fast-spin warning — called when the sub-step
    // changes (AQContext::setFixedTimestep), since that changes ‖ω‖·dt and a
    // body that was fine (or already warned) may now warrant a fresh warning.
    for (auto &body : impl->bodies) body->impl->warnedFastSpin = false;
}

// ============================================================================
// Phase 2 — shape factories (§10 public API additions). Shapes are owned and
// instanced by the space; an invalid handle is returned for malformed input
// (zero radius, empty hull, etc.) so the caller sees a clean "no shape" body.
// ============================================================================

AQShapeHandle AQSpace::createSphereShape(float radius) {
    if (!(radius > 0.f)) return {};
    AQShape s;
    s.type = AQShapeType::Sphere;
    s.sphere.radius = radius;
    return impl->pushShape(s);
}

AQShapeHandle AQSpace::createBoxShape(const FVec<3> &halfExtents) {
    const float hx = halfExtents[0][0], hy = halfExtents[1][0], hz = halfExtents[2][0];
    if (!(hx > 0.f && hy > 0.f && hz > 0.f)) return {};
    AQShape s;
    s.type = AQShapeType::Box;
    s.box.hx = hx; s.box.hy = hy; s.box.hz = hz;
    return impl->pushShape(s);
}

AQShapeHandle AQSpace::createCapsuleShape(float radius, float halfHeight) {
    if (!(radius > 0.f) || halfHeight < 0.f) return {};
    AQShape s;
    s.type = AQShapeType::Capsule;
    s.capsule.radius = radius;
    s.capsule.halfHeight = halfHeight;
    return impl->pushShape(s);
}

AQShapeHandle AQSpace::createPlaneShape(const FVec<3> &normal, float offset) {
    const float n2 = OmegaGTE::dot(normal, normal);
    if (!(n2 > 0.f)) return {};
    const float inv = 1.f / std::sqrt(n2);
    AQShape s;
    s.type = AQShapeType::Plane;
    s.plane.nx = normal[0][0] * inv;
    s.plane.ny = normal[1][0] * inv;
    s.plane.nz = normal[2][0] * inv;
    s.plane.offset = offset * inv;
    return impl->pushShape(s);
}

AQShapeHandle AQSpace::createConvexHullShape(const FVec<3> *pts, std::size_t n) {
    if (pts == nullptr || n == 0) return {};
    const std::uint32_t first = static_cast<std::uint32_t>(impl->hullVerts.size());
    impl->hullVerts.insert(impl->hullVerts.end(), pts, pts + n);
    AQShape s;
    s.type = AQShapeType::ConvexHull;
    s.hull.firstVertex = first;
    s.hull.vertexCount = static_cast<std::uint32_t>(n);
    return impl->pushShape(s);
}

std::vector<AQBroadphasePair> AQSpace::candidatePairs() const {
    return impl->pairs;
}

// ============================================================================
// Phase 2 — sort-based-uniform-grid broadphase (Phase-2 brief §6.B, §11.1).
// Stateless per `advance` tick. Steps:
//   1. Refresh every body's world AABB / fat AABB once at frame dt (static
//      bodies also get an AABB built once — they don't move, so refreshing
//      again is wasted work).
//   2. Choose a cell size from the median fat-AABB extent of dynamic bodies.
//   3. Hash each body to the cells its fat AABB straddles (the broad bound
//      ensures any pair with overlapping fat AABBs lands in some common cell,
//      so the run+neighbour scan finds them at least once).
//   4. Stable-sort (cellHash, bodyIndex) by hash → contiguous runs per cell.
//   5. For each run, test every pair within the run's body set, fattened-AABB
//      overlap + filter accept; emit ordered (min, max) into pairs.
//   6. Sort+unique the pair list. Determinism falls out of the stable sort
//      and the ordered pair invariant (§5).
// Plane bodies are detected and treated as "candidates against every other
// filter-accepted body" — they don't enter the grid (their AABB is huge).
// ============================================================================

namespace {

// Linear 3-axis cell hash. Picked over Morton because every step downstream
// is a sort+scan; the GPU port (Phase 5) will swap this for the Morton form
// the LBVH alternative would also use, with no other code-path changes.
inline std::uint64_t cellKey(int cx, int cy, int cz) {
    // 21-bit signed range per axis: ±1M cells, which at a 1m cell size covers
    // a ±1000 km world — well past any reasonable scene. Bias by 2^20 so the
    // representation is unsigned.
    const std::uint64_t bias = 1ull << 20;
    const std::uint64_t ux = static_cast<std::uint64_t>(static_cast<std::int64_t>(cx) + static_cast<std::int64_t>(bias)) & 0x1FFFFFull;
    const std::uint64_t uy = static_cast<std::uint64_t>(static_cast<std::int64_t>(cy) + static_cast<std::int64_t>(bias)) & 0x1FFFFFull;
    const std::uint64_t uz = static_cast<std::uint64_t>(static_cast<std::int64_t>(cz) + static_cast<std::int64_t>(bias)) & 0x1FFFFFull;
    return (ux << 42) | (uy << 21) | uz;
}

inline int floorDiv(float v, float cell) {
    return static_cast<int>(std::floor(v / cell));
}

struct CellEntry {
    std::uint64_t hash;
    std::uint32_t body;
};

} // namespace

void AQSpace::runBroadphase(float frameDt) {
    impl->pairs.clear();

    auto &bodies = impl->bodies;
    const std::size_t N = bodies.size();
    if (N < 2) {
        // Still drain debug-flag-driven emissions: with 0 or 1 bodies there's
        // nothing to draw or guard against, so just return.
        return;
    }

    // (1) Build the per-body world AABB / fat AABB. The sub-step loop already
    // refreshes dynamic bodies; static bodies and bodies whose fat-AABB is
    // not yet valid get one here. The runBroadphase pass also re-fattens with
    // velocity·frameDt so dilations match the broadphase's once-per-frame
    // cadence (the sub-step uses dt, which is finer).
    std::vector<FAABB> fat(N);
    std::vector<bool>  hasShape(N, false);
    std::vector<bool>  isPlane(N, false);
    for (std::size_t i = 0; i < N; ++i) {
        auto &b = *bodies[i];
        const AQShape *sp = impl->shapeAt(b.impl->shape);
        if (sp == nullptr) continue;
        hasShape[i] = true;
        isPlane[i]  = (sp->type == AQShapeType::Plane);
        AQTransform<float> bx;
        bx.p = b.impl->s.position; bx.q = b.impl->s.orientation;
        const FAABB world = AQshapeAABB(*sp, bx,
                                        impl->hullVerts.data(),
                                        impl->hullVerts.size());
        b.impl->worldAABB = world;
        const float vmag = std::sqrt(OmegaGTE::dot(b.impl->s.velocity, b.impl->s.velocity));
        const float margin = impl->fattenMargin + vmag * frameDt;
        b.impl->fatAABB  = world.fattened(margin);
        b.impl->fatValid = true;
        fat[i] = b.impl->fatAABB;
    }

    // (2) Pick a cell size. Median fat-extent over non-plane shapes; if there
    // are none, fall back to 1.0 — we still complete the pass without dividing
    // by zero, the result is just an empty pair list.
    float cellSize = 1.f;
    {
        std::vector<float> extents;
        extents.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            if (!hasShape[i] || isPlane[i]) continue;
            const auto e = fat[i].extents();
            // Average of the three principal extents — robust to anisotropic
            // bodies; using max would inflate cellSize on a single needle and
            // collapse the grid for the rest of the scene.
            extents.push_back((e[0][0] + e[1][0] + e[2][0]) / 3.f);
        }
        if (!extents.empty()) {
            std::sort(extents.begin(), extents.end());
            const float med = extents[extents.size() / 2];
            if (med > 0.f) cellSize = med;
        }
    }

    // Track plane bodies separately — they pair against every filter-accepting
    // non-plane body without entering the grid.
    std::vector<std::uint32_t> planeBodies;

    // (3) Build (cellHash, body) for every cell each fat AABB straddles.
    // Lower bound and upper bound on the integer grid are inclusive in each
    // axis; the resulting cell set covers the AABB exactly.
    std::vector<CellEntry> entries;
    entries.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        if (!hasShape[i]) continue;
        if (isPlane[i]) { planeBodies.push_back(static_cast<std::uint32_t>(i)); continue; }
        const auto &bb = fat[i];
        const int x0 = floorDiv(bb.min[0][0], cellSize);
        const int y0 = floorDiv(bb.min[1][0], cellSize);
        const int z0 = floorDiv(bb.min[2][0], cellSize);
        const int x1 = floorDiv(bb.max[0][0], cellSize);
        const int y1 = floorDiv(bb.max[1][0], cellSize);
        const int z1 = floorDiv(bb.max[2][0], cellSize);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cy = y0; cy <= y1; ++cy)
                for (int cx = x0; cx <= x1; ++cx) {
                    entries.push_back({cellKey(cx, cy, cz), static_cast<std::uint32_t>(i)});
                }
    }

    // (4) Stable-sort by hash. The secondary key (body index) is implicitly
    // sorted because we push in body-index order and stable_sort preserves
    // equal-key order. Determinism (§5) requires a stable order across runs.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const CellEntry &a, const CellEntry &b) { return a.hash < b.hash; });

    // (5) Walk runs, generate candidates inside each run + filter.
    // De-duplication happens at the end via sort+unique on the ordered pair.
    auto emitPair = [&](std::uint32_t a, std::uint32_t b) {
        if (a == b) return;
        if (!AQfilterAccepts(bodies[a]->impl->filter, bodies[b]->impl->filter)) return;
        if (!fat[a].overlaps(fat[b])) return;
        AQBroadphasePair p;
        if (a < b) { p.a = a; p.b = b; } else { p.a = b; p.b = a; }
        impl->pairs.push_back(p);
    };

    const std::size_t E = entries.size();
    std::size_t runStart = 0;
    while (runStart < E) {
        std::size_t runEnd = runStart + 1;
        const std::uint64_t key = entries[runStart].hash;
        while (runEnd < E && entries[runEnd].hash == key) ++runEnd;
        // Pair within the run. The same pair can also appear in another cell
        // if both bodies' AABBs straddle multiple cells together — sort+unique
        // collapses duplicates below, so we don't track per-run set membership.
        for (std::size_t i = runStart; i < runEnd; ++i)
            for (std::size_t j = i + 1; j < runEnd; ++j)
                emitPair(entries[i].body, entries[j].body);
        runStart = runEnd;
    }

    // (5b) Plane pairs. Each plane vs every non-plane filtered body. The fat
    // AABB overlap is trivially true (plane AABB is huge), so the filter +
    // ordered emit is all we need.
    if (!planeBodies.empty()) {
        for (std::uint32_t pIdx : planeBodies) {
            for (std::size_t j = 0; j < N; ++j) {
                if (!hasShape[j] || isPlane[j]) continue;
                emitPair(pIdx, static_cast<std::uint32_t>(j));
            }
            // Plane-vs-plane pairs intentionally excluded — see §8.
        }
    }

    // (6) Sort + unique → deterministic ordered candidate list (§5 / §8).
    std::sort(impl->pairs.begin(), impl->pairs.end());
    impl->pairs.erase(std::unique(impl->pairs.begin(), impl->pairs.end()),
                      impl->pairs.end());

    // --- Debug emissions (Phase 2 §9) ---
    if (impl->debugFlags & AQDebugAABB) {
        for (std::size_t i = 0; i < N; ++i)
            if (hasShape[i] && !isPlane[i])
                emitAABBDebug(fat[i], impl->debugLines);
    }
    if (impl->debugFlags & AQDebugBroadphasePair) {
        for (const auto &p : impl->pairs) {
            const auto pa = bodies[p.a]->position();
            const auto pb = bodies[p.b]->position();
            // Yellow for an accepted candidate; the filter-rejected ones are
            // simply not emitted (the candidate list IS what passed). The
            // Phase-2 brief §9 colors by filter result; with the present
            // single-pass logic only accepted ones reach here.
            impl->debugLines.push_back(makeLine(pa, pb, 1.f, 1.f, 0.f));
        }
    }
    if (impl->debugFlags & AQDebugBroadphaseGuard) {
        // Single red line at the world origin when candidates approach the
        // brute-force O(n²) baseline — cell size is mistuned for the scene.
        const std::size_t brute = (N * (N - 1)) / 2;
        if (brute > 0 && impl->pairs.size() * 2 > brute) {
            const auto o = AQvec3(0.f, 0.f, 0.f);
            const auto y = AQvec3(0.f, 1.f, 0.f);
            impl->debugLines.push_back(makeLine(o, y, 1.f, 0.f, 0.f));
        }
    }
}
