#include "omegaWTK/Composition/CompositorSurface.h"

namespace OmegaWTK::Composition {

void CompositorSurface::deposit(SharedHandle<CompositeFrame> frame){
    std::lock_guard<std::mutex> lk(mutex_);
    latestFrame_ = std::move(frame);
    generation_.fetch_add(1, std::memory_order_release);
}

SharedHandle<CompositeFrame> CompositorSurface::consume(){
    std::lock_guard<std::mutex> lk(mutex_);
    auto gen = generation_.load(std::memory_order_acquire);
    if(gen <= consumedGeneration_){
        return nullptr;
    }
    consumedGeneration_ = gen;
    return latestFrame_;
}

bool CompositorSurface::hasPendingUpdate() const{
    auto gen = generation_.load(std::memory_order_acquire);
    return gen > consumedGeneration_;
}

uint64_t CompositorSurface::generation() const{
    return generation_.load(std::memory_order_acquire);
}

}
