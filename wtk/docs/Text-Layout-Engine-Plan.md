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

Reconciling those onto WTK's canvas (Y-down, bottom-left origin, baseline
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

  ALEX: OmegaWTK's coordinate system is actually bottom left origin.
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
| **2.5. Skia-style glyph metrics + flip elimination** | Replace `tileOriginY` / `inkH = pxH/tileScale` with Skia-style `(fLeft, fTop, fWidth, fHeight)` per-glyph metrics expressed in canvas pixels. Remove the atlas upload Y-flip; storage becomes top-row-first end-to-end. Pixel-snap pen positions before quad authoring. | Per-glyph vertical jitter goes away; same fix applies to all three backends. |
| **3. BiDi + script runs** | `UBiDi` integration, script run segmentation, mixed-script shaping. Linux only. | Mixed Latin/RTL text on Linux. |
| **3.5. Wrap + sub-pixel positioning** | Honor `TextLayoutDescriptor::Wrapping` (Word / Char) and `lineLimit` against `rect.w`. Drop pen rounding on both axes so the MSDF distance field handles fractional positioning. | Long strings wrap inside the rect; inter-glyph spacing reads cleanly at non-integer pen positions. |
| **4. Linux fallback** | FontConfig-driven fallback for `.notdef` clusters. `FontEngine::adoptResolvedFace` lands. | Latin+CJK text on Linux. |
| **5. macOS port** | Replace Linux shaper backend with `CTLine` per-run. Reuse layout engine entirely. | All Linux behaviors on macOS. |
| **6. Windows** | DWrite `IDWriteTextAnalyzer` backend. Reuse layout engine entirely. | Same on Windows. |
| **7. Retirements** | Delete `GlyphRun::shape()`, the per-engine layout code, `HarfBuzzGlyphRun`, `CTGlyphRun`, the bitmap fallback path's layout side (the bitmap path now just consumes `LayoutResult` like the MSDF path does). | Net code reduction; one layout pipeline used by both MSDF and bitmap paths. |

## Phase 2.5 — Skia-informed glyph rendering

After Phase 2 the layout engine produces correct per-glyph baseline
positions, but `TextCompositorTest` still exhibits visible per-glyph
*vertical jitter*: tall glyphs (`d`, `p`, `g`, `'`) appear shifted
downward relative to short glyphs (`e`, `n`, `r`) sharing the same
line. The bug is downstream of layout — in `GlyphAtlas` /
`emitTextSubRun` — and survives the move from PangoLayout to a
WTK-owned layout engine because both feed the same broken quad
author.

### Root cause

The current pipeline carries the glyph through *three* Y-axis
conventions:

1. **Rasterize** (msdfgen): writes the tile in Y-up bottom-row-first
   into `out.rgb`.
2. **Upload** (`GlyphAtlas::ensureGlyph`): vertically flips the tile
   so texture row 0 lands at the top of the glyph.
3. **Quad UV** (`emitTextSubRun`): pairs canvas-top with `v1` (max V)
   and canvas-bottom with `v0` — i.e. another implicit flip via the
   UV mapping.

Each conversion is correct in isolation but the *positioning math*
that lives between them mixes the conventions:

- `tileOriginY = b` is stored in Y-up shape coordinates (the padded
  bbox bottom relative to the pen origin).
- The render path computes `maxY = penY - tileOriginY; minY = maxY -
  inkH`, where `inkH = pxH / tileScale`.
- `pxH` is `ceil((t - b) * scale)` — *integer tile pixels*. Dividing
  by `tileScale` reconstructs a value close to the original bbox
  height, but the ceil rounding error is asymmetric (it always adds
  rather than subtracts), and it lands on the **wrong end** of the
  glyph (the bbox bottom, not the bbox top, because of the `tileOriginY
  = b` origin choice). The error compounds across tall glyphs.

The shorter the glyph, the smaller the ceil error (tileScale is
larger). The taller the glyph, the larger the absolute error — and
because the math anchors at the *bottom* of the bbox, the visible
displacement appears at the top, producing the jitter pattern.

### What Skia does instead

Skia's `SkGlyph` ([modules/skshaper/src/SkShaper_*.cpp][skia-shaper],
`include/private/SkGlyph.h`, `src/gpu/text/GrAtlasManager.cpp`)
expresses each cached glyph entry as four *integer pixel* values
plus an advance pair:

```cpp
struct SkGlyph {
    int16_t  fLeft;    // pixels from pen origin to the bitmap's left edge,
                       //   positive = to the right of the pen.
    int16_t  fTop;     // pixels from baseline to the bitmap's top edge,
                       //   positive = above the baseline.
    uint16_t fWidth;   // bitmap width in pixels.
    uint16_t fHeight;  // bitmap height in pixels.
    SkFixed  fAdvanceX, fAdvanceY;
    // ...
};
```

