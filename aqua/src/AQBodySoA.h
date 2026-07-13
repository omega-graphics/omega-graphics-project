#ifndef AQUA_AQBODYSOA_H
#define AQUA_AQBODYSOA_H

// AQUA Phase 5b — the struct-of-arrays GPU mirror of per-body dynamics state.
//
// The authoritative CPU state stays AoS: `AQBodyState<float>` per body, owned by
// each `AQRigidBody` (the battle-tested CPU solver is untouched — Phase 5b
// decision, plan §13). `AQBodySoA` is the *mirror* the GPU kernels read/write:
// each `AQBodyState` field is flattened into its own contiguous `float` array so
// an OmegaSL kernel thread for body `i` reads `posX[i]`, `velX[i]`, … at stride-1
// offsets (coalesced). `gatherFrom` packs AoS → SoA before a GPU upload;
// `scatterTo` writes SoA → AoS after a readback. Internal (src/), AQ-prefixed.
//
// The arrays mirror `AQBodyState<float>` field-for-field (AQIntegrator.h:30-57)
// so Phase 5c's integration kernel has every input it needs without extending
// this struct. Quaternion components are read/written directly (.x/.y/.z/.w);
// vector components via the GTE column-major `[row][0]` access the integrator
// uses, so the gather never relies on `Matrix` storage layout implicitly.

#include <aqua/AQBase.h>         // OMEGA_NODISCARD
#include <aqua/AQIntegrator.h>   // AQBodyState<Ty>, AQActivationState
#include <omega-common/utils.h>  // OmegaCommon::Vector
#include <cstdint>

struct AQUA_EXPORT AQBodySoA {
    // --- pose + motion ---
    OmegaCommon::Vector<float> posX, posY, posZ;             ///< position
    OmegaCommon::Vector<float> velX, velY, velZ;             ///< linear velocity
    OmegaCommon::Vector<float> quatX, quatY, quatZ, quatW;   ///< orientation
    OmegaCommon::Vector<float> wbX, wbY, wbZ;                ///< angular velocity (body frame)
    // --- mass properties ---
    OmegaCommon::Vector<float> invMass;                      ///< 0 ⇒ static/kinematic
    OmegaCommon::Vector<float> invInertiaX, invInertiaY, invInertiaZ;  ///< 1 / principal moments
    // --- accumulators (world frame) ---
    OmegaCommon::Vector<float> forceX, forceY, forceZ;
    OmegaCommon::Vector<float> torqueX, torqueY, torqueZ;
    // --- per-body scalars ---
    OmegaCommon::Vector<float> linearDamping, angularDamping, gravityScale, maxAngularSpeed;
    OmegaCommon::Vector<float> comX, comY, comZ;             ///< COM offset (reserved)
    // --- split-impulse pseudo-velocity (Phase 5c; per-substep solver output) ---
    // Not part of AQBodyState (it lives on AQRigidBody::Impl), so gatherFrom
    // zero-fills it and scatterTo leaves the AoS untouched. The GPU position
    // half-step consumes it (pos += pseudoLinear * dt); the 5f position-solve
    // kernel writes it. Test harnesses set it directly on the SoA.
    OmegaCommon::Vector<float> pseudoLinX, pseudoLinY, pseudoLinZ;
    // --- activation (Active / Sleeping / Kinematic) ---
    OmegaCommon::Vector<std::uint8_t> activation;

    /// Number of bodies the arrays currently hold.
    OMEGA_NODISCARD std::size_t size() const { return posX.size(); }

    /// Resize every parallel array to `n` (no-op if already `n`).
    void resize(std::size_t n);

    /// AoS → SoA: copy `n` `AQBodyState<float>` records into the parallel arrays
    /// (resizing first). `states` must point to at least `n` elements.
    void gatherFrom(const AQBodyState<float>* states, std::size_t n);

    /// SoA → AoS: write the parallel arrays back into `n` `AQBodyState<float>`
    /// records. Only the GPU-owned dynamics fields are written; caller-side
    /// fields the GPU path does not touch are left as-is. `states` must point to
    /// at least `min(n, size())` elements.
    void scatterTo(AQBodyState<float>* states, std::size_t n) const;
};

#endif // AQUA_AQBODYSOA_H
