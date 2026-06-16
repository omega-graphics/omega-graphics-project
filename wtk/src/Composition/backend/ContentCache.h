// Phase G.0 scaffolding for the WTK content-cache subsystem.
//
// `ContentCache` is the generic LRU container used by the three caches the
// UIView-Render-Redesign plan introduces in Phase G:
//
//   - G.1 tessellation cache (per `BackendRenderTargetContext`): caches
//     `OmegaGTE::TETriangulationResult` for unchanged vector paths so a
//     repaint that crosses an unchanged shape doesn't re-run
//     `triangulateSync`.
//   - G.2 text-shaping cache (process-wide, alongside `FontEngine::inst()`):
//     caches `Composition::ShapedTextRun` keyed by `(text, font, size,
//     layout)` so static labels skip shaping on every frame.
//   - G.3 primitive / content cache (per `BackendRenderTargetContext`):
//     caches the rasterized output (a GPU texture) of a `View`'s
//     `DisplayList` so a full repaint can blit unchanged widgets from cache
//     instead of re-issuing draw ops.
//
// G.0 ships pure infrastructure: the template, the telemetry counters, the
// env-var config (`OMEGAWTK_CONTENT_CACHE_BYTES`,
// `OMEGAWTK_TESS_CACHE_ENTRIES`, `OMEGAWTK_TEXT_SHAPING_CACHE_ENTRIES`), and
// the CMake gate (`OMEGAWTK_ENABLE_CONTENT_CACHE`, default OFF). Nothing in
// the render path looks at the cache yet — the type lands without changing
// per-frame behavior. G.1, G.2, G.3 instantiate concrete caches against this
// template; their owning sites (the per-window RTC for tess/content, the
// process singleton for text-shaping) are added in those sub-phases when the
// concrete `Value` types come with them.
//
// Eviction policy. An entry is keyed by `Key`, carries a `Value`, and
// records its byte cost at insert time. Eviction fires at the LRU end when
// either a configurable max-entry count or a configurable max-byte budget
// is exceeded. The optional `OnEvict` callback runs synchronously inside
// `insert` / `evict` / `clear` and is the hook the G.5 persistent-handle
// work uses to return pool-backed GPU resources on eviction. Set
// `entryLimit = 0` or `byteLimit = 0` to disable that axis (both zero means
// unbounded — only useful in tests).
//
// Thread-safety. Each `ContentCache` instance is single-threaded by design:
// the per-RTC caches live on the compositor's render thread, and the
// process-wide text-shaping cache is guarded by `FontEngine`'s existing
// lock at the call sites that will integrate with it in G.2. The container
// itself does no locking — keep ownership boundary clean rather than pay
// for mutex contention on a hot per-frame path.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_CONTENTCACHE_H
#define OMEGAWTK_COMPOSITION_BACKEND_CONTENTCACHE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace OmegaWTK::Composition {

    /// Per-cache counters surfaced through `ContentCache::stats()`. G.4 wires
    /// these into `OMEGAWTK_CONTENT_CACHE_STATS=1`'s periodic stderr line;
    /// G.0 only exposes the struct.
    struct ContentCacheStats {
        std::uint64_t hits         = 0;
        std::uint64_t misses       = 0;
        std::uint64_t evictions    = 0;
        std::size_t   entries      = 0;
        std::size_t   currentBytes = 0;
        std::size_t   peakBytes    = 0;
    };

    /// Generic LRU cache.
    ///
    /// `Key` must be hashable (default: `std::hash<Key>`) and equality-
    /// comparable (default: `std::equal_to<Key>`). `Value` is moved into the
    /// cache at insert and moved out at evict (so callers can own
    /// `SharedHandle<GETexture>` / `TETriangulationResult` / `ShapedTextRun`
    /// by value without any copy on the hot path).
    template<typename Key,
             typename Value,
             typename KeyHash = std::hash<Key>,
             typename KeyEq   = std::equal_to<Key>>
    class ContentCache {
    public:
        typedef std::function<void(const Key &, Value &)> OnEvictFn;

        struct Entry {
            Key         key;
            Value       value;
            std::size_t bytes;
        };

    private:
        typedef std::list<Entry> EntryList;
        typedef typename EntryList::iterator EntryIter;

        EntryList lruList;
        std::unordered_map<Key, EntryIter, KeyHash, KeyEq> index;

        std::size_t entryLimit = 0;
        std::size_t byteLimit  = 0;

        OnEvictFn onEvict;

        ContentCacheStats counters;

        void touch(EntryIter it){
            // Move the entry to the front of the list. `std::list::splice`
            // does this in O(1) and preserves the iterator's validity, so
            // the `index` map's stored iterator stays correct.
            lruList.splice(lruList.begin(), lruList, it);
        }

        void evictTail(){
            if(lruList.empty()){
                return;
            }
            EntryIter tail = std::prev(lruList.end());
            if(onEvict){
                onEvict(tail->key, tail->value);
            }
            counters.currentBytes -= tail->bytes;
            counters.entries      -= 1;
            counters.evictions    += 1;
            index.erase(tail->key);
            lruList.erase(tail);
        }

        void enforceLimits(){
            if(entryLimit > 0){
                while(counters.entries > entryLimit){
                    evictTail();
                }
            }
            if(byteLimit > 0){
                while(counters.currentBytes > byteLimit){
                    evictTail();
                }
            }
        }

    public:
        ContentCache() = default;

        ContentCache(std::size_t entryLimitIn,
                     std::size_t byteLimitIn,
                     OnEvictFn   onEvictIn = {})
            : entryLimit(entryLimitIn),
              byteLimit(byteLimitIn),
              onEvict(std::move(onEvictIn)) {}

        ContentCache(const ContentCache &) = delete;
        ContentCache & operator=(const ContentCache &) = delete;

        // Movable — the contained list/map are nothrow-movable, so moving
        // the cache is one pointer swap per member.
        ContentCache(ContentCache &&) noexcept = default;
        ContentCache & operator=(ContentCache &&) noexcept = default;

        ~ContentCache(){
            clear();
        }

        void setMaxEntries(std::size_t limit){
            entryLimit = limit;
            enforceLimits();
        }

        void setMaxBytes(std::size_t limit){
            byteLimit = limit;
            enforceLimits();
        }

        void setOnEvict(OnEvictFn fn){
            onEvict = std::move(fn);
        }

        /// Probe the cache. On hit, the entry is moved to the MRU end and a
        /// pointer to its `Value` is returned. On miss returns `nullptr`.
        /// The pointer is invalidated by the next mutating call (`insert`,
        /// `evict`, `clear`, `setMaxEntries`, `setMaxBytes`).
        Value * find(const Key & key){
            auto it = index.find(key);
            if(it == index.end()){
                counters.misses += 1;
                return nullptr;
            }
            counters.hits += 1;
            touch(it->second);
            return &it->second->value;
        }

        /// Insert (or replace) `key`. `bytes` is the byte cost the caller
        /// attributes to this entry — typically the GPU texture size in
        /// bytes for G.3 entries, the vertex+index buffer size for G.1
        /// entries, or `sizeof(ShapedTextRun) + subRunBytes` for G.2.
        /// Returns a pointer to the live entry's value.
        Value * insert(Key key, Value value, std::size_t bytes){
            auto existing = index.find(key);
            if(existing != index.end()){
                EntryIter it = existing->second;
                if(onEvict){
                    onEvict(it->key, it->value);
                }
                counters.currentBytes -= it->bytes;
                it->value = std::move(value);
                it->bytes = bytes;
                counters.currentBytes += bytes;
                if(counters.currentBytes > counters.peakBytes){
                    counters.peakBytes = counters.currentBytes;
                }
                touch(it);
                enforceLimits();
                return &it->value;
            }

            Entry e;
            e.key   = std::move(key);
            e.value = std::move(value);
            e.bytes = bytes;
            lruList.push_front(std::move(e));
            EntryIter front = lruList.begin();
            index.emplace(front->key, front);
            counters.entries      += 1;
            counters.currentBytes += bytes;
            if(counters.currentBytes > counters.peakBytes){
                counters.peakBytes = counters.currentBytes;
            }
            enforceLimits();
            return &front->value;
        }

        /// Drop one entry by key. Fires `OnEvict` and updates stats.
        /// Counts as an eviction in the telemetry (it is — the entry is
        /// gone before its natural LRU end-of-life).
        bool evict(const Key & key){
            auto it = index.find(key);
            if(it == index.end()){
                return false;
            }
            EntryIter entryIt = it->second;
            if(onEvict){
                onEvict(entryIt->key, entryIt->value);
            }
            counters.currentBytes -= entryIt->bytes;
            counters.entries      -= 1;
            counters.evictions    += 1;
            index.erase(it);
            lruList.erase(entryIt);
            return true;
        }

        /// Drop every entry. Fires `OnEvict` on each. `peakBytes` is
        /// preserved (it is a high-water mark across the cache's lifetime).
        void clear(){
            if(onEvict){
                for(auto & entry : lruList){
                    onEvict(entry.key, entry.value);
                }
            }
            counters.evictions    += counters.entries;
            counters.entries       = 0;
            counters.currentBytes  = 0;
            index.clear();
            lruList.clear();
        }

        [[nodiscard]] std::size_t size()  const { return counters.entries; }
        [[nodiscard]] std::size_t bytes() const { return counters.currentBytes; }
        [[nodiscard]] bool        empty() const { return counters.entries == 0; }

        [[nodiscard]] const ContentCacheStats & stats() const { return counters; }

        /// Zero the runtime counters (`hits`, `misses`, `evictions`). The
        /// structural counters (`entries`, `currentBytes`, `peakBytes`)
        /// reflect live state and are not touched. Useful for per-frame
        /// telemetry windows.
        void resetCounters(){
            counters.hits      = 0;
            counters.misses    = 0;
            counters.evictions = 0;
        }
    };

    /// Process-wide, environment-tunable cache limits. Resolved once on
    /// first access from the env vars listed at the top of the file; the
    /// resolved values are stable for the process lifetime so a `getenv`
    /// during configuration cannot race against a cache instantiation
    /// later. Each cache owner copies the relevant fields at construction
    /// (so per-cache `setMaxEntries` / `setMaxBytes` overrides remain
    /// possible — for tests, for example).
    struct ContentCacheConfig {
        /// G.3 content-cache byte budget per `BackendRenderTargetContext`.
        /// Default 64 MB; override via `OMEGAWTK_CONTENT_CACHE_BYTES`.
        std::size_t contentCacheBytes        = static_cast<std::size_t>(64) * 1024 * 1024;
        /// G.1 tessellation-cache max entry count per
        /// `BackendRenderTargetContext`. Default 1024; override via
        /// `OMEGAWTK_TESS_CACHE_ENTRIES`.
        std::size_t tessellationCacheEntries = 1024;
        /// G.2 text-shaping-cache max entry count (process-wide). Default
        /// 4096; override via `OMEGAWTK_TEXT_SHAPING_CACHE_ENTRIES`.
        std::size_t textShapingCacheEntries  = 4096;
        /// G.3.2 minimum View rect dimension (in logical pixels) below
        /// which the per-View content cache is skipped. Default 64 px;
        /// override via `OMEGAWTK_CONTENT_CACHE_MIN_SIZE_PX`. Plan
        /// rationale: under this size the GPU texture allocation +
        /// per-frame capture overhead exceeds the savings of skipping
        /// the View's own paint emission.
        std::uint32_t cacheMinSizePx = 64;
        /// G.4 telemetry toggle. When true, the per-window frame loop
        /// (`BackendRenderTargetContext::endFrame`) prints a periodic
        /// stats line — per cache: hits, misses, evictions, entries,
        /// currentBytes, hit-rate — to stderr every 60 frames. Default
        /// off; enable with `OMEGAWTK_CONTENT_CACHE_STATS=1`. Only has
        /// an effect in a build with `OMEGAWTK_ENABLE_CONTENT_CACHE=ON`
        /// (the print site is compiled under that gate).
        bool cacheStatsEnabled = false;
        /// G.5.4 resize-drag stretch toggle. When on, a cached View painted
        /// during a live resize drag blits its prior texture STRETCHED to the
        /// live rect (skipping the per-tick re-render) rather than
        /// re-capturing at the new size — trading fidelity (blur while
        /// dragging) for cost. Default OFF (opt-in via `OMEGAWTK_RESIZE_STRETCH=1`)
        /// for the initial landing; flip the default once the blur tradeoff
        /// is visually validated. Only meaningful in an
        /// `OMEGAWTK_ENABLE_CONTENT_CACHE=ON` build.
        bool resizeStretchEnabled = false;

        static const ContentCacheConfig & inst();
    };

}

#endif
