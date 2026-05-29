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
// this warning exists to make loud. (See AQContext.h and Phase-1 doc §11.5.)
constexpr float kMaxStepAngle = 0.05f;
}
#endif

using OmegaGTE::FVec;
using OmegaGTE::FQuaternion;

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
};

AQSpace::AQSpace() : impl(std::make_unique<Impl>()) {}
AQSpace::~AQSpace() = default;

void AQSpace::setGravity(const FVec<3> &g) { impl->gravity = g; }
FVec<3> AQSpace::gravity() const { return impl->gravity; }

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

    // Inverse principal moments; a zero moment means infinite inertia on that
    // axis (no angular response), which a static body has on every axis.
    const FVec<3> &m = desc.inertiaPrincipalMoments;
    s.invInertiaBody = AQvec3(
        (isStatic || m[0][0] <= 0.f) ? 0.f : 1.f / m[0][0],
        (isStatic || m[1][0] <= 0.f) ? 0.f : 1.f / m[1][0],
        (isStatic || m[2][0] <= 0.f) ? 0.f : 1.f / m[2][0]);

    // Desc gives world-frame angular velocity; store it body-frame.
    s.angularVelBody = AQrotate(s.orientation.conjugate(), desc.angularVelocity);

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

void AQSpace::stepInternal(float dt) {
    // Phase 1 integrator: the body-frame symplectic Lie scheme with implicit
    // gyroscopic (AQStepBody, AQIntegrator.h). No collision yet (Phase 2+). The
    // step is per-body independent — the shape a Phase 5 GPU kernel inherits.
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
                          << " rad the Phase 1 integrator's O(dt) drift grows large. "
                          << "Reduce AQContext::setFixedTimestep for this scene's "
                          << "angular rates. (further warnings for this body suppressed)\n";
                body->impl->warnedFastSpin = true;
            }
        }
#endif
        AQStepBody(s, impl->gravity, dt);
    }
}

void AQSpace::resetStepWarnings() {
    // Re-arm the per-body debug fast-spin warning — called when the sub-step
    // changes (AQContext::setFixedTimestep), since that changes ‖ω‖·dt and a
    // body that was fine (or already warned) may now warrant a fresh warning.
    for (auto &body : impl->bodies) body->impl->warnedFastSpin = false;
}
