// Phase-1 layout-engine unit tests (Text-Layout-Engine-Plan.md).
//
// Drives `TextLayoutEngine::layout` with a mock `ITextShaper` that
// returns canned `ShaperGlyph`s and asserts the resulting canvas-
// space positions, alignment, and baseline placement. No GPU, no
// atlas, no platform — this binary links only the `OmegaWTK_Composition`
// static lib (plus OmegaCommon).

#include "omegaWTK/Composition/TextLayoutEngine.h"
#include "omegaWTK/Composition/FontEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace OmegaWTK;
using namespace OmegaWTK::Composition;

namespace {

    bool approx(float a, float b, float eps = 0.001f){
        return std::fabs(a - b) <= eps;
    }

    // Echoes the input text as one glyph per UTF-16 unit, uniform
    // advance, no offsets. Records call count + the last input so
    // tests can assert the layout engine forwarded what it received.
    // Phase 3: also records per-call text / direction / script and
    // (when `mimicRtlReorder`) reverses glyph order for RTL inputs to
    // simulate HarfBuzz's visual-order output.
    class MockShaper : public ITextShaper {
    public:
        float advance = 10.f;
        int callCount = 0;
        OmegaCommon::UniString lastText;
        bool lastRightToLeft = false;
        std::vector<bool> callRTL;
        std::vector<std::int32_t> callScripts;
        std::vector<std::int32_t> callLengths;
        /// When true, reverse the glyph order on RTL calls so the
        /// MockShaper matches HB's "RTL output is visual order"
        /// contract. Default off so Phase-1/2 tests keep their
        /// logical-order behaviour.
        bool mimicRtlReorder = false;

        OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput & input) override {
            ++callCount;
            lastText = input.text;
            lastRightToLeft = input.rightToLeft;
            callRTL.push_back(input.rightToLeft);
            callScripts.push_back(input.script);
            callLengths.push_back(input.text.length());
            OmegaCommon::Vector<ShaperGlyph> out;
            const auto * buf = input.text.getBuffer();
            const auto len = input.text.length();
            out.reserve((std::size_t)len);
            for(std::int32_t i = 0; i < len; ++i){
                ShaperGlyph g;
                g.glyphId = (std::uint32_t)buf[i];
                g.advance = advance;
                // Phase 3.5: shaper-relative cluster offset. One glyph
                // per UTF-16 code unit, so `cluster = i` is exact and
                // matches the contract HarfBuzz / CoreText fulfil for
                // simple scripts.
                g.cluster = i;
                out.push_back(g);
            }
            if(mimicRtlReorder && input.rightToLeft){
                std::reverse(out.begin(), out.end());
            }
            return out;
        }
    };

    // Minimal `Font` subclass so the layout engine has an opaque
    // handle to assign into `ShapedGlyph::resolvedFont`. The layout
    // path never calls `getNativeFont` or touches the atlas.
    class TestFont : public Font {
    public:
        explicit TestFont(FontDescriptor & d) : Font(d) {}
        void * getNativeFont() override { return nullptr; }
    };

    Core::SharedPtr<Font> makeFont(unsigned size = 16){
        static FontDescriptor desc("test", size, FontDescriptor::Regular);
        return std::make_shared<TestFont>(desc);
    }

    // Phase 4: shaper that emits gid=0 (`.notdef`) for every code unit
    // outside the requested font's "coverage". The fallback test fonts
    // each carry a single-char coverage hint (used only here in the
    // mock; the real `Font` API doesn't expose coverage).
    class CoverageAwareShaper : public ITextShaper {
    public:
        float advance = 10.f;
        int callCount = 0;
        // Glyph id 0 (notdef) emitted whenever a code unit falls
        // outside [coverLow, coverHigh]. Per-call coverage is read
        // from the input font's family name: "ascii" = [0..127],
        // "fallback" = [0x80..0xFFFF].
        OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput & input) override {
            ++callCount;
            OmegaCommon::Vector<ShaperGlyph> out;
            const auto * buf = input.text.getBuffer();
            const auto len = input.text.length();
            const std::string family = input.font ? input.font->desc.family : "";
            const bool isAscii = (family == "ascii");
            const bool isFallback = (family == "fallback");
            for(std::int32_t i = 0; i < len; ++i){
                ShaperGlyph g;
                const auto c = buf[i];
                bool inRange =
                    (isAscii    && c < 0x80) ||
                    (isFallback && c >= 0x80) ||
                    (!isAscii && !isFallback); // any face covers all
                g.glyphId = inRange ? (std::uint32_t)c : 0u;
                g.advance = advance;
                g.cluster = i;
                out.push_back(g);
            }
            return out;
        }
    };

    // Phase 4: mock fallback that returns a `fallback` font for any
    // codepoint outside ASCII. Tracks call count so tests can assert
    // both caching and miss behaviour.
    class MockFallback : public IFontFallback {
    public:
        int callCount = 0;
        Core::SharedPtr<Font> substitute;
        // When true, return `substitute` only for the *first* call to
        // simulate a one-off fallback that should then get cached.
        bool returnNull = false;

        Core::SharedPtr<Font> fallbackForCodepoint(
            Core::SharedPtr<Font> requested,
            std::uint32_t codepoint) override {
            (void)requested;
            ++callCount;
            if(returnNull || codepoint < 0x80){
                return nullptr;
            }
            return substitute;
        }
    };

    Core::SharedPtr<Font> makeAsciiFont(){
        static FontDescriptor desc("ascii", 16, FontDescriptor::Regular);
        return std::make_shared<TestFont>(desc);
    }

    Core::SharedPtr<Font> makeFallbackFont(){
        static FontDescriptor desc("fallback", 16, FontDescriptor::Regular);
        return std::make_shared<TestFont>(desc);
    }

    FontMetrics defaultMetrics(){
        FontMetrics m;
        m.ascent  = 12.f;
        m.descent = 4.f;
        m.lineGap = 0.f;
        // lineHeight = 16
        return m;
    }

    void testEmptyInput(){
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString empty;
        auto r = TextLayoutEngine::layout(empty, font, metrics, rect, d, shaper);
        assert(r.glyphs.empty());
        assert(r.lineBaselines.empty());
        assert(approx(r.layoutHeight, 0.f));
        assert(approx(r.layoutWidth, 0.f));
        assert(shaper.callCount == 0);

        std::printf("  [PASS] Empty input short-circuits without shaping\n");
    }

    void testNullFont(){
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        OmegaCommon::UniString text("hi");
        auto r = TextLayoutEngine::layout(text, nullptr, metrics, rect, d, shaper);
        assert(r.glyphs.empty());
        assert(shaper.callCount == 0);
        std::printf("  [PASS] Null font short-circuits without shaping\n");
    }

    void testEmptyShaperOutput(){
        class EmptyShaper : public ITextShaper {
        public:
            OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput &) override {
                return {};
            }
        } shaper;

        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        OmegaCommon::UniString text("hi");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.empty());
        assert(r.lineBaselines.empty());
        std::printf("  [PASS] Empty shaper output yields no glyphs\n");
    }

    void testLeftUpperAlignment(){
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.advance = 10.f;

        OmegaCommon::UniString text("ABC");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 3);
        assert(approx(r.glyphs[0].canvasX, 0.f));
        assert(approx(r.glyphs[1].canvasX, 10.f));
        assert(approx(r.glyphs[2].canvasX, 20.f));
        // Top-aligned: baseline = ascent.
        assert(approx(r.glyphs[0].canvasY, 12.f));
        assert(r.lineBaselines.size() == 1);
        assert(approx(r.lineBaselines[0], 12.f));
        // No fallback in Phase 1 — every glyph resolves to the input font.
        assert(r.glyphs[0].resolvedFont == font);
        assert(r.glyphs[2].resolvedFont == font);
        assert(approx(r.layoutHeight, metrics.lineHeight()));
        // Text-Measurement-API-Plan §6: intrinsic width = the single line's
        // advance (3 glyphs × 10).
        assert(approx(r.layoutWidth, 30.f));

        std::printf("  [PASS] Left+Upper alignment\n");
    }

    void testMiddleCenterAlignment(){
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::MiddleCenter, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("ABC");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 3);
        // Horizontal center: startX = (100 - 30)/2 = 35
        assert(approx(r.glyphs[0].canvasX, 35.f));
        assert(approx(r.glyphs[2].canvasX, 55.f));
        // Vertical center: extra = 50 - 16 = 34; baseline = 12 + 17 = 29
        assert(approx(r.glyphs[0].canvasY, 29.f));

        std::printf("  [PASS] Middle+Center alignment\n");
    }

    void testRightLowerAlignment(){
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::RightLower, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("ABC");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 3);
        // Right: startX = 100 - 30 = 70
        assert(approx(r.glyphs[0].canvasX, 70.f));
        // Lower: extra = 50 - 16 = 34; baseline = 12 + 34 = 46
        assert(approx(r.glyphs[0].canvasY, 46.f));

        std::printf("  [PASS] Right+Lower alignment\n");
    }

    void testTextWiderThanRectClampsToLeftEdge(){
        // A right-aligned string wider than the rect would otherwise
        // start at a negative X. The layout engine clamps to 0 so the
        // first glyph is still visible at the left edge.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 20.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::RightUpper, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("ABCDE"); // totalAdvance = 50 > rect.w
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 5);
        assert(approx(r.glyphs[0].canvasX, 0.f));

        std::printf("  [PASS] Overflow clamps to left edge\n");
    }

    void testShortRectClampsBaselineToTop(){
        // Rect shorter than the line height: vertical alignment can't
        // make use of negative extra and must collapse to top so the
        // baseline never moves above `ascent`.
        auto font = makeFont();
        auto metrics = defaultMetrics(); // lineHeight = 16
        Composition::Rect rect{{0.f, 0.f}, 100.f, 8.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftCenter, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("X");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 1);
        assert(approx(r.glyphs[0].canvasY, metrics.ascent));

        std::printf("  [PASS] Short rect clamps baseline to top\n");
    }

    void testShaperReceivesLayoutEngineInput(){
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("Hi");
        (void)TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(shaper.callCount == 1);
        assert(shaper.lastText.length() == 2);
        assert(shaper.lastRightToLeft == false);

        std::printf("  [PASS] Shaper receives forwarded input\n");
    }

    void testGlyphOffsetsPropagate(){
        // Mark-positioning style shaper: per-glyph (x, y) offsets
        // applied off the cluster's pen origin. The layout engine
        // must preserve these on the way out.
        class OffsetShaper : public ITextShaper {
        public:
            OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput & input) override {
                OmegaCommon::Vector<ShaperGlyph> out;
                for(std::int32_t i = 0; i < input.text.length(); ++i){
                    ShaperGlyph g;
                    g.glyphId = 100u + (std::uint32_t)i;
                    g.advance = 10.f;
                    g.xOffset = (i == 1) ? 2.5f : 0.f;
                    g.yOffset = (i == 1) ? -3.f : 0.f;
                    out.push_back(g);
                }
                return out;
            }
        } shaper;

        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        OmegaCommon::UniString text("AB");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 2);
        assert(r.glyphs[1].glyphId == 101u);
        assert(approx(r.glyphs[0].canvasX, 0.f));
        assert(approx(r.glyphs[0].canvasY, metrics.ascent));
        // Pen origin for glyph 1 = 10 (advance[0]); offset adds (2.5, -3).
        assert(approx(r.glyphs[1].canvasX, 12.5f));
        assert(approx(r.glyphs[1].canvasY, metrics.ascent - 3.f));

        std::printf("  [PASS] Per-glyph offsets propagate\n");
    }

    void testMandatoryLineBreak(){
        // Phase-2 multi-line: `\n` is a mandatory break iter ICU
        // line iterator. Each line gets its own totalAdvance for
        // alignment; baselines stack on `metrics.lineHeight()`.
        auto font = makeFont();
        auto metrics = defaultMetrics();           // lineHeight = 16
        Composition::Rect rect{{0.f, 0.f}, 100.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.advance = 10.f;

        OmegaCommon::UniString text("AB\nCDE");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        // 2 lines, 5 glyphs total (AB, CDE — \n is trimmed).
        assert(r.lineBaselines.size() == 2);
        assert(r.glyphs.size() == 5);
        // Line 1 baseline at ascent.
        assert(approx(r.lineBaselines[0], metrics.ascent));
        // Line 2 baseline at ascent + lineHeight.
        assert(approx(r.lineBaselines[1], metrics.ascent + metrics.lineHeight()));
        // Total layout height = 2 lines × lineHeight.
        assert(approx(r.layoutHeight, 2.f * metrics.lineHeight()));
        // Intrinsic width = the widest line "CDE" (3 × 10), not "AB" (2 × 10).
        assert(approx(r.layoutWidth, 30.f));
        // First glyph of line 2 (C) is at x=0, y=line 2 baseline.
        // Glyphs are emitted in logical order, so g[2] is 'C'.
        assert(approx(r.glyphs[2].canvasX, 0.f));
        assert(approx(r.glyphs[2].canvasY, metrics.ascent + metrics.lineHeight()));

        std::printf("  [PASS] Mandatory \\n splits into multiple lines\n");
    }

    void testBlankLineFromDoubleNewline(){
        // "AB\n\nC" — three lines, middle is blank but still gets a
        // baseline so the visible-vs-blank rhythm is preserved.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("AB\n\nC");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.lineBaselines.size() == 3);
        assert(r.glyphs.size() == 3);    // AB + C
        // Layout height = 3 × lineHeight.
        assert(approx(r.layoutHeight, 3.f * metrics.lineHeight()));

        std::printf("  [PASS] Blank line keeps a baseline\n");
    }

    void testPerLineAlignment(){
        // "ABCD\nE" right-aligned: each line's startX is computed
        // from its own totalAdvance, so the short second line ends
        // at the rect's right edge with no left padding inherited
        // from the longer first line.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::RightUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.advance = 10.f;

        OmegaCommon::UniString text("ABCD\nE");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 5);
        // Line 1: totalAdvance = 40, startX = 60.
        assert(approx(r.glyphs[0].canvasX, 60.f));
        // Line 2 (just 'E', totalAdvance = 10): startX = 90.
        assert(approx(r.glyphs[4].canvasX, 90.f));

        std::printf("  [PASS] Per-line horizontal alignment\n");
    }

    void testRectOriginShiftDoesNotAffectGlyphX(){
        // Phase-1 contract: `canvasX/canvasY` are relative to the
        // rect's logical origin. Translating the rect by (dx, dy)
        // must NOT change the per-glyph values — callers compose the
        // rect offset themselves at draw time.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rectA{{0.f, 0.f}, 100.f, 50.f};
        Composition::Rect rectB{{37.f, 19.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaperA, shaperB;

        OmegaCommon::UniString text("AB");
        auto a = TextLayoutEngine::layout(text, font, metrics, rectA, d, shaperA);
        auto b = TextLayoutEngine::layout(text, font, metrics, rectB, d, shaperB);
        assert(a.glyphs.size() == b.glyphs.size());
        for(std::size_t i = 0; i < a.glyphs.size(); ++i){
            assert(approx(a.glyphs[i].canvasX, b.glyphs[i].canvasX));
            assert(approx(a.glyphs[i].canvasY, b.glyphs[i].canvasY));
        }

        std::printf("  [PASS] Rect origin shift does not bake into positions\n");
    }

    // ---- Phase 3 — BiDi + script run integration ----------------

    OmegaCommon::UniString makeUString(std::initializer_list<std::uint32_t> codepoints){
        std::vector<OmegaCommon::Unicode32Char> buf(codepoints.begin(), codepoints.end());
        return OmegaCommon::UniString::fromUTF32(buf.data(), (std::int32_t)buf.size());
    }

    void testPureRTLOneRun(){
        // Pure Hebrew "אבג" (U+05D0..U+05D2). Single visual run, RTL.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.mimicRtlReorder = true;

        auto text = makeUString({0x05D0u, 0x05D1u, 0x05D2u});
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 3);
        // Exactly one shape call, RTL=true.
        assert(shaper.callCount == 1);
        assert(shaper.callRTL.size() == 1 && shaper.callRTL[0] == true);
        // Glyph order is reversed by the (mimicked) RTL shaper: visual
        // order is gimel, bet, aleph — leftmost on canvas first.
        assert(r.glyphs[0].glyphId == 0x05D2u);
        assert(r.glyphs[1].glyphId == 0x05D1u);
        assert(r.glyphs[2].glyphId == 0x05D0u);
        // Penxs advance L→R as usual.
        assert(approx(r.glyphs[0].canvasX, 0.f));
        assert(approx(r.glyphs[1].canvasX, 10.f));
        assert(approx(r.glyphs[2].canvasX, 20.f));
        std::printf("  [PASS] Pure RTL → single RTL shape call, visual-order glyphs\n");
    }

    void testMixedLatinHebrew(){
        // "ab" + space + "אב" + space + "cd". ICU usually merges
        // trailing whitespace into the surrounding strong-direction
        // run; we don't assert exact run boundaries, just the
        // bookend invariants:
        //   • at least one LTR + one RTL shape call;
        //   • LTR glyphs ('a','b') stay in logical order;
        //   • RTL glyphs (aleph, bet) come out reversed.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 200.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.mimicRtlReorder = true;

        auto text = makeUString({'a','b',' ', 0x05D0u, 0x05D1u, ' ', 'c','d'});
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(!r.glyphs.empty());

        // At least one LTR and one RTL shape call.
        bool sawLtr = false, sawRtl = false;
        for(bool rtl : shaper.callRTL){
            sawLtr = sawLtr || !rtl;
            sawRtl = sawRtl || rtl;
        }
        assert(sawLtr && sawRtl);

        // Locate the Hebrew glyphs in the output. Visual order
        // places gimel/bet rightmost-first (i.e. bet glyph appears
        // at a smaller index than aleph).
        std::size_t aleph = (std::size_t)-1, beth = (std::size_t)-1;
        for(std::size_t i = 0; i < r.glyphs.size(); ++i){
            if(r.glyphs[i].glyphId == 0x05D0u) aleph = i;
            if(r.glyphs[i].glyphId == 0x05D1u) beth  = i;
        }
        assert(aleph != (std::size_t)-1 && beth != (std::size_t)-1);
        assert(beth < aleph);

        std::printf("  [PASS] Mixed Latin+Hebrew splits into LTR and RTL runs\n");
    }

    void testMixedScriptSameDirection(){
        // Latin + Greek, both LTR. Should produce two same-direction
        // script sub-runs within the single visual run.
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 200.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.mimicRtlReorder = true;

        auto text = makeUString({'a','b',0x03B1u,0x03B2u}); // a b α β
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 4);
        // Two shape calls — one Latin, one Greek — both LTR.
        assert(shaper.callCount == 2);
        for(bool rtl : shaper.callRTL) assert(!rtl);
        // Distinct script tags on the two calls.
        assert(shaper.callScripts[0] != shaper.callScripts[1]);
        // Logical order preserved across both LTR runs.
        assert(r.glyphs[0].glyphId == 'a');
        assert(r.glyphs[1].glyphId == 'b');
        assert(r.glyphs[2].glyphId == 0x03B1u);
        assert(r.glyphs[3].glyphId == 0x03B2u);
        std::printf("  [PASS] Mixed-script same-direction → per-script shape calls\n");
    }

    void testPureLatinUnchanged(){
        // Regression: pure-Latin text still produces a single LTR
        // shape call after the BiDi/script segmentation passes (the
        // segmenters identify one visual run, one script).
        auto font = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 100.f, 50.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper, TextLayoutDescriptor::None};
        MockShaper shaper;
        shaper.mimicRtlReorder = true;

        OmegaCommon::UniString text("hello");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 5);
        assert(shaper.callCount == 1);
        assert(shaper.callRTL.size() == 1 && !shaper.callRTL[0]);
        std::printf("  [PASS] Pure Latin unchanged through bidi+script pipeline\n");
    }

    // ─── Phase 3.5: wrap + lineLimit ──────────────────────────────────

    void testWrapByWordSplitsOnSoftBreak(){
        // "hello world" with advance=10 → "hello " ≈ 60, "world" ≈ 50.
        // Rect width 75 fits "hello " (trailing-WS exempt) but adding
        // "world" overflows; wrap should split between the words.
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 75.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::WrapByWord};
        MockShaper shaper;

        OmegaCommon::UniString text("hello world");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        // Two wrap-lines.
        assert(r.lineBaselines.size() == 2);
        // First line: "hello " (6 glyphs, advance 10 each, trailing
        // space stays on line 1 per the CSS / Pango convention).
        // Second line: "world" (5 glyphs).
        assert(r.glyphs.size() == 11);
        // First-line glyphs land between baselines[0]; second-line on
        // baselines[1]. The wrap split puts 'w' (glyph index 6) on
        // line 2.
        assert(approx(r.glyphs[5].canvasY, r.lineBaselines[0]));
        assert(approx(r.glyphs[6].canvasY, r.lineBaselines[1]));
        // 'w' starts at x = 0 on the new line (left alignment).
        assert(approx(r.glyphs[6].canvasX, 0.f));
        std::printf("  [PASS] WrapByWord splits at a soft break\n");
    }

    void testWrapByCharacterForcesMidWord(){
        // "abcdefghij" advance=10 → 100 total. Rect width 35 →
        // ~3 chars/line, so we expect 4 wrap-lines (3+3+3+1).
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 35.f, 200.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::WrapByCharacter};
        MockShaper shaper;

        OmegaCommon::UniString text("abcdefghij");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.lineBaselines.size() == 4);
        // All 10 glyphs present, distributed across the four lines.
        assert(r.glyphs.size() == 10);
        std::printf("  [PASS] WrapByCharacter forces mid-word break\n");
    }

    void testLineLimitTruncates(){
        // "alpha beta gamma" forces three wrap-lines at width 35;
        // lineLimit=2 truncates the third.
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 35.f, 200.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::WrapByWord};
        d.lineLimit = 2;
        MockShaper shaper;

        OmegaCommon::UniString text("alpha beta gamma");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.lineBaselines.size() == 2);
        // Glyphs from the dropped third line are absent.
        for(const auto & g : r.glyphs){
            // "gamma" glyphs (gid >= 'a' && first char is 'g') won't
            // all be present; assert specifically that none of them
            // sit on a third baseline (which doesn't exist).
            assert(g.canvasY == r.lineBaselines[0] ||
                   g.canvasY == r.lineBaselines[1]);
        }
        std::printf("  [PASS] lineLimit truncates wrapped output\n");
    }

    void testTrailingWhitespaceExempt(){
        // Width 60 exactly fits "hello " (6 × 10) including the
        // trailing space — but the trailing space is *exempt* from
        // the fit check, so a wider word after should still split off
        // even when the previous "hello " width equals rect.w.
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 60.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::WrapByWord};
        MockShaper shaper;

        OmegaCommon::UniString text("hello world");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        // Two wrap-lines: "hello " and "world".
        assert(r.lineBaselines.size() == 2);
        assert(approx(r.glyphs[6].canvasY, r.lineBaselines[1]));
        std::printf("  [PASS] Trailing whitespace exempt from fit check\n");
    }

    void testNoWrapWhenWrappingNone(){
        // Wrapping::None must keep the long string on one line even
        // when it overflows the rect — Phase 2 contract preserved.
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 30.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::None};
        MockShaper shaper;

        OmegaCommon::UniString text("hello world");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.lineBaselines.size() == 1);
        assert(r.glyphs.size() == 11);
        std::printf("  [PASS] Wrapping::None disables wrap pass\n");
    }

    void testWrapDoesNotApplyToShortLine(){
        // Even with WrapByWord enabled, a string that already fits
        // should produce exactly one wrap-line (no spurious split).
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 200.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::WrapByWord};
        MockShaper shaper;

        OmegaCommon::UniString text("short");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        assert(r.lineBaselines.size() == 1);
        assert(r.glyphs.size() == 5);
        std::printf("  [PASS] Short line stays on one wrap-line\n");
    }

    // ─── Phase 4: font fallback ───────────────────────────────────────

    void testFallbackSubstitutesNotdefClusters(){
        // Mixed Latin + non-ASCII. The coverage-aware shaper emits
        // .notdef (gid=0) for non-ASCII codepoints against the
        // ascii-only font. The fallback driver returns a substitute
        // face for the .notdef clusters; after orchestration the
        // resolved glyphs land at the right pen position with the
        // substitute font set as `resolvedFont`.
        auto ascii    = makeAsciiFont();
        auto fallback = makeFallbackFont();
        auto metrics  = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 500.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::None};
        CoverageAwareShaper shaper;
        MockFallback fb;
        fb.substitute = fallback;

        // "abécd" — `é` (U+00E9) is non-ASCII, triggers fallback.
        const char16_t buf[] = { 'a', 'b', 0x00E9, 'c', 'd' };
        auto text = OmegaCommon::UniString::fromUTF32(
            (const OmegaCommon::Unicode32Char *)buf, 0); // unused
        // Build via UTF-8 path so the test stays portable.
        OmegaCommon::UniString text2 = OmegaCommon::UniString::fromUTF8("ab\xC3\xA9" "cd");

        auto r = TextLayoutEngine::layout(text2, ascii, metrics, rect, d, shaper, &fb);
        assert(r.glyphs.size() == 5);
        // Glyphs in visual order: a, b, é, c, d.
        assert(r.glyphs[0].resolvedFont == ascii);
        assert(r.glyphs[1].resolvedFont == ascii);
        assert(r.glyphs[2].resolvedFont == fallback); // é routed
        assert(r.glyphs[2].glyphId == 0x00E9);        // fallback shape produced it
        assert(r.glyphs[3].resolvedFont == ascii);
        assert(r.glyphs[4].resolvedFont == ascii);
        // Pen advances are uniform (advance=10), so X is monotonic.
        assert(approx(r.glyphs[0].canvasX, 0.f));
        assert(approx(r.glyphs[1].canvasX, 10.f));
        assert(approx(r.glyphs[2].canvasX, 20.f));
        assert(approx(r.glyphs[3].canvasX, 30.f));
        assert(approx(r.glyphs[4].canvasX, 40.f));
        std::printf("  [PASS] Fallback substitutes .notdef clusters\n");
    }

    void testFallbackNullKeepsNotdef(){
        // When the fallback driver declines (returns nullptr), the
        // original .notdef glyph stays in place with the requested
        // font — no infinite re-shape loop, no missing glyphs.
        auto ascii   = makeAsciiFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 500.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::None};
        CoverageAwareShaper shaper;
        MockFallback fb;
        fb.substitute = nullptr;
        fb.returnNull = true;

        OmegaCommon::UniString text = OmegaCommon::UniString::fromUTF8("a\xC3\xA9");
        auto r = TextLayoutEngine::layout(text, ascii, metrics, rect, d, shaper, &fb);
        assert(r.glyphs.size() == 2);
        assert(r.glyphs[0].glyphId == 'a');
        assert(r.glyphs[0].resolvedFont == ascii);
        // The .notdef glyph survives with id 0 and resolvedFont = ascii.
        assert(r.glyphs[1].glyphId == 0);
        assert(r.glyphs[1].resolvedFont == ascii);
        std::printf("  [PASS] Fallback returning null keeps .notdef\n");
    }

    void testFallbackNotInvokedWithoutDriver(){
        // Pass nullptr for the fallback driver — .notdef glyphs flow
        // through unchanged. Regression for the default-argument path
        // existing callers take.
        auto ascii   = makeAsciiFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 500.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::None};
        CoverageAwareShaper shaper;

        OmegaCommon::UniString text = OmegaCommon::UniString::fromUTF8("\xC3\xA9");
        auto r = TextLayoutEngine::layout(text, ascii, metrics, rect, d, shaper);
        assert(r.glyphs.size() == 1);
        assert(r.glyphs[0].glyphId == 0);
        assert(r.glyphs[0].resolvedFont == ascii);
        std::printf("  [PASS] Fallback skipped when driver is null\n");
    }

    void testFallbackHandlesAllNotdef(){
        // String entirely outside requested coverage — every glyph
        // routes through the fallback face.
        auto ascii    = makeAsciiFont();
        auto fallback = makeFallbackFont();
        auto metrics  = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 500.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::None};
        CoverageAwareShaper shaper;
        MockFallback fb;
        fb.substitute = fallback;

        // "中文" — both glyphs outside ASCII.
        OmegaCommon::UniString text = OmegaCommon::UniString::fromUTF8("\xE4\xB8\xAD\xE6\x96\x87");
        auto r = TextLayoutEngine::layout(text, ascii, metrics, rect, d, shaper, &fb);
        assert(r.glyphs.size() == 2);
        assert(r.glyphs[0].resolvedFont == fallback);
        assert(r.glyphs[1].resolvedFont == fallback);
        assert(r.glyphs[0].glyphId == 0x4E2D); // 中
        assert(r.glyphs[1].glyphId == 0x6587); // 文
        std::printf("  [PASS] Fallback handles all-notdef string\n");
    }

    void testWrapPreservesSubpixelAdvance(){
        // Wrap math feeds the same `advance` value through to the
        // emitted glyphs — assert that fractional advances survive
        // the wrap pass intact (no Phase-2.5 std::round leak).
        auto font    = makeFont();
        auto metrics = defaultMetrics();
        Composition::Rect rect{{0.f, 0.f}, 23.f, 100.f};
        TextLayoutDescriptor d{TextLayoutDescriptor::LeftUpper,
                               TextLayoutDescriptor::WrapByWord};
        MockShaper shaper;
        shaper.advance = 7.3f;

        OmegaCommon::UniString text("ab cd");
        auto r = TextLayoutEngine::layout(text, font, metrics, rect, d, shaper);
        // "ab " = 21.9 fits in 23; "cd" = 14.6 needs its own line.
        assert(r.lineBaselines.size() == 2);
        // 'c' on line 2 lands at penX 0 + xOffset 0.
        assert(approx(r.glyphs[3].canvasX, 0.f));
        // 'd' lands at advance 7.3 (sub-pixel preserved).
        assert(approx(r.glyphs[4].canvasX, 7.3f));
        std::printf("  [PASS] Wrap preserves sub-pixel advance\n");
    }

}

