// Phase G.1 — Tessellation cache key + hash.
//
// Caches `OmegaGTE::TETriangulationResult` for unchanged
// `DrawOp::VectorPath` shapes so a repaint that crosses an unchanged path
// skips `triangulateSync`. The cached result feeds `drawTriangulatedResult`
// directly.
//
// Key fields (per UIView-Render-Redesign-Plan §G.1):
//   - pathHash       FNV-1a over the path's (x, y) points
//   - pointCount     guards against hash collisions that happen to share a
//                    digest across different lengths
//   - strokeWidth    the input to `TETriangulationParams::GraphicsPath2D`
//   - contour, fill  same — toggle which polygons triangulateSync emits
//   - wBucket,       intRound(renderTargetSize_.w / .h). The viewport that
//     hBucket        triangulateSync receives is currently the RTC's logical
//                    rect; live-resize sub-pixel jitter stays in the same
//                    bucket, real size changes miss.
//
// Divergence from the plan spec: the key includes stroke/fill RGBA. The
// plan key lists only `(path, strokeWidth, contour, fill, sizeBucket)`,
// but `triangulateSync` bakes the brush colors from
// `TETriangulationParams::Attachment::makeColor` into every per-vertex
// `attachment->color` on the resulting mesh — and `drawTriangulatedResult`
// reads those colors when authoring the GPU vertex buffer. A cache hit on
// the same geometry with a different stroke or fill color would render the
// wrong color, so the colors must participate in the key. Re-authoring
// colors on hit by walking `mesh.vertexPolygons` is the alternative
// approach and is deferred — it erodes the win, and the keys are still
// stable across the dominant unchanged-shape case the cache targets.
//
// `hasStrokeColor` / `hasFillColor` ride in `flagsBits` so a brush-less
// draw (the legacy single-brush path) keeps a stable key.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_TESSELLATIONCACHE_H
#define OMEGAWTK_COMPOSITION_BACKEND_TESSELLATIONCACHE_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "omegaWTK/Composition/GTEForward.h"

namespace OmegaWTK::Composition {

    struct TessellationCacheKey {
        std::uint64_t pathHash    = 0;
        std::uint32_t pointCount  = 0;
        std::uint32_t wBucket     = 0;
        std::uint32_t hBucket     = 0;
        std::uint32_t strokeRGBA  = 0;
        std::uint32_t fillRGBA    = 0;
        float         strokeWidth = 0.f;
        std::uint8_t  flagsBits   = 0;

        enum Flag : std::uint8_t {
            FlagContour        = 0x1,
            FlagFill           = 0x2,
            FlagHasStrokeColor = 0x4,
            FlagHasFillColor   = 0x8
        };

        bool operator==(const TessellationCacheKey & other) const {
            // Compare the float strokeWidth byte-wise — NaN sentinels then
            // hash/compare deterministically (NaN == NaN is false under
            // IEEE-754, which would prevent any cached entry whose
            // strokeWidth was NaN from ever hitting itself). Cache keys
            // need bit-identity, not floating-point equivalence.
            std::uint32_t aBits = 0;
            std::uint32_t bBits = 0;
            std::memcpy(&aBits, &strokeWidth, sizeof(aBits));
            std::memcpy(&bBits, &other.strokeWidth, sizeof(bBits));
            return pathHash    == other.pathHash
                && pointCount  == other.pointCount
                && wBucket     == other.wBucket
                && hBucket     == other.hBucket
                && strokeRGBA  == other.strokeRGBA
                && fillRGBA    == other.fillRGBA
                && flagsBits   == other.flagsBits
                && aBits       == bBits;
        }
    };

    /// FNV-1a 64-bit over a `GVectorPath2D`'s underlying (x, y) sequence.
    /// `transformEachPoint` is the path's stable point-walk — it visits
    /// every underlying point once (including the start point and the
    /// final point), which matches what `triangulateSync` consumes via
    /// `TETriangulationParams::GraphicsPath2D`.
    ///
    /// Returns `{hash, pointCount}`. `pointCount` rides in the cache key
    /// so two paths with different lengths that happen to share a digest
    /// still miss.
    std::pair<std::uint64_t, std::uint32_t>
    hashPath2D(OmegaGTE::GVectorPath2D & path);

    /// Pack a `(r, g, b, a)` color in [0, 1] floats into a 32-bit RGBA so
    /// the cache key stays compact. Out-of-range floats clamp; NaN maps
    /// to 0 (deterministic — NaN comparisons would otherwise miss every
    /// hit). The packing is lossy at the 1/255 quantization, which is
    /// fine for cache keying — two colors that round to the same byte
    /// quartet render identically.
    inline std::uint32_t packRGBA(float r, float g, float b, float a){
        auto channel = [](float v) -> std::uint32_t {
            if(std::isnan(v)){
                return 0;
            }
            if(v <= 0.f){
                return 0;
            }
            if(v >= 1.f){
                return 255;
            }
            return static_cast<std::uint32_t>(std::lround(v * 255.f));
        };
        return (channel(r) << 24) | (channel(g) << 16) | (channel(b) << 8) | channel(a);
    }

    /// Round a logical render-target dimension to an integer pixel bucket.
    /// Live-resize sub-pixel jitter (e.g. 800.12 → 800.46 → 800.81) stays
    /// in the same bucket; a real size change crosses an integer boundary
    /// and misses. Negative or non-finite inputs collapse to 0 — the
    /// resulting key will not match any sane RTC size and so will always
    /// miss, which is the desired safety behavior.
    inline std::uint32_t bucketDim(float v){
        if(std::isnan(v) || v <= 0.f){
            return 0;
        }
        return static_cast<std::uint32_t>(std::lround(v));
    }

}

namespace std {
    template<>
    struct hash<OmegaWTK::Composition::TessellationCacheKey> {
        std::size_t operator()(const OmegaWTK::Composition::TessellationCacheKey & k) const noexcept {
            constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
            constexpr std::uint64_t FnvPrime  = 1099511628211ULL;
            std::uint64_t h = FnvOffset;
            auto mix64 = [&](std::uint64_t v){
                for(int i = 0; i < 8; ++i){
                    h ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
                    h *= FnvPrime;
                }
            };
            mix64(k.pathHash);
            mix64((static_cast<std::uint64_t>(k.pointCount) << 32)
                  | static_cast<std::uint64_t>(k.flagsBits));
            mix64((static_cast<std::uint64_t>(k.wBucket) << 32)
                  | static_cast<std::uint64_t>(k.hBucket));
            mix64((static_cast<std::uint64_t>(k.strokeRGBA) << 32)
                  | static_cast<std::uint64_t>(k.fillRGBA));
            std::uint32_t swBits = 0;
            std::memcpy(&swBits, &k.strokeWidth, sizeof(swBits));
            mix64(static_cast<std::uint64_t>(swBits));
            return static_cast<std::size_t>(h);
        }
    };
}

#endif
