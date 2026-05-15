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
    std::printf("All tests passed.\n");
    return 0;
}
