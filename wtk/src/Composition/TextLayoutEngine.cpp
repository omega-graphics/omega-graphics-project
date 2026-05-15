// Phase-2 implementation of `TextLayoutEngine::layout`.
//
// Owns line composition (mandatory breaks only — Phase 2 has no
// wrap), horizontal + vertical alignment, and baseline placement.
// Delegates shaping to the injected `ITextShaper`, called once per
// line. No platform calls inside this file — all platform-specific
// behavior is on the other side of `ITextShaper`.

#include "omegaWTK/Composition/TextLayoutEngine.h"

#include "omega-common/unicode.h"

#include <algorithm>

namespace OmegaWTK::Composition {

    namespace {

        float horizontalStartX(const Composition::Rect & rect,
                               float totalAdvance,
                               TextLayoutDescriptor::Alignment alignment){
            switch(alignment){
                case TextLayoutDescriptor::MiddleUpper:
                case TextLayoutDescriptor::MiddleCenter:
                case TextLayoutDescriptor::MiddleLower:
                    return (rect.w - totalAdvance) * 0.5f;
                case TextLayoutDescriptor::RightUpper:
                case TextLayoutDescriptor::RightCenter:
                case TextLayoutDescriptor::RightLower:
                    return rect.w - totalAdvance;
                default:
                    return 0.f;
            }
        }

        // Y-down distance from rect top to the *first* line's baseline.
        // Phase-2 multi-line contract: the block height is
        // `lineCount * lineHeight()` (no inter-line padding beyond
        // lineGap, no half-leading at the bottom — close enough for
        // first-cut and matches how the Pango/Cairo bitmap path
        // behaves). When the rect is shorter than the block, vertical
        // alignment collapses to "top" (no negative extra) so glyphs
        // never escape upward past the rect.
        float firstBaselineY(const Composition::Rect & rect,
                             const FontMetrics & metrics,
                             std::size_t lineCount,
                             TextLayoutDescriptor::Alignment alignment){
            const float blockH = metrics.lineHeight() * static_cast<float>(lineCount);
            const float extra  = rect.h - blockH;
            switch(alignment){
                case TextLayoutDescriptor::LeftCenter:
                case TextLayoutDescriptor::MiddleCenter:
                case TextLayoutDescriptor::RightCenter:
                    return metrics.ascent + (extra > 0.f ? extra * 0.5f : 0.f);
                case TextLayoutDescriptor::LeftLower:
                case TextLayoutDescriptor::MiddleLower:
                case TextLayoutDescriptor::RightLower:
                    return metrics.ascent + (extra > 0.f ? extra : 0.f);
                default:
                    return metrics.ascent;
            }
        }

        OmegaCommon::UniString substringUtf16(const OmegaCommon::UniString & src,
                                              std::int32_t start, std::int32_t end){
            const auto * buf = src.getBuffer();
            const auto len = src.length();
            if(buf == nullptr || start < 0 || end > len || end <= start){
                return {};
            }
            // UniString stores UTF-16; reconstruct via the UTF-32
            // factory so the public surface stays the only entry point.
            // For Phase 2 (Latin) this is one allocation per line — a
            // minor cost we can address in Phase 7 once the public
            // surface gains a `substr`.
            std::vector<OmegaCommon::Unicode32Char> utf32;
            utf32.reserve(static_cast<std::size_t>(end - start));
            for(std::int32_t i = start; i < end; ++i){
                utf32.push_back(static_cast<OmegaCommon::Unicode32Char>(buf[i]));
            }
            return OmegaCommon::UniString::fromUTF32(utf32.data(),
                                                    static_cast<std::int32_t>(utf32.size()));
        }

        // Trim a *trailing* mandatory-break character from a line's
        // text. ICU's line iterator places the boundary *after* the
        // `\n`, so the segment we're given includes it; shaping that
        // character would produce a visible (or zero-width) glyph for
        // the newline that the layout doesn't want.
        std::int32_t trimTrailingMandatoryBreak(const OmegaCommon::UniString & src,
                                                std::int32_t start, std::int32_t end){
            const auto * buf = src.getBuffer();
            if(buf == nullptr || end <= start) return end;
            const auto c = buf[end - 1];
            if(c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029){
                // Also strip a preceding CR in a CRLF pair.
                if(c == u'\n' && end - 2 >= start && buf[end - 2] == u'\r'){
                    return end - 2;
                }
                return end - 1;
            }
            return end;
        }

    }

