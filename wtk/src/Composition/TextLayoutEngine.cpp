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

    namespace {

        // Decode the codepoint starting at UTF-16 offset `idx` in `text`.
        // Handles surrogate pairs; unpaired surrogates degrade to their
        // raw 16-bit value (good enough for the fallback lookup, since
        // FontConfig / CTFontCreateForString treat lone surrogates as
        // unmappable and return null).
        std::uint32_t codepointAt(const OmegaCommon::UniString & text,
                                  std::int32_t idx){
            const auto * buf = text.getBuffer();
            const auto len = text.length();
            if(buf == nullptr || idx < 0 || idx >= len) return 0;
            const char16_t u = buf[idx];
            if(u >= 0xD800 && u <= 0xDBFF && idx + 1 < len){
                const char16_t u2 = buf[idx + 1];
                if(u2 >= 0xDC00 && u2 <= 0xDFFF){
                    return 0x10000u
                        + (static_cast<std::uint32_t>(u - 0xD800) << 10)
                        + static_cast<std::uint32_t>(u2 - 0xDC00);
                }
            }
            return static_cast<std::uint32_t>(u);
        }

    }

    LayoutResult TextLayoutEngine::layout(const OmegaCommon::UniString & text,
                                          Core::SharedPtr<Font> font,
                                          const FontMetrics & metrics,
                                          const Composition::Rect & rect,
                                          const TextLayoutDescriptor & desc,
                                          ITextShaper & shaper,
                                          IFontFallback * fallback){
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
        //
        // Phase 3.5: track per-glyph absolute cluster offset within
        // `lineText` so the wrap pass can map break opportunities to
        // cumulative advance. Also track whether the line is a single
        // bidi direction — mixed-bidi wrap (re-shape per wrap-line)
        // is deferred.
        struct ShapedLine {
            OmegaCommon::Vector<ShaperGlyph> glyphs;
            std::vector<std::int32_t> clusters;     // parallel to glyphs
            // Phase 4: per-glyph resolved font. `nullptr` here means
            // "use the requested face" (the layout engine substitutes
            // the requested `font` at emit time). Fallback runs that
            // pulled in a substitute face fill this in directly so
            // the draw path groups them into separate `TextSubRun`s
            // and the right per-font `GlyphAtlas` services them.
            std::vector<Core::SharedPtr<Font>> resolvedFonts;
            OmegaCommon::UniString lineText;
            float totalAdvance = 0.f;
            bool singleDirection = true;
            bool isRTL = false;
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
                line.lineText = lineText;

                OmegaCommon::BidiParagraph bidi(lineText);
                const std::int32_t runs = bidi.runCount();
                if(runs != 1){
                    line.singleDirection = false;
                }
                for(std::int32_t r = 0; r < runs; ++r){
                    auto visRun = bidi.getVisualRun(r);
                    if(visRun.length <= 0) continue;
                    if(runs == 1) line.isRTL = visRun.rightToLeft;

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

                    auto appendGlyph =
                        [&](const ShaperGlyph & g,
                            std::int32_t absCluster,
                            const Core::SharedPtr<Font> & resolved){
                            line.glyphs.push_back(g);
                            line.clusters.push_back(absCluster);
                            line.resolvedFonts.push_back(resolved);
                            line.totalAdvance += g.advance;
                        };

                    auto shapeScriptRun =
                        [&](const OmegaCommon::ScriptRunIterator::Run & sr){
                            ShaperInput input;
                            input.text = substringUtf16(
                                lineText, sr.start, sr.start + sr.length);
                            input.font = font;
                            input.rightToLeft = visRun.rightToLeft;
                            input.script = sr.script;
                            auto glyphs = shaper.shapeRun(input);
                            if(glyphs.empty()){
                                return;
                            }

                            // Phase 4: per-cluster fallback orchestration.
                            // Walk the shaper output by cluster groups
                            // (contiguous glyphs sharing the same
                            // `cluster` value); whenever any glyph in a
                            // group is `.notdef`, look up a fallback face
                            // for the cluster's first codepoint and
                            // re-shape just that cluster's source text
                            // with the fallback. Fallback glyphs are
                            // spliced into the line in place of the
                            // original `.notdef`s with `resolvedFont` set
                            // to the substitute so the draw path emits
                            // them through the right atlas.
                            //
                            // Without fallback (driver `nullptr` or no
                            // substitute available) the original `.notdef`
                            // glyphs flow through with `resolvedFont =
                            // nullptr` and the emit pass substitutes
                            // the requested face.

                            // Build a sorted-unique cluster table so we
                            // can resolve "next cluster boundary after
                            // c" in O(log n) — direction-agnostic.
                            std::vector<std::int32_t> sortedClusters;
                            sortedClusters.reserve(glyphs.size());
                            for(const auto & g : glyphs){
                                sortedClusters.push_back(g.cluster);
                            }
                            std::sort(sortedClusters.begin(),
                                      sortedClusters.end());
                            sortedClusters.erase(
                                std::unique(sortedClusters.begin(),
                                            sortedClusters.end()),
                                sortedClusters.end());
                            auto clusterExtentEnd =
                                [&](std::int32_t c) -> std::int32_t {
                                    // First sorted cluster strictly
                                    // greater than `c`; if none, the
                                    // cluster runs to the end of the
                                    // shaper input.
                                    auto it = std::upper_bound(
                                        sortedClusters.begin(),
                                        sortedClusters.end(), c);
                                    return it == sortedClusters.end()
                                        ? input.text.length()
                                        : *it;
                                };

                            std::size_t i = 0;
                            while(i < glyphs.size()){
                                // Cluster group: contiguous glyphs
                                // sharing `cluster`. (Shapers emit them
                                // adjacent in visual order — true for
                                // both HarfBuzz and Core Text.)
                                const std::int32_t c = glyphs[i].cluster;
                                std::size_t j = i + 1;
                                bool hasNotdef = (glyphs[i].glyphId == 0);
                                while(j < glyphs.size()
                                      && glyphs[j].cluster == c){
                                    if(glyphs[j].glyphId == 0){
                                        hasNotdef = true;
                                    }
                                    ++j;
                                }

                                Core::SharedPtr<Font> fbFont;
                                if(hasNotdef && fallback != nullptr){
                                    const std::int32_t end =
                                        clusterExtentEnd(c);
                                    const std::uint32_t cp = codepointAt(
                                        input.text, c);
                                    fbFont = fallback->fallbackForCodepoint(
                                        font, cp);
                                    if(fbFont != nullptr && end > c){
                                        ShaperInput fbInput;
                                        fbInput.text = substringUtf16(
                                            input.text, c, end);
                                        fbInput.font = fbFont;
                                        fbInput.rightToLeft = input.rightToLeft;
                                        fbInput.script = input.script;
                                        auto fbGlyphs = shaper.shapeRun(fbInput);
                                        if(!fbGlyphs.empty()){
                                            for(const auto & fg : fbGlyphs){
                                                appendGlyph(fg,
                                                    sr.start + c + fg.cluster,
                                                    fbFont);
                                            }
                                            i = j;
                                            continue;
                                        }
                                    }
                                }

                                // No fallback (or fallback shape failed
                                // / produced nothing): emit the original
                                // cluster glyphs verbatim against the
                                // requested face.
                                for(std::size_t k = i; k < j; ++k){
                                    appendGlyph(glyphs[k],
                                                sr.start + glyphs[k].cluster,
                                                nullptr);
                                }
                                i = j;
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

        // Phase 3.5: wrap pass. For each segment-level line, split into
        // one or more wrap-lines using `BreakIterator(Line)` soft
        // opportunities and the per-glyph cluster offsets. Single
        // bidi-direction only — mixed-bidi lines fall through
        // unchanged (overflow), and re-shape-per-wrap-line is a
        // deferred follow-up. After this pass, every entry in `lines`
        // is one physical wrap-line.
        if(desc.wrapping != TextLayoutDescriptor::None && rect.w > 0.f){
            std::vector<ShapedLine> wrapped;
            wrapped.reserve(lines.size());
            for(auto & line : lines){
                if(!line.singleDirection ||
                   line.totalAdvance <= rect.w ||
                   line.glyphs.empty()){
                    wrapped.push_back(std::move(line));
                    continue;
                }
                const auto * buf = line.lineText.getBuffer();
                const std::int32_t lineLen = line.lineText.length();
                if(buf == nullptr || lineLen <= 0){
                    wrapped.push_back(std::move(line));
                    continue;
                }
                auto isWS = [&](std::int32_t i) -> bool {
                    if(i < 0 || i >= lineLen) return false;
                    const char16_t c = buf[i];
                    return c == u' ' || c == u'\t' || c == 0x00A0;
                };

                // Break opportunities (cluster offsets within
                // `lineText`). The set depends on wrap mode:
                //  - WrapByWord: ICU's line iterator gives us soft +
                //    mandatory boundaries; mandatory already split
                //    upstream, so what's left here is soft (word-end).
                //  - WrapByCharacter: every distinct cluster boundary
                //    in the shaped output. This naturally respects
                //    multi-glyph clusters (combining marks, ligatures)
                //    while still allowing breaks at every code-unit
                //    boundary in plain text.
                std::vector<std::int32_t> breaks;
                breaks.push_back(0);
                if(desc.wrapping == TextLayoutDescriptor::WrapByCharacter){
                    std::vector<std::int32_t> uniq = line.clusters;
                    std::sort(uniq.begin(), uniq.end());
                    uniq.erase(std::unique(uniq.begin(), uniq.end()),
                               uniq.end());
                    for(std::int32_t c : uniq){
                        if(c > 0 && c < lineLen) breaks.push_back(c);
                    }
                    breaks.push_back(lineLen);
                } else {
                    OmegaCommon::BreakIterator it(
                        OmegaCommon::BreakIterator::Type::Line,
                        line.lineText);
                    (void)it.first();
                    std::int32_t pos;
                    while((pos = it.next()) !=
                          OmegaCommon::BreakIterator::DONE){
                        breaks.push_back(pos);
                    }
                    if(breaks.back() != lineLen){
                        breaks.push_back(lineLen);
                    }
                }

                // Build a cluster-sorted index over the glyphs so we
                // can compute width([0, P)) in O(log n) regardless of
                // the visual order (LTR ascending vs RTL descending).
                // For LTR the visual order *is* the cluster order so
                // this is one extra allocation we could short-circuit
                // — keeping the unified path simple for now.
                std::vector<std::size_t> bySortedCluster(line.glyphs.size());
                for(std::size_t i = 0; i < bySortedCluster.size(); ++i){
                    bySortedCluster[i] = i;
                }
                std::sort(bySortedCluster.begin(), bySortedCluster.end(),
                    [&](std::size_t a, std::size_t b){
                        return line.clusters[a] < line.clusters[b];
                    });
                std::vector<float> widthPrefix(line.glyphs.size() + 1, 0.f);
                for(std::size_t i = 0; i < bySortedCluster.size(); ++i){
                    widthPrefix[i + 1] = widthPrefix[i]
                        + line.glyphs[bySortedCluster[i]].advance;
                }
                auto widthBeforeCluster = [&](std::int32_t P) -> float {
                    std::size_t lo = 0, hi = bySortedCluster.size();
                    while(lo < hi){
                        const std::size_t mid = (lo + hi) / 2;
                        if(line.clusters[bySortedCluster[mid]] < P){
                            lo = mid + 1;
                        } else {
                            hi = mid;
                        }
                    }
                    return widthPrefix[lo];
                };

                auto emitWrapLine =
                    [&](std::int32_t startCluster,
                        std::int32_t endCluster) -> ShapedLine {
                    ShapedLine w;
                    w.singleDirection = true;
                    w.isRTL = line.isRTL;
                    w.lineText = line.lineText; // shared (read-only past this)
                    for(std::size_t i = 0; i < line.glyphs.size(); ++i){
                        const std::int32_t c = line.clusters[i];
                        if(c >= startCluster && c < endCluster){
                            w.glyphs.push_back(line.glyphs[i]);
                            w.clusters.push_back(c);
                            w.resolvedFonts.push_back(line.resolvedFonts[i]);
                            w.totalAdvance += line.glyphs[i].advance;
                        }
                    }
                    return w;
                };

                std::int32_t lineStart = 0;
                std::int32_t lastFit   = 0;
                float startWidth = widthBeforeCluster(lineStart);
                for(std::size_t bi = 1; bi < breaks.size(); ++bi){
                    const std::int32_t P = breaks[bi];
                    // Trailing-whitespace exemption (CSS / Pango): skip
                    // whitespace at the end of the candidate when
                    // checking fit, but keep its advance on the
                    // outgoing wrap-line so the next line starts at P
                    // (= first non-space code unit of the next word).
                    std::int32_t Pcut = P;
                    while(Pcut > lineStart && isWS(Pcut - 1)){
                        Pcut--;
                    }
                    const float fitted =
                        widthBeforeCluster(Pcut) - startWidth;
                    if(fitted <= rect.w){
                        lastFit = P;
                        continue;
                    }
                    // Doesn't fit. Emit lastFit if non-trivial,
                    // otherwise force-break-or-overflow.
                    if(lastFit > lineStart){
                        wrapped.push_back(emitWrapLine(lineStart, lastFit));
                        lineStart  = lastFit;
                        startWidth = widthBeforeCluster(lineStart);
                        // Reevaluate current P against the new line.
                        std::int32_t Pcut2 = P;
                        while(Pcut2 > lineStart && isWS(Pcut2 - 1)){
                            Pcut2--;
                        }
                        const float fitted2 =
                            widthBeforeCluster(Pcut2) - startWidth;
                        if(fitted2 <= rect.w){
                            lastFit = P;
                        } else {
                            // Single segment still too wide. Accept
                            // overflow (WrapByWord) or force-break at
                            // this candidate (WrapByCharacter).
                            lastFit = P;
                        }
                    } else {
                        // Single segment from the start doesn't fit.
                        // For WrapByCharacter, this is the per-character
                        // candidate path — emit a wrap-line spanning
                        // [lineStart, P) and continue.
                        if(desc.wrapping ==
                           TextLayoutDescriptor::WrapByCharacter){
                            wrapped.push_back(emitWrapLine(lineStart, P));
                            lineStart  = P;
                            startWidth = widthBeforeCluster(lineStart);
                            lastFit    = P;
                        } else {
                            // WrapByWord can't break inside an
                            // oversized word: accept overflow.
                            lastFit = P;
                        }
                    }
                }
                // Final wrap-line covers [lineStart, lineLen).
                if(lineStart < lineLen){
                    wrapped.push_back(emitWrapLine(lineStart, lineLen));
                }
            }
            lines = std::move(wrapped);
        }

        // Phase 3.5: `lineLimit` truncates after wrap. The last visible
        // wrap-line is not ellipsized (deferred).
        if(desc.lineLimit > 0 && lines.size() > desc.lineLimit){
            lines.resize(desc.lineLimit);
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

        float maxLineAdvance = 0.f;
        for(std::size_t li = 0; li < lines.size(); ++li){
            const auto & line = lines[li];
            const float baseline = firstBaseline + static_cast<float>(li) * lineStride;
            result.lineBaselines.push_back(baseline);
            // Intrinsic width is the widest visible line, tracked here across
            // the same set of lines paint emits (post-wrap, post-lineLimit).
            maxLineAdvance = std::max(maxLineAdvance, line.totalAdvance);

            if(line.glyphs.empty()){
                continue; // Blank line — baseline stride still advanced.
            }

            const float startX = std::max(
                horizontalStartX(rect, line.totalAdvance, desc.alignment), 0.f);
            float penX = startX;
            for(std::size_t gi = 0; gi < line.glyphs.size(); ++gi){
                const auto & g = line.glyphs[gi];
                ShapedGlyph out;
                out.glyphId = g.glyphId;
                // Phase 4: per-glyph resolved font (fallback may have
                // substituted a different face for this cluster). A
                // null entry means "use the requested face" — the
                // common case when no fallback driver is supplied or
                // the cluster didn't trigger fallback.
                out.resolvedFont = (gi < line.resolvedFonts.size()
                                    && line.resolvedFonts[gi] != nullptr)
                    ? line.resolvedFonts[gi]
                    : font;
                out.canvasX = penX + g.xOffset;
                out.canvasY = baseline + g.yOffset;
                result.glyphs.push_back(out);
                penX += g.advance;
            }
        }

        result.layoutHeight = lineStride * static_cast<float>(lines.size());
        result.layoutWidth = maxLineAdvance;
        return result;
    }

}
