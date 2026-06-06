# Resize Clamping Plan

**Status:** active, no implementation yet (2026-06-05).
**Motivating finding:** `UIView-Render-Redesign-Plan.md` Phase F validation
(BasicAppTest, multi-shape row) — Phase F's resize+repaint pipeline works,
but on extreme shrinks the shape primitives **distort vertically** because
nothing in the layout system enforces a per-widget minimum, and nothing at
the window level prevents the user from dragging the window narrow enough
to make a non-degenerate layout impossible. This plan addresses both,
plus the related "content-adaptive dropping" feature the developer
flagged as a follow-up (drop low-priority widgets rather than crush them
once the window passes a configurable threshold).

## 1 Problem statement

### 1.1 Observed distortion (from Phase F validation)

`BasicAppTest`'s shape row contains a `Rectangle` (red), a
`RoundedRectangle` (blue), an `Ellipse` (green), and a `Rectangle`
(yellow) — every shape built at intrinsic 80 × 80 dp, packed into an
`HStack` whose cross-axis is centered. Phase F's window-resize path
relayouts and repaints the whole tree on every drag delta, which is
the correct behavior for Phase F. The visible bug is that as the
window shrinks vertically, the `HStack`'s cross extent drops below
80 dp, and the shapes are then sized to fit whatever vertical space
remains — the green ellipse stops being a circle, the rectangles
visibly squash. Each shape was authored at a single intrinsic size and
neither `LayoutStyle.clamp.minHeight` nor `LayoutStyle.aspectRatio`
is populated at construction, so the layout has no reason to refuse
to shrink them.

### 1.2 Missing feature: window-level resize clamping

The user can drag the window down to the OS-imposed minimum (usually
~50 × 50 px on macOS, even smaller on X11), well below any sensible
size for the widget tree currently displayed. The plumbing exists —
`NativeWindow::setMinSize(float w, float h)` is already on the interface
(`wtk/include/omegaWTK/Native/NativeWindow.h:185`) and every backend
overrides it — but nothing in the WTK side wires the widget tree's
aggregate minimum up to the native window. The result is that the user
can drag the window past the layout's collapse threshold without the OS
stopping them, which is what triggers the §1.1 distortion in the first
place.

### 1.3 Missing feature: content-adaptive dropping

Some widgets are "drop-able" — a toolbar button row should show all
buttons when there is room, hide the lowest-priority ones to fit fewer,
and eventually collapse into a "more" overflow menu. The current layout
system has no expression of priority; every widget is treated as equally
load-bearing during shrink. Without this, a small window either (a)
distorts content (§1.1) or (b) hits an arbitrarily high min-size
floor (§1.2) and stops being resizable. Drop priority gives a third
choice: shed widgets cleanly and keep what fits.

## 2 Goals

- **G1 (correctness).** On any window size, every visible widget renders
  at or above its declared intrinsic minimum — never at a smaller pixel
  geometry. The Phase F validation distortion goes away because the
  layout refuses to honor the impossible shrink request, and the
  remaining space pressure is taken by §G2 / §G3.
- **G2 (predictable resize range).** The native window's minimum drag
  size is the aggregate minimum of its current widget tree, so the OS
  prevents the user from entering an impossible state instead of the
  layout silently producing one.
- **G3 (graceful degradation).** Widgets can declare a drop priority;
  when the layout cannot fit them all at their minimum, the lowest-
  priority widgets are hidden (or routed to an overflow surface) so the
  rest still fit cleanly.

Non-goal: aspect-ratio-locked container behavior (`object-fit`-style
letterboxing for views with intrinsic aspect ratios). That is a
separate plan; this one stays focused on the shrink-direction failure
mode the developer surfaced.

## 3 Existing infrastructure to lean on

The layout system already has the data model these features need —
this plan is mostly about *populating* it and *wiring* it through, not
inventing new primitives.

| Surface                                    | What it does today                          | Phase F follow-up gap |
|--------------------------------------------|---------------------------------------------|------------------------|
| `LayoutClamp` (`Layout.h:52`)               | `minWidth` / `minHeight` / `maxWidth` / `maxHeight` per node | Shape primitives don't populate it. |
| `LayoutStyle.clamp` / `aspectRatio` (`Layout.h:107,123`) | Per-node clamp + aspect-ratio hint | Same — never set by shape ctors. |
| `LayoutStyle.flexShrink` (`Layout.h:122`)  | Default 1.0 = unconstrained shrink         | No widget overrides it; shapes silently collapse. |
| `resolveClampedRect` (`Layout.h:181`)      | Applies the clamp to an available rect     | StackLayout / FlexLayout currently arrange children without consulting it on cross-axis when crossAlign != Stretch. |
| `Widget::measureSelf` (`Widget.h`)         | Per-widget intrinsic-size hint              | Shape primitives return their current rect, not a *minimum*. |
| `NativeWindow::setMinSize` (`NativeWindow.h:185`) | Backend-implemented native min-size sink | No WTK-side caller. |