Drawing one glyph is:

```cpp
const float x = penX + glyph.fLeft;
const float y = penY - glyph.fTop;
drawBitmap(glyph.image, /*dst=*/SkRect::MakeXYWH(x, y, glyph.fWidth, glyph.fHeight));
```

Three properties of this convention matter for the jitter fix:

1. **Top-anchored.** The math origins from the *top* of the glyph
   (`penY - fTop`), then adds `fHeight` to get the bottom. No round-
   trip through a scale factor, no division. The integer `fHeight`
   *is* the canvas-space dimension.
2. **Integer pixel dimensions everywhere.** `fLeft`, `fTop`,
   `fWidth`, `fHeight` are all stored as integers. The bitmap is
   rasterized at exactly the size it will be drawn; there is no
   `tileScale` to compensate for. The sub-pixel positioning Skia
   *does* support is handled by rasterizing N variants of each
   glyph (one per quantized sub-pixel offset), **not** by drawing
   one rasterization at a fractional position.
3. **Single Y-axis convention from rasterizer to texture sample.**
   Skia's mask rasterizer (`SkScalerContext::generateImage`) writes
   the bitmap *top-row-first*. The atlas (`GrAtlasManager`) uploads
   it directly with no flip. The UV authoring (`GrAtlasTextOp`)
   pairs canvas-top with the smaller V coordinate. **Zero flips on
   the path from rasterizer to fragment.**

For the SDF-text path (`GrSDFMaskFilter`, `GrDistanceFieldGenFromVector`),
Skia keeps the same metric convention — the bitmap is sized to the
glyph at the SDF base size (typically 32px tall), and `fLeft / fTop`
are scaled together with the bitmap dimensions when the SDF is
sampled at a different size. The scale factor lives on the *draw
call* (one uniform for the whole text run), not on each glyph.

[skia-shaper]: https://skia.googlesource.com/skia/+/refs/heads/main/modules/skshaper/

### Changes for WTK

**`AtlasGlyph` (drop the tile-space round-trip)**

| Today | After 2.5 |
|---|---|
| `tileOriginX`, `tileOriginY` (Y-up, bbox bottom anchor) | `fLeft`, `fTop` — pen-origin-relative canvas pixels, Y-down, bbox top anchor (positive `fTop` = above baseline) |
| `pxW`, `pxH` (tile texels) | unchanged (still needed for UV) |
| `tileScale`, `inkPxW`, `inkPxH` | **removed** — quad dimensions come from `fWidth`/`fHeight` directly |
| `bearingX`, `bearingY` | **removed** — `fLeft` / `fTop` subsume them |

`fWidth` and `fHeight` are the canvas-space dimensions of the glyph
quad. At MSDF base scale (1×) they equal `pxW`/`pxH` exactly. At
non-unit DPR / render scale they're multiplied by the scale factor
*at draw time*, once — never again on a per-glyph basis.

**`emitTextSubRun`**

```cpp
const AtlasGlyph * g = atlas.lookup(gid);
const float penX = rect.pos.x + subRun.positions[i].x;
const float penY = rect.pos.y + subRun.positions[i].y;
const float minX = std::round(penX + g->fLeft);   // pixel snap
const float minY = std::round(penY - g->fTop);    //  ←
const float maxX = minX + g->fWidth;
const float maxY = minY + g->fHeight;
```

Vertex UV pairing becomes canvas-top ↔ `v0` (smaller V = top of
tile), eliminating the implicit flip.

**`GlyphAtlas::ensureGlyph` (no upload flip)**

Today's code copies each row in reverse — the comment at line 144
in `wtk/src/Composition/backend/GlyphAtlas.cpp` calls this out as
"one of the three orientation flips". After 2.5, the **rasterize
callback** is responsible for emitting the tile top-row-first; the
upload path is a single straight `copyBytes`. Concretely on Linux:

```cpp
// msdfgen emits Y-up; rewrite into top-row-first order at the same
// loop where we quantize float → uint8.
for(unsigned y = 0; y < tileH; ++y){
    for(unsigned x = 0; x < tileW; ++x){
        const float * px = msdf((int)x, (int)(tileH - 1 - y));  // ← flip read, not write
        // ...
        out.rgb[(y * tileW + x) * 3 + 0] = quant(px[0]);
        // ...
    }
}
```

