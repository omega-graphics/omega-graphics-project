#ifndef OMEGAWTK_COMPOSITION_BACKEND_FENCEPOOL_H
#define OMEGAWTK_COMPOSITION_BACKEND_FENCEPOOL_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "ResourceTrace.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace OmegaWTK::Composition {

class FencePool {
    std::mutex mutex;
    std::vector<SharedHandle<OmegaGTE::GEFence>> freeList;

    static constexpr std::size_t kMaxFreeEntries = 8;

    std::uint64_t hits = 0;
    std::uint64_t misses = 0;

public:
    FencePool() = default;

    SharedHandle<OmegaGTE::GEFence> acquire(){
        std::lock_guard<std::mutex> lk(mutex);
        if(!freeList.empty()){
            auto fence = std::move(freeList.back());
            freeList.pop_back();
            ++hits;
            ResourceTrace::emit("PoolHit","Fence",0,"FencePool",this);
            return fence;
        }
        ++misses;
        ResourceTrace::emit("PoolMiss","Fence",0,"FencePool",this);
        return gte.graphicsEngine->makeFence();
    }

    void release(SharedHandle<OmegaGTE::GEFence> fence){
        if(fence == nullptr)
            return;
        std::lock_guard<std::mutex> lk(mutex);
        if(freeList.size() < kMaxFreeEntries){
            ResourceTrace::emit("PoolRelease","Fence",0,"FencePool",this);
            freeList.push_back(std::move(fence));
        }
    }

    void drain(){
        std::lock_guard<std::mutex> lk(mutex);
        freeList.clear();
    }

    struct Stats {
        std::uint64_t poolHits;
        std::uint64_t poolMisses;
        std::size_t freeCount;
    };

    Stats stats() const {
        return {hits, misses, freeList.size()};
    }
};

}

#endif