The plan adds:
- `MeasureResult.minWidthDp` / `minHeightDp` so a widget can publish a
  hard floor distinct from its preferred size.
- A drop-priority field on `LayoutStyle` (or `PaintOptions`) plus the
  overflow-decision step in `FlexLayout::arrange`.
- A `WidgetTreeHost::aggregateMinSize()` that the AppWindow forwards to
  `NativeWindow::setMinSize` whenever the widget tree changes.

## 4 Phases

Implementation order is chosen so each phase is independently shippable
and observably fixes one of the §1 problems.

### Phase 1 — Per-widget intrinsic-minimum: shape primitives stop distorting [smallest, ships first]

**Scope.** Populate `LayoutStyle.clamp.minWidth` / `minHeight` and
`aspectRatio` from each shape primitive's constructor props. Make
`StackLayout::arrange` and `FlexLayout::arrange` consult the clamp on
the cross axis when sizing a centered/start-aligned child (they
already do for stretch alignment via `resolveClampedRect`; this
extends the same check to the non-stretch paths).

**What "ships":** the §1.1 distortion in BasicAppTest visibly goes
away. The shapes refuse to shrink below 80 × 80, and the layout
overflows the HStack instead of squashing them. Overflow is ugly but
honest — Phase 2 turns it into a window-level OS-enforced minimum,
Phase 3 turns it into clean widget dropping.

**Code surface (estimate: ~150 LOC across 5 files).**
- `wtk/include/omegaWTK/UI/Widget/Shape.h` (or wherever Rectangle /
  RoundedRectangle / Ellipse live) — ctor passes the constructor
  `Rect{w,h}` through to `setLayoutStyle({ .clamp = { .minWidth =
  Dp(w), .minHeight = Dp(h) } })`. Ellipse additionally publishes
  `aspectRatio = 1.0` so its minimum is "the smaller of available w/h
  squared", not "shrink to fit cross-axis".
- `wtk/src/UI/StackLayout.cpp` (or wherever StackWidget's arrange
  lives) — replace the unclamped `child->setRect(...)` calls on the
  cross-axis non-stretch path with a `resolveClampedRect` pass that
  honors the child's `LayoutStyle.clamp`.
- `wtk/src/UI/FlexLayout.cpp` — same change for the flex arrange
  path. (FlexLayout's measure pass already reads cached preferred
  sizes; arrange is where the clamp lives in StackLayout terms.)

**Validator.** `BasicAppTest` drag from full-size down to the smallest
draggable size. The shapes hold 80 × 80 across the whole drag range;
the HStack visibly overflows when there is not enough horizontal room
(acceptable interim — Phase 2 fixes it).

**Out of scope for Phase 1.** Aggregate min propagation,
content-dropping. Phase 1 is a "correctness floor" change only.

### Phase 2 — Window-level aggregate min: OS-enforced resize range

**Scope.** Roll up the widget tree's per-widget `LayoutStyle.clamp.min*`
into a single aggregate `{minW, minH}` for the current root, and push
that to `NativeWindow::setMinSize` on (a) `AppWindow::setRootWidget`,
(b) any tree mutation that could change the aggregate (add/remove
child, `setLayoutStyle`), and (c) on each `onRealize` re-fire (DPI
changes can scale the minimum). The aggregate walk runs at the
WidgetTreeHost layer so AppWindow stays thin.

**Aggregate semantics.**
- For a `LayoutDisplay::Stack` parent, the aggregate is the sum along
  the main axis and the max along the cross axis (with padding /
  margins added on).
- For a `LayoutDisplay::Flex` parent (Row / Column), the same — sum on
  main, max on cross.
