#include <aqua/AQSpace.h>
#include <aqua/AQIntegrator.h>
#include <vector>
#include <algorithm>
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
    impl->s.torqueAccum += OmegaGTE::cross(worldPoint - impl->s.position, force);
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
    applyAngularImpulse(OmegaGTE::cross(worldPoint - impl->s.position, impulse));
}
void AQRigidBody::applyAngularImpulse(const FVec<3> &angularImpulse) {
    // World angular impulse -> body frame, then Δω = Ib⁻¹ · (Rᵀ · L) (diagonal).
    const FVec<3> lBody = AQrotate(impl->s.orientation.conjugate(), angularImpulse);
    impl->s.angularVelBody[0][0] += impl->s.invInertiaBody[0][0] * lBody[0][0];
    impl->s.angularVelBody[1][0] += impl->s.invInertiaBody[1][0] * lBody[1][0];
    impl->s.angularVelBody[2][0] += impl->s.invInertiaBody[2][0] * lBody[2][0];
}

AQBodyType AQRigidBody::type() const { return impl->type; }

struct AQSpace::Impl {
    FVec<3> gravity = AQvec3(0.f, -9.81f, 0.f);
    std::vector<std::shared_ptr<AQRigidBody>> bodies;

    // Drainable debug surface (Phase-1.1 §6.5). `flags == AQDebugNone` keeps
    // the buffer empty — the per-step emission early-outs and there is nothing
    // to drain. Pull model, owned by the space.
    std::uint32_t                 debugFlags = AQDebugNone;
    std::vector<AQDebugLine>      debugLines;
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
    FVec<3> moments = desc.inertiaPrincipalMoments;
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

    // Reserved (Phase 2 wires the offset into accessors + torque arms; §11.7).
    s.comOffset       = desc.centerOfMass;

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
void emitBodyDebug(const AQRigidBody &body, std::uint32_t flags,
                   std::vector<AQDebugLine> &out) {
    if (flags == AQDebugNone) return;
    const FVec<3>  p   = body.position();
    const FQuaternion q = body.orientation();

    if (flags & AQDebugBodyAxes) {
        // RGB principal axes at the COM, length 1 (consumer scales). The "body
        // frame" stored on impl is the principal frame (full-tensor descs are
        // folded), so these axes are the principal axes — what the user expects
        // to see for an asymmetric body.
        const auto ex = AQrotate(q, AQvec3(1.f, 0.f, 0.f));
        const auto ey = AQrotate(q, AQvec3(0.f, 1.f, 0.f));
        const auto ez = AQrotate(q, AQvec3(0.f, 0.f, 1.f));
        out.push_back(makeLine(p, p + ex, 1.f, 0.f, 0.f));
        out.push_back(makeLine(p, p + ey, 0.f, 1.f, 0.f));
        out.push_back(makeLine(p, p + ez, 0.f, 0.f, 1.f));
    }
    if (flags & AQDebugVelocity) {
        out.push_back(makeLine(p, p + body.velocity(), 1.f, 1.f, 0.f));
    }
    if (flags & AQDebugAngularVel) {
        out.push_back(makeLine(p, p + body.angularVelocity(), 0.f, 1.f, 1.f));
    }
    if (flags & AQDebugMomentum) {
        out.push_back(makeLine(p, p + body.angularMomentum(), 1.f, 0.f, 1.f));
    }
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

        // Debug emission reflects the post-step state — what the test/consumer
        // is going to verify. AQDebugNone short-circuits inside emitBodyDebug.
        emitBodyDebug(*body, impl->debugFlags, impl->debugLines);
    }
}

void AQSpace::resetStepWarnings() {
    // Re-arm the per-body debug fast-spin warning — called when the sub-step
    // changes (AQContext::setFixedTimestep), since that changes ‖ω‖·dt and a
    // body that was fine (or already warned) may now warrant a fresh warning.
    for (auto &body : impl->bodies) body->impl->warnedFastSpin = false;
}
