#include <aqua/AQSpace.h>
#include <aqua/AQCollision.h>
#include <aqua/AQContact.h>
#include <aqua/AQIntegrator.h>
#include "AQJointBuild.h"
#include "AQQueryMath.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <unordered_map>

// Phase 3 narrowphase entry point — implemented in AQNarrowphase.cpp. The
// declaration lives here (and not in a public header) because the dispatch
// is an internal seam: callers consume `AQContactManifold` via
// `AQSpace::contactManifolds()`, not the dispatcher directly. Returns true
// when at least one contact point was produced.
bool AQnarrowphase(const AQShape &shapeA, const AQShape &shapeB,
                   const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                   const OmegaGTE::FVec<3> *hullVerts, std::size_t hullVertCount,
                   AQContactManifold &out);
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

    // --- Phase 3 material coefficients (per-body) ---
    float restitution = 0.f;   ///< [0, 1]; combined per-pair via AQSpace policy
    float friction    = 0.5f;  ///< ≥ 0; combined per-pair via AQSpace policy

    // --- Phase 3 split-impulse position correction (per-body, per-substep) ---
    // The pseudo-velocity field that the position-correction sweep accumulates
    // into. Applied to `s.position` at the end of the sub-step and zeroed at
    // the top of the next. Kept off `AQBodyState` so the `double`-precision
    // integrator parity oracle and `AQStepBody` remain unaffected.
    FVec<3> pseudoLinear  = AQvec3(0.f, 0.f, 0.f);
    FVec<3> pseudoAngular = AQvec3(0.f, 0.f, 0.f);
    // Angular pseudo-velocity for the JOINT split-impulse pass (Phase 4.x §13),
    // kept SEPARATE from the contact `pseudoAngular` above. Contacts have always
    // dropped their angular pseudo-velocity (position corrected by translation
    // only — see the pose-apply site), and reintroducing it destabilised the
    // settling stack. Joints, whose anchors are off-COM, must rotate to correct
    // position, so their angular correction accumulates here and IS applied to
    // orientation. A joint-less body leaves this 0, so contact scenes are
    // byte-identical to the pre-§13 behaviour.
    FVec<3> pseudoAngularJoint = AQvec3(0.f, 0.f, 0.f);

    // --- Phase 4 per-body state ---
    AQCCDMode ccdMode   = AQCCDMode::Off;  ///< opt-in continuous collision
    bool      isTrigger = false;           ///< overlap events, no constraint rows
    // Per-body sleep-threshold overrides; 0 ⇒ use the space default. Linear m/s,
    // angular rad/s. Read by AQSpace::runIslandsAndSleep (Phase 4b).
    float     sleepLinearVel  = 0.f;
    float     sleepAngularVel = 0.f;
    // Island bookkeeping, refreshed each sub-step by runIslandsAndSleep. Default
    // root is the body's own index; static/kinematic bodies are never unioned.
    std::uint32_t islandId = 0;
    // Consecutive sub-steps this body has been below its sleep thresholds
    // (energy-flavored idle predicate, §6.G). Reset to 0 when it moves or is
    // woken; frozen while Sleeping. The island sleeps when every member's count
    // has reached the threshold.
    std::uint32_t restingFrames = 0;
    // Kinematic target pose. When `hasKinematicTarget`, stepInternal teleports
    // the body to (kinTargetPos, kinTargetOrient) at the next sub-step and sets
    // the implicit velocity ((target − current)/dt) for one-way response.
    FVec<3>     kinTargetPos    = AQvec3(0.f, 0.f, 0.f);
    FQuaternion kinTargetOrient = FQuaternion::Identity();
    bool        hasKinematicTarget = false;
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

// --- Phase 3 material coefficients (per-body) ---
float AQRigidBody::restitution() const            { return impl->restitution; }
void  AQRigidBody::setRestitution(float r) {
    impl->restitution = r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
}
float AQRigidBody::friction() const               { return impl->friction; }
void  AQRigidBody::setFriction(float mu) {
    impl->friction = mu < 0.f ? 0.f : mu;
}

// --- Phase 2 collision accessors (per-body) ---
AQShapeHandle AQRigidBody::shape() const                       { return impl->shape; }
void          AQRigidBody::setShape(const AQShapeHandle &s)    { impl->shape = s; }
FVec<3>       AQRigidBody::aabbMin() const                     { return impl->fatAABB.min; }
FVec<3>       AQRigidBody::aabbMax() const                     { return impl->fatAABB.max; }
AQCollisionFilter AQRigidBody::collisionFilter() const         { return impl->filter; }
void          AQRigidBody::setCollisionFilter(const AQCollisionFilter &f) { impl->filter = f; }

// --- Phase 4: activation / sleep, triggers, CCD, kinematic control ---
AQActivationState AQRigidBody::activation() const { return impl->s.activation; }

void AQRigidBody::wakeUp() {
    // Kinematic / static bodies don't sleep; leave their state alone.
    if (impl->s.activation == AQActivationState::Kinematic) return;
    if (impl->type == AQBodyType::Static) return;
    impl->s.activation = AQActivationState::Active;
    impl->restingFrames = 0;   // start the idle accumulation over
    // The island pass re-evaluates idle accumulation from here; the body is
    // live again immediately and (being in an island) wakes its island next step.
}

void AQRigidBody::putToSleep() {
    if (impl->type != AQBodyType::Dynamic) return;   // only dynamics sleep
    impl->s.activation     = AQActivationState::Sleeping;
    impl->s.velocity       = AQvec3(0.f, 0.f, 0.f);
    impl->s.angularVelBody = AQvec3(0.f, 0.f, 0.f);
}

bool      AQRigidBody::isTrigger() const          { return impl->isTrigger; }
AQCCDMode AQRigidBody::ccdMode()   const          { return impl->ccdMode; }
void      AQRigidBody::setCCDMode(AQCCDMode m)     { impl->ccdMode = m; }

void AQRigidBody::setKinematicTarget(const FVec<3> &p, const FQuaternion &q) {
    impl->kinTargetPos       = p;
    impl->kinTargetOrient    = q.normalized();
    impl->hasKinematicTarget = true;
}

// --- Phase 3: persistence cache record (per contact point) ---
// Holds the accumulated normal and friction impulses across frames so the
// PGS sweep can warm-start (§6.C / §11.7). Keyed by (sortedPairKey,
// featureKey) — `sortedPairKey` is `(uint64(a) << 32) | uint64(b)` with
// `a < b` per the broadphase invariant.
struct AQManifoldCacheEntry {
    float accumNormal       = 0.f;
    float accumFriction[2]  = {0.f, 0.f};
};

// A joint living in the space's joint table (Phase 4, §7/§8). The two endpoints
// are held by shared_ptr so the body index can be re-resolved each sub-step
// (robust to removeBody, which renumbers the body SoA); the rest-pose state
// (`rest`) anchors the angular locks and the hinge/slider axis tracking. `accum`
// is the per-row warm-start carrier, keyed implicitly by (this joint, row index)
// — the joint analogue of the contact persistence cache. `generation` (≥1 when
// live) + the slot index form the public AQJointHandle; `alive` is cleared by
// destroyJoint without renumbering the table.
struct AQJointRecord {
    AQJointType                  type = AQJointType::BallSocket;
    SharedHandle<AQRigidBody> a, b;
    FVec<3>                      anchorA    = AQvec3(0.f, 0.f, 0.f);
    FVec<3>                      anchorB    = AQvec3(0.f, 0.f, 0.f);
    FVec<3>                      axisLocalA = AQvec3(0.f, 1.f, 0.f);
    float                        distanceTarget = 0.f;
    AQJointSoftness              softness;
    AQJointAxisLimit             limit;
    AQJointRest                  rest;
    std::uint32_t                generation = 0;
    bool                         alive = false;
    float                        accum[kAQMaxJointRows] = {0.f};
    // World-frame LINEAR impulse this joint applied last sub-step (Σ over its
    // non-angular rows of direction·accumImpulse). Exposed via jointImpulse()
    // so callers can read the reaction force (impulse/dt) — the bridge
    // deliverable's catenary-tension oracle (§9).
    FVec<3>                      lastLinearImpulse = AQvec3(0.f, 0.f, 0.f);
};

struct AQSpace::Impl {
    FVec<3> gravity = AQvec3(0.f, -9.81f, 0.f);
    OmegaCommon::Vector<SharedHandle<AQRigidBody>> bodies;

    // Drainable debug surface (Phase-1.1 §6.5). `flags == AQDebugNone` keeps
    // the buffer empty — the per-step emission early-outs and there is nothing
    // to drain. Pull model, owned by the space.
    std::uint32_t                 debugFlags = AQDebugNone;
    OmegaCommon::Vector<AQDebugLine>      debugLines;

    // --- Phase 2: shape table + vertex pool (§8 shapes pooled and shared) ---
    // Index 0 is a sentinel "invalid" slot — handles default to {0, 0} and
    // `valid()` returns false on generation 0. Generations start at 1 and tick
    // monotonically on remove; the simple pool never shrinks.
    OmegaCommon::Vector<AQShape>        shapes      = OmegaCommon::Vector<AQShape>(1);
    OmegaCommon::Vector<std::uint32_t>  generations = OmegaCommon::Vector<std::uint32_t>(1, 0);
    OmegaCommon::Vector<FVec<3>>        hullVerts;     ///< vertex pool referenced by shapes

    // --- Phase 2: broadphase output (§5/§8 ordered + de-duplicated) ---
    OmegaCommon::Vector<AQBroadphasePair> pairs;
    float fattenMargin = 0.02f;                 ///< §11.4 fixed margin (≈2cm world units)

    // --- Phase 3: contact data + solver state (§7, §8) ---
    AQMaterialCombine restitutionCombine = AQMaterialCombine::Average;
    AQMaterialCombine frictionCombine    = AQMaterialCombine::Average;
    // Defaults: §11.4 leans Box2D's 8 / 4 for short stacks; a 10-box settling
    // stack (the Phase-3 §1 headline deliverable) is the critical workload.
    // PGS propagates info one contact per iteration, so a 10-tall stack
    // needs ~10 iterations just for end-to-end propagation, plus headroom
    // for refinement. 16 / 8 holds the 5-stack but is borderline on 10. 32
    // velocity / 12 position lands the bottom contact at >90% of the
    // analytic resting force and keeps the stack at <5 cm/s indefinitely.
    int               velocityIters      = 48;
    int               positionIters      = 16;
    OmegaCommon::Vector<AQContactManifold> manifolds;   ///< current sub-step's manifolds (§10)
    OmegaCommon::Vector<AQConstraintRow>   rows;        ///< current sub-step's row buffer (§8)
    OmegaCommon::Vector<std::uint32_t>     manifoldRowOffset; ///< rows[manifoldRowOffset[m]] = first row of manifold m
    std::unordered_map<std::uint64_t, AQManifoldCacheEntry> cache;
    ///< (pair key << 32 | featureKey)-indexed; aged out after one missed frame

