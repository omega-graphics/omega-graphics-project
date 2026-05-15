// WTK-owned text layout engine (Text-Layout-Engine-Plan.md, Phase 1).
//
// Phase 1 ships the skeleton: a `TextLayoutEngine::layout` entry point,
// the data structures it produces (`ShapedGlyph`, `LayoutResult`), and
// a dependency-injection seam for shaping (`ITextShaper`). Real shaper
// implementations (HarfBuzz / Core Text / IDWriteTextAnalyzer) land in
// Phase 2 / 5 / 6 respectively; this file knows nothing about
// platforms.

#ifndef OMEGAWTK_COMPOSITION_TEXTLAYOUTENGINE_H
#define OMEGAWTK_COMPOSITION_TEXTLAYOUTENGINE_H

#include "omegaWTK/Core/Core.h"
#include "omega-common/unicode.h"
#include "Geometry.h"
#include "FontEngine.h"

#include <cstdint>

namespace OmegaWTK::Composition {

    /// A positioned glyph in canvas space — the layout engine's
    /// public output unit. `canvasX/canvasY` is the pen-origin
    /// (baseline) position; `resolvedFont` is the face the glyph
    /// belongs to after the layout engine's font-fallback pass
    /// (Phase 1: always the input `font`, since fallback lands in
    /// Phase 4). The render path looks `glyphId` up against
    /// `resolvedFont->atlas()` for the MSDF tile.
    ///
    /// Y convention matches the current MSDF text path: `canvasY` is
    /// a Y-down distance from the owning rect's logical top. A
    /// long-term migration to a bottom-left-origin canvas (per the
    /// plan annotation) is a one-place flip on this field.
    struct OMEGAWTK_EXPORT ShapedGlyph {
        std::uint32_t glyphId = 0;
        Core::SharedPtr<Font> resolvedFont;
        float canvasX = 0.f;
        float canvasY = 0.f;
    };

    /// Layout-engine result for one `Canvas::drawText` call.
    struct OMEGAWTK_EXPORT LayoutResult {
        /// Visible glyphs in logical order. Empty for whitespace-only
        /// or empty input.
        OmegaCommon::Vector<ShapedGlyph> glyphs;
        /// Total laid-out height: top of the first line to bottom of
        /// the last (ascent + descent + lineGap per line, with no
        /// inter-line padding in Phase 1's single-line case).
        float layoutHeight = 0.f;
        /// Baseline Y per line, in the same convention as
        /// `ShapedGlyph::canvasY`. `lineBaselines.size()` is the
        /// laid-out line count (always 1 in Phase 1).
        OmegaCommon::Vector<float> lineBaselines;
    };

    /// One glyph produced by a shaper. Phase 1 contract: shapers
    /// receive a substring + font + direction and return one of
    /// these per output glyph (not per cluster — kerning / ligatures
    /// already applied). `xOffset / yOffset` are mark-positioning
    /// deltas off the cluster's pen origin in pixels.
    struct OMEGAWTK_EXPORT ShaperGlyph {
        std::uint32_t glyphId = 0;
        float advance = 0.f;
        float xOffset = 0.f;
        float yOffset = 0.f;
    };

    /// Single-shaper-call input. Phase 3: carries an explicit script
    /// tag too, so the shaper sets HarfBuzz's segment properties
    /// without re-guessing from the substring (the layout engine
    /// already segmented by script before calling).
    struct OMEGAWTK_EXPORT ShaperInput {
        OmegaCommon::UniString text;
        Core::SharedPtr<Font> font;
        bool rightToLeft = false;
        /// ICU `UScriptCode` value cast to `int32_t` so this header
        /// stays free of `unicode/uscript.h`. `0` is `USCRIPT_COMMON`
        /// — the shaper will fall back to HB's `hb_buffer_guess_segment_properties`
        /// for that value (Phase 1 / 2 behaviour). Non-zero values map
        /// directly through `uscript_getShortName` →
        /// `hb_script_from_iso15924_tag`.
        std::int32_t script = 0;
    };

    /// Pluggable shaper interface — the Phase-1 dependency-injection
    /// seam. The layout engine drives one of these per script-run
    /// (per cluster of resolved face after fallback). Production
    /// implementations land in Phases 2 / 5 / 6; Phase-1 tests inject
    /// a mock returning canned `ShaperGlyph`s.
    class OMEGAWTK_EXPORT ITextShaper {
    public:
        virtual ~ITextShaper() = default;
        /// Shape one logical run. The shaper is NOT given a layout
        /// rect or wrap width — line composition belongs to the
        /// layout engine.
        virtual OmegaCommon::Vector<ShaperGlyph>
        shapeRun(const ShaperInput & input) = 0;
    };

    /// `FontMetrics` (ascent / descent / lineGap) lives in
    /// `FontEngine.h` as of Phase 2 so `Font::getMetrics()` can
    /// return it directly. Still in this header by name via the
    /// `FontEngine.h` include above.

    /// WTK-owned text layout pipeline. See `Text-Layout-Engine-Plan.md`.
    ///
    /// Phase 2 scope:
    /// - Single-direction LTR or RTL (set on `ShaperInput`); no mixed
    ///   BiDi (Phase 3).
    /// - Single script run; no per-cluster fallback (Phase 4).
    /// - Single font for the entire input (Phase 4).
    /// - Multi-line via *mandatory* breaks only (`\n`, U+2028,
    ///   U+2029) — driven by `OmegaCommon::BreakIterator`. No wrap to
    ///   width (Phase 3) and no `lineLimit` (Phase 3).
    /// - Horizontal alignment: Left / Center / Right from
    ///   `TextLayoutDescriptor::Alignment`, computed per-line off
    ///   that line's totalAdvance.
    /// - Vertical alignment: Upper / Center / Lower, applied
    ///   uniformly to the multi-line block.
    /// - Baseline placement in canvas-space Y-down from the rect top.
    class OMEGAWTK_EXPORT TextLayoutEngine {
    public:
        static LayoutResult layout(const OmegaCommon::UniString & text,
                                   Core::SharedPtr<Font> font,
                                   const FontMetrics & metrics,
                                   const Composition::Rect & rect,
                                   const TextLayoutDescriptor & desc,
                                   ITextShaper & shaper);
    };

}

#endif
