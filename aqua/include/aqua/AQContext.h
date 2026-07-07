
#ifndef AQUA_AQCONTEXT_H
#define AQUA_AQCONTEXT_H

#include <omega-common/utils.h>
#include "AQBase.h"
#include "AQSpace.h"
#include <cstdint>
#include <memory>

namespace OmegaGTE {

class GECommandQueue;
class OmegaGraphicsEngine;
}

/// The compute backend (Phase 5, internal pimpl). Forward-declared so the public
/// header carries no OmegaGTE backend dependency.
struct AQComputeBackend;

/// Which substrate AQUA runs the hot stages (integration, broadphase, contact
/// solve) on. Selection is *data*, never an `#ifdef` (Physics-Roadmap §3
/// principle 3): `Auto` resolves from device capability + kernel availability.
/// `executionPath()` reports the *resolved* path (never `Auto`); lockstep
/// consumers (kREATE netcode) pin a single path because CPU↔GPU agreement is
/// equivalent-within-tolerance, not bitwise (Phase 5 plan §8).
enum class AQExecPath : std::uint8_t {
    Auto,  ///< GPU if a usable compute backend exists, else CPU
    CPU,   ///< force the CPU reference path (the parity oracle / fallback)
    GPU,   ///< prefer the GPU path; falls back to CPU (loud) if unusable
};

/// Main context for AQUA and the owner of all physics state.
///
/// AQContext holds the OmegaGTE graphics engine + command queue that physics
/// work is submitted through (Phase 5 dispatches compute kernels on them),
/// creates and retains the simulation spaces, and keeps simulation
/// time. Timekeeping uses a fixed-timestep accumulator: callers feed real
/// elapsed frame time to `advance`, and the context runs as many fixed-size
/// sub-steps as fit, banking the remainder. This decouples the simulation rate
/// from the caller's frame rate and keeps stepping deterministic.
///
/// REQUIRED — small sub-steps for rotational accuracy. The Phase 1 integrator
/// (body-frame symplectic Lie + implicit gyroscopic) is *stable* but conserves
/// angular momentum and energy only to first order in the sub-step: drift is
/// O(dt) and accumulates. A fast spinner is visibly worse at the default
/// 1/120 s than at 1/2000 s (≈20× the drift). Sub-stepping is therefore not an
/// optimization but a correctness requirement for fast rotational bodies — set
/// `setFixedTimestep` small enough for the angular rates in the scene (the
/// "small steps" posture, Phase-1 doc §11.5). This is also why the integrator
/// is built around a fixed sub-step accumulator rather than per-frame stepping.
class AQUA_EXPORT AQContext
{
    /// The GPU compute backend — the single owner of the OmegaGTE engine +
    /// command queue AQUA dispatches kernels through. Null when there is no
    /// engine (a `CreateCPUOnly` context) or the device offers no usable
    /// compute. Pimpl-hidden.
    std::unique_ptr<AQComputeBackend> compute;
    OmegaCommon::Vector<SharedHandle<AQSpace>> spaces;
    AQExecPath requestedPath = AQExecPath::Auto;  ///< what the caller asked for
    float fixedDt;       ///< Size of one simulation sub-step, in seconds.
    float accumulator;   ///< Real time received but not yet simulated.
    double elapsedTime;  ///< Total simulated time, in seconds.

    AQContext(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
              SharedHandle<OmegaGTE::GECommandQueue> commandQueue);
    public:
    /// The production factory. The graphics `engine` is **required** — kREATE is
    /// GPU-first and always has one. For an engine-less CPU-only context (tests,
    /// headless tools) use `CreateCPUOnly`. A null `engine` here is a contract
    /// violation: it is reported loudly and degrades to a CPU-only context.
    static SharedHandle<AQContext> Create(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                                          SharedHandle<OmegaGTE::GECommandQueue> commandQueue);

    /// Engine-less factory: a context that always runs the CPU reference path.
    /// Keeps AQUA's pure-CPU unit tests free of any GTE device initialization.
    static SharedHandle<AQContext> CreateCPUOnly();

    ~AQContext();

    /// Loads AQUA's precompiled compute-kernel library (`AQKernels.omegasllib`,
    /// staged beside the AQUA runtime artifact by the build) and runs the
    /// device capability probe. On success the LIVE GPU particle path becomes
    /// selectable via `setExecutionPath` (Phase 6h — AQUA's first live GPU
    /// pillar; the rigid step remains CPU until its own flip). Returns false —
    /// loudly — when the context is CPU-only, the library fails to load, or
    /// the probe fails; the CPU path keeps running either way.
    bool loadKernels(const OmegaCommon::String &kernelLibPath);

    /// Requests a substrate. `Auto` (default) picks GPU when usable, else CPU;
    /// `CPU` forces the reference path; `GPU` prefers the GPU and warns + falls
    /// back to CPU when no usable backend exists.
    void setExecutionPath(AQExecPath path);
    /// The *resolved* path that the next `advance` will run (never `Auto`).
    AQUA_NODISCARD AQExecPath executionPath() const;

    /// Creates a simulation space owned and stepped by this context.
    SharedHandle<AQSpace> createSpace();

    /// Advances every space by `realDt` seconds of real time, running as many
    /// fixed sub-steps as fit and banking the surplus for the next call.
    void advance(float realDt);

    /// The fixed sub-step size, in seconds. Defaults to 1/120 s.
    AQUA_NODISCARD float fixedTimestep() const;
    /// Sets the fixed sub-step size. Non-positive values are ignored.
    void setFixedTimestep(float dt);

    /// Total simulated time advanced so far, in seconds.
    AQUA_NODISCARD double elapsed() const;
};

#endif // AQUA_AQCONTEXT_H
