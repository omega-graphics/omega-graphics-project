#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"

#include <unordered_map>
#include <utility>

namespace OmegaWTK::Composition {

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
    return out;
}

}
