#ifndef AQUA_SRC_AQSPACEIMPL_H
#define AQUA_SRC_AQSPACEIMPL_H

// Private (src-only) definition of AQSpace::Impl and the two space-owned record
// types it holds (AQManifoldCacheEntry, AQJointRecord). Extracted out of
// AQSpace.cpp so a second translation unit — AQSpaceParticles.cpp (Phase 6) —
// can operate on the same Impl layout without the whole 100k-line core moving
// with it. There is exactly ONE definition of Impl; both TUs include this header
// so they agree on its layout (ODR). Nothing here is part of the public API —
// no backend types, no new public surface; it is the same pimpl the public
// AQSpace has always hidden, now visible to AQUA's own sources only.
//
// Self-contained: AQSpace.h pulls the public type headers (AQCollision /
// AQContact / AQJoint / AQQuery / AQRigidBody / AQDebug / GTEMath); AQJointBuild.h
// supplies AQJointRest + kAQMaxJointRows. The `using OmegaGTE::FVec;` mirrors the
// one AQSpace.cpp already declares, so the moved structs keep their bare
// `FVec<3>` spelling.

#include <aqua/AQSpace.h>
#include <aqua/AQParticles.h>
#include "AQJointBuild.h"
#include <unordered_map>
#include <utility>
#include <cstdint>

struct AQComputeBackend;   // Phase 6h — non-owning live-GPU-path pointer below

using OmegaGTE::FVec;

// ============================================================================
// Phase 6 — internal particle-system state (§13.3 6b).
// ============================================================================
//
// SoA host backing for one particle system plus its free-list allocator and the
// stable compaction that recycles dead slots. AQUA-private: the public caller
// sees only the opaque AQParticleSystemHandle; this is what the pimpl resolves
// it to. One index == one particle across ALL the SoA arrays.
//
// Determinism (the §2/§8 hard part) is a property of two disciplines here:
//   * the free-list is kept DESCENDING, so pop_back() hands out the SMALLEST
//     free index first — the same slot recycles the same way on any path;
//   * compaction is STABLE — survivors keep their relative order via an in-place
//     stream compaction, never an order-dependent atomic append.
//
// Census accounting, exact at all times:
//   liveCount            = number of ALIVE particles (== capacity − freeCount
//                          − deadPending)
//   freeCount()          = slots available to allocate (freeList.size())
//   deadPending()        = capacity − liveCount − freeCount = slots marked DEAD
//                          but not yet reclaimed; ZERO right after compact()
// so `liveCount + freeCount() == capacity` holds exactly after every compact().

// A static collider snapshot the particle push-out reads (Pass D). Gathered once
// per advance from the space's bodies-with-shapes: the shape, its body world
// transform (pose origin + orientation, the same convention AQshapeAABB uses),
// and the body restitution the reflection damps with. One-way — the collider is
// read, never written (two-way momentum transfer is Phase 9).
struct AQParticleCollider {
    AQShape            shape;
    AQTransform<float> xform;
    float              restitution = 0.f;
};

struct AQParticleSystem {
    std::uint64_t id       = 0;   ///< matches AQParticleSystemHandle.id (never 0 when live)
    std::uint32_t capacity = 0;   ///< fixed backing size (§10 — bounded at creation)

    // SoA backing, each sized to `capacity`.
    OmegaCommon::Vector<FVec<3>>       positions;
    OmegaCommon::Vector<FVec<3>>       velocities;
    OmegaCommon::Vector<FVec<3>>       accels;      ///< per-step scratch
    OmegaCommon::Vector<float>         invMass;
    // Death is an INTEGER sub-step countdown (§14.2/6f), not an fp threshold:
    // computed once at emission in double (`ceil(sampledLifetime / substepDt)`
    // from the same RNG draw on every path) and decremented by one per age()
    // call. Integer death is exact on any hardware — CPU, the double oracle,
    // and the GPU age kernel share the identical schedule without needing
    // device fp64 (which is probed, not guaranteed). This replaces the 6e
    // double-lifetime carry. `lifetime` (float) remains for READBACK DISPLAY
    // only — aged for presentation, never consulted for death. The countdown
    // is fixed at emission, so changing the fixed timestep mid-flight rescales
    // a live particle's remaining wall-clock life (documented edge; the
    // display lifetime keeps honest seconds).
    OmegaCommon::Vector<std::uint32_t> deathCountdown; ///< age() calls until death (≥1 while alive)
    OmegaCommon::Vector<float>         lifetime;    ///< remaining seconds (display only)
    OmegaCommon::Vector<float>         radius;
    OmegaCommon::Vector<std::uint32_t> flags;       ///< AQParticleAlive / AQParticleDead | user bits