    LayoutResult TextLayoutEngine::layout(const OmegaCommon::UniString & text,
                                          Core::SharedPtr<Font> font,
                                          const FontMetrics & metrics,
                                          const Composition::Rect & rect,
                                          const TextLayoutDescriptor & desc,
                                          ITextShaper & shaper){
        LayoutResult result;
        if(font == nullptr || text.length() == 0){
            return result;
        }

        // Walk mandatory line breaks. The line iterator emits
        // boundaries at every break *opportunity* (mandatory + soft);
        // Phase 2's no-wrap contract only acts on mandatory ones.
        // Soft boundaries are still walked so a Phase-3 wrap pass can
        // reuse the same iterator without re-creating it.
        struct Segment { std::int32_t start, end; };
        std::vector<Segment> segments;
        segments.reserve(4);
        {
            OmegaCommon::BreakIterator it(OmegaCommon::BreakIterator::Type::Line, text);
            std::int32_t segStart = 0;
            (void)it.first(); // Position iterator at 0.
            std::int32_t pos;
            while((pos = it.next()) != OmegaCommon::BreakIterator::DONE){
                if(it.isMandatoryBreakAt(pos)){
                    segments.push_back({segStart, pos});
                    segStart = pos;
                }
            }
            if(segStart < text.length()){
                segments.push_back({segStart, text.length()});
            }
            if(segments.empty()){
                // Either an ICU init failure (DONE iterator) or text
                // that contains no breaks at all — treat as a single
                // segment so the shape pass still runs.
                segments.push_back({0, text.length()});
            }
        }

        // Per-line shape pass (Phase 3). For each line, we run a
        // `BidiParagraph` over the line text to split it into visual
        // runs, then sub-segment each run by script. Each (visual run
        // × script sub-run) pair becomes one shape call with the
        // appropriate direction + script tag. The output glyphs from
        // HB-RTL come back in visual order already; we just append
        // each shape call's output to the line in visual order.
        struct ShapedLine {
            OmegaCommon::Vector<ShaperGlyph> glyphs;
            float totalAdvance = 0.f;
        };
        std::vector<ShapedLine> lines;
        lines.reserve(segments.size());

        for(const auto & seg : segments){
            const std::int32_t trimmedEnd =
                trimTrailingMandatoryBreak(text, seg.start, seg.end);
            ShapedLine line;
            if(trimmedEnd > seg.start){
                OmegaCommon::UniString lineText =
                    substringUtf16(text, seg.start, trimmedEnd);

                OmegaCommon::BidiParagraph bidi(lineText);
                const std::int32_t runs = bidi.runCount();
                for(std::int32_t r = 0; r < runs; ++r){
                    auto visRun = bidi.getVisualRun(r);
                    if(visRun.length <= 0) continue;

                    // Collect script sub-runs in logical order; for an
                    // RTL bidi run we walk them back-to-front so the
                    // visual concatenation places the highest-logical
                    // script-run leftmost (HB-RTL within each sub-run
                    // handles its own glyph reversal).
                    std::vector<OmegaCommon::ScriptRunIterator::Run> scriptRuns;
                    {
                        OmegaCommon::ScriptRunIterator sri(
                            lineText, visRun.logicalStart, visRun.length);
                        OmegaCommon::ScriptRunIterator::Run sr;
                        while(sri.next(sr)){
                            scriptRuns.push_back(sr);
                        }
                    }

                    auto shapeScriptRun =
                        [&](const OmegaCommon::ScriptRunIterator::Run & sr){
                            ShaperInput input;
                            input.text = substringUtf16(
                                lineText, sr.start, sr.start + sr.length);
                            input.font = font;
                            input.rightToLeft = visRun.rightToLeft;
                            input.script = sr.script;
                            auto glyphs = shaper.shapeRun(input);
                            for(auto & g : glyphs){
                                line.glyphs.push_back(g);
                                line.totalAdvance += g.advance;
                            }
                        };

                    if(visRun.rightToLeft){
                        for(auto it = scriptRuns.rbegin();
                            it != scriptRuns.rend(); ++it){
                            shapeScriptRun(*it);
                        }
                    } else {
                        for(const auto & sr : scriptRuns){
                            shapeScriptRun(sr);
                        }
                    }
                }
            }
            lines.push_back(std::move(line));
        }

        if(lines.empty()){
            return result;
        }

        // Shaper failure / nothing-to-draw guard: when every line
        // came back empty (e.g. the shaper couldn't shape any of the
        // input), produce an empty `LayoutResult` so callers can
        // distinguish "nothing visible" from a multi-line block with
        // legitimately blank lines. A blank line caused by an
        // explicit `\n\n` keeps its baseline below (one of the other
        // lines will have glyphs).
        std::size_t glyphsAcrossAllLines = 0;
        for(const auto & line : lines){
            glyphsAcrossAllLines += line.glyphs.size();
        }
        if(glyphsAcrossAllLines == 0){
            return result;
        }

        const float firstBaseline =
            firstBaselineY(rect, metrics, lines.size(), desc.alignment);
        const float lineStride = metrics.lineHeight();

        std::size_t totalGlyphCount = 0;
        for(const auto & line : lines){
            totalGlyphCount += line.glyphs.size();
        }
        result.glyphs.reserve(totalGlyphCount);
        result.lineBaselines.reserve(lines.size());

        for(std::size_t li = 0; li < lines.size(); ++li){
            const auto & line = lines[li];
            const float baseline = firstBaseline + static_cast<float>(li) * lineStride;
            result.lineBaselines.push_back(baseline);

            if(line.glyphs.empty()){
                continue; // Blank line — baseline stride still advanced.
            }

            const float startX = std::max(
                horizontalStartX(rect, line.totalAdvance, desc.alignment), 0.f);
            float penX = startX;
            for(const auto & g : line.glyphs){
                ShapedGlyph out;
                out.glyphId      = g.glyphId;
                out.resolvedFont = font;
                out.canvasX      = penX + g.xOffset;
                out.canvasY      = baseline + g.yOffset;
                result.glyphs.push_back(out);
                penX += g.advance;
            }
        }

        result.layoutHeight = lineStride * static_cast<float>(lines.size());
        return result;
    }

}
