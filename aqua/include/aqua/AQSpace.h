#ifndef AQUA_AQSPACE_H
#define AQUA_AQSPACE_H

// The simulation-space public surface. Holds the bodies AQContext advances each
// sub-step and owns the per-step debug stream. The body-side types (AQRigidBody
// / AQBodyDesc / AQBodyType) live in their own header so consumers that only
// touch bodies don't have to drag in the space surface; this header includes
// AQRigidBody.h so existing call sites that only #include AQSpace.h see the
// same names they always did.

#include "AQBase.h"
#include "AQCollision.h"
#include "AQContact.h"
#include "AQDebug.h"
#include "AQJoint.h"
#include "AQParticles.h"
#include "AQQuery.h"
#include "AQRigidBody.h"
#include "AQXPBD.h"
#include <omegaGTE/GTEMath.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/// The GPU compute backend (Phase 5/6h, internal pimpl — forward-declared so
/// this public header carries no OmegaGTE backend dependency).
struct AQComputeBackend;

/// The simulation space: holds bodies that AQContext advances each sub-step.
///
/// Spaces are created by `AQContext::createSpace` and owned by that context,
/// which also drives their stepping — see the timekeeping note on `AQContext`.
///
/// The simulation backend is intentionally hidden behind this pimpl, the same
/// way Omega kREATE hides OmegaGTE behind its own surface. Whether AQUA grows
/// its own solver or wraps a vendored physics SDK is an implementation decision
/// that must not change this public API.
class AQUA_EXPORT AQSpace {
public:
    ~AQSpace();

    void setGravity(const OmegaGTE::FVec<3> &g);
    OMEGA_NODISCARD OmegaGTE::FVec<3> gravity() const;

    /// Adds a body and returns a handle owned by this space.
    SharedHandle<AQRigidBody> addBody(const AQBodyDesc &desc);

    /// Removes a body. Returns false if it was not in this space.
    bool removeBody(const SharedHandle<AQRigidBody> &body);

    /// Number of bodies currently in the space.
    OMEGA_NODISCARD std::size_t bodyCount() const;

    // --- drainable debug surface (Phase 1.1 §6.5) ---
    // AQUA emits structured `AQDebugLine` records each step according to the
    // current flag set; the consumer (kREATE adapter, a test) drains the buffer
    // and the space clears it. Pull model — matches the kREATE debug-draw plan
    // §7 adapter, deterministic, and ports to a GPU append-buffer later. Off
    // by default (AQDebugNone), so the surface is zero-cost when unused.
    /// Sets which debug primitives the space emits each step. OR of `AQDebugFlags`.
    void setDebugFlags(std::uint32_t flags);
    OMEGA_NODISCARD std::uint32_t debugFlags() const;
    /// Moves the accumulated debug lines out of the space and clears its buffer.
    OMEGA_NODISCARD OmegaCommon::Vector<AQDebugLine> drainDebugLines();

    // --- shape factories (Phase 2) ---
    // Shapes are owned and instanced by the space (§11.3) and referenced by
    // handle from descriptors. AQShape stays out of the call site; these
    // mirror GTE's named-ctor idiom. Returning an invalid handle (generation
    // 0) signals a malformed shape (e.g. zero radius, empty hull).
    AQShapeHandle createSphereShape(float radius);
    AQShapeHandle createBoxShape(const OmegaGTE::FVec<3> &halfExtents);
    AQShapeHandle createCapsuleShape(float radius, float halfHeight);
    AQShapeHandle createPlaneShape(const OmegaGTE::FVec<3> &normal, float offset);
    AQShapeHandle createConvexHullShape(const OmegaGTE::FVec<3> *pts, std::size_t n);

    /// Current ordered, de-duplicated broadphase candidate pairs (Phase 2 §10).
    /// Indices are stable for the lifetime of the space (the body's slot in
    /// the space's body-SoA array). Updated once per `AQContext::advance`.
    OMEGA_NODISCARD OmegaCommon::Vector<AQBroadphasePair> candidatePairs() const;

    // --- material combine + solver (Phase 3) ---
    /// Per-space restitution and friction combine rules. Default is Average
    /// for both (the PhysX default; the most physically-defensible
    /// isotropic-material policy). See `AQMaterialCombine`.
    void setMaterialCombine(AQMaterialCombine restCombine,
                            AQMaterialCombine fricCombine);
    OMEGA_NODISCARD AQMaterialCombine restitutionCombine() const;
    OMEGA_NODISCARD AQMaterialCombine frictionCombine() const;