The flip moves from "happens on every upload" to "happens once per
glyph at rasterize time" — same arithmetic cost, but the atlas now
has a single canonical orientation. Same change applies to the
DWrite and Core Text rasterize lambdas when those backends light up.

**Sub-pixel positioning (deferred)**

Skia's per-glyph sub-pixel variant rasterization is *not* in scope
for 2.5. Phase-2.5 ships *integer-pixel pen snapping* via the
`std::round(...)` calls above; full sub-pixel variants can land
later if visual quality demands it. Pixel snapping alone gets us
out of the jitter band.

### Cross-platform impact

The atlas flip lives in `GlyphAtlas.cpp` (backend-shared) and the
rasterize lambdas live in each `*FontEngine.cpp`. Phase 2.5 touches
all three:

| Backend | Today | After 2.5 |
|---|---|---|
| Linux (`HarfbuzzFontEngine.cpp`) | Pango lock + FT outline → msdfgen → Y-up tile + upload flip | Direct FT outline → msdfgen → top-row-first tile + no flip |
| Windows (`DWriteFontEngine.cpp`) | Stub rasterize (returns false) | Same convention when it lights up — Phase 5 / 6 work piggybacks on 2.5 |
| macOS (`CTFontEngine.mm`) | Stub rasterize | Same |

Doing 2.5 *before* the macOS/Windows MSDF lambdas write to disk
saves a churn cycle: those backends arrive in the post-2.5 world
and only ever emit the canonical orientation.

### Verification

- `TextCompositorTest` (windowed, OMEGAWTK_TRACE_TEXT=1): all glyphs
  on the same line share a visible baseline; `'`, `d`, `p`, `g`
  align with `e`, `n`, `r`. Same string rendered at DPR=2 stays
  crisp without re-rasterization.
- `TextLayoutEngineTest`: existing assertions unaffected (they test
  the layout engine, not the renderer).
- Trace logs (`[wtk-text] QUAD gid=`): `canvasY=[minY,maxY]` per
  glyph now has the *top* anchored to `penY - fTop` directly; the
  emit-side trace gains an `fTop`/`fHeight` field so a quick scan
  shows whether jitter is in the rasterizer (varying fTop wrong)
  or the layout engine (varying penY wrong).

## Phase 3.5 — Wrap + sub-pixel positioning

After Phase 3 the engine handles mixed-direction text correctly but
still has two visible gaps:

1. **No wrap.** `TextLayoutDescriptor::Wrapping::WrapByWord` /
   `WrapByCharacter` and `lineLimit` are ignored; any string longer
   than `rect.w` overflows the right edge.
2. **Pen-snapped advances.** Phase 2.5's pixel-snap on the X axis
   eliminates fractional pen positions, which means each glyph's
   left edge lands on an integer pixel column. The cumulative
   rounding error (`round(penX + fLeft) - (penX + fLeft)` mod 1)
   surfaces as uneven inter-glyph gaps — visible on long lines as a
   subtle "compressed/expanded" rhythm. Phase 3.5 drops the snap on
   both axes; the deviations are sub-pixel and the MSDF distance
   field handles them. If baseline shimmer turns out to be visible
   in practice we can re-snap Y, but defaulting to off keeps the
   path uniform.

### Wrap

The pipeline grows one extra pass between *shape* and *layout*:

```
shape per (bidi run × script run)    [Phase 3]
    ↓ shaped clusters + advances + break opportunities
break + measure (this phase)          → split into wrap-lines
    ↓ per wrap-line cluster list
position + align                       [Phase 2's per-line pass]
```

**`OmegaCommon::BreakIterator(Line)`** already exists and exposes
both mandatory and *soft* break opportunities; Phase 2 only acted on
mandatory ones. Phase 3.5 walks the soft boundaries too:

- For `Wrapping::WrapByWord`, prefer breaking at word-end opportunities
  (the line iterator's default behaviour — ICU bundles word + word-end
  rules into its line iterator). Hard-break inside a single run only
  when the run itself is wider than `rect.w` and `Wrapping !=
  Wrapping::WrapByCharacter`.
- For `Wrapping::WrapByCharacter`, accept every break opportunity.
- Trailing whitespace at a wrap boundary doesn't count against `rect.w`
  (otherwise a single trailing space could push a word to the next
  line unnecessarily). The shaped advance for the space stays on the
  *outgoing* line, but the width-fit check excludes it. Matches
  CSS / Pango behaviour.
- `lineLimit > 0` clips the wrapped output to N lines. The last
  line is *not* ellipsized in 3.5 — ellipsis is a follow-up. Any
  text past the limit is dropped silently from `LayoutResult`.