    // --- Phase 4: joint table (§7/§8) ---
    // Slot 0 is a sentinel (handle generation 0 ⇒ invalid). Slots never shrink;
    // destroyJoint clears `alive` and bumps the generation. `jointRowSpans` maps
    // each joint that contributed rows this sub-step back to its (firstRow,count)
    // in `rows`, for the warm-start write-back.
    OmegaCommon::Vector<AQJointRecord> joints = OmegaCommon::Vector<AQJointRecord>(1);
    struct JointRowSpan { std::uint32_t jointIndex, firstRow, count; };
    OmegaCommon::Vector<JointRowSpan> jointRowSpans;

    AQJointRecord *jointAt(const AQJointHandle &h) {
        if (!h.valid()) return nullptr;
        if (h.index >= joints.size()) return nullptr;
        AQJointRecord &j = joints[h.index];
        if (!j.alive || j.generation != h.generation) return nullptr;
        return &j;
    }

    // --- Phase 4: CCD scratch (§6.K) ---
    // Pre-step positions of bodies, captured before the position half-step when
    // any body opts into CCD, so the swept-sphere TOI pass can cast from where
    // each body was to where it landed. Reused buffer (no per-sub-step alloc).
    OmegaCommon::Vector<FVec<3>> ccdPrevPos;

    // --- Phase 4: trigger events (§6.M, §11.9) ---
    // Trigger overlaps are diffed once per advance (not per sub-step) against
    // last advance's set, producing Enter/Stay/Exit. `prevTriggerPairs` is the
    // previous advance's sorted overlap set; `triggerEvts` is the drainable
    // queue rebuilt each advance.
    OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>> prevTriggerPairs;
    OmegaCommon::Vector<AQTriggerEvent> triggerEvts;