- Absolute-positioned children do not contribute to the aggregate
  (their layout is decoupled from the parent's flow).
- DPI conversion happens at the AppWindow → NativeWindow boundary,
  using the current screen's scale factor. Re-runs on `onRealize`
  because the same logical min becomes a different pixel min on a
  different-density display.

**What ships:** the user cannot drag the window narrower or shorter
than what its current widget tree needs. The OS title bar resists the
drag at the right point; the Phase 1 overflow case becomes
unreachable in practice.

**Code surface (estimate: ~200 LOC across 4 files).**
- `wtk/src/UI/WidgetTreeHost.{h,cpp}` — new
  `WidgetTreeHost::aggregateMinSize() const -> {float, float}` walking
  the root widget's view tree (size class returned in dp).
- `wtk/src/UI/AppWindow.cpp` — call `aggregateMinSize`, multiply by
  `currentScreen().scaleFactor`, call `nativeWindow->setMinSize(...)`.
  Wired into `setRootWidget` (already there, just append) and into
  the existing `onRealize` lambda (Phase F) for DPI re-runs.
- A mutation-side hook: `Widget::setLayoutStyle` (and the
  add/removeChild surface) calls
  `treeHost->refreshAggregateMinSize()`. The refresh is a debounced
  request that flushes at the end of the current frame so a burst of
  mutations does not re-walk on each call.

**Validator.** BasicAppTest no longer permits the user to drag the
window into the Phase 1 overflow state. macOS / Windows / GTK each
honor the min-size at the title bar.

### Phase 3 — Drop priority: content-adaptive widget hiding

**Scope.** Add a `LayoutStyle::dropPriority` field (lower = drop first,
default = `Required` which means never drop). FlexLayout / StackLayout
extends their measure pass: if the aggregate of all children's
minimums exceeds the available main-axis extent, drop the lowest-
priority child whose drop priority is below `Required`, remeasure,
repeat until either (a) the remaining children fit, or (b) only
`Required` children remain and the layout overflows / clips (Phase 2
prevents this state at the OS level for the root, but nested layouts
may still need to clip — that is a §4.4 decision).

**Drop semantics.**
- A dropped widget is *hidden*, not destroyed. Its `View` paint is
  suppressed for the frame; layout treats it as zero-sized; input
  hit-testing skips it.
- Drop-and-restore happens during the same `dispatchResize*ToHosts`
  scope as Phase F's repaint — one frame transition, no flicker.
- An optional "overflow sink" — a widget the layout can hand dropped
  children to (think `OverflowMenu`). When set, dropped children are
  routed to it instead of hidden; when not set, they are hidden. The
  overflow sink itself participates in the aggregate min (so a window
  with overflow still has a non-zero min).

**Code surface (estimate: ~350 LOC across 6 files).** Larger than
Phases 1 + 2 because the measure pass gains an iteration loop and a
new widget kind (`OverflowMenu`) joins the tree. Sub-phased internally:

- 3.1 — `LayoutStyle::dropPriority` field + accessor on Widget.
  Drop-aware widgets opt in via `setLayoutStyle({.dropPriority =
  Priority::Low})`. ~30 LOC, no behavior change yet.
- 3.2 — FlexLayout / StackLayout: priority-aware iterative drop in
  `measure`. ~150 LOC. Shippable on its own (no overflow sink, just
  hiding).
- 3.3 — `OverflowMenu` widget + `setOverflowSink` on the parent
  layout. ~150 LOC. Layered on top of 3.2 without changing it.

**Validator.** A button toolbar (think the test's `Click me / Slow
hover / Snap / Disabled` row) where each button has a different drop
priority. Shrinking the window past the toolbar's preferred width drops
buttons one at a time, lowest-priority first; with an overflow sink,
the dropped buttons re-appear inside a "more" menu.

### Phase 4 — Documentation + integration

- Update `wtk/docs/UIModel.rst` to describe the LayoutClamp / aspect /
  drop-priority story end-to-end. Include the BasicAppTest before /
  after screenshots as the canonical example.
- Cross-link from `UIView-Render-Redesign-Plan.md` Phase F's
  follow-up section: Phase F is the resize *correctness* layer
  (every widget repaints at the new size); this plan is the resize
  *semantics* layer (what "the new size" means when it would be too
  small).

## 5 Sequencing and dependencies

- Phase 1 ships first and on its own — it is the smallest unit that
  fixes a visible regression, and depends only on infrastructure that
  already exists.
- Phase 2 depends on Phase 1's per-widget minimums being populated
  (otherwise the aggregate is identically zero and `setMinSize` is a
  no-op). Phase 2 can land next.
- Phase 3.1 / 3.2 / 3.3 are internally sequenced, each shippable,
  each independent of any other plan. They can interleave with other
  work freely.
- Phase 4 is documentation and should land alongside Phase 3.3 (the
  last functional change) so the docs match shipped behavior.

## 6 Open questions

- **Cross-plan: aspect-ratio clamping vs `LayoutClamp` cross-axis
  clamp.** `LayoutStyle.aspectRatio` already exists. For the Ellipse
  case, Phase 1 could either (a) set
  `clamp.minWidth = clamp.minHeight = 80dp` (rectangular minimum),
  or (b) set `aspectRatio = 1.0` and a single dimension as min
  (aspect-locked minimum). The first is simpler; the second matches
  how CSS handles `aspect-ratio` + `min-width`. Phase 1 implementation
  decides; either is correct for the test scene.
- **Resize-Clamping vs Phase F's force-full-repaint cost.** Phase F
  re-emits every widget's DisplayList on every resize delta. With drop
  priority, every shrink past a threshold also runs the drop
  iteration. That iteration is bounded by the number of dropable
  widgets (small in practice), but it does add per-resize-tick CPU.
  Should be fine until Phase G's content cache lands; flag if it
  shows up on a profile before then.
- **Animated drop transitions.** Phase 3.2 drops are instantaneous —
  a button is visible at one window size, invisible one drag delta
  later. CSS-style drop animations (fade-out, collapse-on-axis) are a
  natural follow-up but explicitly out of scope here; they belong to
  the same surface that owns `LayoutTransition` (`Layout.h:189`).
