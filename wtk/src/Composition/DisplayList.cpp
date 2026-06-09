#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"

#include <unordered_map>
#include <utility>

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
#include "TextShapingCache.h"
#include "backend/TessellationCache.h"   // packRGBA, bucketDim
#endif

namespace OmegaWTK::Composition {

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
namespace {

    /// CPU-side byte cost estimate for telemetry. Does not include the
    /// GPU texture pixels held alive by `BitmapBlit::texture` — those
    /// live in GTE and are reference-counted independently.
    std::size_t estimateBytes(const ShapedTextRun & run){
        std::size_t bytes = sizeof(ShapedTextRun);
        for(const auto & sr : run.msdfSubRuns){
            bytes += sizeof(TextSubRun);
            bytes += sr.glyphIds.size() * sizeof(std::uint32_t);
            bytes += sr.positions.size() * sizeof(Composition::Point2D);
        }
        bytes += run.bitmapBlits.size() * sizeof(ShapedTextRun::BitmapBlit);
        return bytes;
    }

    void refreshMsdfResidency(ShapedTextRun & run){
        for(auto & sr : run.msdfSubRuns){
            if(sr.resolvedFont != nullptr && !sr.glyphIds.empty()){
                sr.resolvedFont->ensureGlyphsResident(sr.glyphIds);
            }
        }
    }

}
#endif

// Tier 4 §4.2: rehomed verbatim out of the deleted `Canvas.cpp`. Pure
// shaping helper shared by every DisplayList-emitting paint path
// (UIView::update, SVGView::paint) to build `DrawOp::TextRun` /
// `DrawOp::Bitmap` text ops. No Canvas / GPU state touched.
ShapedTextRun shapeTextForDisplayList(
    const OmegaCommon::UniString & text,
    const Core::SharedPtr<Font> & font,
    const Composition::Rect & rect,
    const Composition::Color & color,
    const TextLayoutDescriptor & layoutDesc,
    float renderScale){
    ShapedTextRun out {};
    if(font == nullptr || text.length() == 0 || rect.w <= 0.f || rect.h <= 0.f){
        return out;
    }

    auto * engine = FontEngine::inst();
    auto * shaper = (engine != nullptr) ? engine->shaper() : nullptr;
    if(engine == nullptr || shaper == nullptr){
        return out;
    }

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
    // Phase G.2: cache lookup. Build the key, probe the process-wide
    // singleton. Hit → re-run `ensureGlyphsResident` on every MSDF
    // sub-run (atlas residency is per-FontEngine state and may have
    // been evicted between frames) and return the cached run.
    TextShapingCacheKey cacheKey;
    cacheKey.textHash        = hashUniString(text);
    cacheKey.textLength      = static_cast<std::uint32_t>(text.length());
    cacheKey.layoutHash      = hashLayoutDescriptor(layoutDesc);
    cacheKey.fontId          = font.get();
    cacheKey.fontSize        = font->desc.size;
    cacheKey.wBucket         = bucketDim(rect.w);
    cacheKey.hBucket         = bucketDim(rect.h);
    cacheKey.renderScaleBits = floatBits(renderScale);
    cacheKey.colorRGBA       = packRGBA(color.r, color.g, color.b, color.a);

    if(auto cached = TextShapingCache::inst().find(cacheKey)){
        refreshMsdfResidency(*cached);
        return std::move(*cached);
    }
#endif

    const FontMetrics metrics = font->getMetrics();
    auto * fallback = engine->fallback();
    auto layoutResult = TextLayoutEngine::layout(
        text, font, metrics, rect, layoutDesc, *shaper, fallback);
    if(layoutResult.glyphs.empty()){
        return out;
    }

    // Group laid-out glyphs by resolved font into sub-runs (one per face).
    std::unordered_map<Font *, std::size_t> subRunIndex;
    OmegaCommon::Vector<TextSubRun> subRuns;
    for(const auto & g : layoutResult.glyphs){
        if(g.resolvedFont == nullptr) continue;
        auto it = subRunIndex.find(g.resolvedFont.get());
        if(it == subRunIndex.end()){
            TextSubRun sr;
            sr.resolvedFont = g.resolvedFont;
            subRuns.push_back(std::move(sr));
            it = subRunIndex.emplace(g.resolvedFont.get(),
                                     subRuns.size() - 1).first;
        }
        auto & sr = subRuns[it->second];
        sr.glyphIds.push_back(g.glyphId);
        sr.positions.push_back(Composition::Point2D{g.canvasX, g.canvasY});
    }

    // Partition by mode: MSDF sub-runs ride the atlas pipeline (residency
    // ensured here, off the compositor frame pass); BitmapFallback
    // sub-runs each rasterize to their own texture and ride the bitmap
    // blit path.
    for(auto & sr : subRuns){
        if(sr.resolvedFont == nullptr || sr.glyphIds.empty()) continue;
        if(sr.resolvedFont->mode() == Font::Mode::MSDF){
            sr.resolvedFont->ensureGlyphsResident(sr.glyphIds);
            out.msdfSubRuns.push_back(std::move(sr));
        } else {
            auto bmp = engine->rasterizeSubRunToTexture(
                sr, rect, color, renderScale);
            if(bmp.texture != nullptr){
                out.bitmapBlits.push_back({std::move(bmp.texture),
                                           std::move(bmp.fence)});
            }
        }
    }

#ifdef OMEGAWTK_CONTENT_CACHE_ENABLED
    // Cache the just-shaped run for the next frame. Copy in (not move)
    // so the caller still gets the run it expects from the function
    // return — the cache holds an independent copy.
    if(!out.msdfSubRuns.empty() || !out.bitmapBlits.empty()){
        TextShapingCache::inst().insert(std::move(cacheKey), out, estimateBytes(out));
    }
#endif

    return out;
}

}