**Where wrap fits in the bidi/script loop.** The shape pass already
produces per-cluster advances *and* cluster start offsets (the
`hb_glyph_info_t::cluster` HarfBuzz exposes). For 3.5 we shape the
*whole logical line* first (as Phase 3 does), then in a second pass:

1. Walk `BreakIterator(Line)` over the original line text.
2. For each break opportunity, look up the cluster boundary in the
   shaped run and accumulate advance width up to that point.
3. When the accumulated width would exceed `rect.w`, emit everything
   up to the *previous* break opportunity as one wrap-line. Restart
   accumulation from there.
4. If a single cluster is wider than `rect.w`, force-break at the
   character level for `WrapByCharacter` (or accept the overflow for
   `WrapByWord`).

This costs one extra shape per logical line and a linear walk over
the break iterator — both already in the budget. Kerning at the new
line ends differs slightly from "shape this segment from scratch"
(the inter-cluster spacing at the wrap point reflects the original
context, not the new line break), but the artifact is sub-pixel and
matches Skia's `SkShaper`-driven wrap output.

**Vertical alignment with multi-wrap-line blocks.** The block
height grows by `wrapLineCount * lineHeight()` and
`firstBaselineY` already accepts a `lineCount`. No change in the
align pass.

### Sub-pixel positioning

Phase 2.5 currently rounds both axes:

```cpp
const float minX = std::round(penX + g->fLeft);
const float minY = std::round(penY - g->fTop);
```

Neither round is load-bearing for correctness, only for "every quad
lands on integer pixel columns/rows". On a long line, the
difference between the accumulated `penX + fLeft` and its rounded
value drifts by up to ±0.5 px per glyph; those tiny offsets are
what the eye reads as uneven spacing. The Y round contributes the
same kind of sub-pixel deviation per glyph, but is small enough
that the MSDF AA absorbs it without visible shimmer.

Phase 3.5 drops both rounds:

```cpp
const float minX = penX + g->fLeft;   // sub-pixel
const float minY = penY - g->fTop;    // sub-pixel
```

The MSDF distance field handles sub-pixel quad placement cleanly:
the fragment shader's `smoothstep(0.5 - aa, 0.5 + aa, median)`
already produces correct fractional coverage along the glyph edge,
and bilinear texture sampling produces the right interpolated
distance value at fractional canvas-pixel positions. No shader
changes — only the per-glyph quad authoring math.

If a future test surface shows visible baseline shimmer on long
runs (the failure mode the Y snap was originally hedging against),
the `std::round` can come back on the Y axis alone. Defaulting to
no snap keeps the code path uniform across axes.

Skia's full sub-pixel handling for bitmap text pre-rasterizes each
glyph at N sub-pixel variants (typically 4 at 0.25 X-offset
increments) and picks the closest variant at draw time. The MSDF
path doesn't need that — the SDF already encodes the silhouette at
sub-pixel resolution; fractional quad placement just samples it at
the right offset.

### Risks

- **Wrap-shape mismatch.** Shaping the whole logical line then
  splitting can give slightly different inter-cluster spacing than
  shaping each wrap-line independently. Mitigation: accept the
  artifact (Skia / Pango both do it the same way). Re-shape per
  wrap-line is a future option if a real use case demands it.
- **Sub-pixel + transformed text.** When a Compositor transform
  scales the text non-uniformly, the sub-pixel position no longer
  corresponds to a pixel boundary anyway; the AA is already coming
  from the MSDF distance band. Sub-pixel and transformed cases stay
  consistent — both rely on the same fragment-shader smoothstep.

### Verification

- `TextLayoutEngineTest`: new mock-shaper tests covering
  (a) `WrapByWord` with one word that fits + one that doesn't;
  (b) `WrapByCharacter` forcing mid-word break;
  (c) `lineLimit = 1` truncating;
  (d) trailing-whitespace exemption from width fit.
  All assert against the new `LayoutResult.lineBaselines.size()`.
- `TextCompositorTest` (windowed): visual check — long Latin string
  wraps at word boundaries inside the rect, and inter-glyph spacing
  reads cleanly across the wrapped lines.

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
- **The MSDF jitter we're chasing right now.** Phase 1 + 2 took
  the *layout* origin in hand but left the *rendering* origin alone;
  the visible jitter on Linux survived the migration to a WTK-owned
  layout engine. Root cause turned out to be in `AtlasGlyph` /
  `emitTextSubRun`, not in the platform layout engine — see the
  Phase 2.5 section above. Phase 2.5 is the fix; Phases 5 / 6 no
  longer have a "jitter resolves here" footnote because 2.5 lands
  for all three backends at once via the shared `GlyphAtlas` /
  render-target code.