int main(){
    std::printf("TextLayoutEngineTest\n");
    testEmptyInput();
    testNullFont();
    testEmptyShaperOutput();
    testLeftUpperAlignment();
    testMiddleCenterAlignment();
    testRightLowerAlignment();
    testTextWiderThanRectClampsToLeftEdge();
    testShortRectClampsBaselineToTop();
    testShaperReceivesLayoutEngineInput();
    testGlyphOffsetsPropagate();
    testMandatoryLineBreak();
    testBlankLineFromDoubleNewline();
    testPerLineAlignment();
    testRectOriginShiftDoesNotAffectGlyphX();
    testPureLatinUnchanged();
    testPureRTLOneRun();
    testMixedLatinHebrew();
    testMixedScriptSameDirection();
    testWrapByWordSplitsOnSoftBreak();
    testWrapByCharacterForcesMidWord();
    testLineLimitTruncates();
    testTrailingWhitespaceExempt();
    testNoWrapWhenWrappingNone();
    testWrapDoesNotApplyToShortLine();
    testWrapPreservesSubpixelAdvance();
    testFallbackSubstitutesNotdefClusters();
    testFallbackNullKeepsNotdef();
    testFallbackNotInvokedWithoutDriver();
    testFallbackHandlesAllNotdef();
    std::printf("All tests passed.\n");
    return 0;
}
