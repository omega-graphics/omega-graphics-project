# Widget / View Paint Lifecycle Standardization Plan

**Status:** Proposal. Nothing below is implemented yet.
**Scope:** Define a strict, phase-separated paint lifecycle for `Widget`
and `View` that eliminates the current ad-hoc paint paths, prevents
layout-during-paint and paint-during-layout reentrancy, and gives
animation a clean slot that does not entangle with style resolution or
frame submission.
**Prerequisite reading:** `UIView-Render-Redesign-Plan.md` (the
scene-tree / display-list plan this lifecycle plugs into) and
`Animation-API-Simplification-Plan.md` (the `Animator` that will drive
the Tick phase).
**Non-goals:** Changing the `DisplayList`/`DrawOp` representation
(covered by the render redesign). Changing the `Compositor` backend.
Changing `StyleSheet` authoring syntax.

---

## 1. What is broken today

The paint lifecycle is not a lifecycle. It is a collection of entry
points that call each other in inconsistent orders, with no enforcement
of phase boundaries.

### 1.1 `Widget::executePaint` is the only frame-building entry point, and it does everything

Reading `Widget.Paint.cpp:10-73`:

1. Creates a `CompositeFrame`.
2. Sets it as active on the proxy.
3. Calls `view->startCompositionSession()`.
4. Calls `onPaint(reason)` — the virtual hook.
5. Calls `view->submitPaintFrame(submissions)`.
6. Deposits frame to window surface.
7. Calls `view->endCompositionSession()`.

There is no separation between "resolve what needs to be drawn" and
"draw it". `onPaint` is expected to do both. Every widget subclass
(`Rectangle`, `Label`, `Icon`, `Image`, `Container`, etc.) follows the
same pattern inside its `onPaint`: rebuild the `UIViewLayoutV2` element
list, set the stylesheet, call `viewAs<UIView>().update()`. Layout,
style, and paint are fused into one call.

### 1.2 `UIView::update()` is called from inside `onPaint` and does its own composition session

`UIView::update()` (`UIView.Update.cpp:120-391`) calls
`startCompositionSession()` and `endCompositionSession()` internally
(lines 208, 384). But `executePaint` *also* calls them (lines 46, 60).
So the composition session is opened twice: once by the Widget
framework, once by UIView. `startCompositionSession` happens to be
idempotent (it just ensures the frontend pointer is set), and
`endCompositionSession` is a no-op, so this doesn't crash — but it
means the contract is undefined. Some code paths assume the session is
managed by Widget, others assume UIView manages it.

### 1.3 `onPaint` does layout work

Every primitive widget's `onPaint` (e.g. `Rectangle::onPaint` at
`Primatives.cpp:55`) clears the layout element list, rebuilds it, sets
the stylesheet, and calls `update()`. `Container::onPaint`
(`BasicWidgets.cpp:145`) calls `layoutChildren()` if a layout is
pending. Layout is not a separate phase — it is embedded in paint.

This means:
- **Paint can trigger layout.** If a widget invalidates during paint
  (e.g. a child resize from `layoutChildren()`), the coalescing flag
  `hasPendingInvalidate` catches it, but the child's `executePaint`
  will then run *after* the current paint finishes, producing a
  second frame in the same event cycle.
- **Layout cannot run without paint.** There is no way to say "lay out
  the tree but don't paint it yet." This blocks any future optimization
  where layout happens once and paint is skipped for clean subtrees.

### 1.4 No animation phase

`UIView::update()` reads animation state inline: `animatedValue(tag,
key)` calls appear interleaved with draw calls
(`UIView.Update.cpp:258-265`). Animation ticking happens via
`advanceAnimations()` which is called from a separate timer path, but
there is no guarantee it runs *before* paint. If paint runs before the
animation tick, it reads stale values. If the tick runs mid-paint on
another thread, it races on the animation state maps.

The `Animation-API-Simplification-Plan.md` gives us a per-view
`Animator` with a `tick()` method. This plan defines *when* that tick
runs relative to layout and paint.

### 1.5 `WidgetTreeHost::paintAndDeposit` is disconnected from per-widget paint

`paintAndDeposit` (`WidgetTreeHost.cpp:352-364`) creates one shared
`CompositeFrame`, sets it on all widgets, then invalidates the entire
tree. Each widget's `executePaint` runs independently and appends to
the shared frame. But `executePaint` also creates *its own*
`CompositeFrame` (line 43 of `Widget.Paint.cpp`), overwriting what
`paintAndDeposit` set. So the shared frame from `paintAndDeposit` ends
up empty, and each widget deposits its own frame individually.