    // Free-list: currently-free slot indices, held DESCENDING so pop_back()
    // yields the smallest index first (deterministic reuse — §8).
    OmegaCommon::Vector<std::uint32_t> freeList;
    std::uint32_t liveCount = 0;   ///< census: # ALIVE particles

    // Config — 6c (emission) / 6d (collision) populate these.
    OmegaCommon::Vector<AQEmitter>    emitters;
    OmegaCommon::Vector<AQForceField> fields;
    std::uint32_t collisionEnabled = 0;

    // Per-emitter emission bookkeeping (parallel to `emitters`, sized lazily).
    // `carry` is the fractional-particle accumulator, DOUBLE on every path so
    // the emitted count is bit-identical cross-path (§9); `ordinal` is the
    // monotonic per-emitter emission count that seeds each particle's RNG.
    OmegaCommon::Vector<double>        emitterCarry;
    OmegaCommon::Vector<std::uint64_t> emitterOrdinal;

    // Phase 6h — GPU-path bookkeeping. Because the pool is compacted to the
    // live prefix at every frame end, this frame's emission is always the
    // contiguous tail [emitBase, emitBase + emittedThisFrame) — the slice the
    // staging upload reads straight out of the host arrays. `gpuResident`
    // marks a created device pool; `gpuDirty` marks the host float mirror
    // (positions/velocities) stale vs the device — the readback sync point
    // (readParticleState / debug draw) downloads and clears it (§14.1).
    std::uint32_t emitBase = 0;
    std::uint32_t emittedThisFrame = 0;
    // The emitted slice's AT-EMISSION death countdowns, staged for the GPU
    // inject. The device replays the whole frame from its start (inject →
    // sub-steps), but by the time the frame encodes, the host's age() calls
    // have already decremented — or zeroed — this frame's countdowns, so the
    // pre-age values must be kept aside. Positions/velocities need no copy:
    // the host never touches them on the GPU path after emit.
    OmegaCommon::Vector<std::uint32_t> emitDeathStage;
    bool gpuResident = false;
    bool gpuDirty = false;

    std::uint32_t freeCount()   const { return static_cast<std::uint32_t>(freeList.size()); }
    std::uint32_t deadPending() const { return capacity - liveCount - freeCount(); }

    // Size the SoA to `cap`, mark every slot DEAD, and fill the free-list with
    // all `cap` slots (descending). liveCount = 0. Idempotent re-init.
    void reset(std::uint32_t cap);

    // Pop up to `k` free slots (smallest index first), mark them ALIVE, and
    // append them ascending to `outSlots`. Returns the count actually taken —
    // less than `k` only when the pool is near-exhausted (§9 saturation). Does
    // NOT write particle attributes (that is emission, 6c) — only reserves.
    std::uint32_t allocate(std::uint32_t k, OmegaCommon::Vector<std::uint32_t>& outSlots);

    // Mark one ALIVE slot DEAD (§6 Pass E). No-op if already dead / out of
    // range. The slot is reclaimed to the free-list at the next compact(), not
    // here — so the mid-frame free-list stays stable for that frame's emission.
    void kill(std::uint32_t slot);

    // Stable stream compaction (§6 Pass F): pack the ALIVE particles into the
    // prefix [0, liveCount) preserving their relative order, mark the tail DEAD,
    // and rebuild the free-list as the vacated tail (descending). After this,
    // deadPending() == 0 and liveCount + freeCount() == capacity.
    void compact();

    // --- Per-sub-step passes (§13.3 6c, death model revised by 6f) ---
    // emit() is Pass A (once per advance-frame): seeded, count-deterministic
    // spawn of Reeves attributes into free slots; `substepDt` is the fixed
    // sub-step the frame will run, from which the integer deathCountdown is
    // derived (in double, identically on every path). accumulateAndIntegrate()
    // is Pass B + C (per sub-step): sum field accelerations, semi-implicit
    // Euler. age() is Pass E (per sub-step): decrement the countdown, kill at
    // zero; `dt` only ages the display lifetime.
    void emit(float dt, float substepDt);
    void accumulateAndIntegrate(float dt);
    void age(float dt);

