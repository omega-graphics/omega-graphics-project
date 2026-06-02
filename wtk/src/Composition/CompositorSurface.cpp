#include "omegaWTK/Composition/CompositorSurface.h"

namespace OmegaWTK::Composition {

void CompositorSurface::deposit(SharedHandle<CompositeFrame> frame){
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latestFrame_ = std::move(frame);
        generation_.fetch_add(1, std::memory_order_release);
        callback = onDeposit_;
    }
    // Fire outside the lock so the wake path can grab the compositor
    // mutex without ordering against ours (avoids deadlock when the
    // compositor's renderCompositeFrame later calls consume()).
    if(callback){
        callback();
    }
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

void CompositorSurface::setOnDeposit(std::function<void()> callback){
    std::lock_guard<std::mutex> lk(mutex_);
    onDeposit_ = std::move(callback);
}

void CompositorSurface::setOwnerAppWindow(::OmegaWTK::AppWindow * appWindow){
    ownerAppWindow_.store(appWindow, std::memory_order_release);
}

::OmegaWTK::AppWindow * CompositorSurface::ownerAppWindow() const{
    return ownerAppWindow_.load(std::memory_order_acquire);
}

}