The two paths — tree-level `paintAndDeposit` and per-widget
`invalidate` — are not composable. You can use one or the other, but
they fight over the active frame pointer.

### 1.6 No dirty-region gate

`invalidate()` calls `executePaint()` directly and synchronously
(`Widget.Paint.cpp:103`). There is no dirty flag that defers paint to
the next frame. The coalescing flag only catches overlapping
invalidations *during* an active paint; it does not batch multiple
`invalidate()` calls that arrive between frames. If three properties
change in sequence, you get three `executePaint()` calls, three
`CompositeFrame`s, and three deposits.

Chromium, Slate, Unity UI Toolkit, and Flutter all defer invalidation
to the next frame boundary. This is the single most important
architectural difference.

### 1.7 Summary of the current (non-)lifecycle

```
Widget::invalidate()
  → executePaint()              ← synchronous, immediate
    → create CompositeFrame
    → startCompositionSession()
    → onPaint(reason)           ← virtual; widget rebuilds layout + style + draws
      → UIView::update()        ← also calls startCompositionSession()
        → resolve layout        ← layout fused into paint
        → read animation state  ← no ordering guarantee
        → draw to Canvas
        → sendFrame()
    → submitPaintFrame()
    → depositFrame()
    → endCompositionSession()
```

No phases. No deferral. No dirty gate. No animation slot. Every
invalidation is a full paint.

---

## 2. What proven systems do

Four reference architectures, each solving a piece of the problem.

### 2.1 Chromium `views::View`

Phase ordering:

1. **Invalidation** — `SchedulePaint()` sets `needs_paint_` and unions a
   dirty rect onto the owning layer. `InvalidateLayout()` sets
   `needs_layout_` and propagates upward. Neither triggers immediate
   work.
2. **Layout** — `Widget::LayoutRootViewIfNecessary()` is called by the
   compositor's `BeginMainFrame`. It walks the tree and calls `Layout()`
   on Views with `needs_layout_`. `Layout()` clears `needs_layout_` at
   entry. A `performing_layout_` boolean on View prevents upward
   propagation of child invalidation during layout.
3. **Paint** — Each `ui::Layer` with a non-empty invalid rect triggers
   `OnPaintLayer()`, which calls `View::Paint()`. `Paint()` clears
   `needs_paint_` at entry. The paint walk is pre-order: parent paints,
   then children in z-order.
4. **Commit** — Display items are committed to the compositor thread as
   an immutable snapshot.

Key properties:
- `needs_layout_` and `needs_paint_` are separate flags.
- Layout always runs before paint.
- A guard (`performing_layout_`) prevents cross-phase contamination.
- Invalidation is deferred — the frame boundary is the only place work
  happens.

### 2.2 Unreal Slate `SWidget`

Phase ordering (per frame, driven by `FSlateApplication::Tick()`):

1. **Tick** — advance animations, poll state.
2. **Prepass** — bottom-up `ComputeDesiredSize()`.
3. **ArrangeChildren** — top-down allocation of final rects.
4. **Paint** — recursive `OnPaint()` into a shared
   `FSlateWindowElementList`.

Key properties:
- `OnPaint` is `const` — it does not mutate widget state.
- The display list (`FSlateWindowElementList`) is one flat list per
  window, rebuilt every frame.
- The `layerId` integer (passed *down* the walk, returned as the max
  used) is the z-ordering mechanism.
- Invalidation panels cache display list fragments; volatile widgets
  opt out and rebuild every frame.

### 2.3 Unity UI Toolkit `VisualElement`

Phase ordering (per panel update):

1. **Styles** — resolve computed styles for dirty elements.
2. **Layout** — Yoga flexbox pass over dirty subtree.
3. **TransformClip** — update world transforms and clip rects.
4. **Repaint** — `generateVisualContent` callback for dirty elements.

Key properties:
- `VersionChangeType` is a flags enum (Layout, Styles, Transform,
  Repaint, Hierarchy, etc.). Each flag triggers only its pass.
- The `generateVisualContent` callback must NOT modify any
  VisualElement property. The element is read-only during the callback.
- Mesh generation appends to a shared vertex buffer.

### 2.4 Flutter `RenderObject`

Phase ordering (per frame, in `RendererBinding.drawFrame()`):

1. `flushLayout()` — layout dirty RenderObjects from relayout
   boundaries downward.
2. `flushCompositingBits()` — update which nodes need their own layer.
3. `flushPaint()` — paint dirty RenderObjects from repaint boundaries
   downward.
4. `compositeFrame()` — submit to GPU.

Key properties:
- `markNeedsLayout()` and `markNeedsPaint()` propagate upward to
  *boundaries* (relayout boundary / repaint boundary), then stop.
