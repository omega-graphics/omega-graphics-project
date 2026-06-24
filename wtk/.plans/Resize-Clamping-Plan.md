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

### 1.4 Observed distortion #2: horizontal-only drag distorts vertically

**New finding (2026-06-24, BasicAppTest, Linux/Vulkan build).** Dragging
the window *wider* — a horizontal resize the user reports as not touching
the height — squashes the whole shape row vertically: the green ellipse
flattens into a wide oval, the rectangles into thin horizontal bands
(screenshots filed with this finding). This looks like a sibling of
§1.1 but the trigger is the opposite axis, which is what makes it worth
a separate diagnostic pass: §1.1 is "shrink the cross axis, the cross
axis distorts" (expected, just unclamped); §1.4 is "shrink the *main*
axis of the row's parent and the row's *cross* axis distorts" — a
cross-coupling that should not exist if every node proposed its
boundaries correctly. The developer's read (2026-06-24): *"the other
elements in the stack aren't properly centered or propose their
boundaries appropriately."*

**Confirmed half of the mechanism (directly observed in code).** The
squash itself is the same code site §1.1 fixes, reached from a
different direction:

- Shape primitives are `resizable` by default —
  `Widget::isLayoutResizable()` returns `true`
  (`wtk/include/omegaWTK/UI/Widget.h:204`); Rectangle / RoundedRectangle
  / Ellipse do not override it.
- In the shape-row HStack's `FlexLayout::arrange`, a centered *resizable*
  child has its cross size clamped down to the row's available cross
  extent: `crossSize = std::min(crossSize, crossAvailable)`
  (`wtk/src/UI/LayoutManager.cpp:806-811`), with `crossAvailable`
  derived from the row's own height
  (`contentCross`, `wtk/src/UI/LayoutManager.cpp:616-620`). The
  `FlexCrossAlign::Center` branch
  (`wtk/src/UI/LayoutManager.cpp:818-820`) re-centers but does **not**
  restore the intrinsic 80 dp. So the moment the row is shorter than
  80 dp, every shape is sized to the row's height — exactly the §1.1
  squash, no per-widget minimum to stop it.
- The shape row's height is itself shrinkable inside the root VStack.
  `shapeRowSlot` sets only `flexGrow = 0`, leaving `flexShrink = 1`
  (`StackSlot` defaults, `wtk/include/omegaWTK/Widgets/Containers.h:48-49`;
  test site `wtk/tests/BasicAppTest/BasicAppTestRun.cpp:186-188`), and
  `flexGrow = 0` alone does **not** lock the main size — the lock
  requires `flexGrow <= 0` **and** `flexShrink <= 0`
  (`wtk/src/UI/LayoutManager.cpp:679-682`). So any vertical overflow in
  the VStack shrinks the row's height below 100 → below 80, and the
  shapes follow it down.

**Unconfirmed half — the width→height coupling (hypotheses, verify
before fixing).** What is *not* yet explained is why a drag that leaves
the window height fixed produces vertical overflow in the VStack at all.
Reading the code, the obvious candidate does **not** account for it:
the description `Label` wraps `WrapByWord`
(`wtk/tests/BasicAppTest/BasicAppTestRun.cpp:210`), so widening the
window reflows its text into *fewer* lines and *reduces* its preferred
height — which would *relieve* vertical pressure, not add it. The
remaining candidates, in rough order of suspicion:

1. **Stale preferred-size cache (lead hypothesis).** `FlexLayout`
   caches each child's `preferredMain` / `preferredCross` and reuses the
   last-seen-good value whenever the live rect looks "suspicious" or
   "placeholder" mid-resize
   (`wtk/src/UI/LayoutManager.cpp:504-520`). If the description Label's
   preferred *height* was cached while the window was narrow (many
   wrapped lines = tall) and a width-only change does not invalidate it
   (the guard sees a plausible height and keeps the stale tall value),
   the VStack measures a phantom over-tall column, computes negative
   free space, and shrinks the `flexShrink = 1` shape row even though
   real vertical room exists. This is a genuine width→height coupling
   bug, and it matches the developer's "doesn't propose its boundaries
   appropriately."
2. **Corner-drag masquerading as horizontal.** The user may be dragging
   a corner that also lowers the height; then §1.4 collapses into §1.1
   and there is no new bug — only the missing per-widget minimum. Rule
   this in/out first: it is the cheapest to falsify (drag a single
   vertical *edge* and confirm height in the resize log is constant).
