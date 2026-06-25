# Text Measurement API Plan

**Status:** proposed, not started (2026-06-24; grounded against source
2026-06-25). Spun out of `Resize-Clamping-Plan.md` §1.7 (content-driven
sizing) — that plan's final open bug is a wrapping `Label` whose height
does not track its width, which needs the real text metrics this plan
exposes.

## 1 Problem

A wrapping `Label`'s height is **content-driven**: it is a function of
the available width (more wrapping → more lines → taller). Today the
layout has no way to learn that height, so the description `Label` in
`BasicAppTest` is frozen at its construction-time height (60 dp). When
the window is narrow the text wraps past 60 dp and either:

- **overflows** its box and the bottom-pinned button row overlaps it
  (the first symptom the developer flagged), or
- **clips** to the box when the box is sized from a guess (the second
  symptom — see below).

The blocker is that **no measurement is wired into the layout**, and the
two estimates that exist are unusable:

1. **`Label::measureSelf` is dead code.** It estimates height from
   `charCount × pointSize × 0.6` and a `1.2` line factor
   (`wtk/src/Widgets/Primatives.cpp:293`), but nothing in the tree ever
   calls `measureSelf` — it is an orphaned `Widget` override with no call
   site. It is not driving any sizing today.
2. **The `ContentMeasure` hook — the live seam — is not installed.**
   `FlexLayout::measure` is the only consumer of measurement
   (`wtk/src/UI/LayoutManager.cpp:525`), via the `View::ContentMeasureFn`
   hook. `Label::onMount` does not install one
   (`wtk/src/Widgets/Primatives.cpp:253`), so the layout falls back to
   the Label's frozen rect.

A rough guess is not good enough regardless of where it is wired: the
`0.6` average-char-width factor ignores per-glyph advances, so the
wrapped line count it predicts does not match what the engine actually
produces. Two heuristic attempts (overlap, then clip) confirmed: **do
not ship a guessed text height.** The engine that lays the text out must
report the size.

### 1.1 What §1 of the proposal originally got wrong (corrected here)

The earlier draft claimed the description `Label` renders with the
**GTK system theme font** (`gtk-font-name`, parsed in
`wtk/src/Native/gtk/GTKTheme.cpp`) and that the widget "cannot read that
resolved size synchronously." **That is not how the code works.** The
font for a text element is always known synchronously
(`wtk/src/UI/UIView.Update.cpp:399`):

```cpp
auto font = fontHandle != nullptr ? fontHandle : impl_->resolveFallbackTextFont();
```

`resolveFallbackTextFont()` returns a hard-coded **Arial 18**
(`wtk/src/UI/UIView.Animation.cpp:405`). So the effective font is:
explicit style `TextFont` (set by `Label::rebuildContent` only when
`props_.font` is non-null, `Primatives.cpp:275`), else Arial 18. The
NativeTheme / `gtk-font-name` path is **not** consulted for `Label`
text. There is therefore no theme-font resolution to write and no
per-backend theme divergence to flag as unverified — that part of the
original Task 2 is dropped.

The genuine subtlety: the fallback font lives on **`UIView::Impl`**, not
on the `Label`, and `LabelProps::font` defaults to `nullptr`
(`Primatives.h:131`). Any measurement that gates on `props_.font` (as
the dead `measureSelf` does) will bail to a no-op for the exact
font-less description `Label` this plan targets. The measurement must
resolve the **same** effective font that paint uses, which is why the
public seam lives on `UIView` (§3).

## 2 What already exists (the dormant plumbing)

The Resize-Clamping work landed the *consumer* side of content-driven
sizing, deliberately inert until measurement is wired:

- **`View::ContentMeasureFn` hook** —
  `void(float availWidthDp, float availHeightDp, float& outWidthDp,
  float& outHeightDp)`, with `setContentMeasure` / `hasContentMeasure` /
  `measureContent` (`wtk/include/omegaWTK/UI/View.h:278`,
  `wtk/src/UI/View.Core.cpp:160`, field `contentMeasure_` in
  `wtk/src/UI/ViewImpl.h:206`). All units are dp, in and out.
- **`FlexLayout::measure` consults it** — for a child with a hook, it
  computes the available cross extent and overrides **only the child's
  main-axis** preferred size with the hook's result; the cross axis stays
  owned by stretch (`wtk/src/UI/LayoutManager.cpp:525-540`). No-op when
  no widget installs a hook (current state). For a vertical stack the
  main axis is height — which is exactly the content-driven dimension.
- **`Label::onMount`** has a descriptive note where the install belongs,
  pointing at this plan (`wtk/src/Widgets/Primatives.cpp:253`). (Note:
  it is prose, not a literal commented-out line.)

So once measurement is exposed, wiring is: `Label::onMount` installs a
`ContentMeasure` hook that delegates to its `UIView`. Nothing else
changes — the layout and the Phase 2 window-min walk pick it up
automatically.

## 3 Proposal: a measure method on `UIView`, wrapping the layout engine

The text layout engine already shapes and wraps the text to render it,
and the height it computes is the height the renderer uses. The
measurement core is therefore **already present**:

`TextLayoutEngine::layout(text, font, metrics, rect, desc, shaper, fallback)`
(`wtk/include/omegaWTK/Composition/TextLayoutEngine.h:141`, impl
`wtk/src/Composition/TextLayoutEngine.cpp:133`) is a **static, CPU-only**
function. It already wraps to `rect.w`
(`TextLayoutEngine.cpp:409`), truncates to `lineLimit`
(`TextLayoutEngine.cpp:587`), and returns
`LayoutResult::layoutHeight = metrics.lineHeight() * lineCount`
(`TextLayoutEngine.cpp:634`). No surface and no paint are required.

The only production caller, `shapeTextForDisplayList`
(`wtk/src/Composition/DisplayList.cpp:47`), assembles the inputs the
engine needs — `FontEngine::inst()->shaper()`, `->fallback()`, and
`font->getMetrics()` (`DisplayList.cpp:59-90`) — then calls `layout()`.
Everything after the `layout()` call (sub-run grouping,
`ensureGlyphsResident`, `rasterizeSubRunToTexture`) is paint-only. A
measurement reuses the inputs and stops at `layoutResult.layoutHeight`.

**Public seam — on `UIView`, not the engine.** Paint/measure parity
requires the *same* font + `TextLayoutDescriptor` + Arial-18 fallback
resolution. `UIView` already owns all three (the text element, the
resolved style, and `resolveFallbackTextFont()`), so the public entry
point belongs there:

> `UIView::measureText(tag, availWidthDp) -> heightDp`

It resolves the element's effective font and descriptor exactly as paint
does, calls `TextLayoutEngine::layout` with a rect of
`{0, 0, availWidthDp, large}` (only `rect.w` matters — `rect.h` feeds
only vertical alignment, not `layoutHeight`), and returns
`layoutResult.layoutHeight`. A standalone engine API the `Label` calls
with a hand-resolved font was rejected: the `Label` would have to reach
`UIView`'s private fallback or duplicate the Arial-18 default, which
drifts from the render path.

**Height-only.** `LayoutResult` exposes `layoutHeight` + `lineBaselines`
but **no laid-out width** (`TextLayoutEngine.h:42-54`). The bug needs
only height: `FlexLayout::measure` overrides the main axis (height for a
vertical stack) and leaves width to stretch
(`LayoutManager.cpp:524-540`). So the API returns height; the hook's
`outWidthDp` passes through the available width unchanged. (If width is
ever wanted, add a `layoutWidth = max(line.totalAdvance)` field in the
emit loop — out of scope here.)

**Units = dp.** `FontMetrics` are "pixels at the font's current point
size" (`FontEngine.h:113`), the font is created at nominal size with no
render-scale baked in, and render scale is applied downstream (only
`rasterizeSubRunToTexture` takes `renderScale`; MSDF glyph positions
come straight out of `layout()`). So `layout()` operates in logical/dp
space and `layoutHeight` is dp — matching the dp-in/dp-out
`ContentMeasureFn` contract. **Verify at implementation time:** each
backend's `Font::getMetrics()` returns nominal-point-size metrics, not
metrics pre-scaled by render scale (header contract says nominal;
spot-check the Vulkan/HarfBuzz path, flag macOS/Win as off-platform
unverified).

### Tasks

1. **Add `UIView::measureText(tag, availWidthDp) -> heightDp`.** Resolve
   the tagged text element's effective font (`resolved` style `TextFont`
   else `resolveFallbackTextFont()`) and `TextLayoutDescriptor`
   (alignment / wrapping / lineLimit) the same way `UIView::update`
   does (`UIView.Update.cpp:396-419`), build `metrics = font->getMetrics()`,
   call `TextLayoutEngine::layout` with `rect.w = availWidthDp`, return
   `layoutResult.layoutHeight`. Returns 0 when the element has no text or
   no font/shaper is available (degrade to the caller's fallback).
2. **Install the `Label` content-measure hook** (`Label::onMount`)
   delegating to `viewAs<UIView>().measureText("label", availWidthDp)`;
   write `outHeightDp` from the result and pass `outWidthDp` through.
   Remove the §1.7 "not installed yet" note.
3. **Delete the dead `Label::measureSelf` heuristic** (`Primatives.cpp:293`)
   — it has no call site and its `props_.font`-gating is the bug pattern
   §1.1 warns against. (Confirm no caller first: no `->measureSelf(` /
   `.measureSelf(` exists in-tree.)
4. **Validate** against `BasicAppTest`: narrow the window so the
   description wraps to many lines — the box grows to fit the text (no
   clip), the buttons stay below (no overlap), and the Phase 2 window
   min-height grows so the window cannot be dragged short enough to clip.

## 4 Cross-links

- `Resize-Clamping-Plan.md` §1.6 (slot-vs-widget model — the spacer slot
  already keeps the buttons below a correctly-sized description) and §1.7
  (the freeze's sanctioned exception: a widget's geometry *may* change on
  resize when its own content requires it — text reflow). The slot model
  already does the right thing **once the Label reports a correct
  height**; this plan supplies that height via the hook.
- The `minSize` Phase 2 caveat (`LayoutManager.cpp:994-1000`) reads leaf
  intrinsics from `getRect()` and its comment points at the now-dead
  `measureSelf`. The real fix routes the same exact wrapped height
  through `measureContent` / `UIView::measureText` instead of the
  laid-out rect.