- `PipelineOwner` has debug-mode assertions (`debugDoingLayout`,
  `debugDoingPaint`) that crash if layout is called during paint or
  vice versa.
- `PaintingContext` manages a canvas that may change across
  `paintChild()` calls (due to compositing layer insertion).

### 2.5 Cross-cutting consensus

Every system agrees on:

| Property | Consensus |
|---|---|
| Invalidation is deferred | Yes — set a flag, don't run work |
| Layout runs before paint | Yes — always |
| Layout and paint don't interleave | Yes — guarded or asserted |
| Animation ticks before paint reads values | Yes — explicit phase |
| One display list per window per frame | Yes (or per repaint boundary) |
| Paint is a pure function of model + layout | Yes — no side effects |

OmegaWTK currently violates all six.

---

## 3. Proposed lifecycle

### 3.1 The five phases

Each frame, `FrameBuilder` (introduced by `UIView-Render-Redesign-Plan.md`
Tier 3) runs these phases in strict order. No phase may invoke work
from an earlier or later phase.

```
 ┌─────────────────────────────────────────────────────────┐
 │                    Frame Boundary                        │
 │  (pacer tick, or AppWindow::invalidate() on next vsync) │
 └─────────────────────────────────────────────────────────┘
         │
         ▼
  ┌──────────────┐
  │  1. Tick      │  Advance animations. Animator::tick() for each View.
  │               │  Writes resolved animation values into side tables.
  │               │  May set DirtyBits::Paint on animated nodes.
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │  2. Style     │  Resolve stylesheets for nodes with DirtyBits::Style.
  │               │  Writes into ResolvedStyleCache per node.
  │               │  May set DirtyBits::Layout if style touched layout
  │               │  properties (width, height, padding, etc.).
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │  3. Layout    │  Measure (bottom-up) then Arrange (top-down) for
  │               │  nodes with DirtyBits::Layout.
  │               │  Writes finalRect on each node.
  │               │  May set DirtyBits::Paint on nodes whose rect changed.
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │  4. Paint     │  Top-down tree walk for nodes with DirtyBits::Paint.
  │               │  Each node appends DrawOps to the window-wide
  │               │  DisplayList. Pure function of (model, layout, style,
  │               │  animation values). No side effects.
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │  5. Commit    │  Hand the completed DisplayList to the compositor.
  │               │  One CompositeFrame per window. One deposit.
  │               │  Clear all DirtyBits.
  └──────────────┘
```

### 3.2 Phase boundaries are enforced

Each phase sets a flag on `FrameBuilder` indicating which phase is
active. Debug builds assert on cross-phase violations:

```cpp
enum class FramePhase : uint8_t {
    Idle, Tick, Style, Layout, Paint, Commit
};

// In debug builds, called by any method that modifies state:
void FrameBuilder::assertPhase(FramePhase expected) {
    OMEGAWTK_ASSERT(currentPhase_ == expected,
        "Attempted " << phaseName(expected) << " work during "
        << phaseName(currentPhase_) << " phase");
}
```

Specific enforcement:

| Action | Legal during | Asserts during |
|---|---|---|
| `Animator::tick()` | Tick | Style, Layout, Paint, Commit |
| `StyleSheet::resolve()` | Style | Tick, Layout, Paint, Commit |
| `LayoutManager::measure/arrange` | Layout | Tick, Style, Paint, Commit |
| `DisplayList::append(DrawOp)` | Paint | Tick, Style, Layout, Commit |
| `invalidate()` (set dirty bits) | Any phase | — (always legal, deferred) |

The critical rule: **`invalidate()` never triggers immediate work.**
It only sets `DirtyBits` and marks the window as needing a frame.
The next frame boundary runs the phases.

### 3.3 `DirtyBits` replace all existing dirty mechanisms

The five UIView dirty flags (`layoutDirty`, `styleDirty`,
`styleDirtyGlobal`, `styleChangeRequiresCoherentFrame`,
`firstFrameCoherentSubmit`) and the Widget-level `paintInProgress` /
`hasPendingInvalidate` / `pendingPaintReason` are replaced by a single
`DirtyBits` field per node:

```cpp
enum DirtyBit : uint8_t {
    Style   = 1 << 0,
    Layout  = 1 << 1,
    Content = 1 << 2,  // element list changed (UIView-specific)
    Paint   = 1 << 3,
};
```

Propagation: setting any bit propagates `Paint` upward to the root
(so the root knows a frame is needed). Layout propagates upward to the
nearest layout boundary. Style does not propagate (each node resolves
its own style).

The `PaintOptions` struct on Widget (`autoWarmupOnInitialPaint`,
`coalesceInvalidates`, etc.) is removed. Coalescing is inherent — all
invalidations within a frame interval are batched. Warmup frames are
unnecessary when the compositor receives one complete frame per vsync.