3. **Cross-axis bound proposed from the wrong axis.** A node somewhere
   in the VStack→HStack chain feeding `crossAvailable` off a
   width-derived quantity. Lower suspicion — `contentCross` reads the
   frame's height directly (`:616-620`) — but the developer's centering
   remark points here, so trace the actual `crossAvailable` value the
   shapes receive during a pure-width drag.

This is a layout-math finding in WTK (not GTE), so it is
backend-independent; it was *observed* on the Linux/Vulkan build, and
the same `FlexLayout` path runs on macOS/Metal and Win/D3D12 unchanged
(those two are screenshot-unverified for this specific symptom).

### 1.5 Refined diagnosis (2026-06-24): the resize is the bug, not the clamp

A second BasicAppTest pass narrows §1.1 / §1.4 to a single root cause
and a cleaner fix than "clamp the minimum." Two observations from the
developer pin it down:

- **Asymmetric.** Vertical *expand* keeps the shapes crisp (red/yellow
  square, green circle); vertical *shrink* squashes them.
- **Persists with free space.** In the shrunk-but-not-tiny window the
  shape row is visibly under 80 dp tall *while the VStack still has
  large vertical slack below the description* — so this is not the
  VStack running out of room and shrinking the row (the §1.4.1 phantom
  overflow). The row is squashed even though, at its original 80 dp, it
  fits with room to spare.

