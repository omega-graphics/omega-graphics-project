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
#include "AQRigidBody.h"
#include <omegaGTE/GTEMath.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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
    AQUA_NODISCARD OmegaGTE::FVec<3> gravity() const;

    /// Adds a body and returns a handle owned by this space.
    std::shared_ptr<AQRigidBody> addBody(const AQBodyDesc &desc);

    /// Removes a body. Returns false if it was not in this space.
    bool removeBody(const std::shared_ptr<AQRigidBody> &body);

    /// Number of bodies currently in the space.
    AQUA_NODISCARD std::size_t bodyCount() const;

    // --- drainable debug surface (Phase 1.1 §6.5) ---
    // AQUA emits structured `AQDebugLine` records each step according to the
    // current flag set; the consumer (kREATE adapter, a test) drains the buffer
    // and the space clears it. Pull model — matches the kREATE debug-draw plan
    // §7 adapter, deterministic, and ports to a GPU append-buffer later. Off
    // by default (AQDebugNone), so the surface is zero-cost when unused.
    /// Sets which debug primitives the space emits each step. OR of `AQDebugFlags`.
    void setDebugFlags(std::uint32_t flags);
    AQUA_NODISCARD std::uint32_t debugFlags() const;
    /// Moves the accumulated debug lines out of the space and clears its buffer.
    AQUA_NODISCARD std::vector<AQDebugLine> drainDebugLines();

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
    AQUA_NODISCARD std::vector<AQBroadphasePair> candidatePairs() const;

    // --- material combine + solver (Phase 3) ---
    /// Per-space restitution and friction combine rules. Default is Average
    /// for both (the PhysX default; the most physically-defensible
    /// isotropic-material policy). See `AQMaterialCombine`.
    void setMaterialCombine(AQMaterialCombine restCombine,
                            AQMaterialCombine fricCombine);
    AQUA_NODISCARD AQMaterialCombine restitutionCombine() const;
    AQUA_NODISCARD AQMaterialCombine frictionCombine() const;

    /// Sequential-impulse PGS sweep counts (Phase-3 §11.4). Defaults: 8
    /// velocity, 4 position. `positionIters == 0` disables the split-impulse
    /// position-correction pass entirely — useful for the energy-non-growth
    /// test and for scenes that don't need penetration recovery.
    void setSolverIterations(int velocityIters, int positionIters);
    AQUA_NODISCARD int velocityIterations() const;
    AQUA_NODISCARD int positionIterations() const;

    /// Read-only manifold view, refreshed by the most recent sub-step. Stable
    /// for the duration of one `AQContext::advance` call between the final
    /// sub-step and the next `advance`. Useful for the Phase 4 joint wiring
    /// and for debug overlays — the value-type copy carries body indices, not
    /// pointers, so it is safe to hold across `advance` boundaries.
    AQUA_NODISCARD std::vector<AQContactManifold> contactManifolds() const;

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

    friend class AQContext;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AQUA_AQSPACE_H
