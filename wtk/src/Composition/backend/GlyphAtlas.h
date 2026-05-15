// Per-font MSDF glyph atlas (Phase 6.7.1).
//
// One `GlyphAtlas` lives on each `Font` (constructed alongside it). Lazy-
// populated: glyph IDs not yet cached trigger an MSDF rasterization on the
// main thread (atlas mutation has to coordinate with GPU sampling) and a
// sub-region update of the atlas texture. The actual MSDF rasterization
// is delegated to a per-platform `RasterizeFn` callback supplied by the
// concrete `FontEngine` at construction time — DWrite walks the geometry
// sink, Core Text walks `CGPathRef`, PangoFc descends to `FT_Face`. The
// callback fills an RGB distance-field buffer + glyph metrics for one
// glyph and returns true on success.
//
// Chunk 1 (current) ships the type contract only — `ensureGlyph` is a
// stub that returns false. Chunks 2-3 add real msdfgen rasterization
// and texture upload.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_GLYPHATLAS_H
#define OMEGAWTK_COMPOSITION_BACKEND_GLYPHATLAS_H

#include "omegaWTK/Core/GTEHandle.h"
#include <omega-common/utils.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace OmegaWTK::Composition {

    /// Per-glyph cache entry. UV rect is normalized against the atlas
    /// texture dimensions. Quad-placement metrics follow Skia's
    /// `SkGlyph` convention (Text-Layout-Engine-Plan §Phase 2.5):
    /// top-anchored, integer pen-relative offsets, no scale round-trip.
    /// Drawing one glyph:
    /// ```
    /// minX = round(penX + fLeft);
    /// minY = round(penY - fTop);
    /// maxX = minX + fWidth;
    /// maxY = minY + fHeight;
    /// ```
    /// All `f*` fields are in canvas pixels at 1× DPR (which equals
    /// the font's current FT_Set_Pixel_Sizes value).
    struct AtlasGlyph {
        float u0 = 0.f;
        float v0 = 0.f;
        float u1 = 0.f;
        float v1 = 0.f;
        /// Pixel size of the rasterized MSDF tile (square or near-square).
        /// Kept for UV / atlas-packing diagnostics; the render path
        /// uses `fWidth` / `fHeight` for the canvas quad.
        std::uint16_t pxW = 0;
        std::uint16_t pxH = 0;
        /// Horizontal advance in canvas pixels at the font's current
        /// size — what the pen moves by after this glyph is laid out.
        float advance = 0.f;
        /// Pen-relative offset, in canvas pixels, from the pen origin
        /// (baseline × pen X) to the glyph quad's *top-left* corner.
        /// - `fLeft` is positive when the glyph silhouette is right of
        ///   the pen (most glyphs); negative for the small left-bearing
        ///   that some italic / kerning glyphs use.
        /// - `fTop` is positive when the top of the glyph is *above*
        ///   the baseline (which is the common case). For glyphs that
        ///   sit entirely below the baseline (rare — diacritic combining
        ///   marks below), `fTop` can be negative.
        ///
        /// Top-anchored math avoids the `inkH = pxH / tileScale`
        /// round-trip that produced sub-pixel jitter (Phase-2.5
        /// notes). The bitmap's actual canvas footprint is
        /// `fWidth × fHeight`, independent of the tile's `pxW × pxH`
        /// pixel count; the tile-vs-canvas ratio implicitly handles
        /// the SDF base-scale projection.
        float fLeft   = 0.f;
        float fTop    = 0.f;
        float fWidth  = 0.f;
        float fHeight = 0.f;
    };

    /// Per-font glyph atlas. Owns the GPU texture and the glyph map.
    /// Not thread-safe — must be touched only from the thread that
    /// drives the canvas paint pass (same constraint that already
    /// applies to every other compositor-side mutable state).
    class GlyphAtlas {
    public:
        /// Square atlas dimension (Phase 6.7.1: "start at 1024×1024,
        /// room for ~1000 glyphs at 32×32 cells"). Backing texture
        /// allocated on first successful `ensureGlyph`.
        static constexpr unsigned kAtlasDim = 1024;

        /// Output of one glyph rasterization. The callback fills
        /// `rgb` with `pxW * pxH * 3` bytes (R, G, B distance channels)
        /// and populates `metrics` (which the atlas will later patch
        /// with the assigned UV rect after packing into the texture).
        struct RasterizedGlyph {
            std::vector<std::uint8_t> rgb;
            std::uint32_t pxW = 0;
            std::uint32_t pxH = 0;
            AtlasGlyph metrics {};
        };

        using RasterizeFn = std::function<bool(std::uint32_t glyphId,
                                               RasterizedGlyph & out)>;

        /// `rasterize` is the per-platform MSDF producer. Pass `nullptr`
        /// for fonts that fall back to the bitmap path — `ensureGlyph`
        /// will then always return false.
        explicit GlyphAtlas(RasterizeFn rasterize);

        /// Install / replace the rasterize callback after construction.
        /// `Font`'s base ctor builds the atlas with `nullptr`; backend
        /// subclasses call this once they've decided MSDF mode applies.
        /// Callers are expected to do this *before* any `ensureGlyph`
        /// invocation — swapping the callback after glyphs have been
        /// rasterized would mix tiles from two different rasterizers
        /// in the same atlas.
        void setRasterizeFn(RasterizeFn fn);
        ~GlyphAtlas();

        GlyphAtlas(const GlyphAtlas &) = delete;
        GlyphAtlas & operator=(const GlyphAtlas &) = delete;

        /// Look up a previously-cached glyph. Returns nullptr if absent.
        const AtlasGlyph * lookup(std::uint32_t glyphId) const;

        /// Ensure the glyph is resident in the atlas. Returns true if
        /// the glyph is now available via `lookup`. Chunk 1 stub:
        /// always returns false. Chunks 2-3 will invoke the
        /// `RasterizeFn`, pack the tile into the atlas texture, and
        /// commit the entry.
        bool ensureGlyph(std::uint32_t glyphId);

        /// Atlas backing texture. May be null until the first successful
        /// `ensureGlyph` (chunk 2 onward).
        const SharedHandle<OmegaGTE::GETexture> & texture() const { return texture_; }

    private:
        /// Lazy-allocate the 1024×1024 atlas texture on first use.
        /// Returns false (and leaves `texture_` null) if allocation
        /// fails; the caller treats this the same as a packing failure.
        bool ensureTexture();

        RasterizeFn rasterize_;
        SharedHandle<OmegaGTE::GETexture> texture_;
        std::unordered_map<std::uint32_t, AtlasGlyph> glyphs_;
        /// Naive shelf packer state. New tiles append along the current
        /// row at `cursorX_`; when a tile won't fit horizontally we wrap
        /// to a new shelf at `cursorY_ + rowH_`. When the next shelf
        /// would overflow vertically the atlas is considered full and
        /// `ensureGlyph` returns false (LRU paging is a Phase-6.7
        /// follow-up).
        unsigned cursorX_ = 0;
        unsigned cursorY_ = 0;
        unsigned rowH_    = 0;
    };

}

#endif
