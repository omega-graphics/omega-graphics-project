# Text Measurement API Plan

**Status:** proposed, not started (2026-06-24). Spun out of
`Resize-Clamping-Plan.md` §1.7 (content-driven sizing) — that plan's
final open bug is a wrapping `Label` whose height does not track its
width, which needs real text metrics this plan provides.

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

The blocker is that **no synchronous text-measurement API exists**.
`Label::measureSelf` (`wtk/src/Widgets/Primatives.cpp`) and the backed-
out hook both estimate height from `charCount × pointSize × 0.6` and a
`1.2` line factor. That estimate is wrong in two ways that make it
unusable:

1. **Unknown font size.** The description sets no explicit `props_.font`,
   so it renders with the **GTK system theme font** (`gtk-font-name`,
   e.g. "Cantarell 11", parsed in `wtk/src/Native/gtk/GTKTheme.cpp`).
   The widget cannot read that resolved size synchronously, so the
   estimate used a hard-coded `16 dp` and produced a box ~40% too short
   → the text truncated to ~3 of ~5 lines.
2. **Rough glyph metrics.** The `0.6` average-char-width factor is a
   guess; real wrapping depends on per-glyph advances.

Two heuristic attempts (overlap, then clip) confirmed: **do not ship a
guessed text height.** It needs the engine that actually lays the text
out to report the size.

## 2 What already exists (the dormant plumbing)

The Resize-Clamping work landed the *consumer* side of content-driven
sizing, deliberately inert until this API exists:

- **`View::ContentMeasureFn` hook** —
  `void(float availWidthDp, float availHeightDp, float& outWidthDp,
  float& outHeightDp)`, with `setContentMeasure` / `hasContentMeasure` /
  `measureContent` (`wtk/include/omegaWTK/UI/View.h`,
  `wtk/src/UI/View.Core.cpp`, field in `wtk/src/UI/ViewImpl.h`).
- **`FlexLayout::measure` consults it** — for a child with a hook, it
  computes the available cross extent and overrides the child's
  main-axis preferred size with the hook's result (the cross axis stays
  owned by stretch). `wtk/src/UI/LayoutManager.cpp`, the
  `child->hasContentMeasure()` branch. No-op when no widget installs a
  hook (current state).
- **`Label::onMount`** has the install site commented out with a pointer
  to this plan (`wtk/src/Widgets/Primatives.cpp`).

So once a measurement API exists, wiring is: `Label::onMount` installs a
`ContentMeasure` hook that calls the API. Nothing else changes — the
layout and the Phase 2 window-min walk (`minSize` reads the resulting
rect) pick it up automatically.

## 3 Proposal: a measurement entry point on the text layout engine

The text layout engine (the Composition-side text/glyph layout that
`Label::rebuildContent` feeds via the `UIView` text element) already
shapes and wraps the text to render it — it has the measurement
internally. Expose it:

> `measure(text, font, availableWidthDp, wrapping, lineLimit) -> {widthDp, heightDp}`

Returns the laid-out (wrapped) size for the given constraints, without
requiring a paint. Same inputs the layout already passes to the text
element; the height is exactly what the renderer will produce, so the
box matches the text (no clip, no overflow).

**Tasks (to detail when scheduled):**

1. **Locate the engine's measure point.** Identify the Composition text
   layout class that `Label`'s text element drives (shaping / line
   breaking). The measurement should reuse that exact path so it matches
   what gets painted — not a parallel estimate.
2. **Add the public measure API.** A pure-measure call (no surface, no
   paint) returning wrapped dp size. Resolve the effective font: explicit
   `props_.font` if set, else the theme default the renderer would use
   (the same `gtk-font-name`-derived size from `GTKTheme`, and the macOS /
   Win equivalents — flag the two off-platform backends as unverified).
3. **Install the `Label` content-measure hook** (`Label::onMount`)
   backed by the API. Remove the §1.7 "not installed yet" note.
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
  height**; this plan supplies that height.
- The `minSize` Phase 2 caveat (leaf intrinsics read from `getRect`): a
  real measure API also lets `minSize` source an exact wrapped height
  instead of the current laid-out rect.