### 3.4 `Widget::invalidate()` becomes a deferred dirty-flag setter

Before (synchronous):
```cpp
void Widget::invalidate(PaintReason reason) {
    executePaint(reason, false);  // runs layout + paint immediately
}
```

After (deferred):
```cpp
void Widget::invalidate(PaintReason reason) {
    uint8_t bits = DirtyBit::Paint;
    if (reason == PaintReason::Resize)
        bits |= DirtyBit::Layout;
    if (reason == PaintReason::ThemeChanged)
        bits |= DirtyBit::Style | DirtyBit::Layout;
    markDirty(bits);
    // Window will run FrameBuilder on the next frame boundary.
}
```

`invalidateNow()` is kept as an escape hatch that forces an immediate
frame build, but its use is discouraged and logged in debug builds.

### 3.5 `Widget::onPaint` contract changes

Before, `onPaint` was responsible for everything: rebuilding the layout
element list, setting the stylesheet, calling `UIView::update()`.

After, `onPaint` becomes `Widget::paint(PaintContext&)` and its
contract is strict:

1. **Read** model state, layout results (finalRect), resolved style,
   animation values.
2. **Append** DrawOps to `PaintContext::displayList`.
3. **Return.** No layout mutations, no style mutations, no Canvas
   creation, no composition sessions, no frame submission.

The element list and stylesheet are set during model updates (e.g.
`setProps()`, `setStyleSheet()`), which set `DirtyBits::Content |
Style`. They are not rebuilt during paint.

### 3.6 Widget subclass migration

Every existing widget subclass currently follows this pattern in
`onPaint`:

```cpp
void Rectangle::onPaint(PaintReason reason) {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(rect());
    lv2.element(spec);
    viewAs<UIView>().setStyleSheet(...);
    viewAs<UIView>().update();
}
```

After migration, this splits into:

```cpp
// Called when model changes — NOT during paint.
void Rectangle::setProps(const RectangleProps & props) {
    props_ = props;
    rebuildContent();   // sets layout elements + stylesheet
    markDirty(DirtyBit::Content | DirtyBit::Style | DirtyBit::Paint);
}

void Rectangle::rebuildContent() {
    auto & lv2 = viewAs<UIView>().layoutV2();
    lv2.clear();
    UIElementLayoutSpec spec;
    spec.tag = "bg";
    spec.shape = Shape::Rect(rect());
    lv2.element(spec);
    viewAs<UIView>().setStyleSheet(
        makeShapeStyleSheet(props_.fill, props_.stroke, props_.strokeWidth));
}

// Phase 4 (Paint). Pure: reads content, writes DrawOps.
void Rectangle::paint(PaintContext & pc) {
    // UIViewNode::paint handles this — see §3.8
}
```

For primitive widgets (`Rectangle`, `RoundedRectangle`, `Ellipse`,
`Path`, `Separator`, `Label`, `Icon`, `Image`), the paint method is
entirely handled by `UIViewNode::paint` from the render redesign plan.
The widget subclass only needs to set up the element list and
stylesheet when model state changes. The `paint(PaintContext&)` override
is optional — the default traversal handles UIView-backed widgets.

### 3.7 `Container::onPaint` no longer does layout

Currently:
```cpp
void Container::onPaint(PaintReason reason) {
    if (layoutPending) {
        layoutChildren();
    }
}
```

After: `layoutChildren()` runs in the Layout phase (Phase 3), triggered
by `DirtyBits::Layout`. `Container::paint(PaintContext&)` does nothing
except recurse to children — Container is a layout-only node.

```cpp
void Container::arrange(const Composition::Rect & finalRect) {
    // Phase 3: position children using LayoutManager.
    layoutChildren();
}

void Container::paint(PaintContext & pc) {
    // Phase 4: no own drawing. Children paint themselves.
}
```

### 3.8 Where animation reads happen

Animation values are produced in Phase 1 (Tick) and consumed in
Phase 4 (Paint). The `Animator::tick()` call writes resolved values
into a side table:

```cpp
// Phase 1 (Tick)
animator_->tick();
// Side table now has: { (elementTag, "shadowOffsetX") → 3.5f, ... }

// Phase 4 (Paint)
void UIViewNode::paint(PaintContext & pc) {
    for (const auto & elem : elements_) {
        if (auto v = animatedValue(elem.tag, ShadowOffsetX))
            shadowParams.x_offset = *v;
        // ... append DrawOps using the resolved values
    }
}
```