    // --- Phase 4: sleep tuning (space defaults; per-body overrides win) ---
    // Energy-flavored idle predicate (§6.G / §11.6): a body counts as idle when
    // ‖v‖ < sleepLinearVel AND ‖ω‖ < sleepAngularVel. An island sleeps once
    // every member has been idle for `sleepIdleSubsteps` consecutive sub-steps.
    // Defaults from §10: 0.01 m/s, 0.01 rad/s, 60 sub-steps (~0.5 s @ 1/120).
    float         sleepLinearVel    = 0.01f;
    float         sleepAngularVel   = 0.01f;
    std::uint32_t sleepIdleSubsteps = 60;

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

SharedHandle<AQRigidBody> AQSpace::addBody(const AQBodyDesc &desc) {
    struct Ctor : AQRigidBody { Ctor() : AQRigidBody() {} };
    auto body = SharedHandle<AQRigidBody>(new Ctor());
    auto &s = body->impl->s;

    body->impl->type = desc.type;
    s.position    = desc.position;
    s.orientation = desc.orientation.normalized();
    s.velocity    = desc.linearVelocity;

    // Static and Kinematic bodies both have infinite mass (invMass 0). The
    // difference is motion: a Static body never moves; a Kinematic body is
    // teleported each sub-step by the user (setKinematicTarget) and pushes
    // dynamics one-way. Dynamic is the only finite-mass type.
    const bool infiniteMass = (desc.type != AQBodyType::Dynamic) || desc.mass <= 0.f;
    s.invMass = infiniteMass ? 0.f : 1.f / desc.mass;

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
    if (!infiniteMass && !anyNonZero(desc.inertiaTensor) && isVec3Zero(moments)) {
        if (const AQShape *sp = impl->shapeAt(desc.shape)) {
            moments = AQshapeInertiaMoments(*sp, desc.mass,
                                            impl->hullVerts.data(),
                                            impl->hullVerts.size());
        }
    }
    if (!infiniteMass && anyNonZero(desc.inertiaTensor)) {
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
        (infiniteMass || moments[0][0] <= 0.f) ? 0.f : 1.f / moments[0][0],
        (infiniteMass || moments[1][0] <= 0.f) ? 0.f : 1.f / moments[1][0],
        (infiniteMass || moments[2][0] <= 0.f) ? 0.f : 1.f / moments[2][0]);

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

    // Phase 3 materials. Clamp the same way the setters do — invalid input
    // becomes the closest valid value rather than tripping a debug assert,
    // because the user-facing descriptor is the most likely place to land
    // a typo (negative, NaN); a silent clamp is the principle-of-least-
    // surprise for descriptor-driven input.
    body->impl->restitution = desc.restitution < 0.f ? 0.f
                            : desc.restitution > 1.f ? 1.f : desc.restitution;
    body->impl->friction    = desc.friction    < 0.f ? 0.f : desc.friction;

    // Phase 4 attachments. Kinematic bodies report the Kinematic activation
    // state (one-way response, user-driven pose); everything else starts Active.
    s.activation       = (desc.type == AQBodyType::Kinematic)
                       ? AQActivationState::Kinematic : AQActivationState::Active;
    body->impl->ccdMode         = desc.ccdMode;
    body->impl->isTrigger       = desc.isTrigger;
    body->impl->sleepLinearVel  = desc.sleepLinearVelocity  < 0.f ? 0.f : desc.sleepLinearVelocity;
    body->impl->sleepAngularVel = desc.sleepAngularVelocity < 0.f ? 0.f : desc.sleepAngularVelocity;
    body->impl->kinTargetPos    = s.position;
    body->impl->kinTargetOrient = s.orientation;

    impl->bodies.push_back(body);
    return body;
}

bool AQSpace::removeBody(const SharedHandle<AQRigidBody> &body) {
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
OmegaCommon::Vector<AQDebugLine> AQSpace::drainDebugLines() {
    OmegaCommon::Vector<AQDebugLine> out;
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
                   std::uint32_t flags, OmegaCommon::Vector<AQDebugLine> &out) {
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
void emitAABBDebug(const FAABB& bb, OmegaCommon::Vector<AQDebugLine>& out) {
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
    // Phase 3 sub-step structure (Phase-3 brief §6, §10):
    //   1. Velocity half-step — apply gravity / accumulated forces / damping /
    //      implicit-gyroscopic Newton. Bodies' angular and linear velocities
    //      now hold the "predicted" velocity the contact solver will modify.
    //   2. Narrowphase + sequential-impulse PGS + split-impulse position
    //      correction. Modifies velocities via contact impulses; accumulates
    //      `pseudoLinear`/`pseudoAngular` per body for the position pass.
    //   3. Position half-step — advance pose with the solver-corrected
    //      velocity, then apply pseudoLinear · dt to position (orientation
    //      stays unaffected per the brief §6.E lean). AABB refresh and debug
    //      emission then run on the post-step state.
    //
    // Phase-1 fast-spin warning lives in the per-body loop just before the
    // velocity half-step, where it always has — the metric is ‖ω‖·dt of the
    // pre-step angular velocity, which is what determines integrator drift.
    // Phase 4 — kinematic target application (runs before integration). A
    // kinematic body is teleported to its target pose and its implicit velocity
    // ((target − current)/dt) is set so the contact solver sees a moving
    // infinite-mass body that pushes dynamics ONE-WAY (invMass/invInertia are 0,
    // so it receives no impulse). With no fresh target this sub-step the body is
    // treated as at rest. Static and dynamic bodies skip this loop.
    for (auto &body : impl->bodies) {
        if (body->impl->type != AQBodyType::Kinematic) continue;
        auto &s = body->impl->s;
        if (body->impl->hasKinematicTarget && dt > 0.f) {
            s.velocity = (body->impl->kinTargetPos - s.position) * (1.f / dt);
            // Angular implicit velocity from the orientation delta:
            // dq = q_target · q_current⁻¹; ω_world = 2·log(dq)/dt (shortest arc).
            FQuaternion dq = (body->impl->kinTargetOrient * s.orientation.conjugate()).normalized();
            if (dq.w < 0.f) { dq.x = -dq.x; dq.y = -dq.y; dq.z = -dq.z; dq.w = -dq.w; }
            const FVec<3> wWorld = AQquatLog(dq) * (2.f / dt);   // AQquatLog → ½·φ
            s.angularVelBody = AQrotate(body->impl->kinTargetOrient.conjugate(), wWorld);
            s.position    = body->impl->kinTargetPos;
            s.orientation = body->impl->kinTargetOrient;
            body->impl->hasKinematicTarget = false;
        } else {
            s.velocity       = AQvec3(0.f, 0.f, 0.f);
            s.angularVelBody = AQvec3(0.f, 0.f, 0.f);
        }
    }

    for (auto &body : impl->bodies) {
        if (body->impl->type == AQBodyType::Static) continue;
        auto &s = body->impl->s;
#ifndef NDEBUG
        // Make a too-coarse sub-step for the scene's angular rates loud rather
        // than silently inaccurate. Latched per body — each body warns at most
        // once, so a newly added fast body still warns even if another already
        // did, and changing the sub-step (AQContext::setFixedTimestep) re-arms
        // every body. ‖ω‖ is frame-invariant, so the body-frame norm is the
        // world angular speed. Kinematic bodies carry a user-driven implicit ω,
        // so the integrator-drift warning does not apply to them.
        if (body->impl->type == AQBodyType::Dynamic && !body->impl->warnedFastSpin) {
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
        AQStepBodyVelocity(s, impl->gravity, dt);
    }

    // Phase 3 — contact solver (operates on the now-predicted velocities).
    runNarrowphaseAndSolve(dt);

    // Phase 4 CCD — if any body opts in, snapshot pre-step positions so the
    // post-step swept-sphere TOI pass can detect tunneling (§6.K). Cheap O(N)
    // scan; skipped entirely (the common case) when no body uses CCD.
    bool ccdActive = false;
    for (auto &body : impl->bodies) {
        if (body->impl->ccdMode != AQCCDMode::Off && body->impl->type == AQBodyType::Dynamic) {
            ccdActive = true; break;
        }
    }
    if (ccdActive) {
        // assign (not resize) — a fresh element would otherwise default-construct
        // FVec<3>, whose default ctor is private (factory-only idiom).
        impl->ccdPrevPos.assign(impl->bodies.size(), AQvec3(0.f, 0.f, 0.f));
        for (std::size_t i = 0; i < impl->bodies.size(); ++i)
            impl->ccdPrevPos[i] = impl->bodies[i]->impl->s.position;
    }

    // Position half-step + pseudo-velocity position correction + AABB refresh
    // + debug emission. Static bodies remain skipped (no pose update).
    for (auto &body : impl->bodies) {
        if (body->impl->type == AQBodyType::Static) continue;
        auto &s = body->impl->s;
        AQStepBodyPosition(s, dt);
        // Split-impulse positional correction (Phase-3 brief §6.E). Applied
        // after the velocity-driven position advance so the corrective shift is
        // layered on top of the integrator's symplectic update. Contacts correct
        // by translation only (their angular pseudo-velocity stays dropped, as
        // before). Joints additionally rotate — their off-COM anchors can't be
        // fixed by translation alone — via the separate `pseudoAngularJoint`
        // accumulator (Phase 4.x §13). It is world-frame (built from world-
        // inverse-inertia); rotate it into the body frame for the exponential-map
        // update, matching the velocity-driven orientation step.
        s.position += body->impl->pseudoLinear * dt;
        const FVec<3> pseudoOmegaW = body->impl->pseudoAngularJoint;
        if (OmegaGTE::dot(pseudoOmegaW, pseudoOmegaW) > 0.f) {
            const FVec<3> pseudoOmegaBody = AQrotate(s.orientation.conjugate(), pseudoOmegaW);
            s.orientation = AQintegrate(s.orientation, pseudoOmegaBody, dt);
        }

        // Debug emission reflects the post-step state. Phase 2 anchors at the
        // world COM (position + R·comOffset); zero offset = pose origin.
        const FVec<3> anchor = s.position + AQrotate(s.orientation, s.comOffset);
        emitBodyDebug(*body, anchor, impl->debugFlags, impl->debugLines);

        // Phase 2 §6.A — refresh the world AABB if the body has a shape, and
        // re-fatten on first refresh or when it has wandered out of fatAABB.
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

    // Phase 4 — continuous collision: stop opted-in fast bodies at their time of
    // impact before islands/sleep run (so a snapped body's corrected state is
    // what the sleep predicate sees).
    if (ccdActive) runCCD(dt);

    // Phase 4 — island detection + sleep/wake on the post-solve state. Sets the
    // activation the next sub-step's velocity sweep and integrator read.
    runIslandsAndSleep(dt);
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

OmegaCommon::Vector<AQBroadphasePair> AQSpace::candidatePairs() const {
    return impl->pairs;
}

// ============================================================================
// Phase 3 — material combine + solver knob + manifold view (§10 public API).
// ============================================================================

void AQSpace::setMaterialCombine(AQMaterialCombine restCombine,
                                 AQMaterialCombine fricCombine) {
    impl->restitutionCombine = restCombine;
    impl->frictionCombine    = fricCombine;
}
AQMaterialCombine AQSpace::restitutionCombine() const { return impl->restitutionCombine; }
AQMaterialCombine AQSpace::frictionCombine()    const { return impl->frictionCombine; }

void AQSpace::setSolverIterations(int velocityIters, int positionIters) {
    // Negative is meaningless; clamp at zero (zero disables that pass — the
    // `positionIters == 0` case is the energy-non-growth test path; zero
    // velocity iters disables the contact solver entirely except for warm-
    // started impulses, which the user can use to short-circuit a settled
    // scene if they ever want).
    impl->velocityIters = velocityIters < 0 ? 0 : velocityIters;
    impl->positionIters = positionIters < 0 ? 0 : positionIters;
}
int AQSpace::velocityIterations() const { return impl->velocityIters; }
int AQSpace::positionIterations() const { return impl->positionIters; }

OmegaCommon::Vector<AQContactManifold> AQSpace::contactManifolds() const {
    return impl->manifolds;
}

// --- Phase 4: sleep tuning (storage; consumed by runIslandsAndSleep, §6.G) ---
void AQSpace::setSleepThresholds(float linearVel, float angularVel,
                                 std::uint32_t idleSubsteps) {
    impl->sleepLinearVel    = linearVel  < 0.f ? 0.f : linearVel;
    impl->sleepAngularVel   = angularVel < 0.f ? 0.f : angularVel;
    impl->sleepIdleSubsteps = idleSubsteps;
}

// ============================================================================
// Phase 4 — joints (§6.C, §7, §10). Five specialized types built on the shared
// constraint-row schema; the per-type Jacobian math lives in AQJoint.cpp.
// ============================================================================

namespace {
// Rest-pose state captured at creation: the relative orientation the angular
// locks hold to, and the hinge/slider axis re-expressed in body B's frame so the
// alignment row can compare it against body A's axis each sub-step.
AQJointRest computeJointRest(const FQuaternion &qA0, const FQuaternion &qB0,
                             const FVec<3> &axisLocalA) {
    AQJointRest r;
    r.relOrient        = (qA0.conjugate() * qB0).normalized();
    const FVec<3> axisW = AQrotate(qA0, axisLocalA);
    r.axisLocalB       = AQrotate(qB0.conjugate(), axisW);
    return r;
}
} // namespace

AQJointHandle AQSpace::createDistanceJoint(const SharedHandle<AQRigidBody> &a,
                                           const SharedHandle<AQRigidBody> &b,
                                           const FVec<3> &anchorALocal,
                                           const FVec<3> &anchorBLocal,
                                           float length) {
    if (!a || !b) return {};
    if (a->type() != AQBodyType::Dynamic && b->type() != AQBodyType::Dynamic) return {};
    AQJointRecord J;
    J.type = AQJointType::Distance;
    J.a = a; J.b = b;
    J.anchorA = anchorALocal; J.anchorB = anchorBLocal;
    J.distanceTarget = length < 0.f ? 0.f : length;
    J.rest = computeJointRest(a->orientation(), b->orientation(), AQvec3(0.f, 1.f, 0.f));
    J.generation = 1; J.alive = true;
    const std::uint32_t idx = static_cast<std::uint32_t>(impl->joints.size());
    impl->joints.push_back(J);
    return AQJointHandle{idx, 1};
}

AQJointHandle AQSpace::createBallSocketJoint(const SharedHandle<AQRigidBody> &a,
                                             const SharedHandle<AQRigidBody> &b,
                                             const FVec<3> &anchorALocal,
                                             const FVec<3> &anchorBLocal) {
    if (!a || !b) return {};
    if (a->type() != AQBodyType::Dynamic && b->type() != AQBodyType::Dynamic) return {};
    AQJointRecord J;
    J.type = AQJointType::BallSocket;
    J.a = a; J.b = b;
    J.anchorA = anchorALocal; J.anchorB = anchorBLocal;
    J.rest = computeJointRest(a->orientation(), b->orientation(), AQvec3(0.f, 1.f, 0.f));
    J.generation = 1; J.alive = true;
    const std::uint32_t idx = static_cast<std::uint32_t>(impl->joints.size());
    impl->joints.push_back(J);
    return AQJointHandle{idx, 1};
}

AQJointHandle AQSpace::createHingeJoint(const SharedHandle<AQRigidBody> &a,
                                        const SharedHandle<AQRigidBody> &b,
                                        const FVec<3> &anchorALocal,
                                        const FVec<3> &anchorBLocal,
                                        const FVec<3> &axisALocal,
                                        const AQJointAxisLimit &limit) {
    if (!a || !b) return {};
    if (a->type() != AQBodyType::Dynamic && b->type() != AQBodyType::Dynamic) return {};
    AQJointRecord J;
    J.type = AQJointType::Hinge;
    J.a = a; J.b = b;
    J.anchorA = anchorALocal; J.anchorB = anchorBLocal;
    J.axisLocalA = axisALocal; J.limit = limit;
    J.rest = computeJointRest(a->orientation(), b->orientation(), axisALocal);
    J.generation = 1; J.alive = true;
    const std::uint32_t idx = static_cast<std::uint32_t>(impl->joints.size());
    impl->joints.push_back(J);
    return AQJointHandle{idx, 1};
}

AQJointHandle AQSpace::createSliderJoint(const SharedHandle<AQRigidBody> &a,
                                         const SharedHandle<AQRigidBody> &b,
                                         const FVec<3> &anchorALocal,
                                         const FVec<3> &anchorBLocal,
                                         const FVec<3> &axisALocal,
                                         const AQJointAxisLimit &limit) {
    if (!a || !b) return {};
    if (a->type() != AQBodyType::Dynamic && b->type() != AQBodyType::Dynamic) return {};
    AQJointRecord J;
    J.type = AQJointType::Slider;
    J.a = a; J.b = b;
    J.anchorA = anchorALocal; J.anchorB = anchorBLocal;
    J.axisLocalA = axisALocal; J.limit = limit;
    J.rest = computeJointRest(a->orientation(), b->orientation(), axisALocal);
    J.generation = 1; J.alive = true;
    const std::uint32_t idx = static_cast<std::uint32_t>(impl->joints.size());
    impl->joints.push_back(J);
    return AQJointHandle{idx, 1};
}

AQJointHandle AQSpace::createFixedJoint(const SharedHandle<AQRigidBody> &a,
                                        const SharedHandle<AQRigidBody> &b) {
    if (!a || !b) return {};
    if (a->type() != AQBodyType::Dynamic && b->type() != AQBodyType::Dynamic) return {};
    AQJointRecord J;
    J.type = AQJointType::Fixed;
    J.a = a; J.b = b;
    // A fixed joint holds B at its CURRENT relative pose, not at A's origin: the
    // point constraint's anchor on A is B's origin expressed in A's local frame
    // at creation (anchorB stays at B's origin). Both anchors then coincide at
    // rest, and the joint preserves the offset rather than collapsing B onto A.
    J.anchorA = AQrotate(a->orientation().conjugate(), b->position() - a->position());
    J.anchorB = AQvec3(0.f, 0.f, 0.f);
    J.rest = computeJointRest(a->orientation(), b->orientation(), AQvec3(0.f, 1.f, 0.f));
    J.generation = 1; J.alive = true;
    const std::uint32_t idx = static_cast<std::uint32_t>(impl->joints.size());
    impl->joints.push_back(J);
    return AQJointHandle{idx, 1};
}

void AQSpace::setJointSoftness(AQJointHandle h, AQJointSoftness s) {
    if (AQJointRecord *J = impl->jointAt(h)) J->softness = s;
}

bool AQSpace::destroyJoint(AQJointHandle h) {
    AQJointRecord *J = impl->jointAt(h);
    if (J == nullptr) return false;
    J->alive = false;
    ++J->generation;          // stale out any outstanding handle; slot is not reused
    J->a.reset(); J->b.reset();
    return true;
}

OmegaCommon::Vector<AQJointDesc> AQSpace::joints() const {
    OmegaCommon::Vector<AQJointDesc> out;
    std::unordered_map<const AQRigidBody *, std::uint32_t> idx;
    idx.reserve(impl->bodies.size());
    for (std::uint32_t i = 0; i < impl->bodies.size(); ++i)
        idx.emplace(impl->bodies[i].get(), i);
    for (std::uint32_t ji = 1; ji < impl->joints.size(); ++ji) {
        const AQJointRecord &J = impl->joints[ji];
        if (!J.alive) continue;
        auto ia = idx.find(J.a.get());
        auto ib = idx.find(J.b.get());
        AQJointDesc d;
        d.type = J.type;
        d.bodyA = (ia != idx.end()) ? ia->second : 0u;
        d.bodyB = (ib != idx.end()) ? ib->second : 0u;
        d.anchorA = J.anchorA; d.anchorB = J.anchorB; d.axisLocalA = J.axisLocalA;
        d.distanceTarget = J.distanceTarget; d.softness = J.softness; d.limit = J.limit;
        out.push_back(d);
    }
    return out;
}

FVec<3> AQSpace::jointImpulse(AQJointHandle h) const {
    if (!h.valid() || h.index >= impl->joints.size()) return AQvec3(0.f, 0.f, 0.f);
    const AQJointRecord &J = impl->joints[h.index];
    if (!J.alive || J.generation != h.generation) return AQvec3(0.f, 0.f, 0.f);
    return J.lastLinearImpulse;
}

void AQSpace::buildJointRows(float dt) {
    impl->jointRowSpans.clear();
    if (impl->joints.size() <= 1) return;        // only the sentinel slot

    // Resolve body pointers to current SoA indices (robust to removeBody, which
    // renumbers the body array).
    std::unordered_map<const AQRigidBody *, std::uint32_t> idx;
    idx.reserve(impl->bodies.size());
    for (std::uint32_t i = 0; i < impl->bodies.size(); ++i)
        idx.emplace(impl->bodies[i].get(), i);

    AQConstraintRow scratch[kAQMaxJointRows];
    for (std::uint32_t ji = 1; ji < impl->joints.size(); ++ji) {
        AQJointRecord &J = impl->joints[ji];
        if (!J.alive) continue;
        auto ia = idx.find(J.a.get());
        auto ib = idx.find(J.b.get());
        if (ia == idx.end() || ib == idx.end()) continue;   // an endpoint was removed
        const std::uint32_t aIdx = ia->second, bIdx = ib->second;

        const auto &sa = impl->bodies[aIdx]->impl->s;
        const auto &sb = impl->bodies[bIdx]->impl->s;

        AQJointBodyKin A, B;
        A.com = sa.position + AQrotate(sa.orientation, sa.comOffset);
        A.q = sa.orientation; A.invMass = sa.invMass;
        A.invI = AQworldInvInertia(sa.orientation, sa.invInertiaBody);
        A.vel = sa.velocity; A.omega = AQrotate(sa.orientation, sa.angularVelBody);
        B.com = sb.position + AQrotate(sb.orientation, sb.comOffset);
        B.q = sb.orientation; B.invMass = sb.invMass;
        B.invI = AQworldInvInertia(sb.orientation, sb.invInertiaBody);
        B.vel = sb.velocity; B.omega = AQrotate(sb.orientation, sb.angularVelBody);

        AQJointDesc desc;
        desc.type = J.type; desc.bodyA = aIdx; desc.bodyB = bIdx;
        desc.anchorA = J.anchorA; desc.anchorB = J.anchorB; desc.axisLocalA = J.axisLocalA;
        desc.distanceTarget = J.distanceTarget; desc.softness = J.softness; desc.limit = J.limit;

        const int nr = AQbuildJointRows(desc, J.rest, A, B, dt, scratch);
        if (nr == 0) continue;

        const std::uint32_t first = static_cast<std::uint32_t>(impl->rows.size());
        for (int r = 0; r < nr; ++r) {
            scratch[r].bodyA = aIdx;
            scratch[r].bodyB = bIdx;
            scratch[r].accumImpulse = J.accum[r];      // warm-start
            impl->rows.push_back(scratch[r]);
        }
        impl->jointRowSpans.push_back({ji, first, static_cast<std::uint32_t>(nr)});
    }
}

// ============================================================================
// Phase 4 — queries (§6.L, §10). Raycast / shapecast / overlap iterate the
// candidate bodies, reading each body's fat AABB (the broadphase output, valid
// until the next advance) for the broad reject, then the analytic ray/shape math
// (AQQuery.cpp). Results are reported in a deterministic (fraction, bodyIndex)
// order. A full 3D-DDA grid walk over the stored cell scratch is the documented
// performance path (Phase-4 §6.L); this correctness-first form reuses the same
// per-body broadphase bounds and matches the brute-force oracle exactly.
// ============================================================================

namespace {
inline bool queryFilterAccepts(const AQQueryFilter &q, const AQCollisionFilter &b) {
    return ((q.layer & b.mask) != 0u) && ((b.layer & q.mask) != 0u);
}
} // namespace

void AQSpace::raycast(const FVec<3> &origin, const FVec<3> &direction, float maxT,
                      const AQQueryFilter &filter, OmegaCommon::Vector<AQRaycastHit> &hits) const {
    hits.clear();
    for (std::uint32_t i = 0; i < impl->bodies.size(); ++i) {
        auto &bi = *impl->bodies[i]->impl;
        const AQShape *sp = impl->shapeAt(bi.shape);
        if (sp == nullptr) continue;
        if (!queryFilterAccepts(filter, bi.filter)) continue;
        float tEnter;
        if (bi.fatValid && !AQrayAABB(bi.fatAABB.min, bi.fatAABB.max, origin, direction, maxT, tEnter))
            continue;                                       // broad reject vs the broadphase bound
        AQTransform<float> xf; xf.p = bi.s.position; xf.q = bi.s.orientation;
        float t; FVec<3> pos = AQvec3(0.f,0.f,0.f), nrm = AQvec3(0.f,0.f,0.f);
        if (AQrayShape(*sp, xf, origin, direction, maxT, 0.f,
                       impl->hullVerts.data(), impl->hullVerts.size(), t, pos, nrm)) {
            AQRaycastHit h; h.bodyIndex = i; h.fraction = t; h.position = pos; h.normal = nrm;
            hits.push_back(h);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const AQRaycastHit &a, const AQRaycastHit &b) {
        return (a.fraction != b.fraction) ? (a.fraction < b.fraction) : (a.bodyIndex < b.bodyIndex);
    });
    if ((impl->debugFlags & AQDebugRaycastHit) && !hits.empty()) {
        const FVec<3> hp = origin + direction * hits.front().fraction;
        impl->debugLines.push_back(makeLine(origin, hp, 1.f, 1.f, 0.f));
        impl->debugLines.push_back(makeLine(hp, hp + hits.front().normal * 0.25f, 0.f, 1.f, 0.f));
    }
}

void AQSpace::shapecast(AQShapeHandle shape, const FVec<3> &origin,
                        const FQuaternion & /*orientation*/, const FVec<3> &direction,
                        float maxT, const AQQueryFilter &filter,
                        OmegaCommon::Vector<AQRaycastHit> &hits) const {
    hits.clear();
    const AQShape *cast = impl->shapeAt(shape);
    if (cast == nullptr) return;
    // Sphere-cast specialization: sweep the cast shape's bounding sphere, i.e.
    // ray vs each target inflated by the cast radius (the Minkowski sum for a
    // swept sphere). Exact for sphere/plane targets; conservative otherwise.
    const float inflate = AQshapeBoundingRadius(*cast, impl->hullVerts.data(), impl->hullVerts.size());
    for (std::uint32_t i = 0; i < impl->bodies.size(); ++i) {
        auto &bi = *impl->bodies[i]->impl;
        const AQShape *sp = impl->shapeAt(bi.shape);
        if (sp == nullptr) continue;
        if (!queryFilterAccepts(filter, bi.filter)) continue;
        float tEnter;
        const FVec<3> inf = AQvec3(inflate, inflate, inflate);
        if (bi.fatValid && !AQrayAABB(bi.fatAABB.min - inf, bi.fatAABB.max + inf,
                                      origin, direction, maxT, tEnter))
            continue;
        AQTransform<float> xf; xf.p = bi.s.position; xf.q = bi.s.orientation;
        float t; FVec<3> pos = AQvec3(0.f,0.f,0.f), nrm = AQvec3(0.f,0.f,0.f);
        if (AQrayShape(*sp, xf, origin, direction, maxT, inflate,
                       impl->hullVerts.data(), impl->hullVerts.size(), t, pos, nrm)) {
            AQRaycastHit h; h.bodyIndex = i; h.fraction = t; h.position = pos; h.normal = nrm;
            hits.push_back(h);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const AQRaycastHit &a, const AQRaycastHit &b) {
        return (a.fraction != b.fraction) ? (a.fraction < b.fraction) : (a.bodyIndex < b.bodyIndex);
    });
}

void AQSpace::overlap(AQShapeHandle shape, const FVec<3> &origin,
                      const FQuaternion &orientation, const AQQueryFilter &filter,
                      bool exactShapes, OmegaCommon::Vector<std::uint32_t> &bodies) const {
    bodies.clear();
    const AQShape *q = impl->shapeAt(shape);
    if (q == nullptr) return;
    AQTransform<float> qx; qx.p = origin; qx.q = orientation;
    const FAABB qbb = AQshapeAABB(*q, qx, impl->hullVerts.data(), impl->hullVerts.size());
    for (std::uint32_t i = 0; i < impl->bodies.size(); ++i) {
        auto &bi = *impl->bodies[i]->impl;
        const AQShape *sp = impl->shapeAt(bi.shape);
        if (sp == nullptr) continue;
        if (!queryFilterAccepts(filter, bi.filter)) continue;
        const FAABB bb = bi.fatValid ? bi.fatAABB : bi.worldAABB;
        if (!qbb.overlaps(bb)) continue;                    // broad AABB reject
        if (exactShapes) {
            AQTransform<float> bx; bx.p = bi.s.position; bx.q = bi.s.orientation;
            AQContactManifold mf;
            if (!AQnarrowphase(*q, *sp, qx, bx, impl->hullVerts.data(), impl->hullVerts.size(), mf)
                || mf.pointCount == 0)
                continue;                                   // AABBs touch but shapes don't
        }
        bodies.push_back(i);                                // pushed in ascending index order
    }
}

// ============================================================================
// Phase 4 — trigger events (§6.M, §11.9). Run once per advance by AQContext.
// ============================================================================
void AQSpace::updateTriggers() {
    impl->triggerEvts.clear();

    // This advance's overlapping trigger pairs (tight world-AABB test, per the
    // §6.M "short-circuit after the AABB test" rule). Candidate pairs are
    // already (a < b)-ordered, so `curr` stays sorted.
    OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>> curr;
    for (const auto &p : impl->pairs) {
        const auto &ba = *impl->bodies[p.a]->impl;
        const auto &bb = *impl->bodies[p.b]->impl;
        if (!ba.isTrigger && !bb.isTrigger) continue;
        if (!ba.worldAABB.overlaps(bb.worldAABB)) continue;
        curr.emplace_back(p.a, p.b);
    }
    std::sort(curr.begin(), curr.end());

    // Merge-diff against the previous advance: prev-only ⇒ Exit, curr-only ⇒
    // Enter, both ⇒ Stay.
    const auto &prev = impl->prevTriggerPairs;
    std::size_t i = 0, j = 0;
    while (i < prev.size() || j < curr.size()) {
        if (j >= curr.size() || (i < prev.size() && prev[i] < curr[j])) {
            impl->triggerEvts.push_back({prev[i].first, prev[i].second, AQTriggerEventKind::Exit});
            ++i;
        } else if (i >= prev.size() || curr[j] < prev[i]) {
            impl->triggerEvts.push_back({curr[j].first, curr[j].second, AQTriggerEventKind::Enter});
            ++j;
        } else {
            impl->triggerEvts.push_back({curr[j].first, curr[j].second, AQTriggerEventKind::Stay});
            ++i; ++j;
        }
    }
    impl->prevTriggerPairs.swap(curr);

    // Deterministic order: by (a, b), then Exit before Stay before Enter (the
    // §11.9 "process exits first" idiom; at most one event per pair so the kind
    // tiebreak is belt-and-suspenders).
    std::sort(impl->triggerEvts.begin(), impl->triggerEvts.end(),
              [](const AQTriggerEvent &x, const AQTriggerEvent &y) {
                  if (x.a != y.a) return x.a < y.a;
                  if (x.b != y.b) return x.b < y.b;
                  return static_cast<int>(x.kind) > static_cast<int>(y.kind);
              });
}

OmegaCommon::Vector<AQTriggerEvent> AQSpace::triggerEvents() {
    OmegaCommon::Vector<AQTriggerEvent> out;
    out.swap(impl->triggerEvts);     // drain: subsequent calls return empty until next advance
    return out;
}

// ============================================================================
// Phase 4 — continuous collision (§6.K). After the regular position step, each
// CCD body's bounding sphere is swept from its pre-step position to where it
// landed; if that sweep hits another shape sooner than the full step, the body
// tunneled — snap it to the time of impact (just touching) and cancel the
// into-surface velocity so it cannot pass through.
//
// The swept test reuses the analytic sphere-cast already shipped for queries
// (AQrayShape with inflate = the body's bounding radius). For a sphere body this
// is exact; for a non-spherical body it is the conservative bounding-sphere
// approximation. (The general convex conservative-advancement over AQshapeSupport
// — Mirtich 1997, the brief's lead — is the heavier generalization, deferred:
// the analytic swept sphere is exact for the Phase 4 bullet deliverable and
// reuses shipped code. Recorded in the Phase-4 plan §13.) `AQCCDMode::Speculative`
// and `Continuous` share this path; Speculative additionally benefits from the
// broadphase's velocity-proportional AABB fattening already applied each frame.
// ============================================================================
void AQSpace::runCCD(float dt) {
    (void)dt;
    auto &bodies = impl->bodies;
    const std::size_t N = bodies.size();
    for (std::size_t i = 0; i < N; ++i) {
        auto &bi = *bodies[i]->impl;
        if (bi.ccdMode == AQCCDMode::Off || bi.type != AQBodyType::Dynamic) continue;
        const AQShape *sp = impl->shapeAt(bi.shape);
        if (sp == nullptr) continue;
        if (i >= impl->ccdPrevPos.size()) continue;

        const FVec<3> p0 = impl->ccdPrevPos[i];
        const FVec<3> p1 = bi.s.position;
        const FVec<3> disp = p1 - p0;
        const float dlen = std::sqrt(OmegaGTE::dot(disp, disp));
        const float r = AQshapeBoundingRadius(*sp, impl->hullVerts.data(), impl->hullVerts.size());
        if (dlen <= r || r <= 0.f) continue;          // moved less than its own radius ⇒ discrete is safe

        // Sweep the bounding sphere from p0 along `disp` (t ∈ [0,1]) against every
        // other eligible shape; keep the earliest impact.
        float bestT = 1.f;
        FVec<3> bestN = AQvec3(0.f, 0.f, 0.f);
        bool hit = false;
        for (std::size_t j = 0; j < N; ++j) {
            if (j == i) continue;
            auto &bj = *bodies[j]->impl;
            if (bj.isTrigger) continue;                // triggers never block motion
            const AQShape *sj = impl->shapeAt(bj.shape);
            if (sj == nullptr) continue;
            if (!AQfilterAccepts(bi.filter, bj.filter)) continue;
            AQTransform<float> xj; xj.p = bj.s.position; xj.q = bj.s.orientation;
            float t; FVec<3> pos = AQvec3(0.f,0.f,0.f), nrm = AQvec3(0.f,0.f,0.f);
            if (AQrayShape(*sj, xj, p0, disp, 1.f, r,
                           impl->hullVerts.data(), impl->hullVerts.size(), t, pos, nrm)) {
                if (t < bestT) { bestT = t; bestN = nrm; hit = true; }
            }
        }

        if (hit && bestT < 1.f) {
            bi.s.position = p0 + disp * bestT;          // snap to the time of impact (touching)
            const float vn = OmegaGTE::dot(bi.s.velocity, bestN);
            if (vn < 0.f) bi.s.velocity = bi.s.velocity - bestN * vn;  // kill the into-surface component
            // Refresh the body's bounds at the corrected pose so the next frame's
            // broadphase / queries see it where it actually stopped.
            AQTransform<float> xf; xf.p = bi.s.position; xf.q = bi.s.orientation;
            bi.worldAABB = AQshapeAABB(*sp, xf, impl->hullVerts.data(), impl->hullVerts.size());
            bi.fatAABB   = bi.worldAABB.fattened(impl->fattenMargin);
            bi.fatValid  = true;
            if (impl->debugFlags & AQDebugCCDSweep)
                impl->debugLines.push_back(makeLine(p0, bi.s.position, 1.f, 0.f, 1.f));
        }
    }
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
    OmegaCommon::Vector<FAABB> fat(N);
    OmegaCommon::Vector<bool>  hasShape(N, false);
    OmegaCommon::Vector<bool>  isPlane(N, false);
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
        OmegaCommon::Vector<float> extents;
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
    OmegaCommon::Vector<std::uint32_t> planeBodies;

    // (3) Build (cellHash, body) for every cell each fat AABB straddles.
    // Lower bound and upper bound on the integer grid are inclusive in each
    // axis; the resulting cell set covers the AABB exactly.
    OmegaCommon::Vector<CellEntry> entries;
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

// ============================================================================
// Phase 3 — narrowphase + sequential-impulse PGS solver + split-impulse pos
// correction + persistence cache + debug emissions. The whole §6 pipeline,
// per sub-step.
//
// Layout: `manifolds` is the per-substep contact-pair list, sorted by the
// deterministic `(a, b)` pair index (inherited from `impl->pairs`). `rows` is
// the constraint-row buffer the PGS sweep iterates — three rows per contact
// point (one normal, two friction) in manifold order. `manifoldRowOffset[m]`
// is the index in `rows` where manifold m's first row sits, so the
// persistence handoff can write the accumulated impulses back to the right
// `AQContactPoint`. The cache is keyed by a packed
// `(bodyA, bodyB, featureKey)` 64-bit word; the warm-start lookup and the
// write-back use the same key.
// ============================================================================

namespace {

// Combine two per-body material coefficients into a per-pair coefficient.
// Defaults match PhysX `Average`; `Min`/`Max`/`Multiply` are the gameplay
// overrides (a Max-restitution "super bouncy ball" wins regardless of the
// surface's restitution).
inline float combineMaterial(AQMaterialCombine mode, float a, float b) {
    switch (mode) {
    case AQMaterialCombine::Average:  return (a + b) * 0.5f;
    case AQMaterialCombine::Min:      return std::min(a, b);
    case AQMaterialCombine::Max:      return std::max(a, b);
    case AQMaterialCombine::Multiply: return a * b;
    }
    return (a + b) * 0.5f;
}

// Build an orthonormal pair of tangents perpendicular to `n`. The choice is
// deterministic and continuous in `n` for the cone we care about (n on the
// upper hemisphere); for the resting-stack and incline tests this stays
// stable across sub-steps so the warm-started friction impulses remain
// meaningful frame to frame.
inline void buildTangentBasis(const FVec<3> &n, FVec<3> &t1, FVec<3> &t2) {
    const FVec<3> alt = (std::abs(n[0][0]) < 0.6f) ? AQvec3(1.f, 0.f, 0.f)
                       : (std::abs(n[1][0]) < 0.6f) ? AQvec3(0.f, 1.f, 0.f)
                                                    : AQvec3(0.f, 0.f, 1.f);
    t1 = OmegaGTE::cross(n, alt);
    const float t1n2 = OmegaGTE::dot(t1, t1);
    t1 = (t1n2 > 1e-12f) ? t1 * (1.f / std::sqrt(t1n2)) : AQvec3(1.f, 0.f, 0.f);
    t2 = OmegaGTE::cross(n, t1);
    const float t2n2 = OmegaGTE::dot(t2, t2);
    t2 = (t2n2 > 1e-12f) ? t2 * (1.f / std::sqrt(t2n2)) : AQvec3(0.f, 1.f, 0.f);
}

// Pack a 64-bit cache key from (bodyA, bodyB, featureKey). Body indices use
// 16 bits each (we never expect 65k+ bodies in a single space; the broadphase
// is the tighter ceiling). Feature key uses the top 32 bits — wide enough for
// the box/box composite (refAxis, refSign, incAxis, incSign, vertex slot)
// without collisions.
inline std::uint64_t pairFeatureKey(std::uint32_t a, std::uint32_t b, std::uint32_t feat) {
    return (static_cast<std::uint64_t>(a)    & 0xFFFFull)       |
           ((static_cast<std::uint64_t>(b)   & 0xFFFFull) << 16) |
           (static_cast<std::uint64_t>(feat) << 32);
}

} // namespace

void AQSpace::runNarrowphaseAndSolve(float dt) {
    impl->manifolds.clear();
    impl->rows.clear();
    impl->manifoldRowOffset.clear();

    // Zero the split-impulse pseudo-velocities. They accumulate during the
    // position-correction pass and are applied to `s.position` in
    // `stepInternal` after the position half-step.
    for (auto &body : impl->bodies) {
        body->impl->pseudoLinear       = AQvec3(0.f, 0.f, 0.f);
        body->impl->pseudoAngular      = AQvec3(0.f, 0.f, 0.f);
        body->impl->pseudoAngularJoint = AQvec3(0.f, 0.f, 0.f);
    }

    // NOTE: no early-out on an empty pair list — joints may still contribute
    // rows even when nothing is in contact (the bridge before it touches
    // anything). The narrowphase loop below simply iterates zero pairs.

    // A body can absorb constraint impulses only if it is Dynamic, has finite
    // mass, and is Active. Static, kinematic, and sleeping bodies cannot — a
    // row/manifold whose two bodies are both inert is skipped in the warm-start,
    // the velocity sweep, and the position-correction pass (Phase 4 sleep skip).
    // Connected bodies share an island activation state, so a mixed
    // awake/asleep contact row does not arise for bodies actually in contact.
    auto movable = [&](std::uint32_t bi) {
        const auto &st = impl->bodies[bi]->impl->s;
        return st.invMass > 0.f && st.activation == AQActivationState::Active;
    };
    auto rowInert = [&](const AQConstraintRow &r) {
        return !movable(r.bodyA) && !movable(r.bodyB);
    };

    // --- A. Narrowphase: build manifolds from candidate pairs ---
    for (const auto &pair : impl->pairs) {
        AQRigidBody *bodyA = impl->bodies[pair.a].get();
        AQRigidBody *bodyB = impl->bodies[pair.b].get();
        // Trigger pairs produce overlap EVENTS (updateTriggers, per advance), not
        // contact rows — short-circuit here so a trigger never pushes anything.
        if (bodyA->impl->isTrigger || bodyB->impl->isTrigger) continue;
        const AQShape *shA = impl->shapeAt(bodyA->impl->shape);
        const AQShape *shB = impl->shapeAt(bodyB->impl->shape);
        if (shA == nullptr || shB == nullptr) continue;

        AQTransform<float> xfA;
        xfA.p = bodyA->impl->s.position;
        xfA.q = bodyA->impl->s.orientation;
        AQTransform<float> xfB;
        xfB.p = bodyB->impl->s.position;
        xfB.q = bodyB->impl->s.orientation;

        AQContactManifold mf;
        mf.a = pair.a;
        mf.b = pair.b;
        if (!AQnarrowphase(*shA, *shB, xfA, xfB,
                           impl->hullVerts.data(), impl->hullVerts.size(),
                           mf) || mf.pointCount == 0) {
            continue;
        }

        // --- B. Material combine ---
        mf.restitutionCombined = combineMaterial(impl->restitutionCombine,
                                                 bodyA->impl->restitution,
                                                 bodyB->impl->restitution);
        mf.frictionCombined    = combineMaterial(impl->frictionCombine,
                                                 bodyA->impl->friction,
                                                 bodyB->impl->friction);

        // --- C. Persistence lookup (warm-start) ---
        for (std::uint32_t i = 0; i < mf.pointCount; ++i) {
            const std::uint64_t key = pairFeatureKey(pair.a, pair.b,
                                                    mf.points[i].featureKey);
            auto it = impl->cache.find(key);
            if (it != impl->cache.end()) {
                mf.points[i].accumNormal       = it->second.accumNormal;
                mf.points[i].accumFriction[0]  = it->second.accumFriction[0];
                mf.points[i].accumFriction[1]  = it->second.accumFriction[1];
            }
        }
        impl->manifolds.push_back(mf);
    }

    if (impl->manifolds.empty()) {
        // Drop the contact cache so settled contacts don't carry stale impulses
        // if bodies separate and re-contact later (zero-frame grace, §11.8). We
        // do NOT return here — joints may still produce rows below.
        impl->cache.clear();
    }

    // --- Constraint-row build: 1 normal + 2 friction per contact point ---
    for (std::size_t mi = 0; mi < impl->manifolds.size(); ++mi) {
        AQContactManifold &mf = impl->manifolds[mi];
        impl->manifoldRowOffset.push_back(static_cast<std::uint32_t>(impl->rows.size()));

        const auto &bA = impl->bodies[mf.a]->impl->s;
        const auto &bB = impl->bodies[mf.b]->impl->s;
        const FMatrix<3,3> invIA = AQworldInvInertia(bA.orientation, bA.invInertiaBody);
        const FMatrix<3,3> invIB = AQworldInvInertia(bB.orientation, bB.invInertiaBody);
        const FVec<3> comA = bA.position + AQrotate(bA.orientation, bA.comOffset);
        const FVec<3> comB = bB.position + AQrotate(bB.orientation, bB.comOffset);

        FVec<3> t1 = AQvec3(1.f, 0.f, 0.f), t2 = AQvec3(0.f, 1.f, 0.f);
        buildTangentBasis(mf.normalWorld, t1, t2);

        // Effective mass denominator for a single direction `dir` at arms rA, rB.
        // Catto 2005 §3: Keff = 1/m_A + 1/m_B + (rA × dir)ᵀ · invI_A · (rA × dir)
        //                                     + (rB × dir)ᵀ · invI_B · (rB × dir)
        auto effMass = [&](const FVec<3> &rA, const FVec<3> &rB, const FVec<3> &dir) {
            const FVec<3> rAxN  = OmegaGTE::cross(rA, dir);
            const FVec<3> rBxN  = OmegaGTE::cross(rB, dir);
            const FVec<3> IArAN = invIA * rAxN;
            const FVec<3> IBrBN = invIB * rBxN;
            const float k = bA.invMass + bB.invMass
                          + OmegaGTE::dot(rAxN, IArAN)
                          + OmegaGTE::dot(rBxN, IBrBN);
            return (k > 1e-12f) ? (1.f / k) : 0.f;
        };

        // Pre-step relative normal velocity for the restitution bias (Catto's
        // velocity-threshold model — resting contacts get bias 0 so they
        // don't artifact-bounce).
        const FVec<3> wAworld = AQrotate(bA.orientation, bA.angularVelBody);
        const FVec<3> wBworld = AQrotate(bB.orientation, bB.angularVelBody);

        for (std::uint32_t pi = 0; pi < mf.pointCount; ++pi) {
            const AQContactPoint &cp = mf.points[pi];
            const FVec<3> rA = cp.positionWorld - comA;
            const FVec<3> rB = cp.positionWorld - comB;

            const FVec<3> velA = bA.velocity + OmegaGTE::cross(wAworld, rA);
            const FVec<3> velB = bB.velocity + OmegaGTE::cross(wBworld, rB);
            const float relVN = OmegaGTE::dot(velB - velA, mf.normalWorld);

            constexpr float kRestitutionVelThr = 1.0f;  // m/s
            const float bias = (relVN < -kRestitutionVelThr)
                ? (mf.restitutionCombined * relVN)
                : 0.f;

            const std::uint32_t normalRowIdx = static_cast<std::uint32_t>(impl->rows.size());

            AQConstraintRow nRow;
            nRow.kind          = AQConstraintKind::ContactNormal;
            nRow.bodyA         = mf.a;
            nRow.bodyB         = mf.b;
            nRow.contactPoint  = cp.positionWorld;
            nRow.rA            = rA;
            nRow.rB            = rB;
            nRow.direction     = mf.normalWorld;
            nRow.effectiveMass = effMass(rA, rB, mf.normalWorld);
            nRow.bias          = bias;
            nRow.accumImpulse  = cp.accumNormal;
            nRow.peerRow       = normalRowIdx;       // self-peer for the normal row
            nRow.frictionCoeff = 0.f;
            impl->rows.push_back(nRow);

            AQConstraintRow fRow;
            fRow.kind          = AQConstraintKind::ContactFriction;
            fRow.bodyA         = mf.a;
            fRow.bodyB         = mf.b;
            fRow.contactPoint  = cp.positionWorld;
            fRow.rA            = rA;
            fRow.rB            = rB;
            fRow.direction     = t1;
            fRow.effectiveMass = effMass(rA, rB, t1);
            fRow.bias          = 0.f;
            fRow.accumImpulse  = cp.accumFriction[0];
            fRow.peerRow       = normalRowIdx;
            fRow.frictionCoeff = mf.frictionCombined;
            impl->rows.push_back(fRow);

            fRow.direction     = t2;
            fRow.effectiveMass = effMass(rA, rB, t2);
            fRow.accumImpulse  = cp.accumFriction[1];
            impl->rows.push_back(fRow);
        }
    }

    // --- C. Joint-row build (appends to the same row buffer, after contacts) ---
    buildJointRows(dt);

    if (impl->rows.empty()) {
        // Nothing to solve this sub-step (no contacts and no joints).
        impl->cache.clear();
        return;
    }

    // Apply a row's impulse increment `dl` to both bodies. LINEAR rows apply a
    // `direction·dl` impulse at `contactPoint` (the Phase 3 path, unchanged);
    // ANGULAR rows (joint orientation locks / angular limits / motors) apply a
    // pure torque impulse `direction·dl`. Defined once, used by warm-start and
    // the sweep so the two paths can never drift.
    auto applyRowImpulse = [&](const AQConstraintRow &row, float dl) {
        const FVec<3> P = row.direction * dl;
        if (row.isAngular) {
            impl->bodies[row.bodyB]->applyAngularImpulse(P);
            impl->bodies[row.bodyA]->applyAngularImpulse(P * -1.f);
        } else {
            impl->bodies[row.bodyB]->applyImpulseAtPoint(P,        row.contactPoint);
            impl->bodies[row.bodyA]->applyImpulseAtPoint(P * -1.f, row.contactPoint);
        }
    };

    // --- D. Warm-start: apply cached impulses once before the iteration ---
    for (const AQConstraintRow &row : impl->rows) {
        if (rowInert(row)) continue;            // both bodies inert (sleep skip)
        if (row.accumImpulse == 0.f) continue;
        applyRowImpulse(row, row.accumImpulse);
    }

    // --- E. Sequential-impulse PGS sweep ---
    // Soft-constraint CFM regularization: γ = compliance/dt² (0 for contacts and
    // hard joints, so the term vanishes — no branch, the Phase 3 math is exact).
    const float invDt2 = (dt > 0.f) ? (1.f / (dt * dt)) : 0.f;
    for (int iter = 0; iter < impl->velocityIters; ++iter) {
        for (AQConstraintRow &row : impl->rows) {
            if (rowInert(row)) continue;        // both bodies inert (Phase 4 sleep skip)
            const auto &bA = impl->bodies[row.bodyA]->impl->s;
            const auto &bB = impl->bodies[row.bodyB]->impl->s;
            const FVec<3> wA = AQrotate(bA.orientation, bA.angularVelBody);
            const FVec<3> wB = AQrotate(bB.orientation, bB.angularVelBody);

            // Relative velocity along the constraint direction: angular rows use
            // the relative angular velocity; linear rows the point velocity.
            float relV;
            if (row.isAngular) {
                relV = OmegaGTE::dot(wB - wA, row.direction);
            } else {
                const FVec<3> velA = bA.velocity + OmegaGTE::cross(wA, row.rA);
                const FVec<3> velB = bB.velocity + OmegaGTE::cross(wB, row.rB);
                relV = OmegaGTE::dot(velB - velA, row.direction);
            }

            const float gamma  = row.compliance * invDt2;          // CFM (0 ⇒ hard)
            float lambda = -(relV + row.bias + gamma * row.accumImpulse) * row.effectiveMass;

            // Per-kind bound clamp.
            float newAccum = row.accumImpulse + lambda;
            switch (row.kind) {
            case AQConstraintKind::ContactNormal:
            case AQConstraintKind::JointLimit:                     // one-sided: λ ≥ 0
                if (newAccum < 0.f) newAccum = 0.f;
                break;
            case AQConstraintKind::ContactFriction: {              // cone clamp |λ| ≤ μ·λ_n
                const float peerN = impl->rows[row.peerRow].accumImpulse;
                const float maxF  = row.frictionCoeff * peerN;
                if (newAccum >  maxF) newAccum =  maxF;
                if (newAccum < -maxF) newAccum = -maxF;
                break;
            }
            case AQConstraintKind::JointMotor: {                   // |λ| ≤ motorMaxImpulse
                const float maxM = row.frictionCoeff;
                if (newAccum >  maxM) newAccum =  maxM;
                if (newAccum < -maxM) newAccum = -maxM;
                break;
            }
            case AQConstraintKind::JointAxis:                      // bilateral: unbounded
            default:
                break;
            }
            const float dl = newAccum - row.accumImpulse;
            row.accumImpulse = newAccum;

            if (dl != 0.f) applyRowImpulse(row, dl);
        }
    }

    // --- F. Split-impulse position correction (pseudo-velocity sweep) ---
    if (impl->positionIters > 0 && dt > 0.f) {
        // Precompute world-frame inverse inertia per body once; reused for
        // every iteration's pseudo-impulse application.
        OmegaCommon::Vector<FMatrix<3,3>> invIWorld;
        invIWorld.reserve(impl->bodies.size());
        for (auto &body : impl->bodies) {
            invIWorld.push_back(AQworldInvInertia(body->impl->s.orientation,
                                                  body->impl->s.invInertiaBody));
        }

        constexpr float kSlop                = 0.005f;  // 5 mm tolerated penetration
        constexpr float kPositionERP         = 0.2f;    // Catto split-impulse rate
        constexpr float kMaxPosCorrectionVel = 2.f;     // m/s clamp on pseudo-velocity

        for (int iter = 0; iter < impl->positionIters; ++iter) {
            for (std::size_t mi = 0; mi < impl->manifolds.size(); ++mi) {
                const AQContactManifold &mf = impl->manifolds[mi];
                if (!movable(mf.a) && !movable(mf.b)) continue;  // inert pair (sleep skip)
                AQRigidBody::Impl &biA = *impl->bodies[mf.a]->impl;
                AQRigidBody::Impl &biB = *impl->bodies[mf.b]->impl;
                const auto &bA = biA.s;
                const auto &bB = biB.s;
                const FMatrix<3,3> &invIA = invIWorld[mf.a];
                const FMatrix<3,3> &invIB = invIWorld[mf.b];
                const FVec<3> comA = bA.position + AQrotate(bA.orientation, bA.comOffset);
                const FVec<3> comB = bB.position + AQrotate(bB.orientation, bB.comOffset);

                for (std::uint32_t pi = 0; pi < mf.pointCount; ++pi) {
                    const AQContactPoint &cp = mf.points[pi];
                    const float pen = cp.depth - kSlop;
                    if (pen <= 0.f) continue;

                    const FVec<3> rA = cp.positionWorld - comA;
                    const FVec<3> rB = cp.positionWorld - comB;

                    const FVec<3> pVelA = biA.pseudoLinear + OmegaGTE::cross(biA.pseudoAngular, rA);
                    const FVec<3> pVelB = biB.pseudoLinear + OmegaGTE::cross(biB.pseudoAngular, rB);
                    const float relPV = OmegaGTE::dot(pVelB - pVelA, mf.normalWorld);

                    float targetVel = kPositionERP * pen / dt;
                    if (targetVel > kMaxPosCorrectionVel) targetVel = kMaxPosCorrectionVel;

                    const FVec<3> rAxN  = OmegaGTE::cross(rA, mf.normalWorld);
                    const FVec<3> rBxN  = OmegaGTE::cross(rB, mf.normalWorld);
                    const FVec<3> IArAN = invIA * rAxN;
                    const FVec<3> IBrBN = invIB * rBxN;
                    const float k = bA.invMass + bB.invMass
                                  + OmegaGTE::dot(rAxN, IArAN)
                                  + OmegaGTE::dot(rBxN, IBrBN);
                    if (k < 1e-12f) continue;

                    float lambda = (targetVel - relPV) / k;
                    if (lambda < 0.f) lambda = 0.f;

                    const FVec<3> P = mf.normalWorld * lambda;
                    biB.pseudoLinear  += P * bB.invMass;
                    biB.pseudoAngular += invIB * OmegaGTE::cross(rB, P);
                    biA.pseudoLinear  -= P * bA.invMass;
                    biA.pseudoAngular -= invIA * OmegaGTE::cross(rA, P);
                }
            }

            // Joint split-impulse (Phase 4.x §13): bilateral JointAxis rows
            // correct their position error `C` as a pseudo-velocity here, exactly
            // as contacts do above — so the velocity solve (whose `bias` is now 0
            // for these rows) never injects the Baumgarte energy that inflated the
            // reported joint impulse. Motors (a velocity goal), limits (one-sided,
            // kept on Baumgarte — see AQJoint.cpp), and soft rows (spring ERP) are
            // skipped. Angular correction accumulates into `pseudoAngularJoint`,
            // separate from the contact pass so contact scenes stay byte-identical.
            for (const auto &span : impl->jointRowSpans) {
                for (std::uint32_t ri = 0; ri < span.count; ++ri) {
                    const AQConstraintRow &row = impl->rows[span.firstRow + ri];
                    if (row.kind != AQConstraintKind::JointAxis) continue;  // motors/limits keep Baumgarte
                    if (row.compliance != 0.f) continue;                    // soft joint: spring handles position
                    if (!movable(row.bodyA) && !movable(row.bodyB)) continue;

                    // Bilateral: drive C → 0 → target relative velocity = −ERP·C/dt.
                    float target = -kPositionERP * row.positionError / dt;
                    if (target >  kMaxPosCorrectionVel) target =  kMaxPosCorrectionVel;
                    if (target < -kMaxPosCorrectionVel) target = -kMaxPosCorrectionVel;

                    AQRigidBody::Impl &biA = *impl->bodies[row.bodyA]->impl;
                    AQRigidBody::Impl &biB = *impl->bodies[row.bodyB]->impl;
                    const auto &bA = biA.s;
                    const auto &bB = biB.s;
                    const FMatrix<3,3> &invIA = invIWorld[row.bodyA];
                    const FMatrix<3,3> &invIB = invIWorld[row.bodyB];

                    if (row.isAngular) {
                        const float relPV = OmegaGTE::dot(biB.pseudoAngularJoint - biA.pseudoAngularJoint, row.direction);
                        const float k = OmegaGTE::dot(row.direction, invIA * row.direction)
                                      + OmegaGTE::dot(row.direction, invIB * row.direction);
                        if (k < 1e-12f) continue;
                        const float lambda = (target - relPV) / k;
                        const FVec<3> P = row.direction * lambda;
                        biB.pseudoAngularJoint += invIB * P;
                        biA.pseudoAngularJoint -= invIA * P;
                    } else {
                        const FVec<3> rA = row.rA;
                        const FVec<3> rB = row.rB;
                        const FVec<3> pVelA = biA.pseudoLinear + OmegaGTE::cross(biA.pseudoAngularJoint, rA);
                        const FVec<3> pVelB = biB.pseudoLinear + OmegaGTE::cross(biB.pseudoAngularJoint, rB);
                        const float relPV = OmegaGTE::dot(pVelB - pVelA, row.direction);
                        const FVec<3> rAxN  = OmegaGTE::cross(rA, row.direction);
                        const FVec<3> rBxN  = OmegaGTE::cross(rB, row.direction);
                        const FVec<3> IArAN = invIA * rAxN;
                        const FVec<3> IBrBN = invIB * rBxN;
                        const float k = bA.invMass + bB.invMass
                                      + OmegaGTE::dot(rAxN, IArAN)
                                      + OmegaGTE::dot(rBxN, IBrBN);
                        if (k < 1e-12f) continue;
                        const float lambda = (target - relPV) / k;
                        const FVec<3> P = row.direction * lambda;
                        biB.pseudoLinear       += P * bB.invMass;
                        biB.pseudoAngularJoint += invIB * OmegaGTE::cross(rB, P);
                        biA.pseudoLinear       -= P * bA.invMass;
                        biA.pseudoAngularJoint -= invIA * OmegaGTE::cross(rA, P);
                    }
                }
            }
        }
    }

    // --- G. Persistence handoff: write the accumulated impulses back to the
    // manifold points and into the cache. The new cache replaces the old
    // unconditionally (zero-frame grace per §11.8: a contact that misses a
    // frame loses its warm-start).
    decltype(impl->cache) newCache;
    newCache.reserve(impl->rows.size() / 3 + 1);
    for (std::size_t mi = 0; mi < impl->manifolds.size(); ++mi) {
        AQContactManifold &mf = impl->manifolds[mi];
        const std::uint32_t off = impl->manifoldRowOffset[mi];
        for (std::uint32_t pi = 0; pi < mf.pointCount; ++pi) {
            const std::uint32_t base = off + pi * 3;
            const float ln  = impl->rows[base + 0].accumImpulse;
            const float lt0 = impl->rows[base + 1].accumImpulse;
            const float lt1 = impl->rows[base + 2].accumImpulse;
            mf.points[pi].accumNormal       = ln;
            mf.points[pi].accumFriction[0]  = lt0;
            mf.points[pi].accumFriction[1]  = lt1;
            AQManifoldCacheEntry ce;
            ce.accumNormal = ln;
            ce.accumFriction[0] = lt0;
            ce.accumFriction[1] = lt1;
            newCache.emplace(pairFeatureKey(mf.a, mf.b, mf.points[pi].featureKey), ce);
        }
    }
    impl->cache = std::move(newCache);

    // Joint warm-start write-back: each joint's per-row accumulated impulse is
    // saved into its record for next sub-step's warm-start (§6.N). Keyed by the
    // joint and the local row index — the joint analogue of the contact cache.
    for (const auto &span : impl->jointRowSpans) {
        AQJointRecord &J = impl->joints[span.jointIndex];
        FVec<3> linImp = AQvec3(0.f, 0.f, 0.f);
        for (std::uint32_t r = 0; r < span.count && r < kAQMaxJointRows; ++r) {
            const AQConstraintRow &row = impl->rows[span.firstRow + r];
            J.accum[r] = row.accumImpulse;
            if (!row.isAngular) linImp += row.direction * row.accumImpulse;
        }
        J.lastLinearImpulse = linImp;
    }

    // --- Debug emissions (Phase-3 brief §9 / new AQDebugContact* flags) ---
    if (impl->debugFlags & (AQDebugContactPoint | AQDebugContactNormal | AQDebugContactImpulse)) {
        for (std::size_t mi = 0; mi < impl->manifolds.size(); ++mi) {
            const AQContactManifold &mf = impl->manifolds[mi];
            const std::uint32_t off = impl->manifoldRowOffset[mi];
            for (std::uint32_t pi = 0; pi < mf.pointCount; ++pi) {
                const AQContactPoint &cp = mf.points[pi];
                if (impl->debugFlags & AQDebugContactPoint) {
                    constexpr float e = 0.05f;
                    impl->debugLines.push_back(makeLine(cp.positionWorld - AQvec3(e, 0.f, 0.f),
                                                        cp.positionWorld + AQvec3(e, 0.f, 0.f),
                                                        1.f, 0.f, 0.f));
                    impl->debugLines.push_back(makeLine(cp.positionWorld - AQvec3(0.f, e, 0.f),
                                                        cp.positionWorld + AQvec3(0.f, e, 0.f),
                                                        0.f, 1.f, 0.f));
                    impl->debugLines.push_back(makeLine(cp.positionWorld - AQvec3(0.f, 0.f, e),
                                                        cp.positionWorld + AQvec3(0.f, 0.f, e),
                                                        0.f, 0.f, 1.f));
                }
                if (impl->debugFlags & AQDebugContactNormal) {
                    impl->debugLines.push_back(makeLine(
                        cp.positionWorld,
                        cp.positionWorld + mf.normalWorld * cp.depth,
                        1.f, 0.5f, 0.f));
                }
                if (impl->debugFlags & AQDebugContactImpulse) {
                    const float ln = impl->rows[off + pi * 3].accumImpulse;
                    impl->debugLines.push_back(makeLine(
                        cp.positionWorld,
                        cp.positionWorld + mf.normalWorld * ln,
                        0.f, 1.f, 1.f));
                }
            }
        }
    }
}

// ============================================================================
// Phase 4 — island detection (union-find) + per-island sleep/wake (§6.F/§6.G).
// Runs at the END of stepInternal on POST-solve velocities: a resting body
// whose gravity the contact solver just cancelled reads as idle, so the idle
// counter accumulates instead of being reset by the pre-solve gravity kick. The
// activation it sets is what the NEXT sub-step's velocity sweep and the
// integrator fast-path (AQStepBody*) consume.
// ============================================================================
void AQSpace::runIslandsAndSleep(float dt) {
    (void)dt;
    auto &bodies = impl->bodies;
    const std::uint32_t N = static_cast<std::uint32_t>(bodies.size());
    if (N == 0) return;

    // (1) Per-body idle counter on the post-solve velocity. Energy-flavored
    //     predicate (§6.G / §11.6): below BOTH the linear and angular thresholds
    //     (a non-zero per-body override wins over the space default). Sleeping
    //     bodies are idle by fiat; their counter is frozen (skipped here).
    for (std::uint32_t i = 0; i < N; ++i) {
        auto &bi = *bodies[i]->impl;
        if (bi.type != AQBodyType::Dynamic) continue;
        if (bi.s.activation == AQActivationState::Sleeping) continue;
        const float thrLin = bi.sleepLinearVel  > 0.f ? bi.sleepLinearVel  : impl->sleepLinearVel;
        const float thrAng = bi.sleepAngularVel > 0.f ? bi.sleepAngularVel : impl->sleepAngularVel;
        const float vlin = std::sqrt(OmegaGTE::dot(bi.s.velocity, bi.s.velocity));
        const float vang = std::sqrt(OmegaGTE::dot(bi.s.angularVelBody, bi.s.angularVelBody));
        if (vlin < thrLin && vang < thrAng) {
            if (bi.restingFrames != 0xFFFFFFFFu) ++bi.restingFrames;
        } else {
            bi.restingFrames = 0;
        }
    }

    // (2) Union-find over dynamic-dynamic constraint edges (contacts ∪ joints).
    //     Static / kinematic bodies are never unioned — they would merge every
    //     island that touches them (the §6.F gotcha). The row order is
    //     deterministic, so the roots are stable across runs.
    OmegaCommon::Vector<std::uint32_t> parent(N), rnk(N, 0u);
    for (std::uint32_t i = 0; i < N; ++i) parent[i] = i;
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](std::uint32_t a, std::uint32_t b) {
        std::uint32_t ra = find(a), rb = find(b);
        if (ra == rb) return;
        if (rnk[ra] < rnk[rb]) std::swap(ra, rb);
        parent[rb] = ra;
        if (rnk[ra] == rnk[rb]) ++rnk[ra];
    };
    auto isDyn = [&](std::uint32_t bi) { return bodies[bi]->impl->type == AQBodyType::Dynamic; };
    for (const AQConstraintRow &row : impl->rows) {
        if (isDyn(row.bodyA) && isDyn(row.bodyB)) unite(row.bodyA, row.bodyB);
    }
    for (std::uint32_t i = 0; i < N; ++i) bodies[i]->impl->islandId = find(i);

    // (3) Per-island collective decision. minResting = smallest idle counter
    //     among the island's dynamic members (a sleeping member counts as
    //     "infinitely idle"). The island is eligible to sleep iff EVERY member
    //     has been idle for at least `sleepIdleSubsteps`. The activation of every
    //     member is then set to the island target — so one moving member (a new
    //     contact, an external wakeUp) drops minResting and wakes the whole
    //     island. That is the §2 chain-with-one-sleeping-body fix.
    std::unordered_map<std::uint32_t, std::uint32_t> minResting;  // root -> min idle counter
    std::unordered_map<std::uint32_t, std::uint32_t> islandSize;  // root -> dynamic member count
    for (std::uint32_t i = 0; i < N; ++i) {
        auto &bi = *bodies[i]->impl;
        if (bi.type != AQBodyType::Dynamic) continue;
        const std::uint32_t rf = (bi.s.activation == AQActivationState::Sleeping)
                               ? 0xFFFFFFFFu : bi.restingFrames;
        auto it = minResting.find(bi.islandId);
        if (it == minResting.end()) minResting.emplace(bi.islandId, rf);
        else if (rf < it->second)   it->second = rf;
        ++islandSize[bi.islandId];
    }
    const std::uint32_t thr = impl->sleepIdleSubsteps;
    for (std::uint32_t i = 0; i < N; ++i) {
        auto &bi = *bodies[i]->impl;
        if (bi.type != AQBodyType::Dynamic) continue;
        const bool eligible = (thr > 0u) && (minResting[bi.islandId] >= thr);
        if (eligible) {
            if (bi.s.activation != AQActivationState::Sleeping) {
                bi.s.activation     = AQActivationState::Sleeping;
                bi.s.velocity       = AQvec3(0.f, 0.f, 0.f);
                bi.s.angularVelBody = AQvec3(0.f, 0.f, 0.f);
            }
        } else if (bi.s.activation == AQActivationState::Sleeping) {
            bi.s.activation = AQActivationState::Active;   // wake (idle counter preserved)
        }
    }

    // (4) Debug emissions (§9): island spokes colored by sleep state, sleeping-
    //     body markers, and the over-connection guard (a red origin tick if any
    //     island has grown implausibly large — the classic missed-static-body
    //     symptom, which the dynamic-only union above is designed to prevent).
    const std::uint32_t dbg = impl->debugFlags;
    if (dbg & (AQDebugIsland | AQDebugSleepingBody)) {
        for (std::uint32_t i = 0; i < N; ++i) {
            auto &bi = *bodies[i]->impl;
            if (bi.type != AQBodyType::Dynamic) continue;
            const FVec<3> com = bi.s.position + AQrotate(bi.s.orientation, bi.s.comOffset);
            const bool asleep = (bi.s.activation == AQActivationState::Sleeping);
            if (dbg & AQDebugIsland) {
                auto &rb = *bodies[bi.islandId]->impl;
                const FVec<3> rootCom = rb.s.position + AQrotate(rb.s.orientation, rb.s.comOffset);
                if (asleep) impl->debugLines.push_back(makeLine(com, rootCom, 0.4f, 0.4f, 0.4f));
                else        impl->debugLines.push_back(makeLine(com, rootCom, 0.f, 0.8f, 0.2f));
            }
            if ((dbg & AQDebugSleepingBody) && asleep) {
                constexpr float e = 0.1f;
                impl->debugLines.push_back(makeLine(com - AQvec3(e,0.f,0.f), com + AQvec3(e,0.f,0.f), 0.5f,0.5f,0.5f));
                impl->debugLines.push_back(makeLine(com - AQvec3(0.f,e,0.f), com + AQvec3(0.f,e,0.f), 0.5f,0.5f,0.5f));
                impl->debugLines.push_back(makeLine(com - AQvec3(0.f,0.f,e), com + AQvec3(0.f,0.f,e), 0.5f,0.5f,0.5f));
            }
        }
        if (dbg & AQDebugIsland) {
            constexpr std::uint32_t kIslandWarnSize = 1024u;   // §9 over-connection guard
            for (const auto &kv : islandSize) {
                if (kv.second > kIslandWarnSize) {
                    impl->debugLines.push_back(makeLine(AQvec3(0.f,0.f,0.f), AQvec3(0.f,1.f,0.f), 1.f,0.f,0.f));
                    break;
                }
            }
        }
    }
}