    /// Sequential-impulse PGS sweep counts (Phase-3 §11.4). Defaults: 8
    /// velocity, 4 position. `positionIters == 0` disables the split-impulse
    /// position-correction pass entirely — useful for the energy-non-growth
    /// test and for scenes that don't need penetration recovery.
    void setSolverIterations(int velocityIters, int positionIters);
    OMEGA_NODISCARD int velocityIterations() const;
    OMEGA_NODISCARD int positionIterations() const;

    /// Read-only manifold view, refreshed by the most recent sub-step. Stable
    /// for the duration of one `AQContext::advance` call between the final
    /// sub-step and the next `advance`. Useful for the Phase 4 joint wiring
    /// and for debug overlays — the value-type copy carries body indices, not
    /// pointers, so it is safe to hold across `advance` boundaries.
    OMEGA_NODISCARD OmegaCommon::Vector<AQContactManifold> contactManifolds() const;

    // ========================================================================
    // Phase 4 — joints, queries, triggers, sleep tuning (§10 public API).
    // ========================================================================

    // --- joints ---
    // Named-ctor idiom matching `createSphereShape` etc. Each returns a handle
    // into the space's joint table. A joint between two infinite-mass bodies
    // (static/static, static/kinematic, …) returns an invalid handle — it would
    // be a no-op. Anchors are in each body's LOCAL frame; axes are body-A local.
    AQJointHandle createDistanceJoint(const SharedHandle<AQRigidBody> &a,
                                      const SharedHandle<AQRigidBody> &b,
                                      const OmegaGTE::FVec<3> &anchorALocal,
                                      const OmegaGTE::FVec<3> &anchorBLocal,
                                      float length);
    AQJointHandle createBallSocketJoint(const SharedHandle<AQRigidBody> &a,
                                        const SharedHandle<AQRigidBody> &b,
                                        const OmegaGTE::FVec<3> &anchorALocal,
                                        const OmegaGTE::FVec<3> &anchorBLocal);
    AQJointHandle createHingeJoint(const SharedHandle<AQRigidBody> &a,
                                   const SharedHandle<AQRigidBody> &b,
                                   const OmegaGTE::FVec<3> &anchorALocal,
                                   const OmegaGTE::FVec<3> &anchorBLocal,
                                   const OmegaGTE::FVec<3> &axisALocal,
                                   const AQJointAxisLimit &limit = AQJointAxisLimit{});
    AQJointHandle createSliderJoint(const SharedHandle<AQRigidBody> &a,
                                    const SharedHandle<AQRigidBody> &b,
                                    const OmegaGTE::FVec<3> &anchorALocal,
                                    const OmegaGTE::FVec<3> &anchorBLocal,
                                    const OmegaGTE::FVec<3> &axisALocal,
                                    const AQJointAxisLimit &limit = AQJointAxisLimit{});
    AQJointHandle createFixedJoint(const SharedHandle<AQRigidBody> &a,
                                   const SharedHandle<AQRigidBody> &b);

    void setJointSoftness(AQJointHandle h, AQJointSoftness s);
    bool destroyJoint(AQJointHandle h);

    /// Read-only joint view, refreshed per `advance`. Value-type copies carry
    /// body indices, not pointers — safe to hold across `advance` boundaries.
    OMEGA_NODISCARD OmegaCommon::Vector<AQJointDesc> joints() const;

    /// World-frame linear impulse this joint applied during the most recent
    /// sub-step (zero vector for an invalid handle). Divide by the sub-step `dt`
    /// for the reaction force the joint exerts — the bridge deliverable reads
    /// this for its catenary-tension / support-force oracle.
    OMEGA_NODISCARD OmegaGTE::FVec<3> jointImpulse(AQJointHandle h) const;

    // --- queries (valid between `advance` calls; stale during one) ---
    // `hits` is cleared then appended; results are sorted by (fraction, body).
    // The grid walked is the same per-step structure the broadphase builds.
    void raycast(const OmegaGTE::FVec<3> &origin,
                 const OmegaGTE::FVec<3> &direction,
                 float maxT,
                 const AQQueryFilter &filter,
                 OmegaCommon::Vector<AQRaycastHit> &hits) const;
    void shapecast(AQShapeHandle shape,
                   const OmegaGTE::FVec<3> &origin,
                   const OmegaGTE::FQuaternion &orientation,
                   const OmegaGTE::FVec<3> &direction,
                   float maxT,
                   const AQQueryFilter &filter,
                   OmegaCommon::Vector<AQRaycastHit> &hits) const;
    void overlap(AQShapeHandle shape,
                 const OmegaGTE::FVec<3> &origin,
                 const OmegaGTE::FQuaternion &orientation,
                 const AQQueryFilter &filter,
                 bool exactShapes,
                 OmegaCommon::Vector<std::uint32_t> &bodies) const;

