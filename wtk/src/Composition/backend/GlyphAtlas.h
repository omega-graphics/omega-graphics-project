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
    /// texture dimensions; metric offsets are in design-units the
    /// layout pass already produced (advance / bearing reproduced here
    /// so the render path can author quads without re-querying the
    /// platform face).
    struct AtlasGlyph {
        float u0 = 0.f;
        float v0 = 0.f;
        float u1 = 0.f;
        float v1 = 0.f;
        /// Pixel size of the rasterized MSDF tile (square or near-square).
        std::uint16_t pxW = 0;
        std::uint16_t pxH = 0;
        /// Horizontal advance (font units → already converted to pixels
        /// at the font's current size by the rasterizer).
        float advance = 0.f;
        /// Bearing from the pen origin to the upper-left of the tile,
        /// in pixels. `bearingY` is positive upward (i.e. distance from
        /// the baseline up to the top of the glyph). FT *layout*
        /// metrics — kept for completeness, but quad authoring uses the
        /// `tileOrigin*` / `tileScale` fields below instead.
        float bearingX = 0.f;
        float bearingY = 0.f;
        /// MSDF tile placement (Phase 6.7-c3). The rasterized tile
        /// covers, in the font's pixel space (FT_Set_Pixel_Sizes at the
        /// design size), the square box whose lower-left corner is
        /// `(tileOriginX, tileOriginY)` relative to the glyph pen
        /// origin / baseline (Y positive upward). `tileScale` is
        /// tile-pixels per font-pixel; the box edge length in
        /// font-pixels is `pxW / tileScale`. Quad authoring (canvas
        /// space, Y down): `minX = penX + tileOriginX`,
        /// `maxY = penY - tileOriginY`, edge = `pxW / tileScale`.
        /// The MSDF tile is sized to the glyph's padded bounding box.
        /// `pxW × pxH` is the *allocated* tile — `ceil` of the content
        /// size — while `inkPxW × inkPxH` is the *exact* content size
        /// in tile pixels (un-rounded `(r-l)*scale` / `(t-b)*scale`).
        /// The `ceil` rounding leaves a sub-pixel sliver of empty
        /// distance field at one tile edge; addressing `inkPx*` instead
        /// of `pxW/pxH` excludes that sliver, so it can't shove glyphs
        /// off the baseline. `(tileOriginX, tileOriginY)` is the padded
        /// bbox's lower-left corner relative to the pen origin (Y up,
        /// font pixels); `tileScale` is tile-pixels per font-pixel, so
        /// the render quad is `inkPx* / tileScale` font-pixels.
        float tileOriginX = 0.f;
        float tileOriginY = 0.f;
        float tileScale   = 1.f;
        float inkPxW = 0.f;
        float inkPxH = 0.f;
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