The animation side table is written once (during Tick) and read once
(during Paint). No locking needed because the phases are sequential.
This is the Slate model: "Tick advances state. Paint reads it."

### 3.9 `UIView::update()` is eliminated

`UIView::update()` currently contains ~270 lines that resolve layout,
sort by z-index, open a composition session, resolve styles, read
animation state, draw shapes, draw text, call `sendFrame()`, and clear
dirty flags.

In the new lifecycle, every responsibility of `update()` moves to a
specific phase:

| `update()` responsibility | New phase | New location |
|---|---|---|
| Resolve layout rects from V2 elements | Phase 3 (Layout) | `UIViewNode::arrange()` via LayoutManager |
| Stable-sort by z-index | Phase 3 (Layout) | `UIViewNode::arrange()` |
| Resolve view-level background style | Phase 2 (Style) | `StyleResolver` |
| Resolve element brush/effects/text style | Phase 2 (Style) | `StyleResolver` |
| Read animation values | Phase 4 (Paint) | `UIViewNode::paint()` reads side table |
| Clamp rects to parent | Phase 3 (Layout) | `LayoutManager::arrange()` |
| Draw shapes, text, shadows | Phase 4 (Paint) | `UIViewNode::paint()` appends to DisplayList |
| sendFrame() | Phase 5 (Commit) | `FrameBuilder` replays DisplayList |
| Clear dirty flags | Phase 5 (Commit) | `FrameBuilder::commit()` |
| startCompositionSession | Deleted | `FrameBuilder` owns session lifetime |
| endCompositionSession | Deleted | `FrameBuilder` owns session lifetime |
| Create rootCanvas | Deleted | DisplayList replaces per-view Canvas |

### 3.10 `WidgetTreeHost::paintAndDeposit` becomes `FrameBuilder::buildFrame`

The current dual-path problem (tree-level `paintAndDeposit` vs.
per-widget `invalidate`) is eliminated. There is one entry point:

```cpp
void FrameBuilder::buildFrame() {
    currentPhase_ = FramePhase::Tick;
    tickSubtree(root_);

    currentPhase_ = FramePhase::Style;
    if (root_->dirty() & DirtyBit::Style)
        resolveStylesSubtree(root_);

    currentPhase_ = FramePhase::Layout;
    if (root_->dirty() & DirtyBit::Layout) {
        measureSubtree(root_, windowSize_);
        arrangeSubtree(root_, windowRect_);
    }

    currentPhase_ = FramePhase::Paint;
    if (root_->dirty() & DirtyBit::Paint) {
        displayList_.clear();
        PaintContext pc { displayList_, identityTransform_ };
        paintSubtree(root_, pc);
    }

    currentPhase_ = FramePhase::Commit;
    compositor_.submitDisplayList(displayList_);
    clearDirtyBitsSubtree(root_);

    currentPhase_ = FramePhase::Idle;
}
```

Called by the window pacer (vsync or manual `AppWindow::invalidate()`).
One frame, one `CompositeFrame`, one deposit.

### 3.11 The `PaintContext` argument

```cpp
struct PaintContext {
    DisplayList & displayList;
    Transform2D   currentTransform;
    Rect          currentClip;
    float         currentOpacity = 1.0f;
    // No Canvas. No CompositeFrame. No composition session.
};
```

`PaintContext` is a scratch struct threaded through the paint walk. It
is the Slate `FSlateWindowElementList` / Chromium `gfx::Canvas`
equivalent. Nodes append to `displayList` and push/pop transforms and
clips. Nothing in `PaintContext` is stored on the node.

---

## 4. Where the old concepts go

| Old | New |
|---|---|
| `Widget::executePaint()` | Deleted. `FrameBuilder::buildFrame()` replaces it. |
| `Widget::onPaint(PaintReason)` | Replaced by `Widget::paint(PaintContext&)`. |
| `Widget::init()` trigger of `executePaint(Initial)` | `FrameBuilder` runs the first frame after `initWidgetTree()`. |
| `Widget::Impl::paintInProgress` | Deleted. Phase enforcement replaces reentrancy guards. |
| `Widget::Impl::hasPendingInvalidate` | Deleted. Deferral is inherent. |
| `PaintOptions` (warmup, coalesce) | Deleted. One frame per vsync, always. |
| `PaintReason` enum | Collapsed into `DirtyBits`. The reason is the bits. |
| `UIView::update()` 270 lines | Split across Style, Layout, Paint phases. |
| `UIView::Impl` 5 dirty flags | One `DirtyBits` field per node. |
| `Container::onPaint → layoutChildren()` | `Container::arrange()` in Layout phase. |
| `WidgetTreeHost::paintAndDeposit()` | `FrameBuilder::buildFrame()`. |
| `WidgetTreeHost::invalidateWidgetRecurse()` | Not needed — `buildFrame` walks the tree. |
| `WidgetTreeHost::setActiveCompositeFrameRecurse()` | Deleted. One frame, managed by `FrameBuilder`. |
| `View::startCompositionSession()` | Deleted. `FrameBuilder` owns session. |
| `View::endCompositionSession()` | Deleted. |
| `View::submitPaintFrame()` | Deleted. DisplayList commit replaces it. |
| `CanvasView::submitPaintFrame()` | Deleted. |
| Per-widget `CompositeFrame` creation | Deleted. One per window, owned by `FrameBuilder`. |