**Mechanism (grounded in code).** `FlexLayout` resizes the shapes
because they are `resizable` (§1.4), and the squash *sticks* because
`measure` writes each child's just-arranged size back into the
preferred-size cache: `entry.preferredCross = curCross`
(`wtk/src/UI/LayoutManager.cpp:512-516`, reading the child's live rect).
So the sequence is:

1. A shrink drag passes through a state where the row's cross extent
   dips under 80 dp; `arrange` clamps each shape down
   (`crossSize = std::min(crossSize, crossAvailable)`,
   `wtk/src/UI/LayoutManager.cpp:806-811`) and writes the squashed
   height to the shape's rect.
2. The next `measure` reads that squashed rect and caches it as the
   shape's `preferredCross`.
3. From then on `min(preferredCross, crossAvailable)` is bounded by the
   *cached* squashed value, so the shape never returns to 80 dp even
   when vertical room comes back — exactly the "plenty of room, still
   squashed" symptom. Pristine→expand never enters step 1, so it stays
   crisp: the asymmetry falls out of the cache write-back.

**Developer directive / Principle (2026-06-24).** *"For most shaped /
text widgets, they cannot be resized by the layout when the window
changes size — only a `scaleFactor` (DPI) change rescales them."* This
is the real fix and it is cleaner than the min-clamp: an intrinsic-
sized leaf widget should report `isLayoutResizable() == false`, so
`FlexLayout` positions it (centering, spacing) but never rewrites its
width/height. A non-resizable child takes the
`item.spec.resizable ? … : item.currentCross` / `childRect.h` branches
(`wtk/src/UI/LayoutManager.cpp:805,836,800`), skips the cross clamp,
and — because `arrange` never writes a squashed size — never poisons
its own cache (step 2 above cannot fire). Both §1.1 and §1.4 / §1.5
distortions disappear with this one change; the min-clamp becomes a
secondary safety net for the widgets that *are* legitimately resizable
(containers).

- **Rule.** Intrinsic-sized leaf widgets (shape primitives, `Label`,
  and most non-container primitives) are not layout-resizable: window
  resize repositions them but does not change their dp geometry.
- **Scope.** Frozen: every direct-`Widget` leaf (`Rectangle`,
  `RoundedRectangle`, `Ellipse`, `Separator`, `Label`, `Icon`, `Image`,
  `Button`, `TextInput`, `Slider`). Resizable: the layout containers —
  `StackWidget` / `HStack` / `VStack` and future container kinds
  (Grid / Table / Tree). Mechanism (implemented 2026-06-24): flip the
  `Widget` base default to `false` and override
  `Container::isLayoutResizable()` to `true` so every layout container —
  current and future — resizes for free. `TextInput` / `Slider` were
  re-parented from `Container` to `Widget` (developer, 2026-06-24) so
  they are now plain frozen leaves needing no override; the `Container`
  boundary partitions the hierarchy exactly along the resizable/frozen
  line.
- **Reasoning.** A square should stay square when the window resizes; a
  density change (DPI / `scaleFactor`) is the only thing that should
  rescale an intrinsic widget, and that already rides a separate path:
  `AppWindow`'s `onRealize` handler calls
  `rootViewRenderTarget->setRenderScale(screen.scaleFactor)`
  (`wtk/src/UI/AppWindow.cpp:435-454`) on every re-realize (including
  cross-screen / DPI changes). That is a render-scale (dp→px) operation
  on the render target, **orthogonal** to the layout's `resize`. So
  freezing a widget's dp geometry costs nothing at DPI-change time: the
  widget keeps its dp rect and `setRenderScale` makes it render crisp at
  the new density. "Unless `scaleFactor` changes" is therefore a wired
  behavior, not aspirational — DPI rescale and layout resize are already
  decoupled, which is exactly why the directive is safe.

This supersedes the §1.1 "populate `minWidth` / `minHeight` and consult
the clamp on the cross axis" framing as Phase 1's *primary* mechanism;
that clamp work survives as the safety net for resizable containers
(and feeds Phase 2's aggregate min), but it is no longer what stops the
shapes from deforming.

### 1.6 Governing model: resize moves spacing, not widgets

**Developer principle (2026-06-24).** *"When a layout resizes, the size
of the Widget doesn't change — the padding/spacing changes. Unless it's
absolute-padded (by pixel, not fractional), and then the main-axis
`flexGrow` retains the exact padding."* This is the rule the whole plan
serves, and it is stronger than "frozen leaves don't resize": it says
the layout absorbs a resize into the **empty space** (slots / gaps /
padding), and every widget — frozen leaf or not — keeps its intrinsic
size unless it is itself a resizable container that fills its slot.

Consequences, and where each lands in `FlexLayout`:

- **Slot vs widget.** Main-axis flex distributes a child's *slot* (its
  allocation), never the widget's size directly. A frozen leaf keeps
  `widgetMain` and is placed at the slot's start; the leftover slot is
  spacing. (`FlexLayout::arrange`, the `slotMain` / `widgetMain` split.)
- **`flexGrow` grows the slot, not the widget.** A frozen `flexGrow`
  spacer (the description `Label`) pushes its siblings to the far edge
  by enlarging its *slot*; its own glyph box is unchanged.
- **A frozen widget is never shrunk to its parent.** It overflows
  honestly; the window-level min-size (Phase 2) is what keeps the
  overflow off-screen-edge, not a per-widget squash.
  (`clampRectToParent`, frozen branch.)
- **Absolute vs fractional padding (forward-looking).** Today
  `StackOptions.spacing` / `padding` are absolute pixels, so `flexGrow`
  already "retains the exact padding" — it distributes only the space
  left after the fixed padding. Fractional (proportional) padding that
  scales with the container is a *future* option this model leaves room
  for; not built yet. Flagged so the "unless it's absolute-padded"
  clause is not mistaken for existing behavior.

## 2 Goals

- **G1 (correctness).** On any window size, an intrinsic-sized leaf
  widget (shape, text) renders at exactly its intrinsic dp geometry —
  a window resize repositions it but never deforms it (§1.5 directive).
  Widgets that *are* resizable (containers) render at or above their
  declared intrinsic minimum, never at a smaller pixel geometry. The
  Phase F validation distortion goes away because intrinsic widgets no
  longer honor the resize request at all and the remaining space
  pressure is taken by §G2 / §G3.
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

### Phase 1 — Intrinsic widgets stop resizing: shape primitives hold their shape [smallest, ships first]

**Scope (primary — the §1.5 directive).** Implemented (2026-06-24):
**layout containers are the only layout-resizable widgets.** The
hierarchy partitions cleanly along the `Container` base — every shape /
text / input leaf is a direct `Widget` (`Rectangle`, `RoundedRectangle`,
`Ellipse`, `Separator`, `Label`, `Icon`, `Image`, `Button`, and — after
the developer re-parented them this session — `TextInput`, `Slider`),
and the only `Container` subclass is the stack family
(`StackWidget` → `HStack` / `VStack`). Two edits:

- Flip the base default: `Widget::isLayoutResizable()`
  (`wtk/include/omegaWTK/UI/Widget.h:204`) now returns `false`.
- Override on the container base: `Container::isLayoutResizable()`
  (`wtk/include/omegaWTK/Widgets/BasicWidgets.h`) returns `true`,
  inherited by every layout-container subclass (and any future Grid /
  Table / Tree for free).

With the flag off on a leaf, `FlexLayout` keeps positioning it
(centering, spacing, main-axis order) but takes the `currentMain` /
`currentCross` / `childRect` branches
(`wtk/src/UI/LayoutManager.cpp:800,805,836`) and skips the cross clamp,
so a window resize never rewrites its dp geometry and `measure` never
caches a squashed size. This is the change that makes both §1.1 and
§1.4 / §1.5 distortion disappear. (No production widget overrides
`isLayoutResizable()` today and only tests set it explicitly, so the
flip's blast radius is exactly "leaves freeze, containers unchanged.")

*Transparency flag:* `Image` is a direct `Widget`, so it freezes too;
its `ImageFitMode` then fits the bitmap inside a **fixed** dp frame
rather than growing the frame with the window. That is coherent with
the rule but worth confirming against any caller that expected an Image
to stretch — out of scope for this test scene, noted for Phase 4.

**Scope (secondary — survives from the old framing).** Populate
`LayoutStyle.clamp.minWidth` / `minHeight` / `aspectRatio` and have
`StackLayout::arrange` / `FlexLayout::arrange` consult the clamp on the
cross axis for the children that *are* still resizable (containers).
This no longer stops the shapes from deforming (the non-resizable flag
does that); it is the floor that keeps a genuinely-flexible child from
collapsing, and it feeds Phase 2's aggregate-min walk.

**Note on the §1.4 coupling.** Making the shapes non-resizable renders
the §1.4 width→height *coupling* moot for the test scene — a
non-resizable shape cannot be squashed by any axis. The coupling is
still worth one falsification pass (drag a single vertical edge, log
that height is constant; §1.4.1 / §1.4.2) only to confirm no *resizable*
container is silently mis-measuring its cross axis before Phase 2 trusts
the aggregate; it is no longer on the critical path for the visible bug.

**Scope decisions — resolved + implemented (2026-06-24).**
- *Which widgets.* Layout containers (`StackWidget` family + future Grid
  / Table / Tree) are the only resizable widgets. Everything else
  freezes: every direct-`Widget` leaf, `Button` / `TextInput` / `Slider`
  included.
- *Mechanism.* Base-default `false` + `Container` override `true`. No
  per-widget exceptions: the developer re-parented `TextInput` / `Slider`
  from `Container` to `Widget` this session, so the `Container` boundary
  now partitions the hierarchy exactly along the resizable/frozen line
  and the rule lives in one place.
- *scaleFactor path.* Already wired: `onRealize` →
  `setRenderScale(screen.scaleFactor)` (`wtk/src/UI/AppWindow.cpp:435-454`)
  is the DPI rescale, decoupled from layout `resize` (see §1.5
  Reasoning). Phase 1 just freezes dp geometry; no `scaleFactor`
  re-measure is needed here.
- *Cross-axis `Stretch` on frozen leaves — option (c), IMPLEMENTED
  2026-06-24.* Freezing every leaf dropped the root VStack's
  `crossAlign = Stretch` for the dividers — a frozen `Separator` kept
  its constructed `contentW` and stopped short on widen. Fix: decouple
  explicit `Stretch` from `isLayoutResizable` — `FlexLayout::arrange`
  now applies `crossSize = crossAvailable` for any `Stretch`-aligned
  child (`wtk/src/UI/LayoutManager.cpp`, the `Stretch` case) and writes
  it through via `useCross = resizable || align==Stretch`. The min-clamp
  (implicit shrink-to-fit) stays gated on `resizable`, so a *non*-
  Stretch frozen leaf (the `Center`-aligned shapes) still keeps its
  intrinsic cross size. Rationale: `Stretch` is an explicit fill the
  author chose; it is idempotent (always `= crossAvailable`, recomputed
  from the live frame) so it cannot drift the preferred-size cache.
  (`ScrollableContainer` is declared `: public Widget`, not `: public
  Container` — `BasicWidgets.h:92` — so it freezes despite its name;
  revisit when it graduates past its stub.)
- *Main-axis `flexGrow` + the SLOT-vs-WIDGET model — IMPLEMENTED,
  revised 2026-06-24.* The same freeze dropped main-axis `flexGrow`: the
  button row pins to the bottom only because the description `Label`
  carries `flexGrow = 1` (`BasicAppTestRun.cpp:213-215`), and a frozen
  `Label` stopped growing, so the buttons floated up. The **first** fix
  (make the frozen `Label` itself grow) was wrong — it changed a widget's
  size, which the developer's model forbids (see §1.6). The squash also
  came back: once a parent shrank, `clampRectToParent` clamped its frozen
  children down to the parent box. Corrected fix, two parts:
  1. **`clampRectToParent` preserves frozen size** — the parent-box
     min-clamp (`output = min(output, parent)`) now runs only in the
     `spec.resizable` branch (`wtk/src/UI/LayoutManager.cpp`). A frozen
     child keeps its intrinsic size and overflows honestly instead of
     squashing. Safe globally: `ChildResizeSpec.resizable` defaults to
     `true` (`View.h:91`), so only `FlexLayout`'s frozen leaves hit the
     changed branch.
  2. **Slot vs widget in `FlexLayout::arrange`** — main-axis flex now
     distributes the *slot* (the allocation), not the widget. `flexGrow`
     grows the slot; a frozen leaf keeps its intrinsic `widgetMain` and
     sits at the slot's start, so the leftover slot becomes trailing
     spacing. The cursor advances by `slotMain`, the widget is sized to
     `widgetMain`. `flexShrink` stays gated on `resizable`, so a frozen
     leaf's slot never shrinks below its widget — the widget never
     overflows its own slot and its cached size never drifts. Result: the
     description's *slot* grows to push the buttons down while the
     description *widget* stays its intrinsic text height. No test change.

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

**Validator.** Two drags. (1) §1.1 — `BasicAppTest` drag from full-size
down to the smallest draggable size. The shapes hold 80 × 80 across the
whole drag range; the HStack visibly overflows when there is not enough
horizontal room (acceptable interim — Phase 2 fixes it). (2) §1.4 —
drag the *right edge only* (height held constant) from narrow to wide
and back. The shapes must stay 80 × 80 tall throughout; the green
ellipse stays circular. Capture the before/after to confirm the
height-fixed drag no longer flattens the row, and note in the result
whether the §1.4 coupling turned out to be the stale cache or a
corner-drag artifact.

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

### Phase 5 — Complete Add/Drop animation: unified visibility transitions [follow-up]

This phase resolves §6's "Animated drop transitions" open question, but
generalizes it past Phase 3's drop case on the developer's direction
(2026-06-23): the same enter/exit transition applies to **any** widget
crossing the shown↔hidden boundary, not just a widget the layout drops
under space pressure. Three call sites flip the same boundary today,
all instantaneously:

1. **Phase 3 drop / restore** — a low-priority widget hidden because the
   layout cannot fit it (§4 Phase 3.2), and restored when room returns.
2. **`Widget::show()` / `Widget::hide()`** — the explicit programmatic
   flip. Today `show()` calls `view->enable()` and `hide()` calls
   `view->disable()` (`wtk/src/UI/Widget.Core.cpp:26-34`), which set
   `enabled_` and toggle the `:disabled` pseudo-class bit in one frame
   (`wtk/src/UI/View.Core.cpp:277-290`).
3. **Closing a virtual popup / menu / dialog** — a WTK-composited
   overlay widget dismissed by the app. Mechanically this is just
   `hide()` on the overlay's root widget, so it inherits whatever
   `hide()` does. (Out of scope: `OmegaWTK::Menu` in `UI/Menu.h` is a
   *native* OS menu — NSMenu / Win32 / GTK via `Native::NativeMenu` —
   not an in-tree composited surface; this phase animates the WTK
   overlay widgets only, never the native menu.)

All three are the same flip, so they get one mechanism: a transition
that sits *between* "shown" and "hidden" instead of stepping across it
in a single frame. CSS calls this enter/leave; this plan calls it
Add/Drop to match Phase 3's vocabulary.

> **Altitude note.** This surface is broader than resize-clamping — it
> is the general widget visibility-transition layer, and only the drop
> case (#1) is resize-driven. It lives here per the developer's call
> (it grew out of the §6 follow-up); if it accretes much past the four
> sub-phases below it should graduate to its own plan, with Phase 3.2
> depending on it. Filed here for now.

**Spine: a View-level visibility state machine.** The unifying move is
to replace the `enabled_` bool's two states with four —
`VisibilityState { Shown, Entering, Leaving, Hidden }` on `View` — and
route all three call sites through it. `isEnabled()`
(`wtk/src/UI/View.Core.cpp:590`) returns `state != Hidden` for
back-compat; the `:disabled` pseudo-class continues to flip at the
`Leaving`-start / `Entering`-complete boundaries so the cascade timing
(`PseudoClass::Disabled == 0x08`) is unchanged. The existing
per-element `visibilityDirty` bit (`wtk/src/UI/UIViewImpl.h:220`,
driven through `UIView::Impl::markElementDirty`) is the frame-pipeline
hook the state machine already has.

**Code surface (estimate: ~450 LOC across ~7 files), sub-phased:**

- **5.1 — Visibility state machine + deferred-removal lifecycle.**
  The hard part is *not* the tween — it is that an exiting widget must
  stay alive and painted while it animates out. Today `Widget::hide()`
  is synchronous, and the scheduler reaps a node's animations the
  moment it leaves the tree (`AnimationScheduler::cancelAllForNode`,
  "Called when a node leaves the tree" — `AnimationScheduler.h:163`).
  So a naive fade-on-hide would tear the node down before the first
  frame of the fade. 5.1 splits the *logical* removal (synchronous:
  hit-testing skips a `Leaving` view, and the Phase 2 min-size
  aggregate + Phase 3 drop accounting treat it as already gone — no
  semantic change to §1.2 / §1.3) from the *visual* removal
  (deferred: the view keeps painting until its exit animation reports
  `Completed`, then the real detach + `cancelAllForNode` fires). This
  is the load-bearing change; 5.2–5.4 are wiring on top of it.
  ~180 LOC, no animation yet — lands as "hide is deferred by N frames,
  still a hard cut at the end."

- **5.2 — Exit (Drop) and enter (Add) transitions.** Reuse
  `LayoutTransitionSpec` (`Layout.h:200`) — its `properties` already
  enumerate `Opacity`, `Width`, `Height` (`Layout.h:189-198`) — wrapped
  in a `VisibilityTransitionSpec { Optional<enter>, Optional<exit> }`
  (exit defaults to enter, reversed). Two modes:
  - **Fade** (default for overlays): tween `PropertyKey::Opacity`
    1→0 on exit / 0→1 on enter via
    `scheduler->tweenProperty<float>` — the footprint is freed
    immediately (siblings snap), only the leaving view's alpha
    animates. No per-tick relayout.
  - **Collapse** (default for in-flow drops): tween `Width` / `Height`
    to zero so siblings reflow each tick. This rides the *existing*
    geometry-tween rail — `View::applyLayoutDelta` already tweens
    `LayoutWidth` / `LayoutHeight` through the scheduler
    (`wtk/src/UI/View.Core.cpp:475-490`); 5.2 drives it from the
    visibility machine rather than only from a resize `LayoutDelta`.
  ~120 LOC. **Gated on prerequisite P0 below for the fade mode;**
  collapse mode is shippable without P0.

- **5.3 — Reversal / interruption.** Under a resize drag the drop
  threshold oscillates: a button can start fading out and need to come
  back before the fade finishes. A `Leaving` view that re-enters must
  reverse *from its current sampled value*, not restart from fully
  shown. Reuse the scheduler's smooth-retarget — the same machinery
  `AnimationScheduler::transition()` uses (`AnimationScheduler.h:216`,
  "capture its current sampled value … use it as the new `from`"), or
  `AnimationHandle::seek` + `Direction::Reverse`
  (`Composition/Animation.h:164,223`). Without this, fast drags flicker.
  ~80 LOC.

- **5.4 — Call-site integration.** Point `Widget::show()` /
  `Widget::hide()` at the 5.1 state machine instead of bare
  `enable()` / `disable()`; route Phase 3.2's drop/restore through the
  same entry points so a dropped widget collapses out and a restored
  one expands in. Virtual popup/menu/dialog close needs no new code —
  it is `hide()` on the overlay, so it inherits the exit animation once
  5.1–5.3 land. ~70 LOC + tests.

**Prerequisites / known gaps (flag honestly):**

- **P0 — element-level animated opacity is not wired to paint.**
  `PropertyKey::Opacity` exists (`StyleProperty.h:40`) and resolves in
  the cascade (`StyleResolver.cpp:41`), but the only animated opacity
  the paint path consumes today is *drop-shadow* opacity
  (`wtk/src/UI/UIView.Update.cpp:264`) and the *native-window* alpha
  (`AppWindow::setOpacity` → `NativeWindow::setOpacity`,
  `AppWindow.cpp:502`). Nothing multiplies an *element's* composited
  alpha by an animated per-node opacity. The fade mode (5.2) needs
  `UIView::paint` to read `scheduler->value<float>(node,
  PropertyKey::Opacity)` and fold it into the element's layer alpha.
  This is the first implementation task and **gates the fade path**;
  until it lands, only collapse mode works.
- **Per-tick relayout cost compounds §6's force-full-repaint
  question.** Collapse-mode exit reflows siblings every frame for the
  animation's duration, stacked on Phase F's existing per-resize-tick
  full repaint. Bounded (one collapse at a time per drop, short
  durations), but it is the same CPU surface §6 flags — profile before
  Phase G's content cache lands. Fade mode (no reflow) is the cheap
  default and is why overlays default to it.

**Open questions for Phase 5 (developer decides):**

- **OQ-5a — completion hook vs poll.** Deferred removal (5.1) must
  learn when an exit animation finishes. Add an explicit completion
  callback to the scheduler / `AnimationHandle`, or have the tree-host
  poll `handle.state() == Completed` at frame end? *Recommendation:*
  poll-at-frame-end — it reuses the existing `state()` accessor and
  avoids re-entering the tree from inside `AnimationScheduler::tick()`
  (which is reaping the very animation whose callback would mutate the
  tree).
- **OQ-5b — symmetric vs separate enter/exit specs.** One spec played
  in reverse for exit (simplest), or independent enter/exit timing +
  curve (real UI is asymmetric — fast ease-in fade-out, slower
  ease-out fade-in)? *Recommendation:* single `VisibilityTransitionSpec`
  with an optional `exit` that overrides the reversed-enter default.
- **OQ-5c — animate on first mount.** Should a widget added to an
  already-realized tree animate its enter, or appear instantly?
  *Recommendation:* no enter animation on the first frame a widget is
  mounted (otherwise the whole tree fades in on window open); enter
  fires only on `show()` / un-drop *after* mount, with an opt-in
  `animateOnMount`.
- **OQ-5d — fade vs collapse default, per widget kind.** Confirm the
  rule: absolute / overlay widgets (popup, menu, dialog) → fade;
  in-flow widgets (a dropped toolbar child) → collapse. Edge case: a
  dropped in-flow widget *could* fade in place (footprint freed
  instantly, cheaper) instead of collapse — which is the BasicAppTest
  toolbar default?

**Validator.** Two scenes. (1) The Phase 3 toolbar: shrink past a drop
threshold — the lowest-priority button collapses out over ~150 ms and
its siblings slide to fill; grow back mid-collapse — it reverses
smoothly (5.3) rather than snapping. (2) A virtual dialog overlay:
`hide()` fades it out over its `exit` spec and the node is not detached
until the fade completes (5.1); `show()` fades it back in.

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
- Phase 5 (Complete Add/Drop animation) is an independent follow-up.
  Its drop-animation aspect depends on Phase 3.2 (there must be a drop
  to animate), but 5.1–5.4 are otherwise self-contained and the
  `show()` / `hide()` / virtual-overlay cases need no other phase. P0
  (element opacity → paint) gates only the fade mode; collapse mode can
  land first on the existing geometry-tween rail. Sequence after
  Phase 3.2; can interleave with Phase 4.

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
- **Animated drop transitions.** *Resolved → see Phase 5.* Phase 3.2
  drops are instantaneous — a button is visible at one window size,
  invisible one drag delta later. CSS-style drop animations (fade-out,
  collapse-on-axis) are now planned as Phase 5, generalized past the
  drop case to every shown↔hidden flip (`Widget::show()` / `hide()`,
  closing a virtual popup / menu / dialog) on the developer's direction
  (2026-06-23). They ride the surface that owns `LayoutTransition`
  (`Layout.h:189`) and the `AnimationScheduler`. The remaining genuine
  forks moved into Phase 5's own OQ-5a … OQ-5d.
