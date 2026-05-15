# WTK-Owned Text Layout Engine Plan

## Problem

The current text path delegates layout to the platform's text layout
engine — `CTFramesetter` / `CTFrame` on macOS, `PangoLayout` on Linux,
`IDWriteTextLayout` on Windows. Each of these imposes its **own
coordinate convention** on the output positions:

- Core Text gives glyph positions in a Y-up frame-relative space
  (`CTFrameGetLineOrigins` from bottom).
- Pango gives Y-down baseline positions in Pango units, scaled by an
  internal DPI.
- DWrite gives positions in DIPs relative to its own layout box, with
  its own metric/baseline conventions.

Reconciling those onto WTK's canvas (Y-down, top-left origin, baseline
placed explicitly per glyph) requires a different fix-up on every
platform — and the recent debugging round around Phase 6.7 showed that
**per-glyph vertical offset bugs keep surfacing** from subtle
mismatches between what the layout engine thinks the baseline is and
what the rasterizer + render path think it is. The atlas flip /
sub-rect / UV-mirror dance is fixing a symptom; the cause is that
**layout decisions are made in a convention WTK doesn't own.**

A second symptom: layout-engine behavior we don't want (line spacing
rules, vertical alignment heuristics, frame-rect clipping) is baked
into the same call that gives us glyph positions. We can't tweak one
without disturbing the other.

## Goal

WTK owns text **layout** (line breaking, line composition, alignment,
baseline placement, final glyph positions in canvas-space). Platform
APIs are kept *only* for:

1. **Font discovery + style resolution** — which file is "Helvetica
   Bold 14"?
2. **Glyph outline extraction** — used by the MSDF rasterizer (already
   factored, lives on `Font::atlas().rasterizeFn`).
3. **Shaping** — taking a unicode substring + font + script +
   direction and returning `(glyph_id, advance, x_offset, y_offset)`
   per cluster. This is the nontrivial bit: kerning, ligatures,
   complex-script joining, mark positioning. Reimplementing it is out
   of scope.
4. **Font fallback** — finding a substitute face for a codepoint the
   requested face can't render (CJK in a Latin font, emoji, etc.).

Everything between shaping and rendering — line composition, baseline
calculus, our canvas-space output — belongs to WTK.

## Guiding Principles

- **One coordinate convention through the whole layout pipeline:**
  Y-down, top-left origin, baseline expressed as a canvas-space Y for
  each line. No frame-relative Y-up math, no scale-dependent unit
  conversions hidden inside platform calls.
- **Shaping is per-script-run, not per-text-block.** The layout engine
  segments the input into bidi/script runs, calls the platform shaper
  for each (which is where complex-script intelligence lives), and
  assembles the runs into lines itself.
- **Platform APIs return their data and step out.** We don't pass them
  a frame rect, layout descriptor, or wrap width — they don't decide
  line breaks or vertical positioning.
- **Layout is testable in isolation.** No GPU, no atlas, no canvas —
  given (text, font metrics, layout descriptor, shaped runs), produce
  positioned glyphs deterministically.

## Scope split, per platform

### Linux

The Pango dependency goes away **entirely**.

| Concern | Today | New path |
|---|---|---|
| Font discovery | PangoFontMap → PangoFcFont | FontConfig directly (`FcConfigSubstitute`, `FcFontMatch`) |
| Outline extraction | PangoFc → FreeType `FT_Face` | FreeType `FT_Face` directly |
| Shaping | PangoLayout (which wraps HarfBuzz internally) | HarfBuzz directly (`hb_buffer_t`, `hb_shape`) |
| Layout | PangoLayout | WTK layout engine |
| Fallback | PangoLayout's automatic fallback | FontConfig substitute chain + WTK-owned fallback orchestration |
| BiDi | PangoLayout (uses fribidi) | ICU `UBiDi` (or fribidi directly) |
| Line-break opportunities | PangoLayout | ICU `BreakIterator` (line / word) |

