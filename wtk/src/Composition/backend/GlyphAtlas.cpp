#include "GlyphAtlas.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace OmegaWTK::Composition {

    namespace {
        bool textTraceEnabled() {
            // Cached on first call. Reads OMEGAWTK_TRACE_TEXT once at
            // process start; flipping the env var mid-process does not
            // re-trigger logging (consistent with the project's other
            // OMEGAWTK_TRACE_* flags, which are compile-time `#ifdef`s
            // — this one is runtime because it is hot enough that we
            // want to leave the call sites in but cheap enough that
            // a single getenv at startup is fine).
            static const bool enabled = []() {
                const char *e = std::getenv("OMEGAWTK_TRACE_TEXT");
                return e != nullptr && e[0] != '\0' && e[0] != '0';
            }();
            return enabled;
        }
    }

    GlyphAtlas::GlyphAtlas(RasterizeFn rasterize)
        : rasterize_(std::move(rasterize)) {
    }

    GlyphAtlas::~GlyphAtlas() = default;

    void GlyphAtlas::setRasterizeFn(RasterizeFn fn) {
        rasterize_ = std::move(fn);
    }

    const AtlasGlyph * GlyphAtlas::lookup(std::uint32_t glyphId) const {
        auto it = glyphs_.find(glyphId);
        if(it == glyphs_.end()){
            return nullptr;
        }
        return &it->second;
    }

    bool GlyphAtlas::ensureTexture() {
        if(texture_ != nullptr){
            return true;
        }
        OmegaGTE::TextureDescriptor desc {};
        desc.usage         = OmegaGTE::GETexture::ToGPU;
        desc.storage_opts  = OmegaGTE::Shared;
        desc.pixelFormat   = OmegaGTE::TexturePixelFormat::RGBA8Unorm;
        desc.kind          = OmegaGTE::TextureKind::Tex2D;
        desc.width         = kAtlasDim;
        desc.height        = kAtlasDim;
        texture_ = gte.graphicsEngine->makeTexture(desc);
        if(texture_ == nullptr){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] GlyphAtlas: makeTexture(1024x1024 RGBA8Unorm) failed" << std::endl;
            }
            return false;
        }
        // Initial full-surface upload of zeros so the unused regions
        // sample as transparent black if a stale UV happens to read
        // outside a packed glyph.
        std::vector<std::uint8_t> zeroes(static_cast<std::size_t>(kAtlasDim) * kAtlasDim * 4, 0);
        texture_->copyBytes(zeroes.data(), kAtlasDim * 4);
        if(textTraceEnabled()){
            std::cout << "[wtk-text] GlyphAtlas: allocated " << kAtlasDim << "x" << kAtlasDim
                      << " RGBA8Unorm atlas texture" << std::endl;
        }
        return true;
    }

    bool GlyphAtlas::ensureGlyph(std::uint32_t glyphId) {
        if(glyphs_.find(glyphId) != glyphs_.end()){
            return true;
        }
        if(!rasterize_){
            return false;
        }

        RasterizedGlyph out;
        if(!rasterize_(glyphId, out)){
            return false;
        }
        if(out.pxW == 0 || out.pxH == 0 || out.rgb.size() < static_cast<std::size_t>(out.pxW) * out.pxH * 3){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] GlyphAtlas: rasterize callback returned empty/invalid buffer for glyph "
                          << glyphId << std::endl;
            }
            return false;
        }

        // Reserve the tile *before* allocating the texture: otherwise
        // a single failing rasterize would still pay the 4 MiB upload
        // cost. Naive shelf packer:
        //   - if the tile fits on the current row, place it at
        //     (cursorX_, cursorY_) and advance cursorX_ by tile width
        //   - else wrap to a new row at cursorY_ + rowH_
        //   - else (won't fit vertically) fail
        unsigned tileW = out.pxW;
        unsigned tileH = out.pxH;
        if(tileW > kAtlasDim || tileH > kAtlasDim){
            return false;
        }
        if(cursorX_ + tileW > kAtlasDim){
            cursorX_  = 0;
            cursorY_ += rowH_;
            rowH_     = 0;
        }
        if(cursorY_ + tileH > kAtlasDim){
            // Atlas full. Phase-6.7 follow-up: LRU paging.
            if(textTraceEnabled()){
                std::cout << "[wtk-text] GlyphAtlas: atlas full at glyph " << glyphId
                          << " (cursor " << cursorX_ << "," << cursorY_ << ", tile "
                          << tileW << "x" << tileH << ")" << std::endl;
            }
            return false;
        }

        if(!ensureTexture()){
            return false;
        }

        // Expand the 3-channel MSDF into RGBA8 (A=255). The shader's
        // median-of-three reads R, G, B; alpha is unused today but kept
        // at 255 so a misconfigured sampler doesn't surprise us with
        // zeros.
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(tileW) * tileH * 4);
        for(std::size_t i = 0; i < static_cast<std::size_t>(tileW) * tileH; ++i){
            rgba[i * 4 + 0] = out.rgb[i * 3 + 0];
            rgba[i * 4 + 1] = out.rgb[i * 3 + 1];
            rgba[i * 4 + 2] = out.rgb[i * 3 + 2];
            rgba[i * 4 + 3] = 0xFF;
        }

        // Upload the tile straight — no per-row Y-flip. Phase-2.5
        // (Text-Layout-Engine-Plan §Phase 2.5) consolidates the
        // orientation handling: the rasterize callback emits the
        // tile top-row-first (reads msdfgen's Y-up bitmap in reverse
        // Y inside the quantize loop), and the canvas-top ↔ `v0`
        // UV pairing in `emitTextSubRun` carries that orientation
        // through to the fragment. One contiguous sub-rect upload
        // instead of `tileH` single-row uploads.
        const std::size_t srcBpr = static_cast<std::size_t>(tileW) * 4;
        OmegaGTE::TextureRegion region {cursorX_, cursorY_, 0, tileW, tileH, 1};
        texture_->copyBytes(rgba.data(), srcBpr, region);

        // Patch metrics with the assigned UV rect (normalized). The UV
        // addresses the *whole* integer tile — the same `tileW × tileH`
        // the upload flip reverses and the render quad covers. The
        // `ceil` row/column is transparent distance field that is part
        // of the tile uniformly, so it never displaces the glyph.
        // Mixing a fractional content sub-rect with the integer flip is
        // what produced the per-glyph mis-positioning.
        const float invDim = 1.f / static_cast<float>(kAtlasDim);
        AtlasGlyph entry = out.metrics;
        entry.pxW = static_cast<std::uint16_t>(tileW);
        entry.pxH = static_cast<std::uint16_t>(tileH);
        entry.u0 = static_cast<float>(cursorX_) * invDim;
        entry.v0 = static_cast<float>(cursorY_) * invDim;
        entry.u1 = static_cast<float>(cursorX_ + tileW) * invDim;
        entry.v1 = static_cast<float>(cursorY_ + tileH) * invDim;
        glyphs_.emplace(glyphId, entry);

        if(textTraceEnabled()){
            std::cout << "[wtk-text] GlyphAtlas: rasterized glyph " << glyphId
                      << " into " << tileW << "x" << tileH << " tile at ("
                      << cursorX_ << "," << cursorY_ << "), advance="
                      << entry.advance << std::endl;

            // Read the whole atlas texture back and write it to a PPM so
            // the packed tiles can be inspected directly — orientation
            // of stored glyphs, whether tiles abut with no gutter, and
            // whether the shelf packing matches what the UVs assume.
            // Overwrites each call; the final write has the most glyphs.
            std::vector<std::uint8_t> px(
                static_cast<std::size_t>(kAtlasDim) * kAtlasDim * 4);
            texture_->getBytes(px.data(), static_cast<std::size_t>(kAtlasDim) * 4);
            if(FILE *f = std::fopen("/tmp/wtk_glyph_atlas.ppm", "wb")){
                std::fprintf(f, "P6\n%u %u\n255\n", kAtlasDim, kAtlasDim);
                for(std::size_t i = 0;
                    i < static_cast<std::size_t>(kAtlasDim) * kAtlasDim; ++i){
                    std::fputc(px[i * 4 + 0], f);
                    std::fputc(px[i * 4 + 1], f);
                    std::fputc(px[i * 4 + 2], f);
                }
                std::fclose(f);
                std::cout << "[wtk-text] GlyphAtlas: dumped atlas to "
                             "/tmp/wtk_glyph_atlas.ppm" << std::endl;
            }
        }

        cursorX_ += tileW;
        rowH_     = std::max(rowH_, tileH);
        return true;
    }

}