    // --- triggers ---
    /// Drains the per-`advance` trigger-event queue; subsequent calls until the
    /// next advance return empty. Events are ordered by `(a, b)` ascending.
    OMEGA_NODISCARD OmegaCommon::Vector<AQTriggerEvent> triggerEvents();

    // --- sleep tuning ---
    /// Space-wide sleep thresholds. Defaults: 0.01 m/s linear, 0.01 rad/s
    /// angular, 60 idle sub-steps (~0.5 s @ 1/120). Non-zero per-body overrides
    /// on `AQBodyDesc` take precedence. Negative inputs are clamped to 0.
    void setSleepThresholds(float linearVel, float angularVel,
                            std::uint32_t idleSubsteps);

    // --- Particle systems (Phase 6, §13.3) ----------------------------------
    // Fixed-capacity particle systems simulated on the CPU path each advance.
    // Handles are opaque IDs (never pointers), resolved behind the pimpl so a
    // caller can never dangle into a pool that compaction relocated.

    /// Creates a particle system backed by a fixed-capacity pool. Capacity is
    /// bounded at creation (§2 — the guards fire before it is exceeded).
    AQParticleSystemHandle createParticleSystem(std::uint32_t capacity);
    /// Destroys a particle system. No-op on an unknown/stale handle.
    void destroyParticleSystem(AQParticleSystemHandle system);

    /// Attaches an emitter (copied by value — AQEmitter is POD).
    void addEmitter(AQParticleSystemHandle system, const AQEmitter &emitter);
    /// Enables/disables emitter `idx` on the system. Out-of-range is a no-op.
    void setEmitterEnabled(AQParticleSystemHandle system, std::uint32_t idx, bool on);

    /// Adds a force field (gravity/drag/wind/vortex/point; copied by value).
    void addForceField(AQParticleSystemHandle system, const AQForceField &field);
    /// Enables/disables force field `idx` on the system. Out-of-range is a no-op.
    void setForceFieldEnabled(AQParticleSystemHandle system, std::uint32_t idx, bool on);

    /// Toggles one-way collision of the system's particles against the static
    /// AQShapes registered in this space (Phase 2 colliders). Off by default.
    void setParticleCollisionEnabled(AQParticleSystemHandle system, bool on);

    /// Snapshots the packed live particle state into caller-owned SoA buffers for
    /// rendering (kREATE). Fills [0, returned) with the live set and returns the
    /// count written; any out array may be null to skip that attribute. Valid
    /// after `AQContext::advance` returns — the live set is packed into the
    /// prefix by the end-of-frame compaction.
    OMEGA_NODISCARD std::uint32_t readParticleState(AQParticleSystemHandle system,
                                       OmegaGTE::FVec<3> *outPositions,
                                       OmegaGTE::FVec<3> *outVelocities,
                                       float *outLifetimes,
                                       std::uint32_t *outFlags,
                                       std::uint32_t maxCount) const;
    /// Number of live particles in the system (0 on an unknown handle).
    OMEGA_NODISCARD std::uint32_t liveParticleCount(AQParticleSystemHandle system) const;

    // --- XPBD constraint core (Phase 7, §10/§13) -----------------------------
    // An XPBD body is a persistent particle set + typed constraints projected
    // by the shared XPBD solver each sub-step. Handles are opaque ids resolved
    // behind the pimpl (the particle-system idiom). This is the authoring
    // surface Phases 8–10 extend with their own constraint types; the solver
    // core they share lands here.

    /// Creates an XPBD body from parallel position/invMass arrays
    /// (`invMass[i] == 0` pins particle i). Returns an invalid handle for an
    /// empty/malformed desc (null arrays, zero count).
    AQXPBDBodyHandle createXPBDBody(const AQXPBDBodyDesc &desc);
    /// Destroys an XPBD body. No-op on an unknown/stale handle.
    void destroyXPBDBody(AQXPBDBodyHandle body);

    /// Authors a distance constraint between two particles of `body`.
    /// `restLength < 0` derives the rest length from the current particle
    /// distance. Returns an invalid handle on a bad body/indices/self-pair.
    AQConstraintHandle addDistanceConstraint(AQXPBDBodyHandle body,
                                             std::uint32_t a, std::uint32_t b,
                                             float restLength, float compliance);