HarfBuzz + FreeType + FontConfig are already transitively pulled in by
Pango. After this change WTK links them **directly** and drops
`pango`, `pangocairo`, `pangoft2`. Net dependency reduction.

### macOS

Core Text stays, but only the lower-level APIs are used.

| Concern | Today | New path |
|---|---|---|
| Font discovery | `CTFontCreateWithName...` | unchanged |
| Outline extraction | `CTFontCreatePathForGlyph` | unchanged |
| Shaping | `CTFramesetter` → `CTFrame` → walk `CTLine` / `CTRun` | `CTLine` directly, **without** `CTFramesetter` — fed a single shaping run (substring + font + direction). Core Text's shaper still produces kerning + ligatures; we just don't ask it to lay out a frame. |
| Layout | `CTFramesetter` | WTK layout engine |
| Fallback | implicit via `CTLine` | explicit via `CTFontCreateForString` per cluster of `.notdef` glyphs |
| BiDi | implicit via `CTLine` | ICU `UBiDi` |
| Line-break opportunities | implicit | ICU `BreakIterator` |

### Windows

DirectWrite stays, but `IDWriteTextLayout` and `IDWriteTextFormat`
retire from the WTK text path.

| Concern | Today | New path |
|---|---|---|
| Font discovery | `IDWriteFontCollection::FindFamilyName` | unchanged |
| Outline extraction | `IDWriteFontFace::GetGlyphRunOutline` | unchanged |
| Shaping | `IDWriteTextAnalyzer` (low-level: `GetGlyphs`, `GetGlyphPlacements`) | same — this is already the low-level shaping API DWrite exposes; no high-level layout involved |
| Layout | `IDWriteTextLayout` | **never used** — straight to WTK layout engine |
| Fallback | `IDWriteFontFallback::MapCharacters` | unchanged — our orchestrator calls it explicitly |
| BiDi | `IDWriteTextAnalyzer::AnalyzeBidi` | unchanged |
| Line-break opportunities | `IDWriteTextAnalyzer::AnalyzeLineBreakpoints` | unchanged |

DWrite is the **cleanest** of the three platforms for this design —
its API already separates shaping from layout. We just commit to using
only the analyzer + font face side.

## Architecture

```
                   ┌──────────────────────────────────────────┐
                   │           WTK Text Layout Engine         │
                   │                                          │
   text + font ──▶ │  1. Bidi analysis (ICU/UBiDi)            │ ──▶ positioned
   layout desc     │  2. Script + font-run segmentation       │     glyphs in
                   │  3. Per-run shaping (platform shaper)    │     canvas space
                   │  4. Font fallback per .notdef cluster    │
                   │  5. Line composition + wrap (ICU breaks) │
                   │  6. Alignment (left/center/right + vert) │
                   │  7. Baseline placement (Y-down canvas)   │
                   └──────────────────────────────────────────┘
                            ▲                        │
                            │ outline data           │ ShapedGlyphs (gid, canvasX, canvasY)
                            │                        ▼
                   ┌────────┴───────┐        ┌───────────────┐
                   │ Platform face  │        │ MSDF atlas    │
                   │ + outlines     │        │ + render path │
                   │ (CTFont, FT,   │        │ (unchanged)   │
                   │  IDWriteFace)  │        └───────────────┘
                   └────────────────┘
```

Public API skeleton:

```cpp
namespace OmegaWTK::Composition {

struct ShapedGlyph {
    std::uint32_t glyphId;
    Core::SharedPtr<Font> resolvedFont;     // for fallback runs
    float canvasX;
    float canvasY;                          // baseline Y in canvas space
};

struct LayoutResult {
    OmegaCommon::Vector<ShapedGlyph> glyphs;
    float layoutHeight;                     // total laid-out height
    OmegaCommon::Vector<float> lineBaselines; // for caret / hit-test
};

class TextLayoutEngine {
public:
    static LayoutResult layout(const OmegaCommon::UniString & text,
                               Core::SharedPtr<Font> font,
                               const Composition::Rect & rect,
                               const TextLayoutDescriptor & desc);
};

}
```

