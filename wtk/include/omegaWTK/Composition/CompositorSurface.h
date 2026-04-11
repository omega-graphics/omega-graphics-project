#include "omegaWTK/Core/Core.h"
#include "CompositeFrame.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITORSURFACE_H
#define OMEGAWTK_COMPOSITION_COMPOSITORSURFACE_H

namespace OmegaWTK::Composition {

class CompositorSurface {
    std::mutex mutex_;
    SharedHandle<CompositeFrame> latestFrame_;
    std::atomic<uint64_t> generation_ {0};
    uint64_t consumedGeneration_ = 0;

public:
    void deposit(SharedHandle<CompositeFrame> frame);

    SharedHandle<CompositeFrame> consume();

    bool hasPendingUpdate() const;

    uint64_t generation() const;
};

}

#endif