    // Pass D (§6, per sub-step): one-way push-out of ALIVE particles against the
    // static colliders. A hit is `signedDistance < radius`; the particle is
    // pushed out along the surface normal and its into-surface velocity is
    // reflected/damped by the collider restitution. `hullVerts` is the space's
    // vertex pool (unused — hull collision is deferred, its SDF returns +inf).
    void collide(const OmegaCommon::Vector<AQParticleCollider>& colliders,
                 const OmegaGTE::FVec<3>* hullVerts, std::size_t hullVertCount);

    // Guards (§9). anyNonFinite scans ALIVE particles for a NaN/Inf position or
    // velocity (the classic silent corruptor); partitionOK verifies the slots
    // partition exactly — every index in [0,capacity) is either ALIVE or on the
    // free-list, with no slot appearing twice (no leak, no double-free).
    bool anyNonFinite() const;
    bool partitionOK() const;
};



// ============================================================================
// Phase 7 — internal XPBD body state (§13 7b/7c).
// ============================================================================
//
// A PERSISTENT particle set + the typed constraint arrays over it, plus the
// deterministic graph-coloring that makes parallel projection conflict-free.
// Deliberately NOT an emitter-driven AQParticleSystem: those particles expire
// and compaction relocates their slots, which would corrupt constraint indices.
// XPBD particles keep stable indices for the body's lifetime (§13.1).
//
// Determinism (brief §5/§8): the coloring is a pure function of authoring
// order (greedy, smallest-free-color, constraints visited in authoring order),
// and the solve visits colors in ascending order, constraints within a color
// in ascending authoring order — the SAME fixed order on every path. The
// color-sorted mirror `distanceSorted` (+ `sortedAuthoring` mapping each
// sorted slot back to its authoring index) is rebuilt only on topology change,
// never per frame.

// --- Phase 7g — rigid↔XPBD contact coupling records (§13.6) ---
//
// A rigid collider snapshot the XPBD contact coupling reads each engine
// sub-step. Gathered from the space's bodies-with-shapes via the PUBLIC
// AQRigidBody accessors — the same convention AQParticleCollider follows,
// extended with the mass/inertia/body-index the TWO-WAY reaction needs. The
// collider is read for the push-out and written back (as a reaction impulse)
// only through the body index, never here.
struct AQXPBDCollider {
    AQShape            shape;
    AQTransform<float> xform;                          // body world pose (p + q)
    AQMat3F            worldInvInertia = AQMat3F::Create(); // for the body term w_b
    FVec<3>            com             = FVec<3>::Create(); // world COM (impulse arm)
    // Contact-point velocity is v = vel + omega × r. Held per-collider and
    // EVOLVED within a body's advance() as reactions land, so the velocity-level
    // impulse self-limits across the sub-step's slices (a frozen collider pose
    // would otherwise re-apply the full stopping impulse every slice — §13.6).
    FVec<3>            vel   = FVec<3>::Create();
    FVec<3>            omega = FVec<3>::Create();       // world angular velocity
    float              invMassBody = 0.f;
    float              restitution = 0.f;
    std::uint32_t      bodyIndex   = 0;                // into AQSpace::Impl::bodies
    bool               dynamic     = false;            // mass > 0 ⇒ receives reaction
};

// One reaction impulse an XPBD body applied to a rigid collider this engine
// sub-step. Buffered on the body during advance() in a deterministic fixed
// order (body → particle → collider) and drained by xpbdSubstep through
// AQRigidBody::applyImpulseAtPoint — the same impulse path the PGS sweep uses,
// so the COM offset the descriptor may set is handled correctly (not snapshot).
struct AQXPBDReaction {
    std::uint32_t bodyIndex = 0;
    FVec<3>       impulse   = FVec<3>::Create();
    FVec<3>       point     = FVec<3>::Create();
    // Split-impulse position correction (§13.6): a pseudo-position push that
    // removes the body's penetration share WITHOUT adding velocity, so a body
    // resting on a particle bed holds continuous contact instead of being
    // bounced off (a velocity bias) or creeping through (no recovery). Applied
    // as a MAX per body in xpbdSubstep, never summed (many contacts, one push).
    FVec<3>       posCorrection = FVec<3>::Create();
};