`Canvas::drawText` collapses to: `TextLayoutEngine::layout(...)` →
`Font::ensureGlyphsResident(glyphs)` → emit `TextRun` visual command.
Per-platform `GlyphRun::shape()` retires; `GlyphRun` itself may retire
once the bitmap fallback path is also converted (Phase 7).

## Phases

| Phase | Scope | Lands |
|---|---|---|
| **1. Skeleton + tests** | `TextLayoutEngine` interface, `ShapedGlyph` / `LayoutResult` structs, unit tests with mock shaping. No platform code. | Compiles on all 3 platforms; tests pass without any real shaping. |
| **2. Linux first cut** | Pure HarfBuzz + FreeType implementation. Single-script, single-font, LTR, no wrap. Tab handling = simple advance. ICU `BreakIterator` for line breaks. | `TextCompositorTest` renders Latin text on Linux through the new engine; bitmap fallback path kept in parallel. |
| **3. BiDi + script runs** | `UBiDi` integration, script run segmentation, mixed-script shaping. Linux only. | Mixed Latin/RTL text on Linux. |
| **4. Linux fallback** | FontConfig-driven fallback for `.notdef` clusters. `FontEngine::adoptResolvedFace` lands. | Latin+CJK text on Linux. |
| **5. macOS port** | Replace Linux shaper backend with `CTLine` per-run. Reuse layout engine entirely. | All Linux behaviors on macOS. The current Core Text-via-CTFramesetter path retires; **the per-glyph offset symptom on macOS disappears here.** |
| **6. Windows** | DWrite `IDWriteTextAnalyzer` backend. Reuse layout engine entirely. | Same on Windows. |
| **7. Retirements** | Delete `GlyphRun::shape()`, the per-engine layout code, `HarfBuzzGlyphRun`, `CTGlyphRun`, the bitmap fallback path's layout side (the bitmap path now just consumes `LayoutResult` like the MSDF path does). | Net code reduction; one layout pipeline used by both MSDF and bitmap paths. |

## Risks + open questions

- **Complex script support.** BiDi + Indic / Arabic shaping is exactly
  what platform shapers exist for. We keep them as **shapers** but
  compose the runs ourselves. Risk: a shaper expects layout context
  (e.g., `CTLine` does some line-level decisions). Mitigation: shape
  one logical run at a time, never give the shaper a wrap width.
- **Font fallback completeness.** Platform engines fall back through a
  system-wide chain we don't see. `FontConfig` /
  `CTFontCreateForString` / `IDWriteFontFallback::MapCharacters`
  expose this chain explicitly, but each platform's chain differs.
  Mitigation: defer to platform fallback orchestration APIs; don't
  reinvent the chain.
- **Justification, kashida, vertical text** — explicitly out of scope
  for phases 1–7. Revisit later if a real use case appears.
- **ICU footprint.** `UBiDi` + `BreakIterator` + script tables = a
  chunk of binary size. OmegaCommon already vendors ICU, so this is
  "import what's already there." On macOS we could use system libicu
  (different versions vary; risky). Recommend: use vendored ICU on all
  platforms for consistency.
- **Performance.** Native layout engines are heavily tuned. Our
  composed pipeline (BiDi + run segmentation + per-run shaping + line
  composition) will be slower on a per-call basis. Mitigation: cache
  `LayoutResult` per (text, font, rect, descriptor) tuple; today's
  per-frame re-layout becomes per-string-change re-layout.
- **The MSDF jitter we're chasing right now.** This plan is the *root*
  fix — the symptom will dissolve once layout is single-convention.
  But we should still ship a workaround for the existing path in the
  meantime (the atlas flip stays; the offset is something users will
  see until phase 5 lands on macOS).
