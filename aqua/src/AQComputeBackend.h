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
    struct GTEShaderLibrary;
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

    /// Whether the GPU path can run the *full step* right now. Still false in
    /// 5b — the kernel library + buffer + dispatch toolchain is proven here
    /// (`loadKernelLibrary` + `selfTest`), but the step kernels (integration,
    /// broadphase, solver) are not ported until 5c+, so the resolved execution
    /// path stays CPU. 5f/5g flips this true once the GPU step is end-to-end.
    AQUA_NODISCARD bool usable() const { return gpuUsable; }

    /// The held engine/queue, for the 5b buffer pools and 5c pipeline build.
    AQUA_NODISCARD SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine() const { return gpuEngine; }
    AQUA_NODISCARD SharedHandle<OmegaGTE::GECommandQueue> queue() const { return cmdQueue; }

    /// Load AQUA's precompiled kernel library (`AQKernels.omegasllib`) from
    /// @p path via `OmegaGraphicsEngine::loadShaderLibrary`. Returns false if the
    /// engine is null or the load fails. (Phase 5b precompile path, plan §7.5.)
    bool loadKernelLibrary(const OmegaCommon::String& path);

    /// Capability probe: build a compute pipeline for the `AQProbeDouble` kernel,
    /// dispatch it over a small buffer, and verify the GPU doubled the values.
    /// Proves the precompile -> load -> pipeline -> buffer -> dispatch ->
    /// readback toolchain end to end on the active device. Requires
    /// `loadKernelLibrary` to have succeeded and a non-null command queue.
    /// Returns true iff the GPU produced the expected results.
    AQUA_NODISCARD bool selfTest();

private:
    AQComputeBackend(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                     SharedHandle<OmegaGTE::GECommandQueue> queue);

    SharedHandle<OmegaGTE::OmegaGraphicsEngine> gpuEngine;
    SharedHandle<OmegaGTE::GECommandQueue>      cmdQueue;
    SharedHandle<OmegaGTE::GTEShaderLibrary>    kernelLib;   ///< loaded AQKernels.omegasllib
    bool                                        gpuUsable = false;
};

#endif // AQUA_AQCOMPUTEBACKEND_H
