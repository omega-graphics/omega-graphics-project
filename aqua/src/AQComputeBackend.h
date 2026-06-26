#ifndef AQUA_AQCOMPUTEBACKEND_H
#define AQUA_AQCOMPUTEBACKEND_H

// AQUA Phase 5 — the GPU compute backend (internal; pimpl-only, never in
// include/aqua/*). This is the one AQUA-side object that holds the OmegaGTE
// handles physics kernels are dispatched through: the OmegaGraphicsEngine
// (which makes compute pipelines + buffers) and the GECommandQueue (which
// submits the dispatches). AQ-prefixed, no namespace, per aqua/AGENTS.md.
//
// Phase 5a scope (this file): own the handles and report whether the GPU path
// can run *right now*. It cannot yet — no kernels are ported — so `usable()` is
// false and every context resolves to the CPU path (byte-for-byte Phase 4).
// Phase 5b grows the pooled buffers on this object; Phase 5c ports the kernels,
// runs a compute-pipeline capability probe, and flips `usable()` true.
//
// Capability-gate note: OmegaGraphicsEngine does NOT expose its GTEDevice or
// GTEDeviceFeatures (only `underlyingNativeDevice()` → void* and a protected
// feature bitmask), so the gate cannot read `GTEDeviceFeatures` off the engine.
// The honest probe is therefore behavioural — "can this engine build and run a
// trivial compute pipeline" — which is what 5c will do. Workgroup-size limits
// (for dispatch tiling) come later, via a probe or a small GTE accessor.

#include <aqua/AQBase.h>   // AQUA_NODISCARD
#include <omega-common/utils.h>
#include <memory>

namespace OmegaGTE {
    class OmegaGraphicsEngine;
    class GECommandQueue;
}

/// Owns the OmegaGTE handles AQUA dispatches physics compute kernels through.
struct AQComputeBackend {
    /// Stand up a compute backend for `engine` + `queue`.
    /// @returns nullptr when `engine` is null — that is a CPU-only context
    /// (`AQContext::CreateCPUOnly`), where there is no device to run kernels on.
    /// Otherwise a backend that *holds* the engine and queue; whether the GPU
    /// path is actually selectable is reported by `usable()` (false in 5a).
    static std::unique_ptr<AQComputeBackend> TryCreate(
        SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
        SharedHandle<OmegaGTE::GECommandQueue> queue);

    /// Whether the GPU path can run the step right now. Phase 5a: always false
    /// (handles held, no kernels ported yet), so the resolved execution path is
    /// CPU on every device. Phase 5c flips this true after a successful
    /// compute-pipeline capability probe.
    AQUA_NODISCARD bool usable() const { return gpuUsable; }

    /// The held engine/queue, for the 5b buffer pools and 5c pipeline build.
    AQUA_NODISCARD SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine() const { return gpuEngine; }
    AQUA_NODISCARD SharedHandle<OmegaGTE::GECommandQueue> queue() const { return cmdQueue; }

private:
    AQComputeBackend(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                     SharedHandle<OmegaGTE::GECommandQueue> queue);

    SharedHandle<OmegaGTE::OmegaGraphicsEngine> gpuEngine;
    SharedHandle<OmegaGTE::GECommandQueue>      cmdQueue;
    bool                                        gpuUsable = false;
};

#endif // AQUA_AQCOMPUTEBACKEND_H