// A long-range attachment (§13.7 7h #1; Kim/Chentanez/Müller 2012): dynamic
// particle `p` may be no farther than `maxDist` (the geodesic sum of rest
// lengths along the constraint graph) from pinned particle `pin`. A unilateral
// inextensibility constraint that converges a long pinned chain in one
// iteration, attacking the ~(N²/2)·g·h² 1-iteration residual quantified in 7e.
struct AQLongRangeAttachment {
    std::uint32_t p       = 0;
    std::uint32_t pin     = 0;
    float         maxDist = 0.f;
};

struct AQXPBDBody {
    std::uint64_t id = 0;   ///< matches AQXPBDBodyHandle.id (never 0 when live)

    // Particle SoA — stable indices, sized once per addParticles batch.
    OmegaCommon::Vector<FVec<3>> positions;
    OmegaCommon::Vector<FVec<3>> prevPositions;
    OmegaCommon::Vector<FVec<3>> velocities;
    OmegaCommon::Vector<float>   invMass;      ///< 0 ⇒ pinned (skipped everywhere)

    // Typed constraint storage. `distance` is AUTHORING order — the stable
    // index space AQConstraintHandle exposes. `distanceSorted` is the solver's
    // color-sorted mirror (contiguous per color, coalesced-read shape for the
    // GPU port); `batches` indexes its color ranges. λ lives in the sorted
    // records (pure solver state); compliance/rest edits go to the authoring
    // records and re-sync on recolor().
    OmegaCommon::Vector<AQDistanceConstraint> distance;
    OmegaCommon::Vector<AQDistanceConstraint> distanceSorted;
    OmegaCommon::Vector<std::uint32_t>        sortedAuthoring; ///< sorted slot → authoring index
    OmegaCommon::Vector<AQConstraintBatch>    batches;
    bool colorsDirty = true;

    // Explosion-guard bookkeeping (§6 loud guard): cumulative trip count for
    // tests/telemetry, the per-frame set of tripped sorted-slots for the debug
    // bus, and the once-per-body stderr latch (the Phase 1 fast-spin idiom —
    // loud once, not 10k lines a second).
    std::uint32_t                      guardTrips = 0;
    OmegaCommon::Vector<std::uint32_t> trippedThisFrame;
    bool                               guardWarned = false;

    // Phase 7f live-GPU bookkeeping (the 6h idiom). `gpuResident` marks a
    // created device pool; `gpuDirty` marks the host float mirror stale vs
    // the device (readXPBDState / debug draw download + clear it);
    // `gpuUploadNeeded` forces a particle re-upload after addParticles (a
    // constraint-topology change is tracked by colorsDirty). `gpuTripsPrev`
    // is the last-seen per-constraint trip snapshot, so xpbdFrameEnd can turn
    // the device's cumulative counters into this-frame deltas for guardTrips
    // and the flat-red AQDebugConstraint flagging.
    bool gpuResident = false;
    bool gpuDirty = false;
    bool gpuUploadNeeded = false;
    OmegaCommon::Vector<std::uint32_t> gpuTripsPrev;

    // --- Phase 7g — rigid↔XPBD contact coupling (§13.6). Opt-in per body so
    // every existing oracle is untouched (default off). `particleRadius` is the
    // contact thickness (0 ⇒ point particle); `friction` is the Coulomb μ vs
    // colliders (0 ⇒ frictionless, the Phase 6 particle-path default).
    // `reactionsThisStep` buffers the two-way impulses advance() applies to the
    // particles' own positions, for xpbdSubstep to hand back to the rigid bodies.
    bool  collisionEnabled = false;
    float particleRadius   = 0.f;
    float friction         = 0.f;
    OmegaCommon::Vector<AQXPBDReaction> reactionsThisStep;

    // --- Phase 7h #1 — long-range attachments (§13.7). Opt-in (default off — it
    // would wrongly clamp a COMPLIANT chain; it is for inextensible pinned
    // ropes/cloth). `lra` is the geodesic pin/maxDist per dynamic particle,
    // rebuilt by recolor() on topology change.
    bool  longRangeAttach = false;
    OmegaCommon::Vector<AQLongRangeAttachment> lra;

