
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
