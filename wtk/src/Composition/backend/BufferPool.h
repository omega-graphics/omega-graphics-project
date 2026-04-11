#ifndef OMEGAWTK_COMPOSITION_BACKEND_BUFFERPOOL_H
#define OMEGAWTK_COMPOSITION_BACKEND_BUFFERPOOL_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "ResourceTrace.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace OmegaWTK::Composition {

class BufferPool {
    struct Entry {
        SharedHandle<OmegaGTE::GEBuffer> buffer;
        std::size_t capacity;
        std::chrono::steady_clock::time_point lastUsed;
    };

    std::mutex mutex;
    std::map<std::size_t, std::vector<Entry>> buckets;
    SharedHandle<OmegaGTE::GEHeap> heap;

    static constexpr std::size_t kMinBucketSize = 4096;
    static constexpr std::size_t kMaxPoolBytes = 8 * 1024 * 1024;
    static constexpr std::chrono::seconds kEvictionAge {1};

    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::size_t totalPooledBytes = 0;

    static std::size_t roundUpToPowerOfTwo(std::size_t v){
        if(v <= kMinBucketSize)
            return kMinBucketSize;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        v++;
        return v;
    }

    void trimLocked(std::chrono::steady_clock::time_point now){
        for(auto bucketIt = buckets.begin(); bucketIt != buckets.end(); ){
            auto & entries = bucketIt->second;
            auto entryIt = entries.begin();
            while(entryIt != entries.end()){
                bool evict = (totalPooledBytes > kMaxPoolBytes) ||
                             ((now - entryIt->lastUsed) >= kEvictionAge);
                if(evict){
                    ResourceTrace::emit("PoolEvict","Buffer",0,"BufferPool",this,
                                        static_cast<float>(entryIt->capacity));
                    totalPooledBytes -= entryIt->capacity;
                    entryIt = entries.erase(entryIt);
                }
                else {
                    ++entryIt;
                }
            }
            if(entries.empty()){
                bucketIt = buckets.erase(bucketIt);
            }
            else {
                ++bucketIt;
            }
        }
    }

public:
    explicit BufferPool(SharedHandle<OmegaGTE::GEHeap> backingHeap)
        : heap(std::move(backingHeap)) {}

    SharedHandle<OmegaGTE::GEBuffer> acquire(std::size_t minBytes, std::size_t stride){
        std::size_t bucketSize = roundUpToPowerOfTwo(minBytes);
        std::lock_guard<std::mutex> lk(mutex);
        auto now = std::chrono::steady_clock::now();
        trimLocked(now);

        if(((hits + misses) & 63u) == 0 && (hits + misses) > 0){
            ResourceTrace::emitPoolSnapshot("BufferPool", hits, misses, 0, totalPooledBytes);
        }

        auto bucketIt = buckets.lower_bound(bucketSize);
        while(bucketIt != buckets.end()){
            auto & entries = bucketIt->second;
            if(!entries.empty()){
                auto entry = std::move(entries.back());
                entries.pop_back();
                totalPooledBytes -= entry.capacity;
                ++hits;
                ResourceTrace::emit("PoolHit","Buffer",0,"BufferPool",this,
                                    static_cast<float>(entry.capacity));
                return std::move(entry.buffer);
            }
            ++bucketIt;
        }

        ++misses;
        ResourceTrace::emit("PoolMiss","Buffer",0,"BufferPool",this,
                            static_cast<float>(bucketSize));

        OmegaGTE::BufferDescriptor desc {
            OmegaGTE::BufferDescriptor::Upload,
            bucketSize,
            stride,
            OmegaGTE::Shared
        };
        SharedHandle<OmegaGTE::GEBuffer> result;
        if(heap != nullptr && desc.usage != OmegaGTE::BufferDescriptor::Upload){
            result = heap->makeBuffer(desc);
        }
        if(result == nullptr){
            result = gte.graphicsEngine->makeBuffer(desc);
        }
        return result;
    }

    void release(SharedHandle<OmegaGTE::GEBuffer> buffer, std::size_t capacity){
        if(buffer == nullptr)
            return;
        std::size_t bucketSize = roundUpToPowerOfTwo(capacity);
        std::lock_guard<std::mutex> lk(mutex);
        ResourceTrace::emit("PoolRelease","Buffer",0,"BufferPool",this,
                            static_cast<float>(capacity));
        totalPooledBytes += bucketSize;
        buckets[bucketSize].push_back({std::move(buffer), bucketSize, std::chrono::steady_clock::now()});
    }

    void drain(){
        std::lock_guard<std::mutex> lk(mutex);
        buckets.clear();
        totalPooledBytes = 0;
    }

    struct Stats {
        std::uint64_t poolHits;
        std::uint64_t poolMisses;
        std::size_t totalPooledBytes;
    };

    Stats stats() const {
        return {hits, misses, totalPooledBytes};
    }
};

}

#endif