    /// Convenience: chains `count` particles with `count − 1` distance
    /// constraints at the given compliance, rest lengths taken from the
    /// current particle spacing (the rope deliverable). Returns the number of
    /// constraints created (0 on a bad body / fewer than two particles).
    std::uint32_t addRope(AQXPBDBodyHandle body,
                          const std::uint32_t *particles, std::uint32_t count,
                          float compliance);

    /// Per-space XPBD solve tuning (substeps × iterations, damping, guard).
    /// Values are sanitized: substeps/iterations clamp to ≥ 1, damping to
    /// [0, 1), explosionThreshold to > 0.
    void setXPBDParams(const AQXPBDParams &params);
    OMEGA_NODISCARD AQXPBDParams xpbdParams() const;

    /// Snapshots particle state into caller-owned arrays (either may be null
    /// to skip). Returns the count written (min(particleCount, maxCount); 0 on
    /// an unknown handle). Valid between `AQContext::advance` calls.
    OMEGA_NODISCARD std::uint32_t readXPBDState(AQXPBDBodyHandle body,
                                   OmegaGTE::FVec<3> *outPositions,
                                   OmegaGTE::FVec<3> *outVelocities,
                                   std::uint32_t maxCount) const;

    /// Snapshots the color-sorted constraint records (with live λ and color —
    /// the solver's own view, one contiguous range per color batch) and the
    /// batch table. Any out array may be null. Returns the constraint count
    /// written. Re-colors first if the topology changed since the last step.
    OMEGA_NODISCARD std::uint32_t readXPBDConstraints(AQXPBDBodyHandle body,
                                   AQDistanceConstraint *outConstraints,
                                   std::uint32_t maxConstraints,
                                   OmegaCommon::Vector<AQConstraintBatch> *outBatches) const;

    /// Cumulative explosion-guard trips on this body (0 on an unknown handle).
    /// A non-zero value means the solve diverged and was clamped — loud by
    /// contract (§9.7), so tests can assert both "tripped" and "never trips".
    OMEGA_NODISCARD std::uint32_t xpbdGuardTrips(AQXPBDBodyHandle body) const;

    /// Enables two-way contact coupling of this XPBD body's particles against
    /// the space's rigid colliders (Phase 7g, §13.6). Off by default. Particles
    /// are pushed out of penetrating colliders and the equal-and-opposite
    /// reaction impulse is applied back to dynamic rigid bodies; static /
    /// kinematic colliders behave as the one-way push-out. `particleRadius` is
    /// the contact thickness (0 ⇒ point particle); `friction` is the Coulomb μ
    /// against colliders (0 ⇒ frictionless). A collision-enabled body solves on
    /// the CPU even when the context resolved the GPU path (the coupling reads
    /// rigid poses and writes rigid impulses, both CPU-resident).
    void setXPBDCollisionEnabled(AQXPBDBodyHandle body, bool on,
                                 float particleRadius = 0.f, float friction = 0.f);

    /// Enables long-range attachments on this body (Phase 7h #1, §13.7): each
    /// dynamic particle gains a unilateral max-distance constraint to its
    /// nearest pinned particle (geodesic sum of rest lengths), converging a long
    /// pinned chain toward inextensibility in one iteration. Off by default —
    /// it is for INEXTENSIBLE pinned ropes/cloth; it would wrongly clamp a
    /// compliant (α > 0) chain. No-op on a body with no pinned particles.
    void setXPBDLongRangeAttachment(AQXPBDBodyHandle body, bool on);

private:
    AQSpace();

    /// Advances this space by one fixed sub-step of `dt` seconds. Driven by
    /// AQContext::advance; deliberately not public so all timekeeping lives in
    /// the context rather than being duplicated per call site.
    void stepInternal(float dt);

    /// Re-arms the per-body debug fast-spin warning. Called by AQContext when
    /// the fixed sub-step changes; a no-op effect in release builds.
    void resetStepWarnings();

    /// Refreshes per-body fattened world AABBs and runs the sort-based-grid
    /// broadphase (Phase 2 §6.B), once per `AQContext::advance` tick. `frameDt`
    /// is the real frame time the context received — used by the velocity-
    /// proportional fattening (§11.4). Drives the new AQDebugAABB /
    /// AQDebugBroadphasePair / AQDebugBroadphaseGuard emissions.
    void runBroadphase(float frameDt);

    /// Phase 3 narrowphase + contact solver. Consumes the current candidate
    /// pair list, builds manifolds via the specialized + GJK/EPA branch
    /// table, runs the sequential-impulse PGS velocity sweep with Coulomb
    /// friction, and applies split-impulse position correction. Called by
    /// `AQSpace::stepInternal` between the velocity half-step and the
    /// position half-step of `AQStepBody*` (Phase-3 brief §6, §10).
    void runNarrowphaseAndSolve(float dt);

