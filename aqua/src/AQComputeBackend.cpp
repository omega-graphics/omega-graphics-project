#include "AQComputeBackend.h"

// NOTE: deliberately does NOT include <omegaGTE/GE.h> / <omegaGTE/GECommandQueue.h>.
// Those headers #error without a target-platform macro, and AQUA TUs don't define
// one today (AQUA used only header-only GTE math). 5a needs no complete GTE types —
// SharedHandle copies/moves and `operator bool` work on incomplete types (the
// type-erased deleter was captured at the handle's original construction site).
// When 5c calls makeComputePipelineState / makeBuffer it will need GE.h, and that
// is gated on AQUA's build defining the target platform (a 5b/5c build change).

#include <utility>

AQComputeBackend::AQComputeBackend(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                                   SharedHandle<OmegaGTE::GECommandQueue> queue)
    : gpuEngine(std::move(engine)), cmdQueue(std::move(queue)) {}

std::unique_ptr<AQComputeBackend> AQComputeBackend::TryCreate(
    SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
    SharedHandle<OmegaGTE::GECommandQueue> queue) {
    // No engine ⇒ CPU-only context: there is no device to dispatch kernels to.
    if (!engine) {
        return nullptr;
    }

    // Phase 5a: hold the engine + queue so 5b can grow the buffer pools on this
    // object and 5c can build pipelines. `usable_` stays false until 5c ports
    // the kernels and a compute-pipeline probe confirms the device runs them, so
    // until then the execution path resolves to CPU on every device.
    return std::unique_ptr<AQComputeBackend>(
        new AQComputeBackend(std::move(engine), std::move(queue)));
}
