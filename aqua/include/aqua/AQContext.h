
#ifndef AQUA_AQCONTEXT_H
#define AQUA_AQCONTEXT_H

#include <omega-common/utils.h>
#include "AQBase.h"
#include "AQSpace.h"
#include <vector>

namespace OmegaGTE {

class GECommandQueue;
}

/// Main context for AQUA and the owner of all physics state.
///
/// AQContext holds the OmegaGTE command queue that physics work is submitted
/// through, creates and retains the simulation spaces, and keeps simulation
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
    SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
    std::vector<SharedHandle<AQSpace>> spaces;
    float fixedDt;       ///< Size of one simulation sub-step, in seconds.
    float accumulator;   ///< Real time received but not yet simulated.
    double elapsedTime;  ///< Total simulated time, in seconds.

    explicit AQContext(SharedHandle<OmegaGTE::GECommandQueue> commandQueue);
    public:
    static SharedHandle<AQContext> Create(SharedHandle<OmegaGTE::GECommandQueue> commandQueue);

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