    /// Phase 4 — island detection + per-island sleep/wake. Runs once per
    /// sub-step at the end of `stepInternal` (on the POST-solve velocities, so
    /// a resting body whose gravity the solver just cancelled reads as idle).
    /// Union-find over the (contact ∪ joint) edges in the row buffer (statics /
    /// kinematics excluded), an energy-flavored per-body idle counter, and a
    /// collective per-island activation flip. Sets the activation the NEXT
    /// sub-step's velocity sweep and integrator fast-path read.
    void runIslandsAndSleep(float dt);

    /// Phase 4 — build the constraint rows for every live joint into the shared
    /// row buffer (after the contact rows), warm-starting each from its joint
    /// record and recording the row span for the post-sweep write-back. Called
    /// by `runNarrowphaseAndSolve` between the contact-row build and the sweep.
    void buildJointRows(float dt);

    /// Phase 4 — diff this advance's trigger overlaps against the previous
    /// advance's set, producing Enter/Stay/Exit events. Called once per advance
    /// by AQContext (after the post-step broadphase refresh), NOT per sub-step,
    /// so a steady overlap yields one Enter, then one Stay per advance, then one
    /// Exit (the §9 event-count contract).
    void updateTriggers();

    /// Phase 4 — opt-in continuous collision (§6.K). After the position
    /// half-step, sweep each CCD body's bounding sphere from its pre-step
    /// position to where it landed; if it would have passed through another
    /// shape this sub-step, snap it back to the time of impact and cancel the
    /// into-surface velocity, so a fast/thin body cannot tunnel. Reads the
    /// pre-step positions captured in `impl->ccdPrevPos`.
    void runCCD(float dt);

    /// Phase 6 — particle stepping, driven by AQContext::advance on the same
    /// fixed-step cadence as the rigid pillar. `particlesEmit` runs once per
    /// advance (gathers the space's static colliders, then seeds new particles
    /// for the simulated slice `simDt`; `substepDt` is the fixed sub-step the
    /// integer death countdown is derived from — §14.2/6f); `particlesSubstep`
    /// runs each sub-step (field accumulate + integrate + one-way collide +
    /// age); `particlesCompact` runs once per advance (stable stream
    /// compaction + free-list rebuild).
    void particlesEmit(float simDt, float substepDt);
    void particlesSubstep(float dt);
    void particlesCompact();

    /// Phase 6h — the live GPU particle path (§14). When the context resolves
    /// the GPU path, the per-sub-step HOST work shrinks to the deterministic
    /// integer bookkeeping (`particlesAgeSubstep`: countdown decrement +
    /// display lifetime — the float physics runs on the device), and
    /// `particlesGpuFrame` encodes the whole advance's device work once per
    /// advance (staged-emission inject → sub-steps × (integrate [+ collide] +
    /// age) → scan/scatter compaction), before the host-side compaction
    /// mirrors the same permutation. `detachCompute` clears the space's
    /// non-owning backend pointer — called by the owning context's destructor
    /// so a space handle that outlives it never dangles.
    void particlesAgeSubstep(float dt);
    void particlesGpuFrame(AQComputeBackend *backend, float dt, int substeps);
    void detachCompute();

    /// Phase 7 — XPBD stepping, driven by AQContext::advance on the same fixed
    /// sub-step cadence as the rigid and particle pillars (one clock, §13.1).
    /// `xpbdSubstep` runs each sub-step: n = params.substeps XPBD slices of
    /// h = dt/n, each predict → colored projection → [long-range clamp] →
    /// [rigid contact coupling] → derive-velocity. `xpbdFrameEnd` runs once per
    /// advance: debug-bus emission (AQDebugConstraint / AQDebugConstraintColor)
    /// + guard-trip reporting. `gpuActive` is set when the context resolved the
    /// GPU path: xpbdSubstep then advances ONLY collision-enabled bodies on the
    /// CPU (their two-way coupling needs CPU-resident rigid state), leaving the
    /// rest to xpbdGpuFrame's device batch (§13.6).
    void xpbdSubstep(float dt, bool gpuActive);
    void xpbdFrameEnd();

    /// Phase 7f — the live GPU XPBD path (post-6h flip). Encodes the whole
    /// advance-frame's slices for every body once per advance (uploads on
    /// topology change only, buffers resident across frames); xpbdFrameEnd
    /// then downloads the guard-trip counters and, when debug flags demand
    /// positions, the particle state.
    void xpbdGpuFrame(AQComputeBackend *backend, float dt, int substeps);

    friend class AQContext;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AQUA_AQSPACE_H