---

## 5. Migration tiers

This plan is designed to layer on top of the
`UIView-Render-Redesign-Plan.md` tiers. Each tier here corresponds to
one there.

### Tier A — Deferred invalidation (can ship independently)

**Ship alongside Render Redesign Tier 1 (sync lane cleanup).**

- `Widget::invalidate()` no longer calls `executePaint()` directly.
  Instead it sets `DirtyBits` on the view and calls
  `AppWindow::requestFrame()` (a new method that ensures the next
  vsync will run `FrameBuilder`).
- `Widget::invalidateNow()` still runs `executePaint()` synchronously,
  but is marked `[[deprecated]]` and emits a debug log.
- `executePaint()` internal logic unchanged — it still creates a
  `CompositeFrame`, calls `onPaint`, etc. But it only runs at the frame
  boundary, not inline with property changes.
- Add `DirtyBits` field to `View::Impl`.
- `PaintOptions::coalesceInvalidates` becomes always-true. The field
  stays but is ignored.
- `PaintOptions::autoWarmupOnInitialPaint` and `warmupFrameCount` are
  deprecated. The first frame is submitted once, like every other
  frame.

**Risk:** Low. The behavioral change is timing — invalidation is batched
to the next frame instead of synchronous. Any app code that depends on
paint happening inline with `invalidate()` will see a one-frame delay.
This is the correct behavior.

**Files touched:** `Widget.Paint.cpp`, `Widget.h`, `WidgetImpl.h`,
`View.h`, `ViewImpl.h`, `AppWindow.h`, `AppWindow.cpp`.

### Tier B — Phase separation inside `executePaint` (ship alongside Render Redesign Tier 2)

- `executePaint()` splits into five sequential calls:
  `tickAnimations()`, `resolveStyles()`, `runLayout()`,
  `runPaint()`, `commitFrame()`.
- `FramePhase` enum and `currentPhase_` field added to a new
  `FrameBuilder` class (or to `WidgetTreeHost` temporarily).
- Debug assertions added: `OMEGAWTK_ASSERT` on phase violations.
- `Widget::onPaint(PaintReason)` renamed to `Widget::paint(PaintContext&)`.
  The old `onPaint` signature stays as a deprecated forwarding wrapper
  for one release cycle.
- Widget subclasses migrated: the element-list rebuild moves out of
  `paint()` into `rebuildContent()` called from `setProps()` / model
  mutators. `paint()` becomes read-only.
- `UIView::update()` body is split:
  - Layout resolution → `UIViewNode::arrange()` (called in Layout phase).
  - Style resolution → called in Style phase.
  - Draw loop → `UIViewNode::paint(PaintContext&)` (called in Paint phase).
  - `startCompositionSession` / `endCompositionSession` calls removed
    from UIView. `FrameBuilder` manages the session.
- `Container::onPaint → layoutChildren()` moves to
  `Container::arrange()`.

**Risk:** Medium. This is the mechanical refactor of every widget
subclass. Each widget is small and formulaic, but there are ~10 of them.

**Files touched:** `Widget.h`, `Widget.Paint.cpp`, `WidgetImpl.h`,
`UIView.h`, `UIView.Update.cpp`, `UIViewImpl.h`, `CanvasView.h`,
`CanvasView.cpp`, `Primatives.cpp`, `BasicWidgets.cpp`,
`Containers.cpp`, `WidgetTreeHost.h`, `WidgetTreeHost.cpp`.

### Tier C — Single frame builder per window (ship alongside Render Redesign Tier 3)

- `FrameBuilder` lives on `AppWindow`, one per window.
- `WidgetTreeHost::paintAndDeposit()` deleted. `FrameBuilder::buildFrame()`
  replaces it.
- `executePaint()` deleted. The entry point is `buildFrame()`.
- The per-widget `CompositeFrame` creation inside `executePaint()` is
  removed. One `CompositeFrame` per frame, owned by `FrameBuilder`.
- `Widget::Impl::paintInProgress`, `hasPendingInvalidate`,
  `pendingPaintReason` all deleted.
