#ifndef OMEGAWTK_COMPOSITION_BACKEND_TEXTUREPOOL_H
#define OMEGAWTK_COMPOSITION_BACKEND_TEXTUREPOOL_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "ResourceTrace.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace OmegaWTK::Composition {

struct TexturePoolKey {
    unsigned width;
    unsigned height;
    OmegaGTE::TexturePixelFormat pixelFormat;
    OmegaGTE::GETexture::GETextureUsage usage;

    bool operator==(const TexturePoolKey & other) const {
        return width == other.width &&
               height == other.height &&
               pixelFormat == other.pixelFormat &&
               usage == other.usage;
    }

    bool fitsOversized(const TexturePoolKey & requested) const {
        if(pixelFormat != requested.pixelFormat || usage != requested.usage)
            return false;
        if(width < requested.width || height < requested.height)
            return false;
        return width <= static_cast<unsigned>(requested.width * 1.5f) &&
               height <= static_cast<unsigned>(requested.height * 1.5f);
    }
};

class TexturePool {
    struct Entry {
        TexturePoolKey key;
        SharedHandle<OmegaGTE::GETexture> texture;
        std::chrono::steady_clock::time_point lastUsed;
    };

    std::mutex mutex;
    std::vector<Entry> freeList;
    SharedHandle<OmegaGTE::GEHeap> heap;

    static constexpr std::size_t kMaxFreeEntries = 16;
    static constexpr std::chrono::seconds kEvictionAge {2};

    std::uint64_t hits = 0;
    std::uint64_t misses = 0;

    void trimLocked(std::chrono::steady_clock::time_point now){
        auto it = freeList.begin();
        while(it != freeList.end()){
            if(freeList.size() <= kMaxFreeEntries / 2 &&
               (now - it->lastUsed) < kEvictionAge){
                ++it;
                continue;
            }
            if(freeList.size() > kMaxFreeEntries ||
               (now - it->lastUsed) >= kEvictionAge){
                ResourceTrace::emit("PoolEvict","Texture",0,"TexturePool",this,
                                    static_cast<float>(it->key.width),
                                    static_cast<float>(it->key.height));
                it = freeList.erase(it);
            }
            else {
                ++it;
            }
        }
    }

public:
    explicit TexturePool(SharedHandle<OmegaGTE::GEHeap> backingHeap)
        : heap(std::move(backingHeap)) {}

    SharedHandle<OmegaGTE::GETexture> acquire(const TexturePoolKey & key){
        std::lock_guard<std::mutex> lk(mutex);
        auto now = std::chrono::steady_clock::now();
        trimLocked(now);

        if(((hits + misses) & 63u) == 0 && (hits + misses) > 0){
            ResourceTrace::emitPoolSnapshot("TexturePool", hits, misses, freeList.size());
        }

        int bestIdx = -1;
        std::size_t bestArea = SIZE_MAX;
        for(std::size_t i = 0; i < freeList.size(); ++i){
            if(freeList[i].key == key){
                bestIdx = static_cast<int>(i);
                break;
            }
            if(freeList[i].key.fitsOversized(key)){
                std::size_t area = static_cast<std::size_t>(freeList[i].key.width) * freeList[i].key.height;
                if(area < bestArea){
                    bestArea = area;
                    bestIdx = static_cast<int>(i);
                }
            }
        }

        if(bestIdx >= 0){
            auto texture = std::move(freeList[static_cast<std::size_t>(bestIdx)].texture);
            freeList.erase(freeList.begin() + bestIdx);
            ++hits;
            ResourceTrace::emit("PoolHit","Texture",0,"TexturePool",this,
                                static_cast<float>(key.width),
                                static_cast<float>(key.height));
            return texture;
        }

        ++misses;
        ResourceTrace::emit("PoolMiss","Texture",0,"TexturePool",this,
                            static_cast<float>(key.width),
                            static_cast<float>(key.height));

        OmegaGTE::TextureDescriptor desc {};
        desc.type = OmegaGTE::GETexture::Texture2D;
        desc.storage_opts = OmegaGTE::Shared;
        desc.width = key.width;
        desc.height = key.height;
        desc.pixelFormat = key.pixelFormat;
        desc.usage = key.usage;

        SharedHandle<OmegaGTE::GETexture> result;
        if(heap != nullptr){
            result = heap->makeTexture(desc);
        }
        if(result == nullptr){
            result = gte.graphicsEngine->makeTexture(desc);
        }
        return result;
    }

    void release(SharedHandle<OmegaGTE::GETexture> texture, const TexturePoolKey & key){
        if(texture == nullptr)
            return;
        std::lock_guard<std::mutex> lk(mutex);
        ResourceTrace::emit("PoolRelease","Texture",0,"TexturePool",this,
                            static_cast<float>(key.width),
                            static_cast<float>(key.height));
        freeList.push_back({key, std::move(texture), std::chrono::steady_clock::now()});
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
