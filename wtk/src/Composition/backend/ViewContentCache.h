// Phase G.3.0 — Per-View content cache key + entry.
//
// The "primitive / content cache" the UIView-Render-Redesign plan
// describes as the load-bearing cache for Phase F: caches a rasterized
// GPU texture of each View's `DisplayList` so a full repaint with no
// changes to the View's painted content can blit the cached texture
// instead of re-issuing every draw op.
//
// G.3.0 ships the *types* only — `ViewCacheKey`, `std::hash` for the
// key, and `ViewCacheEntry`. The per-`BackendRenderTargetContext`
// `ContentCache<ViewCacheKey, ViewCacheEntry>` slot is also added (in
// RenderTarget.h via the existing PIMPL pattern), but the paint walker
// does not yet consult it. That integration arrives in G.3.1
// (render-into-cache-texture capture) and G.3.2 (blit-from-cache fast
// path + eligibility).
//
// Key fields (per UIView-Render-Redesign-Plan §G.3.0):
//   - nodeId           `View::nodeId()`. Stable per-View identity
//                      allocated at View construction; never reused.
//   - contentVersion   `View::contentVersion()`. Monotonic counter
//                      bumped by `markDirty(Paint)`; never cleared.
//                      Together with `nodeId` it identifies a unique
//                      paint generation of a unique View.
//   - wBucket,         `intRound(rect.w / .h)`. Live-resize sub-pixel
//     hBucket          jitter stays in the bucket; real size changes
//                      miss and re-rasterize.
//   - renderScaleBits  Raw float bits of `renderScale`. The Phase F-G
//                      bucketed-allocation work will eventually quantize
//                      this too; G.3.0 keeps raw bits so the key is
//                      deterministic across the renderScale changes
//                      that move a window across monitors.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_VIEWCONTENTCACHE_H
#define OMEGAWTK_COMPOSITION_BACKEND_VIEWCONTENTCACHE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

#include "omegaWTK/Composition/Geometry.h"
#include "omegaWTK/Composition/GTEForward.h"
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/GTEHandle.h"

namespace OmegaWTK::Composition {

    struct ViewCacheKey {
        std::uint64_t nodeId          = 0;
        std::uint64_t contentVersion  = 0;
        std::uint32_t wBucket         = 0;
        std::uint32_t hBucket         = 0;
        std::uint32_t renderScaleBits = 0;

        bool operator==(const ViewCacheKey & other) const {
            return nodeId          == other.nodeId
                && contentVersion  == other.contentVersion
                && wBucket         == other.wBucket
                && hBucket         == other.hBucket
                && renderScaleBits == other.renderScaleBits;
        }
    };

    /// Cached rasterized output of a View's `DisplayList`. `texture`
    /// is the captured render-into-texture surface (allocated by
    /// `BackendRenderTargetContext::beginCacheTarget` in G.3.1);
    /// `rasterizedSize` is the View's rect at the moment of capture,
    /// in logical (canvas-space) pixels. G.3.2's blit emits a
    /// `DrawOp::Bitmap` against `texture` with the source rect coming
    /// from `rasterizedSize` (after applying `renderScale`).
    ///
    /// `texture` is a `SharedHandle` so the cache entry holds the
    /// GPU resource alive across hits; eviction drops the last
    /// reference and (via the G.5 persistent-handle work, later) may
    /// return the underlying texture to the per-RTC pool.
    struct ViewCacheEntry {
        SharedHandle<OmegaGTE::GETexture> texture;
        Composition::Rect rasterizedSize {Composition::Point2D{0.f, 0.f}, 0.f, 0.f};

        // Phase G.5.1b follow-up: the persistent fullscreen-quad vertex
        // buffer this entry's blit is drawn with. `emitBitmapPrimitive`
        // populates it on the capture-end composite (the miss frame) and
        // every subsequent cache-hit blit re-binds it and skips the
        // six-vertex re-encode — the same hold-and-replace shape as the
        // tessellation cache (the buffer is never overwritten in place, so
        // an in-flight GPU read is never raced; a re-encode acquires a
        // fresh buffer and hands the old one to the completion-gated
        // recycler). `null` until the first blit encodes it.
        //
        // `blitQuad*` mirror the per-draw state baked into those vertices.
        // The NDC positions fold in the dest rect, the viewport, and the
        // current transform; the UVs ride alongside. A hit re-binds the
        // held buffer only when every field still matches what this blit
        // would encode. The transform is captured as a single
        // `blitBakedNoTransform` flag rather than the full matrix: reuse is
        // allowed only when the bake AND the current draw both ran under an
        // identity transform (the near-universal case for a top-level
        // cached-View blit). That keeps this header free of the GTE math
        // surface (it deliberately holds only forward-declared GTE handles)
        // while staying correct — any active transform simply re-encodes.
        SharedHandle<OmegaGTE::GEBuffer> blitQuadBuf;
        bool        blitQuadBaked        = false;
        bool        blitBakedNoTransform = false;
        float       blitDestX     = 0.f, blitDestY = 0.f;
        float       blitDestW     = 0.f, blitDestH = 0.f;
        float       blitViewportW = 0.f, blitViewportH = 0.f;
        float       blitUMin      = 0.f, blitVMin = 0.f;
        float       blitUMax      = 0.f, blitVMax = 0.f;
        std::size_t blitQuadBytes = 0;
    };

}

namespace std {
    template<>
    struct hash<OmegaWTK::Composition::ViewCacheKey> {
        std::size_t operator()(const OmegaWTK::Composition::ViewCacheKey & k) const noexcept {
            constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
            constexpr std::uint64_t FnvPrime  = 1099511628211ULL;
            std::uint64_t h = FnvOffset;
            auto mix64 = [&](std::uint64_t v){
                for(int i = 0; i < 8; ++i){
                    h ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu);
                    h *= FnvPrime;
                }
            };
            mix64(k.nodeId);
            mix64(k.contentVersion);
            mix64((static_cast<std::uint64_t>(k.wBucket) << 32)
                  | static_cast<std::uint64_t>(k.hBucket));
            mix64(static_cast<std::uint64_t>(k.renderScaleBits));
            return static_cast<std::size_t>(h);
        }
    };
}

#endif