    // --- Phase 7h #2 — far-from-origin hardening (§13.7). Particle positions
    // and prevPositions are stored as OFFSETS from this origin. Default 0 ⇒
    // offset == world ⇒ byte-identical to the pre-7h path. When
    // AQXPBDParams.originRelative is on, advance() re-bases the origin toward the
    // particle centroid so the solve always runs on small (precise) offsets.
    // `originActive` disambiguates "positions are offsets" from "origin happens
    // to be 0": false ⇒ positions are WORLD and origin is 0 (the default, and
    // exactly the pre-7h state); true ⇒ positions are offsets from `origin`.
    FVec<3> origin       = FVec<3>::Create();
    bool    originActive = false;

    // Append `count` particles (positions/invMass parallel arrays); returns the
    // index of the first appended particle.
    std::uint32_t addParticles(const OmegaGTE::FVec<3>* pos, const float* w,
                               std::uint32_t count);

    // Author one distance constraint (rest/compliance validated by the caller);
    // returns its authoring index. Marks the coloring dirty.
    std::uint32_t addDistance(std::uint32_t a, std::uint32_t b,
                              float restLength, float compliance);

    // Deterministic greedy coloring + (color, authoring-index)-sorted mirror +
    // batch ranges. Runs on demand (topology change), never per frame. Also
    // rebuilds the long-range-attachment set (§13.7 7h #1) when enabled.
    void recolor();

    // Rebuild the long-range-attachment records: BFS the constraint graph from
    // every pinned particle, accumulating rest lengths, to give each dynamic
    // particle its nearest pin and geodesic max distance (§13.7 7h #1). Called
    // by recolor(); a no-op (clears `lra`) when longRangeAttach is off.
    void buildLongRange();

    // One ENGINE sub-step of dt: n XPBD slices of h = dt/n, each slice
    // predict → (iterations ×) colored projection → [long-range clamp] →
    // [rigid contact coupling] → derive (brief §6 loop + §13.6/§13.7). Gravity
    // is the space's; params are the space's AQXPBDParams. `colliders` is the
    // rigid-collider snapshot for the two-way contact coupling — empty when
    // this body has collision off, so the coupling pass is skipped. Reaction
    // impulses accumulate in `reactionsThisStep` for xpbdSubstep to apply.
    void advance(float dt, const AQXPBDParams& params, const FVec<3>& gravity,
                 const OmegaCommon::Vector<AQXPBDCollider>& colliders);

    // Guards (mirroring AQParticleSystem): any NaN/Inf in live particle state.
    bool anyNonFinite() const;
};

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

    // --- Phase 6: particle systems (§13.3) ---
    // Systems are held by UniqueHandle so a system's SoA keeps a stable address
    // as the table grows/shrinks; the opaque AQParticleSystemHandle carries the
    // monotonic id (0 == invalid), which particleSystemAt resolves to live
    // state each call so a caller can never dangle into a destroyed system.
    OmegaCommon::Vector<UniqueHandle<AQParticleSystem>> particleSystems;
    std::uint64_t nextParticleSystemId = 1;
    // Static collider snapshot, rebuilt once per advance in particlesEmit and
    // reused by every sub-step's push-out (colliders don't move within a frame).
    OmegaCommon::Vector<AQParticleCollider> particleColliders;
    // Phase 6h — the compute backend while the live GPU particle path steps
    // this space. NON-owning (the AQContext owns the backend); set by
    // particlesGpuFrame each advance and cleared by AQSpace::detachCompute
    // when the owning context is destroyed, so a space handle that outlives
    // its context can never dangle into a freed backend. Null on the CPU path.
    AQComputeBackend *gpuBackend = nullptr;

    AQParticleSystem       *particleSystemAt(std::uint64_t id);
    const AQParticleSystem *particleSystemAt(std::uint64_t id) const;

    // --- Phase 7: XPBD bodies (§13 7b/7c) ---
    // Same ownership idiom as the particle table: UniqueHandle for stable SoA
    // addresses, monotonic opaque ids resolved per call (0 == invalid).
    OmegaCommon::Vector<UniqueHandle<AQXPBDBody>> xpbdBodies;
    std::uint64_t nextXPBDBodyId = 1;
    AQXPBDParams  xpbdParams;   ///< per-space solve tuning (setXPBDParams)

    AQXPBDBody       *xpbdBodyAt(std::uint64_t id);
    const AQXPBDBody *xpbdBodyAt(std::uint64_t id) const;
};

#endif // AQUA_SRC_AQSPACEIMPL_H
