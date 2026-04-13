#include "omegaWTK/Core/Core.h"
#include "CompositeFrame.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITORSURFACE_H
#define OMEGAWTK_COMPOSITION_COMPOSITORSURFACE_H

namespace OmegaWTK::Composition {

class CompositorSurface {
    std::mutex mutex_;
    SharedHandle<CompositeFrame> latestFrame_;
    std::atomic<uint64_t> generation_ {0};
    uint64_t consumedGeneration_ = 0;
    std::function<void()> onDeposit_;

public:
    void deposit(SharedHandle<CompositeFrame> frame);

    SharedHandle<CompositeFrame> consume();

    bool hasPendingUpdate() const;

    uint64_t generation() const;

    /// Set a callback fired (outside the surface lock) every time a new
    /// frame is deposited. The compositor uses this to wake its frame
    /// loop without polling.
    void setOnDeposit(std::function<void()> callback);
};

}

#endif
