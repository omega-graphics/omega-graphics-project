// Phase G.2 — TextShapingCache implementation.
//
// The singleton lives until process exit (Meyers idiom). The internal
// cache uses both the entry-count cap from `ContentCacheConfig::inst()`
// (env var `OMEGAWTK_TEXT_SHAPING_CACHE_ENTRIES`, default 4096) and a
// hardcoded byte cap; the plan §G.2 calls for a memory cap but does
// not specify a value, so we pick 4 MB — generous for the CPU-side
// `ShapedTextRun` weight (a static label runs ~hundreds of bytes after
// vector overhead). The GPU texture memory referenced by
// `BitmapBlit::texture` is *not* counted here; the cache merely keeps
// those textures alive for the duration of the entry.

#include "TextShapingCache.h"

#include <cstddef>

#include "backend/ContentCache.h"

namespace OmegaWTK::Composition {

    namespace {

        constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
        constexpr std::uint64_t FnvPrime  = 1099511628211ULL;

        void fnvMix64(std::uint64_t & h, std::uint64_t v){
            for(int i = 0; i < 8; ++i){
                h ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
                h *= FnvPrime;
            }
        }

        /// Internal byte cap for the shaping cache (plan §G.2: "LRU cap:
        /// 4096 entries + a memory-bytes cap" — value left to
        /// implementation). Sized for typical shaped text runs which are
        /// in the hundreds of bytes range; 4 MB covers ~5–10k cached
        /// entries' worth of CPU-side ShapedTextRun structures.
        constexpr std::size_t InternalByteCap = static_cast<std::size_t>(4) * 1024 * 1024;

    }

    std::uint64_t hashUniString(const OmegaCommon::UniString & text){
        std::uint64_t h = FnvOffset;
        const auto * buf = text.getBuffer();
        const std::int32_t len = text.length();
        if(buf == nullptr || len <= 0){
            return h;
        }
        for(std::int32_t i = 0; i < len; ++i){
            // `UnicodeChar` is a UTF-16 code unit (char16_t-shaped). Mix
            // in both bytes — same logical text always produces the same
            // digest.
            const std::uint16_t unit = static_cast<std::uint16_t>(buf[i]);
            h ^= static_cast<std::uint8_t>(unit & 0xFFu);
            h *= FnvPrime;
            h ^= static_cast<std::uint8_t>((unit >> 8) & 0xFFu);
            h *= FnvPrime;
        }
        return h;
    }

    std::uint64_t hashLayoutDescriptor(const TextLayoutDescriptor & desc){
        std::uint64_t h = FnvOffset;
        fnvMix64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.alignment)));
        fnvMix64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.wrapping)));
        fnvMix64(h, static_cast<std::uint64_t>(desc.lineLimit));
        return h;
    }

    TextShapingCache::TextShapingCache()
        : cache(ContentCacheConfig::inst().textShapingCacheEntries,
                InternalByteCap) {}

    TextShapingCache & TextShapingCache::inst(){
        static TextShapingCache instance;
        return instance;
    }

    Core::Optional<ShapedTextRun> TextShapingCache::find(const TextShapingCacheKey & key){
        std::lock_guard<std::mutex> lk(mutex);
        auto * entry = cache.find(key);
        if(entry == nullptr){
            return Core::Optional<ShapedTextRun>{};
        }
        // Copy out under the lock so a concurrent insert/eviction can't
        // invalidate the entry pointer between unlock and read.
        return Core::Optional<ShapedTextRun>{*entry};
    }

    void TextShapingCache::insert(TextShapingCacheKey key, ShapedTextRun value, std::size_t bytes){
        std::lock_guard<std::mutex> lk(mutex);
        cache.insert(std::move(key), std::move(value), bytes);
    }

    void TextShapingCache::clear(){
        std::lock_guard<std::mutex> lk(mutex);
        cache.clear();
    }

    TextShapingCache::Snapshot TextShapingCache::snapshot(){
        std::lock_guard<std::mutex> lk(mutex);
        Snapshot snap;
        snap.stats = cache.stats();
        return snap;
    }

}
