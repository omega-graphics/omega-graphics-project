// Phase G.2 — Text-shaping cache.
//
// Caches `ShapedTextRun`s for unchanged text inputs so a repaint that
// crosses an unchanged label (the dominant case for buttons, title bars,
// menu items) skips the whole `TextLayoutEngine::layout` →
// `ensureGlyphsResident` / `rasterizeSubRunToTexture` pipeline and reuses
// the prior result. The cached `BitmapBlit::texture` keeps the
// pre-rasterized fallback texture alive across hits at no extra GPU
// cost; MSDF sub-runs re-run `ensureGlyphsResident` on every hit because
// atlas residency is per-`FontEngine` state and may have been evicted
// between frames (plan §G.2 explicit decision).
//
// Process-wide singleton. The plan §G.0 locked decision: "Fonts are
// globally shared (`FontEngine::inst()`), and static labels frequently
// appear verbatim across windows (button labels, title bars, dialogs); a
// per-window cache would miss those cross-window reuses for no gain.
// Lifetime is the FontEngine's; eviction is LRU by entry count plus a
// total-subRuns memory cap." Implemented here as a Meyers singleton (lives
// until process exit) with an internal `std::mutex` so concurrent
// compositor-thread shapers across windows can share it safely.
//
// Key fields (per UIView-Render-Redesign-Plan §G.2):
//   - textHash         FNV-1a over the UTF-16 buffer's bytes. Stable
//                      across UTF-32 distinct code points because the
//                      UTF-16 encoding is itself unique.
//   - textLength       UTF-16 code-unit count; guards against the rare
//                      hash collision across length-distinct inputs.
//   - layoutHash       FNV-1a over `TextLayoutDescriptor`'s three fields
//                      (alignment, wrapping, lineLimit).
//   - fontId           `Font *` — pointer identity. The cached entry
//                      holds a `SharedPtr<Font>` indirectly via each
//                      `TextSubRun::resolvedFont`, so the Font stays
//                      alive as long as the cache entry survives.
//   - fontSize         `Font::desc.size`. Belt-and-suspenders against
//                      hypothetical Font re-creation at the same address.
//   - wBucket,         `intRound(rect.w / .h)`. Same live-resize policy
//     hBucket          as G.1: sub-pixel jitter stays in the bucket,
//                      real size changes miss.
//   - renderScaleBits  Raw float bits of `renderScale`. NaN sentinels
//                      then hash deterministically.
//   - colorRGBA        Packed 8-bit RGBA of the input `Composition::Color`.
//
// Divergence from the plan key spec: the key includes the input color.
// The plan key lists only `(text, font-id, font-size, layoutDesc,
// rect, renderScale)` — but the bitmap-fallback rasterization path
// (`FontEngine::rasterizeSubRunToTexture` in `DisplayList.cpp`) bakes
// the input color into the rasterized texture. A same-text-
// different-color hit would reuse a wrongly-tinted texture. The
// pure-MSDF case is color-independent (color is applied in the
// fragment shader), so a future refactor that separates MSDF caching
// from bitmap caching could drop color from the MSDF half of the key;
// for now, treating the two as one entry simplifies the cache shape.

#ifndef OMEGAWTK_COMPOSITION_TEXTSHAPINGCACHE_H
#define OMEGAWTK_COMPOSITION_TEXTSHAPINGCACHE_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>

#include "omegaWTK/Composition/DisplayList.h"   // ShapedTextRun
#include "omegaWTK/Composition/FontEngine.h"    // Font, TextLayoutDescriptor

namespace OmegaWTK::Composition {

    struct TextShapingCacheKey {
        std::uint64_t textHash        = 0;
        std::uint64_t layoutHash      = 0;
        Font *        fontId          = nullptr;
        std::uint32_t textLength      = 0;
        std::uint32_t fontSize        = 0;
        std::uint32_t wBucket         = 0;
        std::uint32_t hBucket         = 0;
        std::uint32_t renderScaleBits = 0;
        std::uint32_t colorRGBA       = 0;

        bool operator==(const TextShapingCacheKey & other) const {
            return textHash        == other.textHash
                && layoutHash      == other.layoutHash
                && fontId          == other.fontId
                && textLength      == other.textLength
                && fontSize        == other.fontSize
                && wBucket         == other.wBucket
                && hBucket         == other.hBucket
                && renderScaleBits == other.renderScaleBits
                && colorRGBA       == other.colorRGBA;
        }
    };