- `PaintOptions` struct deprecated and ignored.
- `View::startCompositionSession()` / `endCompositionSession()` removed.
- `View::submitPaintFrame()` / `CanvasView::submitPaintFrame()` removed.

**Risk:** High. This is the structural change. Same risk profile as
Render Redesign Tier 3 — it must land behind a feature flag or on a
branch.

**Files touched:** All of Tier B plus `AppWindow.h`, `AppWindow.cpp`,
`AppWindowImpl.h`, `CompositorClient.h`, `CompositorClient.cpp`.

### Tier D — Full lifecycle with scene tree (ship alongside Render Redesign Tier 4)

- `Widget::paint(PaintContext&)` becomes `SceneNode::paint(PaintContext&)`.
- `Widget` is removed as a concept (per the render redesign plan
  §6 question 2 answer: "Widgets are effectively a light wrapper around
  View and can be removed").
- `WidgetTreeHost` becomes `WindowFrameHost` or is folded into
  `AppWindow`.
- The `FrameBuilder::buildFrame()` call chain is the only paint path.
- Phase assertions are the only reentrancy guard.

**Risk:** High. API break. Ship after Tier C has been in main for at
least two weeks.

---

## 6. `FrameBuilder` implementation sketch

```cpp
class FrameBuilder {
    SceneNode * root_ = nullptr;
    DisplayList displayList_;
    FramePhase currentPhase_ = FramePhase::Idle;
    Composition::CompositorSurface * surface_ = nullptr;
    Composition::Rect windowRect_ {};

    void tickSubtree(SceneNode * node) {
        if (node->animator())
            node->animator()->tick();
        for (auto * child : node->children())
            tickSubtree(child);
    }

    void resolveStylesSubtree(SceneNode * node) {
        if (node->dirty() & DirtyBit::Style) {
            node->resolveStyle();
            // If style change affects layout properties:
            if (node->styleAffectsLayout())
                node->addDirty(DirtyBit::Layout);
        }
        for (auto * child : node->children())
            resolveStylesSubtree(child);
    }

    void measureSubtree(SceneNode * node, Size available) {
        if (!(node->dirty() & DirtyBit::Layout))
            return;
        // Bottom-up: measure children first
        for (auto * child : node->children())
            measureSubtree(child, available);
        if (node->layoutManager())
            node->setDesiredSize(
                node->layoutManager()->measure(*node, available));
    }

    void arrangeSubtree(SceneNode * node, Rect finalRect) {
        if (!(node->dirty() & DirtyBit::Layout))
            return;
        node->setFinalRect(finalRect);
        if (node->layoutManager())
            node->layoutManager()->arrange(*node, finalRect);
        node->addDirty(DirtyBit::Paint);  // layout changed → repaint
    }

    void paintSubtree(SceneNode * node, PaintContext & pc) {
        if (!(node->dirty() & DirtyBit::Paint))
            return;
        pc.pushTransform(node->localTransform());
        pc.pushClip(node->clipRect());
        node->paint(pc);
        for (auto * child : node->children())
            paintSubtree(child, pc);
        pc.popClip();
        pc.popTransform();
    }

public:
    void buildFrame() {
        if (root_ == nullptr || !(root_->dirty()))
            return;

        currentPhase_ = FramePhase::Tick;
        tickSubtree(root_);

        currentPhase_ = FramePhase::Style;
        resolveStylesSubtree(root_);

        currentPhase_ = FramePhase::Layout;
        if (root_->dirty() & DirtyBit::Layout) {
            measureSubtree(root_, windowRect_.size());
            arrangeSubtree(root_, windowRect_);
        }

        currentPhase_ = FramePhase::Paint;
        displayList_.clear();
        PaintContext pc { displayList_ };
        paintSubtree(root_, pc);

        currentPhase_ = FramePhase::Commit;
        if (!displayList_.empty())
            surface_->deposit(displayList_.replay());
        clearDirtySubtree(root_);

        currentPhase_ = FramePhase::Idle;
    }
};
```

---

## 7. Interaction with existing plans

- **`UIView-Render-Redesign-Plan.md`** — This plan defines the
  lifecycle that the render redesign's `FrameBuilder`, `SceneNode`,
  `DisplayList`, and `PaintContext` operate within. The render redesign
  defines the data structures; this plan defines when they are used.
  Migration tiers are aligned.

- **`Animation-API-Simplification-Plan.md`** — The new `Animator` has
  a `tick()` method. This plan places `tick()` in Phase 1 and requires
  that animation values are written to a side table during Tick, read
  during Paint, and never written during Paint. This is already the
  design intent of the animation plan.

- **`Render-Execution-Efficiency-Plan.md`** — The deferred
  invalidation model (Tier A) directly fixes the "every invalidate
  triggers a full paint" problem called out in that plan. The single
  `buildFrame()` entry point produces one display list per frame per
  window, which is what the batched compositor scheduler wants.

---

## 8. Open questions

1. **Frame pacing.** `buildFrame()` should run at most once per vsync.
   The render redesign plan mentions a "frame pacer" but does not
   specify it. This plan assumes `AppWindow` has a platform-provided
   vsync callback (CVDisplayLink on macOS, IDXGIOutput::WaitForVBlank
   on Windows) that calls `buildFrame()`. If the pacer doesn't exist
   yet, Tier A can use a simple flag + manual trigger.

2. **Manual-mode widgets.** `PaintMode::Manual` widgets currently skip
   `executePaint` entirely. In the new lifecycle, manual widgets skip
   the `paint()` call in Phase 4 but still participate in Tick, Style,
   and Layout phases (since those affect their children). The manual
   widget can call `FrameBuilder::forcePaintNode(node)` to paint
   on demand.

3. **Warmup frames.** The current `PaintOptions::autoWarmupOnInitialPaint`
   submits the initial frame multiple times. This was a workaround for
   compositor pipeline latency. With one-frame-per-vsync and proper
   double/triple buffering in the compositor, warmup should be
   unnecessary. Verify with the single-surface rendering path before
   removing.

4. **`invalidateNow()` callers.** Grep for all callers of
   `invalidateNow()` in the codebase. Each one is a potential
   assumption that paint completes synchronously. These must be audited
   and either converted to deferred `invalidate()` or justified as
   legitimate synchronous paint needs (e.g., screenshot capture).

5. **Resize path.** `WidgetTreeHost::notifyWindowResize*()` currently
   creates a `CompositeFrame`, sets it on all widgets, calls
   `handleHostResize`, and deposits. In the new lifecycle, resize
   calls `invalidate(PaintReason::Resize)` on the root widget, which
   sets `DirtyBits::Layout | Paint`, and the next `buildFrame()`
   handles it. The question is whether resize needs to be synchronous
   (to avoid one frame of stale content during a window drag) or
   whether the vsync-paced frame is fast enough.

---

## 9. What gets deleted

At the end of Tier D:

- `Widget::executePaint()` (73 lines) — gone
- `Widget::init()` initial paint trigger — folded into `FrameBuilder`
  first-frame logic
- `Widget::Impl` paint state (`paintInProgress`, `hasPendingInvalidate`,
  `pendingPaintReason`, `initialDrawComplete`) — gone
- `PaintOptions` struct — gone
- `PaintReason` enum — replaced by `DirtyBits`
- `Widget::onPaint(PaintReason)` — replaced by `SceneNode::paint(PaintContext&)`
- `UIView::update()` (270 lines) — split into ~80 lines across three
  phase methods
- `WidgetTreeHost::paintAndDeposit()` — gone
- `WidgetTreeHost::invalidateWidgetRecurse()` — gone
- `WidgetTreeHost::setActiveCompositeFrameRecurse()` — gone
- `View::startCompositionSession()` / `endCompositionSession()` — gone
- `View::submitPaintFrame()` — gone
- `CanvasView::submitPaintFrame()` — gone
- Duplicate `startCompositionSession()` calls in UIView — gone
- Layout-during-paint in `Container::onPaint` — gone
- Element-list rebuild during paint in every primitive widget — moved
  to model update time

**Estimated deletion:** ~500 LOC of lifecycle plumbing.
**Estimated addition:** ~200 LOC (`FrameBuilder`, `DirtyBits`, phase
assertions).
**Net:** ~300 LOC removed. The real win is not LOC but the elimination
of undefined phase ordering.

---

## 10. Honest uncertainty

I have not profiled the cost of the Tick phase walking the entire tree
every frame. If most nodes have no active animations, the walk is
wasted. The optimization — skip Tick for subtrees with no animator — is
straightforward but adds a propagation bit (`HasActiveAnimation`) that
complicates the dirty flag model. Start without it; add if profiling
shows Tick is expensive.

I am assuming that deferred invalidation (Tier A) does not break any
existing app code that depends on synchronous paint. The test apps in
the repository should be exercised with the deferred model to confirm.
If any app reads back compositor state immediately after `invalidate()`,
it will see stale data.

The resize path (open question 5) is the most likely place where
deferred invalidation fails. Window resize events arrive at high
frequency and the user expects immediate visual feedback. If
vsync-paced rendering produces visible lag during window drag, the
resize path may need a synchronous `buildFrame()` call, making it the
one exception to the deferred model.