    /// FNV-1a 64-bit over a `UniString`'s UTF-16 buffer. Stable for
    /// the same logical text. The plan calls for "FNV-1a over UTF-32
    /// code points"; UTF-16 over the same string yields a different
    /// digest but is identically uniquely-identifying — two distinct
    /// strings always have distinct UTF-16 encodings — and avoids the
    /// surrogate-pair decoding pass.
    std::uint64_t hashUniString(const OmegaCommon::UniString & text);

    /// FNV-1a 64-bit over `TextLayoutDescriptor`'s three relevant
    /// fields: alignment, wrapping, lineLimit. Each is mixed in as a
    /// 32-bit value.
    std::uint64_t hashLayoutDescriptor(const TextLayoutDescriptor & desc);

    /// Raw float bits, so NaN-equal-NaN behaves deterministically in
    /// the cache key.
    inline std::uint32_t floatBits(float v){
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

}

// `std::hash<TextShapingCacheKey>` must be declared BEFORE the
// `ContentCache<TextShapingCacheKey, …>` instantiation inside the
// `TextShapingCache` class body below — otherwise the implicit
// instantiation triggered by ContentCache's default `KeyHash` template
// parameter happens first, and clang reports
// "explicit specialization after instantiation".
namespace std {
    template<>
    struct hash<OmegaWTK::Composition::TextShapingCacheKey> {
        std::size_t operator()(const OmegaWTK::Composition::TextShapingCacheKey & k) const noexcept {
            constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
            constexpr std::uint64_t FnvPrime  = 1099511628211ULL;
            std::uint64_t h = FnvOffset;
            auto mix64 = [&](std::uint64_t v){
                for(int i = 0; i < 8; ++i){
                    h ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
                    h *= FnvPrime;
                }
            };
            mix64(k.textHash);
            mix64(k.layoutHash);
            // Mix in the Font* identity by copying the pointer bits into
            // an integer via memcpy — `reinterpret_cast<uintptr_t>` is the
            // canonical idiom but the cppcoreguidelines check flags it.
            std::uintptr_t fontPtrBits = 0;
            // Explicit `const void *` cast to silence the multi-level
            // pointer-conversion check; `&k.fontId` has type `Font *const *`.
            std::memcpy(&fontPtrBits,
                        static_cast<const void *>(&k.fontId),
                        sizeof(fontPtrBits));
            mix64(static_cast<std::uint64_t>(fontPtrBits));
            mix64((static_cast<std::uint64_t>(k.textLength) << 32)
                  | static_cast<std::uint64_t>(k.fontSize));
            mix64((static_cast<std::uint64_t>(k.wBucket) << 32)
                  | static_cast<std::uint64_t>(k.hBucket));
            mix64((static_cast<std::uint64_t>(k.renderScaleBits) << 32)
                  | static_cast<std::uint64_t>(k.colorRGBA));
            return static_cast<std::size_t>(h);
        }
    };
}

#include "backend/ContentCache.h"

namespace OmegaWTK::Composition {

    /// Process-wide shaping cache, lifetime tied to process exit (Meyers
    /// singleton). Thread-safe: a single mutex guards both find and
    /// insert; the critical section is small (an unordered_map lookup
    /// plus a list splice).
    class TextShapingCache {
        std::mutex mutex;
        ContentCache<TextShapingCacheKey, ShapedTextRun> cache;

        TextShapingCache();

    public:
        TextShapingCache(const TextShapingCache &) = delete;
        TextShapingCache & operator=(const TextShapingCache &) = delete;
        TextShapingCache(TextShapingCache &&) = delete;
        TextShapingCache & operator=(TextShapingCache &&) = delete;
        ~TextShapingCache() = default;

        static TextShapingCache & inst();

        /// Probe the cache. On hit, returns a copy of the entry (cheap —
        /// the entry's vectors are small and texture handles are
        /// `SharedPtr`s). On miss, returns an empty Optional.
        Core::Optional<ShapedTextRun> find(const TextShapingCacheKey & key);

        /// Insert (or replace) an entry. `bytes` is the caller's CPU-side
        /// size estimate for telemetry; the cache enforces both the
        /// entry-count cap (from `ContentCacheConfig`) and a separate
        /// byte cap. The GPU texture memory held by `BitmapBlit::texture`
        /// is *not* counted here — that's GTE-side storage; the cache
        /// keeps the texture alive only for the entry's lifetime.
        void insert(TextShapingCacheKey key, ShapedTextRun value, std::size_t bytes);

        /// Drop everything. Used for tests and for explicit teardown if
        /// font lifetimes ever need to be force-revoked; production
        /// callers don't need this.
        void clear();

        struct Snapshot {
            ContentCacheStats stats;
        };
        /// Copy out the telemetry counters under the lock — safe to
        /// inspect mid-flight.
        Snapshot snapshot();
    };

}

#endif
