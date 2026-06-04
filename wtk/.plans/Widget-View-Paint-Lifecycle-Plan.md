# Widget / View Paint Lifecycle Standardization Plan

**Status:** Partially implemented; tier markers reconciled against code
on 2026-05-21 — see §0. Tier A is in. The render-redesign plumbing this
plan rides on (per-window `FrameBuilder`, `DisplayList`, collapsed
per-view canvas) has also landed via `UIView-Render-Redesign-Plan.md`.
**Tier B (phase-model-in-place + Style Tier 1) is COMPLETE as of
2026-05-29** — B0–B5 all landed (§11 Block 1): `StyleSheet`→`Style`
rename + layout strip (B1), `ComputedStyle` cache + `resolveStyles()`
(B2), `FramePhase`/`PaintContext` + `update()` split into
Tick/Style/Layout/Paint (B3), `rebuildContent()` out of `onPaint` +
Container layout out of paint (B4), cross-phase assertion teeth + verify
(B5). **The active target is now Block 2 — Tier C cleanup** (delete
`executePaint` reentrancy machinery, retire `PaintOptions`, remove the
no-op session shims). Tier D is open — but see §0.2: **`SceneNode` is no
longer planned for adoption.** The `View` base already absorbed the
phase virtuals (`paint(PaintContext &)`, `resolveStyles()`,
`arrangeContent()`) it would have hosted, so Tier D becomes "finish the
deletions on `View`/`Widget` directly" rather than "introduce a new node
type." The render redesign's §9.0 ("`View` *is* the `SceneNode`")
captures the same decision.
**Scope:** Define a strict, phase-separated paint lifecycle for `Widget`
and `View` that eliminates the current ad-hoc paint paths, prevents
layout-during-paint and paint-during-layout reentrancy, and gives
animation a clean slot that does not entangle with style resolution or
frame submission.
**Prerequisite reading:** `UIView-Render-Redesign-Plan.md` (the
scene-tree / display-list plan this lifecycle plugs into),
`Style-StyleSheet-Refactor-Plan.md` (whose Tier 1 is merged into this
plan's Tier B), and `Animation-Scheduler-Plan.md` (the per-window
`AnimationScheduler` that will drive the Tick phase — it supersedes the
stale `Animation-API-Simplification-Plan.md` and is folded into
Render Redesign Tier 4, so it is **not** a Tier B prerequisite).
**Non-goals:** Changing the `DisplayList`/`DrawOp` representation
(covered by the render redesign). Changing the `Compositor` backend.
Changing `StyleSheet` authoring syntax.

**Compositor backend assumed by this plan:** the
Direct-To-Drawable / SDF backend
(see `Direct-To-Drawable-And-SDF-Plan.md`) is in for the simple
primitives. Concrete implications for this plan:

  - A styled shape (Rect / RoundedRect / Ellipse with optional border)
    produces **one** `VisualCommand` and **one** SDF draw call. The
    prior fill-then-stroked-frame-path pair is gone.
  - The triangulator is opened lazily and only on `VectorPath` /
    `Bitmap` / gradient-fallback paint.
  - `Canvas::drawRect` / `drawRoundedRect` / `drawEllipse` no longer
    side-emit a `RectFrame` / `RoundedRectFrame` / `EllipseFrame`
    visual when a border is set.

These behaviors are already shipped and assumed in the migration
sketches below. The lifecycle changes are about *who triggers paint
and when*, not about further changes to the visual-command shape.

---

## 0. Implementation status (reconciled against code 2026-05-21)

The tier markers in §5 had drifted from the code. This section is the
authoritative reconciliation; trust it over any stale `[DONE]` marker.

**Done:**

- **Tier A (deferred invalidation)** — real. `View::DirtyBit
  {Style,Layout,Content,Paint}`, `invalidate()` sets bits +
  `treeHost->requestFrame()`, `invalidateNow()` is `[[deprecated]]`,
  `WidgetTreeHost::paintDirty()` flushes at the frame boundary.
- **The render-redesign plumbing this plan rides on** (from
  `UIView-Render-Redesign-Plan.md`, *not* this plan): `DisplayList` /
  `DrawOp` exist; `UIView::update()` already builds a `DisplayList` and
  hands it to a window-level `FrameBuilder`; per-view canvases are
  collapsed (`submitPaintFrame` / `endCompositionSession` are no-ops).

**NOT done — the catch:** the `FrameBuilder` that exists today is the
render-redesign's **frame-submission orchestrator** (begin/end session,
offset stack, replay queue). It is **not** this plan's phase engine —
there is **no `FramePhase` enum, no Tick→Style→Layout→Paint→Commit
ordering, no phase assertions.** Same name, different object.
`UIView::update()` is still the ~310-line monolith. `onPaint(PaintReason)`
is unchanged; there is no `PaintContext`. Widgets still rebuild their
layout *during* paint. `executePaint`, `paintInProgress`,
`hasPendingInvalidate`, `PaintOptions` all still exist.

So **Tier B (the actual phase model) is untouched**, and **Tier C is at
most partial** — the per-window `FrameBuilder` / single `CompositeFrame`
infrastructure landed via the render plan, but every lifecycle-specific
*deletion* Tier C lists (kill `executePaint`, the reentrancy guards,
`PaintOptions`, the session shims) is still pending.

**The Tier B rescope (decided 2026-05-21).** Tier B as originally written
referenced `UIViewNode` / `SceneNode` / `PaintContext`-on-a-tree, which
are Tier D structures that do not exist — taken literally it collapses
Tier B+C+D into one change. Tier B (§5) is therefore rewritten as
**phase-model-in-place**: the phase ordering, assertions, and a real
`PaintContext` are introduced on the *existing* types; `SceneNode` /
`UIViewNode` and the `onPaint`→`paint(PaintContext&)` widget-hook rename
stay in Tier D. **`Style-StyleSheet-Refactor-Plan.md` Tier 1 + the
`ComputedStyle` cache are merged into Tier B** — the resolved-style cache
*is* the content of the new Style phase. Animation does **not** leave
`Style` in Tier B (the `AnimationScheduler` is folded into Render
Redesign Tier 4); the existing per-view animator keeps running and is
read during Paint.

---

## 0.1 B0 pre-flight results (greps run 2026-05-29)

Authoritative survey for the Tier B sub-phases. Counts are src-tree
unless a `tests/` path is named. `[clear]` = no migration hazard found.

**Surface inventory:**

| Surface | Definition | In-tree (src) callers | Migrates in |
|---|---|---|---|
| `setStyleSheet` | `UIView.h:314`, `UIView.Style.cpp:305` | 12 (all `Primatives.cpp`) | B1 (alias `setStyle`) |
| `StyleSheet::Create` | `UIView.Core.cpp:190` | 7 (all `Primatives.cpp`) | B1 (alias) |
| `StyleSheet::layout{Width,Height,Margin,Padding,Clamp,Transition}` | `UIView.h:200-206`, `UIView.Core.cpp:456-523` | **0 callers** — API surface only | B1 (strip → `UIElementLayoutSpec.layout`) |
| `StyleSheet::element{Animation,BrushAnimation,PathAnimation}` | `UIView.h:169-179`, `UIView.Core.cpp:361-389` | **0 authoring callers** | stays on `Style` (B1) |
| `invalidateNow` | `Widget.h:213`, `Widget.Paint.cpp:133` | **0 callers** | B5 audit is trivial; safe to deprecate/convert |
| `onPaint` overrides | — | 10 src: `Rectangle`,`RoundedRectangle`,`Ellipse`,`Path`,`Separator`,`Label`,`Icon`,`Image` (`Primatives.cpp`), `Container` (`BasicWidgets.cpp`), `StackWidget` (`Containers.cpp`) + 9 test subclasses | B4 |
| `UIView::update()` callers | `UIView.Update.cpp:111` (~300 LOC, lines 111-415) | 14 (all `Primatives.cpp`); `Container`/`StackWidget` `onPaint` use `layoutChildren()`, not `update()` | B3 (split) |

**Greenfield (nothing exists yet — confirms §0):**

- `FramePhase` / `currentPhase_` / `assertPhase` / `PaintContext` — **none present.** `FrameBuilder` (`src/UI/FrameBuilder.h`) is the submission orchestrator only (begin/end session, offset stack, replay queue). B3 adds the phase engine here.
- `ComputedStyle` — **does not exist.** B2 greenfield.

**Rename-collision decisions:**

- **`LayoutStyle` → `Layout`: CONFIRM defer to Tier D — keep `LayoutStyle` through Tier B.** There is *no* bare `Layout` type today (and no `Widget::Layout`), so no hard symbol clash — but `Layout.h` already hosts a dense `Layout*` family (`LayoutUnit`, `LayoutLength`, `LayoutEdges`, `LayoutClamp`, `LayoutDisplay`, `LayoutNode`, `LayoutBehavior`, `LayoutContext`, `LayoutStyle`, `LayoutTransitionSpec`, …). A bare `Layout` amid that family is ambiguous, and the rename would ripple through the `Widget` API (`setLayoutStyle`/`layoutStyle`/`hasExplicitLayoutStyle`), `UIViewImpl`, and 8 `tests/` sites for no Tier-B benefit.
- **`StyleSheet` → `Style` (B1): SAFE with the deprecated alias.** The only existing `Style` symbol is `NativeDialog::Style` (`NativeDialog.h:56`, a namespaced enum) — no top-level `OmegaWTK::Style` collision. `StyleSheet` is a `struct` (`UIView.h:58`, fwd-declared `UIView.h:16` + `Layout.h:307`). `StyleRule` already exists (`Layout.h:250`); `Selector` does not (correctly deferred to Block 3).

**SDF / VisualCommand checklist item (§10) — CLEAR.** `VisualCommand` appears only under `Composition/` (Canvas, DisplayList, backend). **No `src/Widgets/` file constructs or mutates a `VisualCommand` directly** — every widget paints through `Canvas`/`UIView`. The "in-tree widget that bypasses Canvas" hazard does not exist.

**B3 composition-session dedup target (confirmed present):** the duplicate session bracket is real — `Widget.Paint.cpp:55/69` *and* `UIView.Update.cpp:193/414` both open/close it. (`SVGView.cpp` and `VideoView.cpp` keep their own brackets — out of B3 scope.)

**B1/B4 merge + risk targets located:** `convertEntriesToRules` (`Layout.cpp:286`) and `mergeLayoutRulesIntoStyle` (`Layout.cpp:461`) — the layout half called from `UIView.Update.cpp:131/148` — are the B1 strip points. The B4 regression site is confirmed: `Container::relayout()` (`BasicWidgets.cpp:321`) calls `layoutChildren()` *synchronously*, and `Container::onPaint` (`BasicWidgets.cpp:145`) *also* calls it when `layoutPending` — B4 must move this into the Layout phase without double-running it.

---

## 0.2 Status reconciliation (2026-05-31)

Two changes since the 2026-05-21 reconciliation: Block 1 (Tier B) shipped
in full on 2026-05-29, and the render-redesign plan landed Phase 4.7.5
plus Phase 4.8 (UIView-Render-Redesign §0 / §9.0). The lifecycle-side
impact is recorded here so this plan does not have to be re-walked
against the render plan.

**Block 2 (Tier C cleanup) — partial.** The session-shim half landed via
the render plan; the executePaint / PaintOptions / reentrancy-guard half
has **not** started. Current state in tree (verified against the headers
2026-05-31):

| Tier C item | State | Notes |
|---|---|---|
| `View::startCompositionSession()` / `endCompositionSession()` | **Deleted** | Render Phase 4.7.5 (`View.h:220-228`). |
| Duplicate session bracket in `UIView::update()` | **Deleted** | Removed in B3. `FrameBuilder::ScopedFrame` is the sole session owner. |
| `View::submitPaintFrame(int)` | Reduced to a public no-op (`View.h:311`) | Render Phase 3.8. Survives as a `Widget` call-site shim; final removal still pending. |
| `Widget::executePaint(PaintReason, bool)` | Still alive (`Widget.h:109`, `Widget.Paint.cpp:13`) | Block 2 deletion target unchanged. |
| `Widget::Impl::paintInProgress` / `hasPendingInvalidate` / `pendingPaintReason` | Still alive (`WidgetImpl.h:92-94`) | Phase enforcement (B5) has made the reentrancy guards dead code at the assertion level, but the fields and the if-branch in `executePaint` are still compiled in. |
| `PaintOptions` | Still alive (`Widget.h:57`, `Widget.h:200-201`) | `coalesceInvalidates` is always-on per Tier A; `autoWarmupOnInitialPaint` is also slated to go now that shaders are pre-compiled (see §8 Q3). |
| `WidgetTreeHost::paintAndDeposit()` | **Gone** | Replaced by `paintDirty()` already; the per-frame deposit lives on `FrameBuilder`. |
| `WidgetTreeHost::invalidateWidgetRecurse()` | Still alive (`WidgetTreeHost.cpp:277`) | Block 2 — fold into the FrameBuilder walk. |
| `WidgetTreeHost::paintDirty()` | Still alive (`WidgetTreeHost.cpp:310`) | Block 2 — fold into the FrameBuilder walk. `paintDirtyRecurse` is already marked vestigial (Phase 4.7.4). |
| `WidgetTreeHost::setActiveCompositeFrameRecurse()` | **Gone** | Single window-owned `CompositeFrame` since render Tier 3. |
| `CanvasView::submitPaintFrame` | **Gone (with the class)** | Render-redesign §9.1 deleted the header in Tier 3. |

**Tier D / SceneNode — dropped.** The original Tier D introduced a
`SceneNode` base, renamed `Widget::onPaint(PaintReason)` →
`SceneNode::paint(PaintContext &)`, and folded `Widget` into the scene
tree. None of that is being adopted. The replacement, already in tree
through render Phase 4.7.0–4.7.5 and now §9.0 of
[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md):

- `View` *is* the node type. The new phase virtuals live directly on
  `View`: `View::paint(Composition::PaintContext &)` (Phase 4.7.0),
  `View::resolveStyles()` and `View::arrangeContent()` (Phase 4.7.2).
  `UIView` overrides all three; `SVGView` overrides `paint`; the
  `ScrollView` paint path takes `DisplayList &` directly (signature
  deviation noted in the render-redesign §9.6 table).
- `Widget` stays. The widget tree continues to wrap the view tree; the
  pre-Tier-B `onPaint(PaintReason)` hook stays on `Widget` rather than
  becoming `paint(PaintContext &)`. The widget-hook rename is no longer
  planned.
- The Style → Layout → Paint orchestration that Tier D would have
  hoisted into a tree walker already happened on the per-window
  `FrameBuilder` (render plan's, not this plan's earlier `SceneNode`
  sketch). `FrameBuilder::buildFrame(View &)` walks `View` directly.

Tier D's *other* contents — `ComputedStyle` re-key from
`UIElementTag` to `(NodeId, PropertyKey)` and the
`AnimationScheduler` adoption (render Phases 4.3 / 4.4 / 4.8 =
`Animation-Scheduler-Plan.md` Tiers A–E) — are unaffected by the
SceneNode decision and remain on the Block 4 list.

**Net effect on §5 Tier D and §11 Block 4.** The "`Widget` is removed"
bullet, the `SceneNode::paint(PaintContext &)` rename bullet, and the
"`WidgetTreeHost` becomes `WindowFrameHost`" bullet are all withdrawn.
What remains is the *cleanup half* of Tier D — finish Block 2's
deletions and execute the animation/style migrations on the existing
`View`/`Widget` pair.

---

## 0.3 Tier D scoping (decided 2026-06-01)

Tier D was originally a SceneNode introduction; §0.2 rescoped it to
"finish Block 2 + Animation-Scheduler Tiers B/C/E + ComputedStyle
re-key + Style Tier 3." The 2026-06-01 pass adds a multi-phase
breakdown (§5 Tier D below, §11 Block 4 below) and resolves four open
decisions:

1. **Stylesheet ownership.** `AppWindow` owns its local stack;
   `StyleSheet` is a `SharedHandle`-typed value so multiple
   `AppWindow`s can share sheets. Sheets are
   immutable-once-installed (mutations produce a new sheet handle and
   re-install) so concurrent windows reading the same sheet are safe.
   The `StyleResolver` per window walks that window's stack.
2. **Style Tier 2 lands inside Tier D as D6.** Style Tier 3
   (transitions / themes) requires Tier 2 (`StyleSheet` + `Selector`
   + `StyleResolver`), and Tier D requires Style Tier 3 to make the
   `scheduler.transition(...)` hook real. Folding Tier 2 in here
   collapses the prior "Block 3 lands independently" path; the
   former Block 3 is retired.
3. **`ComputedStyle` re-key splits per-property.** The cache becomes
   `Map<(NodeId, PropertyKey), TypedValue>`, sharing shape with the
   scheduler side table (Animation-Scheduler-Plan §3.2). Paint reads
   collapse to one lookup per `(node, key)` with a uniform fallback
   chain: scheduler side table → resolved-style table → UA default.
   This is a bigger D5 than the aggregate-cache option, but it is what
   Style Tier 3's property-grained transition invalidation wants and
   it removes the aggregate-vs-side-table impedance mismatch.
4. **`WidgetTreeHost::paintDirty()` stays on `WidgetTreeHost`.** It is
   a 6-line `FrameBuilder::ScopedFrame + buildFrame` wrapper today and
   moving it churns the resize path for no real reduction.

The 2026-06-01 pass also adds **keyframe animations to `StyleSheet`**
alongside transitions (WML requires them). The scheduler API already
accepts `KeyframeTrack<T>` via `animateProperty<T>(node, key, track,
timing)` (`AnimationScheduler.h:144`); what's missing is the
authoring + binding surface:

- `StyleSheet` gains named keyframe declarations (a CSS `@keyframes`-
  equivalent: `KeyframeAnimation{name, tracks per-property, default
  timing}`).
- `Style` / `StyleRule` gains a binding (`animation: <name> <timing>`)
  per property or per rule, paralleling `transition`.
- `StyleResolver` fires `scheduler.animateProperty<T>(node, key,
  track, timing)` when a binding becomes active and cancels the
  handle when the binding stops applying. Lifecycle parallel to
  transitions but with explicit track data rather than implicit
  prev→next interpolation.

Keyframe declarations land in D6.1 (alongside `StyleSheet`/`Selector`);
the resolver-driven firing lands in D7.3.

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

`Animation-Scheduler-Plan.md` gives us a per-window
`AnimationScheduler` with a `tick()` method (it supersedes the stale
per-view `Animation-API-Simplification-Plan.md`). This plan defines
*when* that tick runs relative to layout and paint. Note: that scheduler
is folded into Render Redesign Tier 4 — until then (i.e. through Tier B),
the Tick slot drives the *existing* per-view animator's `advanceAnimations()`.

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

The "full paint" is now significantly cheaper than the original
analysis (Render-Execution-Efficiency-Plan §current-execution-profile
shows ~2–8 ms after the SDF spine, down from 39–95 ms). The lifecycle
problems still matter — undefined phase ordering, no dirty gate, and
the "every property change runs a full paint" coupling are
architectural issues independent of how cheap the paint itself is —
but the *urgency* of deferring invalidation is reduced from "we're
spending 50 ms per resize tick" to "we're rebuilding the same display
list three times per setProps call". Treat the migration as a
correctness / clarity refactor, not an emergency performance fix.

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

When `UIViewNode::paint` walks an element with both a fill brush and
a stroke (border), the SDF backend renders both bands from one
`VisualCommand` — i.e. the DisplayList op set produced for that
element is a single `Rect` / `RoundedRect` / `Ellipse` op with the
border field populated, not a fill op followed by a stroked-path op.
This is the §3.2 design of the render-redesign plan made concrete by
the SDF spine. Widget subclasses that previously authored two style
elements (one for fill, one for stroke) can collapse them into one.

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

### Tier A — Deferred invalidation (can ship independently) [DONE]

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

### Tier B — Phase separation in place + Style Tier 1 merge (COMPLETE 2026-05-29)

**Rescoped 2026-05-21 (see §0).** Introduce the phase model on the
*existing* types — no `SceneNode` / `UIViewNode`; `onPaint(PaintReason)`
is kept. Merges `Style-StyleSheet-Refactor-Plan.md` Tier 1 + the
`ComputedStyle` cache (the resolved cache *is* the new Style phase's
output). **Animation stays on `Style`** and keeps running on the existing
per-view animator; the `AnimationScheduler` migration is Render Redesign
Tier 4 / Tier D (Render Phases 4.3 / 4.4 / 4.8), not this tier.

Six sub-phases, each independently shippable:

- **B0 — Pre-flight (no code).** Grep `setStyleSheet`, `StyleSheet::Create`,
  `->layout{Width,Height,Margin,Padding,Clamp,Transition}`,
  `->element{Animation,BrushAnimation,PathAnimation}`, `invalidateNow`,
  `onPaint` overrides, `.update()` callers. Confirm the `LayoutStyle`→
  `Layout` rename collision (existing `Layout` / `Widget::Layout`) —
  recommendation: **keep `LayoutStyle` through Tier B**, defer the rename
  to Tier D.
- **B1 — Style Tier 1: rename + strip *layout only*.** Rename
  `StyleSheet`→`Style` (per-view, element-tag-keyed inline visual+text
  surface). Keep `[[deprecated]] using StyleSheetPtr = StylePtr;` and a
  `setStyleSheet`→`setStyle` forwarding alias for one cycle. Move layout
  authoring off the sheet onto `UIElementLayoutSpec.layout` (the existing
  `LayoutStyle` field, renamed `.layout`); drop the layout half of
  `convertEntriesToRules` / `mergeLayoutRulesIntoStyle`. **Animation
  tracks and the per-property `transition`/`duration` flags stay on
  `Style`** (consumed by the existing animator until Render Phase 4.8).
- **B2 — `ComputedStyle` + extract style resolution.** Define
  `ComputedStyle` (resolved, no `Optional<>`; visual+text only — layout
  is separate now). Cache it per element on `UIView::Impl`. Extract the
  resolution slice of `update()` into `UIView::resolveStyles()` writing
  `ComputedStyle` (inline `Style` + CSS-style inheritance; **no** global
  selector stack — that is Style Tier 2). Paint reads `ComputedStyle`.
- **B3 — Phase model.** Add `FramePhase {Idle,Tick,Style,Layout,Paint,
  Commit}` + `currentPhase_` + debug `assertPhase()` to the existing
  `FrameBuilder` (its natural home). Split `UIView::update()` into ordered
  `tickAnimations()` / `resolveStyles()` / `arrange()` (the layout-resolve
  + z-sort block) / `paint(PaintContext&)` (the `DisplayList` build), with
  `update()` orchestrating them in order and flipping `currentPhase_`
  around each. Introduce `PaintContext { DisplayList&; Transform2D; Rect
  clip; float opacity; }`. `tickAnimations()` drives the existing
  `advanceAnimations()` pump (Tier D swaps the body for
  `scheduler.tick()`); `paint()` layers `animatedValue(tag,key)` over
  `ComputedStyle`. Remove the duplicate `startCompositionSession` /
  `endCompositionSession` calls from `update()` — the window
  `FrameBuilder::ScopedFrame` already owns the session.
- **B4 — Widget contract: rebuild out of paint.** Add `rebuildContent()`
  (the `lv2.clear()` → rebuild element list → `setStyle(...)` body now in
  each `onPaint`), called from `onMount` / `setProps` / `resize`, setting
  `Content|Style|Paint`. `onPaint(PaintReason)` becomes read-only.
  `Container`'s `layoutChildren()` moves into the Layout phase.
- **B5 — Assertion teeth + verify.** Turn on cross-phase assertions
  (`DisplayList::append` only in Paint; `ComputedStyle` writes only in
  Style; layout resolve/sort only in Layout). Audit `invalidateNow()`
  callers. Verify against `tests/RootWidget/Main.cpp`.

**Risk:** Medium. B1 is the widest mechanical change (~10 widget files +
`Primatives`/`BasicWidgets`/`Containers`), alias-cushioned. B4 is the most
likely regression site — moving `Container` layout to run *before* paint
changes when children get their rects.

**Carried to Tier D (explicitly not done here):** `SceneNode` /
`UIViewNode`; the `onPaint`→`paint(PaintContext&)` widget-hook rename;
animation off `Style` onto the `AnimationScheduler`; re-keying
`ComputedStyle` from `UIElementTag` to `(NodeId,PropertyKey)`.

**Files touched:** `UIView.h`, `UIView.Core.cpp`, `UIView.Style.cpp`,
`UIView.Update.cpp`, `UIViewImpl.h`, `Layout.h`, `Layout.cpp`,
`FrameBuilder.h`, `FrameBuilder.cpp`, `Widget.h`, `Widget.Paint.cpp`,
`WidgetImpl.h`, `Primatives.cpp`, `BasicWidgets.cpp`, `Containers.cpp`,
plus a header for `PaintContext` (in `DisplayList.h` or a new file).

### Tier C — Delete the old paint plumbing (ship after Tier B) [PARTIAL]

**Status (2026-05-21):** the *infrastructure* — per-window `FrameBuilder`,
one `CompositeFrame` per window — already landed via Render Redesign
Tier 3. What remains is the lifecycle-specific **deletion** of the old
paint plumbing, now that Tier B's phase ordering + Tier A's deferred
invalidation make the reentrancy machinery redundant. The bullets below
are the *remaining* (not-yet-done) work, except where annotated `[done]`.

- `FrameBuilder` lives on `AppWindow`, one per window. `[done — render Tier 3]`
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

### Tier D — Animation, style cascade, and final cleanup (ship alongside Render Redesign Tier 4)

**Rescoped 2026-05-31 — `SceneNode` is no longer adopted.** The phase
virtuals (`paint(PaintContext &)`, `resolveStyles()`, `arrangeContent()`)
landed directly on `View` in render Phases 4.7.0–4.7.2, and
`FrameBuilder::buildFrame(View &)` walks the `View` tree as the canonical
paint path. Tier D therefore inherits the dirty work that *isn't*
SceneNode-shaped — the animation / style migrations and the residual
deletions — and drops the scene-tree introduction bullets.

**Re-decided 2026-06-01 (see §0.3).** AppWindow owns the local
stylesheet stack and sheets are sharable across windows. Style Tier 2
lands inside Tier D as D6 (the former Block 3 is retired). The
resolved-style cache splits per-property to share shape with the
scheduler side table. `WidgetTreeHost::paintDirty()` stays put.
`StyleSheet` gains named keyframe animations alongside transitions
(needed by WML).

Withdrawn from Tier D (no longer planned):

- ~~`Widget::paint(PaintContext &)` becomes `SceneNode::paint(PaintContext &)`.~~
- ~~`Widget` is removed as a concept.~~
- ~~`WidgetTreeHost` becomes `WindowFrameHost` / folded into `AppWindow`.~~
- ~~`onPaint(PaintReason)` → `paint(PaintContext &)` widget-hook rename.~~

Nine sub-phases, each independently shippable. Dependencies in §5.D9.

- **D0 — Reconciliation (doc-only). [DONE 2026-06-03.]** Tree-truth
  pass over §11 Block 4 and this sub-phase list, run from codedb +
  direct header/source reads (file:line citations below). Results:

  **Confirmed landed (already marked):**
  - Anim Tier A — `AnimationScheduler` lives at
    `wtk/src/UI/AnimationScheduler.{h,cpp}` (class at
    `AnimationScheduler.h:124`). `FrameBuilder::beginFrame` calls
    `impl->animationScheduler_->tick(FrameTime{...})` once per
    outermost frame at `FrameBuilder.cpp:80` (the cited line 66
    was stale — comment block now starts at 76, call at 80).
  - Anim Tier C scaffolding — `View::nodeId()` at `View.h:305`;
    `UIView::Impl::elementNodeIds_` at `UIViewImpl.h:167`;
    `animationTargets_` at `UIViewImpl.h:173`.

  **Newly confirmed landed (plan said pending / "ambiguous"):**
  - **D3 (Anim Tier B `applyLayoutDelta` migration) is DONE in
    tree.** Both axes are already routed:
    - `View::applyLayoutDelta` (`View.Core.cpp:207`) emits four
      `scheduler->tweenProperty<float>(node, PropertyKey::LayoutX/Y/
      Width/Height, ...)` calls at lines 255 / 259 / 263 / 267,
      with a no-scheduler fallback at line 226.
    - `UIView::applyLayoutDelta` (`UIView.Layout.cpp:41`) does the
      same at lines 81 / 85 / 89 / 93 (fallback at line 58).
    No further work for D3; it collapses to a "skip — already in
    tree" entry in D9's shipping order.
  - **D4 routing is DONE; D4 deletions are still pending.**
    `UIView::Impl::startOrUpdateAnimation`
    (`UIView.Animation.cpp:92`) routes to
    `scheduler->tweenProperty<float>(node, propKey, from, to, timing,
    curve)` at line 154 (with a zero-duration cancel branch that
    issues `(to,to)` at line 138). The `(tag, key)` to-match
    short-circuit is preserved via `animationTargets_` at lines
    143–148, exactly as §5 D4 requires. `animatedValue` reads
    `scheduler->value<float>(*node, elementKeyToProperty(key))` at
    `UIView.Animation.cpp:219`. `advanceAnimations` is already a
    no-op stub returning `false` at `UIView.Animation.cpp:194` —
    declared at `UIViewImpl.h:217`, so the public symbol still
    exists and must be deleted in D4 (header + impl together).

  **Confirmed still pending:**
  - **D1 targets all present.**
    - `Widget::executePaint` defined at `Widget.Paint.cpp:13`,
      declared at `Widget.h:109`. Live callers: `init()`,
      `invalidateNow()`, `flushPendingPaint()`,
      `WidgetTreeHost::invalidateWidgetRecurse`
      (`WidgetTreeHost.cpp:297`).
    - `Widget::Impl::{paintInProgress, hasPendingInvalidate,
      pendingPaintReason, deferredReason}` at
      `WidgetImpl.h:92–100`.
    - `PaintOptions::{autoWarmupOnInitialPaint, warmupFrameCount,
      coalesceInvalidates}` at `Widget.h:58–60`. The
      `coalesceInvalidates` field is read once at
      `Widget.Paint.cpp:38`; `invalidateOnResize` (`Widget.h:69`)
      stays per §5 D1.
    - `Widget::flushPendingPaint` at `Widget.Paint.cpp:140`,
      declared at `Widget.h:115`. Currently called from
      `Widget::invalidate()` (`Widget.Paint.cpp:137` is the
      `executePaint(reason,true)` immediate-mode tail; the deferred
      path at line 123 + 147–151 routes through this symbol). D1
      collapses it into the new `invalidateNow` body; D8 confirms
      callerless and deletes.
  - **D2 targets all present.**
    - `View::submitPaintFrame(int)` declared as an inline `{}`
      virtual at `View.h:311`. Grep across `wtk/src` + `wtk/include`
      shows the declaration is the *only* mention — no overrides,
      no callers. Pure dead code; safe to delete in D2.
    - `WidgetTreeHost::invalidateWidgetRecurse` defined at
      `WidgetTreeHost.cpp:277`, declared at `WidgetTreeHost.h:144`,
      called from the resize path at `WidgetTreeHost.cpp:300`
      (self-recursion) and indirectly from the same module — the
      sole entry is the resize path as §5 D2 expects.
    - The four no-op recurse shims:
      `observeWidgetLayerTreesRecurse` at `WidgetTreeHost.cpp:258`,
      `unobserveWidgetLayerTreesRecurse` at line 271,
      `paintDirtyRecurse` at line 332,
      `beginResizeCoordinatorSessionRecurse` at line 340 (all
      declared at `WidgetTreeHost.h:142–148`).
      `observe/unobserveWidgetLayerTreesRecurse` *are* still called
      from `WidgetTreeHost::onWidgetTreeChanged` (lines 228, 435,
      659, 663) but the bodies are no-ops — D2 deletes them and
      drops the calls in the same pass.
  - **D4 deletions** (in addition to the `advanceAnimations` symbol
    pair noted above): `PropertyAnimationState`
    (`UIViewImpl.h:106`), `PathNodeAnimationState`
    (`UIViewImpl.h:119`). Both are unreferenced in `.cpp` files —
    header-only carcasses.
    **CORRECTION (re-verified 2026-06-03 during D4 prep): the
    `EffectAnimationKey*` enum (`UIViewImpl.h:125–138`) is NOT
    deletable in full.** The D0 grep missed `UIView.Update.cpp:244–
    266`, which reads
    `Impl::EffectAnimationKey{ShadowOffsetX, ShadowOffsetY,
    ShadowRadius, ShadowBlur, ShadowOpacity, ShadowColorR,
    ShadowColorG, ShadowColorB, ShadowColorA}` via
    `Impl::animatedValue(tag, key)` to merge animated shadow channels
    into the resolved effect state. The Shadow* nine values are the
    live read surface for the scheduler-driven shadow animation
    pipeline. D4 keeps them. The trailing three (`GaussianRadius`,
    `DirectionalRadius`, `DirectionalAngle`) are unreferenced
    anywhere in `wtk/` — D4 trims them.
  - **Path-node animation migration is still open.** Plan §5 D4
    says path animations migrate to
    `scheduler.animatePropertyAt(node, PathNodeX/Y, nodeIndex, ...)`.
    The destination exists (`AnimationScheduler.h:158`,
    `PathNodeX/Y` enum at `AnimationScheduler.h:73`) but has zero
    callers in `wtk/src`. `ElementAnimationKeyPathNodeX/Y` (the
    public enum at `UIView.h:67–68`) is the legacy entry point; it
    is *not* currently routed through the scheduler the way
    scalar keys are. D4 must add this routing alongside the
    `startOrUpdateAnimation` cleanup — the previous D4 wording
    implied it was already covered by the same shim.
  - **D5 — `computedStyles_` aggregate cache still in place.**
    `UIView::Impl::computedStyles_` at `UIViewImpl.h:185`
    (`Map<UIElementTag, ComputedStyle>`). `computedStyleFor(tag)`
    at `UIView.Style.cpp:301–305` reads it. Paint reads via
    `computedStyleFor(entry.tag)` at `UIView.Update.cpp:222`.
    `ResolvedViewStyle` / `ResolvedTextStyle` / `ResolvedEffectStyle`
    are still aggregate types in `UIViewImpl.h:15, 22, 38`.
  - **D8 residuals confirmed present.**
    - `Composition/Animation.h` still carries stale forward
      declarations: `class LayerAnimator;` and `class ViewAnimator;`
      surface at `Animation.h:184–185` as `AnimationHandle`
      `friend`s, plus `class detail::AnimationRuntimeRegistry;`
      forward at `Animation.h:29` and friend at line 186. The
      classes themselves are gone (codedb misses), so these
      `friend`s point at nothing and must be stripped in D8.
    - `UIView::Impl::{lastAnimationDiagnostics,
      lastObservedDroppedPacketCount, hasObservedLaneDiagnostics}`
      still declared at `UIViewImpl.h:193–195`. The 2026-06-01
      §0.3 note guessed these were already gone — they are not.
      D8 deletes them.

  **Net adjustments to §5 Tier D and §11 Block 4:**
  - Mark D3 as **done in tree (no-op phase)**. The §5 D3 sub-phase
    becomes a one-line "already routed; skip" entry. §11 Block 4's
    D3 checkbox flips to checked. The D9 shipping unit "D3 + D4 +
    D5 ship as one unit" loses its D3 leg.
  - Rephrase D4 as "scheduler-routing **deletions** + path-node
    migration." Scalar routing (`startOrUpdateAnimation`,
    `animatedValue`) is already in tree; what D4 actually does is
    (a) delete the dormant header symbols above, and (b) add the
    missing `animatePropertyAt(node, PathNodeX/Y, ...)` routing for
    path-node animations.
  - No other sub-phase changes scope; D1, D2, D5, D6, D7, D8 stay
    as written.

  These edits land in the next commit alongside the §11 Block 4
  checkbox flips and the §5 D3 / D4 rewording — D0 itself is just
  this reconciliation block.

- **D1 — Tier C deletions, pass 1: `Widget` paint plumbing. [DONE
  2026-06-03.]** Phase asserts (B5) already make the reentrancy
  state dead; this just removes the carrier. Delete `Widget::executePaint`
  (`Widget.Paint.cpp:13`); inline "mark dirty bits + request frame"
  at each live caller (`init()`, `invalidateNow()`,
  `flushPendingPaint()`, `WidgetTreeHost::invalidateWidgetRecurse`).
  Delete `Widget::Impl::{paintInProgress, hasPendingInvalidate,
  pendingPaintReason, deferredReason}` (`WidgetImpl.h:92-100`).
  Retire `PaintOptions::{autoWarmupOnInitialPaint, warmupFrameCount,
  coalesceInvalidates}` (§8 Q3 — shaders are pre-compiled, coalesce
  is always-on per Tier A); keep `invalidateOnResize` (live
  semantics). `invalidateNow()` stays as the `[[deprecated]]` escape
  hatch: clear bits → `treeHost->paintDirty()`. `flushPendingPaint`
  is collapsed into the new `invalidateNow` body. Estimated ~80 LOC
  removed.

- **D2 — Tier C deletions, pass 2: `View` / `WidgetTreeHost` shims.
  [DONE 2026-06-03.]**
  Remove `View::submitPaintFrame(int)` (`View.h:311`) once D1 kills
  its last `Widget`-side caller. Replace
  `WidgetTreeHost::invalidateWidgetRecurse` (sole caller is the
  resize path) with a direct walk that marks bits + calls
  `requestFrame()`. Delete the already-no-op
  `WidgetTreeHost::{paintDirtyRecurse, observeWidgetLayerTreesRecurse,
  unobserveWidgetLayerTreesRecurse, beginResizeCoordinatorSessionRecurse}`.
  Keep `WidgetTreeHost::paintDirty()` (decision §0.3 #4).

- **D3 — Anim Tier B: `applyLayoutDelta` migration. [DONE in tree —
  no work; D0 verified 2026-06-03.]** Both `View::applyLayoutDelta`
  (`View.Core.cpp:207`, scheduler calls at lines 255 / 259 / 263 /
  267 with a no-scheduler fallback) and `UIView::applyLayoutDelta`
  (`UIView.Layout.cpp:41`, scheduler calls at lines 81 / 85 / 89 /
  93) already emit `scheduler->tweenProperty<float>(node, LayoutX/Y/
  Width/Height, ...)` per axis. Tree-truth status flips this
  sub-phase to a no-op; it is no longer part of the D9 shipping
  unit.

- **D4 — Anim Tier C: scheduler-routing deletions + path-node
  migration. [DONE 2026-06-03.]** Scalar tween routing is already in
  tree (D0 verified): `UIView::Impl::startOrUpdateAnimation`
  (`UIView.Animation.cpp:92`) routes to
  `scheduler->tweenProperty<float>(...)` at lines 138 / 154 and
  preserves the `(tag, key)` `to`-match short-circuit via
  `animationTargets_` at lines 143–148; `animatedValue`
  (`UIView.Animation.cpp:204`) reads via
  `scheduler->value<float>(*node, elementKeyToProperty(key))` at
  line 219. What D4 actually does:

  1. **Path-node animation routing.** The legacy public keys
     `ElementAnimationKeyPathNodeX/Y` (`UIView.h:67–68`) currently
     route through `startOrUpdateAnimation` like scalar element
     channels but never reach `scheduler.animatePropertyAt(node,
     PathNodeX/Y, nodeIndex, ...)`. Add the routing — extend
     `startOrUpdateAnimation` (or split out a path-node sibling) to
     dispatch path-node keys to `animatePropertyAt` so the
     scheduler keys them by `(node, PropertyKey, subIndex=nodeIndex)`
     instead of collapsing every node's X/Y onto a single cell.
  2. **Header / impl deletions.** Delete the truly dormant per-tag
     tween state:
     `UIView::Impl::PropertyAnimationState` (`UIViewImpl.h:106`),
     `PathNodeAnimationState` (`UIViewImpl.h:119`), and the
     `advanceAnimations` declaration (`UIViewImpl.h:217`) plus its
     no-op body at `UIView.Animation.cpp:194–202`. **Do NOT delete
     the `EffectAnimationKey*` enum** — `UIView.Update.cpp:244–266`
     reads the Shadow* nine values to merge animated shadow channels
     into the resolved effect state. D4 keeps Shadow* and trims only
     the unreferenced trailing constants `GaussianRadius`,
     `DirectionalRadius`, `DirectionalAngle` (D0's "all three are
     unreferenced" grep was wrong; re-verified 2026-06-03 during D4
     prep).

- **D5 — `ComputedStyle` re-key: split per-property,
  `(NodeId, PropertyKey) → TypedValue`. [DONE 2026-06-03.]** With D4
  in, the resolved-style cache and the scheduler side table share the
  same identity *and* the same shape. Replace
  `UIView::Impl::computedStyles_` (`UIViewImpl.h:185`,
  `Map<UIElementTag, ComputedStyle>`) with a per-property table keyed
  by `(NodeId, PropertyKey, subIndex)`, using the same
  `PropertyTableKey` shape the scheduler uses
  (`AnimationScheduler.h:102`). `resolveStyles()` writes one cell per
  resolved property per element; `paint()` reads via a uniform
  lookup helper:

      template<typename T>
      T resolved(NodeId n, PropertyKey k, T fallback) const {
          if (auto v = scheduler->value<T>(n, k)) return *v;
          if (auto v = styleTable_.value<T>(n, k)) return *v;
          return fallback;   // UA default
      }

  The `ResolvedViewStyle` / `ResolvedTextStyle` / `ResolvedEffectStyle`
  aggregate types in `UIViewInternal` become thin convenience views
  rebuilt on demand from the per-property table (or are deleted
  outright once Paint reads property-at-a-time). `ComputedStyle`
  aggregate as a stored value is gone.

- **D6 — Style Tier 2: global `StyleSheet` + `Selector` +
  `StyleResolver` + keyframe declarations. [DONE 2026-06-03.]**
  Largest single phase (per Style plan §5 Tier 2). Five sub-steps:

  - **D6.1 — Vocabulary.** `Selector` (Tier-1 single-compound: tag +
    class + id + pseudo-class), `StyleRule` (selector + property →
    `StyleValue`), `StyleSheet` (rule vector + named
    `KeyframeAnimation` declarations). Reuse the cascade comparator
    (`StyleRule::beats()`, `Layout.h:250`). The keyframe shape:

        struct KeyframeAnimation {
            OmegaCommon::String                              name;
            OmegaCommon::Vector<KeyframeAnimationProperty>   properties;
            Composition::TimingOptions                       defaultTiming;
        };
        struct KeyframeAnimationProperty {
            PropertyKey                                  key;
            Composition::KeyframeTrack<AnimatedValue>    track;
        };

    Sheets are immutable-once-installed (per §0.3 #1); mutating a
    sheet produces a new handle.

  - **D6.2 — `AppWindow::styleSheets()` stack** + addition / removal
    API. `SharedHandle<StyleSheet>` so sheets are shareable across
    windows. The cascade walks the stack top-to-bottom.

  - **D6.3 — `StyleResolver::resolve(node)`** runs the cascade across
    the window's stack + layers inline `Style` over it + writes one
    cell per property into the D5 per-property table. Lives inside
    Phase 2 (Style) of `FrameBuilder`. Bottom of the stack is the UA
    default sheet (D7.5).

  - **D6.4 — Pseudo-class state.** `:hover` / `:pressed` / `:focused`
    / `:disabled` as node state bits on `View`. The input layer flips
    them and sets `DirtyBit::Style`. Resolver consults them during
    selector match.

  - **D6.5 — Transition + keyframe-binding *recording* (no driving
    yet).** When a rule carries `transition:` or `animation:`
    metadata, the resolver records the binding on the node but does
    not yet call the scheduler. D7 wires the firing.

- **D7 — Style Tier 3 + Anim Tier D: themes, transitions, keyframes,
  custom states.**

  - **D7.1 — `ThemeVars` on `AppInst`. [DONE 2026-06-04.]**
    `StyleValue` gains a `StyleSheets::Var{name}` variant alternative
    (rule values may name a theme variable). `AppInst` owns a
    `SharedHandle<ThemeVars>` — a `StyleSheet`-shaped immutable
    handle (Builder + `Create()`) so themes are shareable across
    `AppWindow`s. The `StyleResolver` substitutes `Var` against the
    active theme during the Style phase via a new local
    `resolveVar(value, theme)` helper; unresolved Vars (no theme
    installed / missing name / `Var`→`Var` chain / `monostate`
    binding) collapse to `monostate` so the cell write is skipped
    and the inline-`Style` writes that follow the resolver still
    get to author the property — matching CSS `var()` fallthrough.
    Chains are intentionally not followed in D7.1; revisit if a
    real use case appears. `AppInst::setThemeVars(...)` dirties
    every known `AppWindow`'s cascade via the new public
    `AppWindow::applyCascadeChange()` walker, which runs
    `Widget::invalidate(PaintReason::ThemeChanged)` over the whole
    widget tree (this fix also closes the pre-existing
    `addStyleSheet` / `removeStyleSheet` runtime hole — pre-D7.1
    they only `requestFrame()`'d, which the `FrameBuilder::
    styleSubtree` walker gates against per-view, so clean views
    silently missed runtime sheet swaps). Files: new
    `omegaWTK/UI/ThemeVars.h` + `wtk/src/UI/ThemeVars.cpp`;
    StyleValue extension in `omegaWTK/UI/StyleProperty.h`; AppInst
    surface in `omegaWTK/UI/App.h` + `wtk/src/UI/App.cpp`; AppWindow
    helper in `omegaWTK/UI/AppWindow.h` + `wtk/src/UI/AppWindow.cpp`;
    Var substitution in `wtk/src/UI/StyleResolver.cpp`. Full-tree
    build green (89/89 on this macOS host). Multi-window note:
    `AppWindowManager` currently tracks only `rootWindow`, so the
    dirty fanout reaches that window only; when multi-window lands,
    `setThemeVars` is the single call site that needs to grow.

  - **D7.2 — Transitions wired. [DONE 2026-06-04.]** New
    `UIView::Impl::previousStyleTable_` (`StyleTable`) snapshots the
    previous Style phase's resolved cells — `UIView::resolveStyles`
    swaps the current and previous tables at the top of the method,
    so the resolver / inline-style writes that follow populate a
    fresh `styleTable_` while `previousStyleTable_` carries last
    frame's content for comparison. New
    `StyleResolver::applyTransitions(view)` (called at the bottom
    of `resolveStyles` after both the cascade and the inline
    writes have settled) walks `sheetBindings_.transitions` —
    populated by D6.5 — and for each record looks up the raw cell
    in both tables. A `std::visit` over the `StyleValue` variant
    dispatches on the cell's runtime type; only the transitionable
    intersection (`Color`, `std::uint32_t`,
    `LayerEffect::DropShadowParams`) reaches the scheduler. Brush
    handle swaps and Font / TextLayoutDescriptor / monostate / Var
    cells snap directly — matching CSS "non-animatable property
    snaps." `AnimationScheduler::transition<T>(...)` is the new
    friend-only retargeting hook (templated; `friend class
    StyleSheets::StyleResolver`): looks up an active animation for
    the `(node, key)`; if found, captures the current sampled
    value as the new `from` (smooth retarget); seeds the
    scheduler's side table with `from` via the also-new
    `seedTableFromStyle` (Phase-Style-allowed bypass of the public
    `setTableValue`'s Tick-only assert) so this frame's Paint
    reads the pre-transition value; then delegates to
    `tweenProperty<T>` which cancels the prior animation and
    installs a fresh one with the spec's timing + curve.
    Animation.h gains a `KeyframeLerp<Composition::Color>`
    specialization — the canonical transition target had no
    specialization pre-D7.2 and would have tripped the
    `is_arithmetic_v<T>` static_assert. Files touched:
    `wtk/include/omegaWTK/Composition/Animation.h` (Color lerp),
    `wtk/include/omegaWTK/UI/StyleResolver.h` (`applyTransitions`
    decl), `wtk/src/UI/StyleResolver.cpp` (`applyTransitions` +
    type-dispatched valuesEqual table), `wtk/src/UI/UIViewImpl.h`
    (`previousStyleTable_`, `StyleTable::swap` /
    `StyleTable::getRaw`), `wtk/src/UI/UIView.Style.cpp` (snapshot
    swap + applyTransitions call), `wtk/src/UI/AnimationScheduler.h`
    (`friend class StyleResolver`, private `transition<T>` +
    `seedTableFromStyle`), `wtk/src/UI/AnimationScheduler.cpp`
    (`seedTableFromStyle` impl with Style-phase assert). Full-tree
    build green (59/59 on this macOS host).

    **Follow-up landed same session 2026-06-04: scheduler auto-pump
    + ContainerClampAnimationTest conversion.** During runtime
    verification, the menu-triggered drop-shadow animation in
    `wtk/tests/ContainerClampAnimationTest` ran one tick then froze
    — the framework stopped dispatching `Widget::onPaint(PaintReason)`
    at the 4.7.4 cutover, so the test's self-pump (which lived
    inside an `onPaint` override per the D8 caveat) was dead. Without
    a pump, *no* scheduler-driven animation observes the second
    frame — neither D7.2 transitions nor pre-existing
    `animateElement` tweens. Added the canonical scheduler-side
    auto-pump in `FrameBuilder::beginFrame`: when
    `AnimationScheduler::stats()` reports any active
    property/callback animations after `tick()`, invalidate the
    widget tree's root via
    `root->invalidate(PaintReason::StateChanged)` (marks Paint
    dirty + calls `treeHost->requestFrame()` for the next vsync)
    so this frame's `buildFrame` doesn't early-return on
    `rootMask == 0` and the next frame fires its own tick. Winds
    down naturally when the active set empties. This replaces the
    D8-tracked `BlueRectWidget::onPaint` self-pump caveat.
    Simultaneously rewrote `ContainerClampAnimationTest` to actually
    exercise the D7.2 path: built TWO sheets (`buildClampInitialSheet`
    + `buildClampHighlightSheet`), both with a `TransitionSpec` for
    `PropertyKey::DropShadow` (1.5 s, EaseInOut). The "Animate"
    menu toggles `addStyleSheet` / `removeStyleSheet` on the
    highlight sheet; the cascade tie (same selector, source-order
    `>=` tiebreak) flips the resolved `DropShadow` cell, the new
    `applyTransitions` pass detects the prev→curr delta, the
    transition record matches, and `scheduler.transition<DropShadowParams>`
    fires the lerp. Deleted the pre-D7.2 `triggerShadowAnimation`,
    the `Widget::onPaint` self-pump, the `chrono`-driven deadline,
    and the `invalidateAncestors` workaround — all replaced by the
    sheet swap. Files: `wtk/src/UI/FrameBuilder.cpp` (auto-pump in
    `beginFrame`),
    `wtk/tests/ContainerClampAnimationTest/main.cpp` (sheet-swap
    test rewrite). Full-tree build green (57/57 on this macOS
    host).

    **Two further fixups landed same session 2026-06-04 after
    runtime verification showed the lerp still wasn't visible:**

    *Sub-UIView markDirty propagation* (the D8 (b) bug, surfacing
    at runtime). `applyCascadeChange` was calling
    `widget->invalidate(ThemeChanged)` per widget, which marks the
    widget's MAIN view dirty but does NOT propagate down — sub-
    UIViews created via `makeSubView<UIView>(...)` stayed clean.
    `View::markDirty` only walks UP (OR-ing into ancestor
    `descendantDirty`), so the FrameBuilder's `styleSubtree`
    walker gated each node on its OWN `dirtyBits() & Style` and
    skipped sub-UIViews entirely. Their `resolveStyles` never ran,
    their `sheetBindings_.transitions` stayed empty,
    `applyTransitions` never saw the cell change. Fixed by
    replacing the per-widget `invalidate` call with a free-helper
    `markViewSubtreeDirty(view)` that walks the View subtree at
    each widget, marking Style|Layout|Paint on every render node
    including sub-UIViews. The fix is the cascade-path
    instantiation of one of the D8 (b) options noted there.

    *Late auto-pump check* in `FrameBuilder::endFrame`. The
    pre-Style auto-pump in `beginFrame` catches "an animation was
    already running when this frame started," but a transition
    registered DURING this frame's Style phase (`applyTransitions`
    → `scheduler.transition<T>` → `tweenProperty`) only becomes
    active AFTER that snapshot. Without a late check, the very
    first cascade-change-triggered transition registered a tween
    that never got ticked. Added a `stats()` re-check at the end
    of every outermost `endFrame`: if any animation is active,
    call `window_.requestFrame()` so the next frame's `beginFrame`
    auto-pump takes over the per-frame pump duty.

    Files: `wtk/src/UI/AppWindow.cpp` (`markViewSubtreeDirty`
    replaces the old per-widget invalidate path),
    `wtk/src/UI/FrameBuilder.cpp` (`endFrame` stats-check). Full-
    tree build green (57/57 on this macOS host).

    **Third runtime fixup — cascade sheet-order tie-break.** The
    second round of verification (diag logs in `applyTransitions`
    + `endFrame`) revealed `transitions.size=1` on `blue_rect_view`
    but no `scheduler.transition<T>` call and
    `endFrame schedStats=0/0`. Root cause: the `StyleRule::beats`
    tie-break ordered `(specificity, sourceOrder)`, but
    `StyleSheet::Builder::addRule` stamps `sourceOrder`
    independently per sheet — so an "earlier" sheet whose
    `blue_rect` rule had `sourceOrder=2` (the third rule added)
    beat a "later" highlight sheet whose `blue_rect` rule had
    `sourceOrder=0` (its only rule). The cell never changed
    between frames, `applyTransitions` saw equal prev/curr, and
    no transition fired. The fix lives in
    `StyleResolver::apply` — the cascade tie-break now threads a
    `sheetIndex` through the `consider()` closure, ordering as
    `(specificity, sheetIndex, sourceOrder)`. This matches the
    canonical CSS cascade (later-in-the-stack wins on specificity
    tie). The `StyleRule::beats` method itself is unchanged —
    only the resolver's inline comparator is updated, because
    `beats` does not have access to the sheet stack and the
    cleanest fix is at the call site that does. Files:
    `wtk/src/UI/StyleResolver.cpp`. Full-tree build green.

    **Runtime-verified end-to-end 2026-06-04.** Screenshot from
    `ContainerClampAnimationTest.app` after the menu click shows
    the blue rounded rect with a fully lerped red drop shadow —
    the canonical D7.2 verification. The diag `[WTK_RP]` traces
    (`applyCascadeChange`, `applyTransitions`,
    `scheduler.transition<T>`, `endFrame schedStats`) added
    during the bisect were removed in the same pass once the
    chain was confirmed.

  - **D7.3 — Keyframe animations wired.** When a keyframe binding
    becomes active on a node (a rule with `animation: <name>`
    matches), the resolver looks up the named `KeyframeAnimation` on
    the originating sheet and calls
    `scheduler.animateProperty<T>(node, key, track, timing)` per
    property. When the binding stops matching, the resolver cancels
    the returned handle. Re-application with the same `(node, name)`
    is a no-op (preserves running animation); a different `name` on
    the same `(node, key)` replaces. This is the parallel to D7.2 for
    explicitly authored animations.

  - **D7.4 — `:state(name)` custom pseudo-class.** Nodes carry a
    `Set<String>` of custom states; set/clear dirties `Style`.

  - **D7.5 — User-agent default stylesheet.** Built-in `StyleSheet`
    for `Button` / `Label` / `Icon` / `Image` / `Rectangle` /
    `RoundedRectangle` / `Ellipse` / `Path` / `Separator` etc., sat
    at the bottom of every `AppWindow`'s stack. Widget subclasses
    stop authoring inline visual defaults in `rebuildContent()` —
    they author only model-state-dependent overrides.

- **D8 — Final cleanup (Anim Tier E + residuals).** Audit
  `Composition/Animation.h` for residual `LayerAnimator` /
  `ViewAnimator` / `LayerClip` / `ViewClip` /
  `AnimationRuntimeRegistry` forward decls or `friend` declarations
  and remove them. Delete `UIView::Impl::{lastAnimationDiagnostics,
  lastObservedDroppedPacketCount, hasObservedLaneDiagnostics}`
  (likely already gone per render-redesign §0 — verify in D0).
  Delete `Widget::flushPendingPaint` if D1's inlining left it
  callerless. Update `API.rst` per Animation-Scheduler-Plan Tier E
  §5: replace the `LayerClip` / `ViewClip` / `LayerAnimator` /
  `ViewAnimator` sections with one `AnimationScheduler` section, fix
  the §1025 / §1035 cross-references, add a `StyleSheet` /
  `StyleResolver` / `ThemeVars` section.

  **D8 also retires `Widget::onPaint(PaintReason)`.** Added to D8
  scope 2026-06-03 (observation from external review of D6 test
  refactors). The Phase 4.7.4 cutover already stopped the framework
  from dispatching `Widget::onPaint` — `FrameBuilder::buildFrame`
  walks the `View` tree and calls `View::paint(PaintContext &)`
  (the 4.7.0 hook) at each node, and D1 (2026-06-03) then deleted
  `Widget::executePaint`, removing the last symbol that could ever
  have called it. The base virtual was marked `[[deprecated]]` in
  `Widget.h` on the same date with the deletion target pointing
  here. D8's job: (a) delete the base virtual declaration in
  `Widget.h`; (b) delete every in-tree override of it —
  `Rectangle::onPaint`, `RoundedRectangle::onPaint`,
  `Ellipse::onPaint`, `Path::onPaint`, `Separator::onPaint`,
  `Label::onPaint`, `Icon::onPaint`, `Image::onPaint`
  (`Primatives.cpp`), `Container::onPaint`
  (`BasicWidgets.cpp`), `StackWidget::onPaint`
  (`Containers.cpp`) — all of which are vestigial empty bodies that
  the framework no longer dispatches; (c) sweep test overrides
  (the canonical pattern is `onMount()` + `resize()`, see
  `wtk/tests/TextCompositorTest/main.cpp`); (d) the previously-
  legitimate `BlueRectWidget::onPaint` override in
  `wtk/tests/ContainerClampAnimationTest/main.cpp` was deleted
  during the D7.2 auto-pump landing (2026-06-04). The auto-pump
  lives in `FrameBuilder::beginFrame`: when
  `AnimationScheduler::stats()` reports any active animations
  after `tick()`, the FrameBuilder invalidates the widget-tree
  root Paint-dirty and requests another vsync, keeping the loop
  alive until the active set empties. D8 no longer has to choose
  between killing the test's animation and preserving an
  `onPaint` override. Removing `onPaint` is a clean source-level
  break for every downstream override — the deprecation comment
  in `Widget.h` documents the migration path so external code can
  prep for D8.

  **D8 should also retire two layout-manager / View-tree bugs
  uncovered during the D6 test conversions on 2026-06-03.**

  (a) `AbsoluteLayout::arrange` and `FillLayout::arrange`
  (`wtk/src/UI/LayoutManager.cpp:128, :154`) were passing the input
  `finalRectLocal` straight to `clampRectToParent`, but the caller
  in `FrameBuilder::layoutSubtree` recurses with
  `child->getRect()` — a rect whose `pos` is the node's position in
  its OWN parent's space (non-zero whenever any parent layout had
  already positioned the node), not a local-origin clamp box.
  Children get addressed in the node's LOCAL space (origin 0), so
  passing the parent-space rect forced every child's `pos` UP to
  `finalRectLocal.pos` as a minimum. The paint walker then added
  the parent's offset again, doubling the accumulated `paintOffset`
  by exactly the parent's pos. Visible in `EllipsePathCompositorTest`
  and `ContainerClampAnimationTest` as children drifting off-screen
  on Retina builds. Patched 2026-06-03 by building the clamp box at
  origin (0, 0) inside each manager. `FlexLayout` accidentally
  side-stepped the same bug because it already built its own
  `contentBoundsRect` with `pos = padding`. **D8 should harden the
  contract**: rename `LayoutSubtree`'s argument from `finalRect` to
  something that makes the parent-space interpretation explicit
  (e.g., `nodeRectInParent`), and have either the walker or the
  manager base class derive a local-origin rect before dispatching
  to `arrange`. The fix landed at the manager level for now because
  that was the targeted patch; doing it at the walker level would
  remove the same trap from any future manager subclass.

  (b) The sub-UIView `markDirty` propagation sharp edge —
  Surfaced 2026-06-03 by the D6 test conversions: when a widget
  hosts a `UIView` created via `makeSubView<UIView>(rect, "tag")`
  (i.e. the UIView is a CHILD of the widget's own view, not the
  widget's view itself — `EllipsePathCompositorTest`,
  `ContainerClampAnimationTest::BlueRectWidget`,
  `RootWidgetTest::Phase32Widget` all do this), the new UIView
  starts with `dirtyBits_ = 0`. `Widget::init()` marks the
  widget's OWN view dirty but does not propagate down, and
  `View::addSubView` does NOT mark the new subview dirty either —
  so the FrameBuilder's pre-order Style/Layout/Paint walkers
  (`FrameBuilder.cpp` `styleSubtree` / `layoutSubtree` /
  `paintSubtree`) never visit the new UIView, the resolver never
  writes its sheet cells, and Paint reads back UA defaults.
  Today every widget that hosts sub-UIViews papers over this by
  calling `uiView->update()` after `setLayout(...)` — a
  rediscover-every-time bug class. Pick one of these for D8 (or
  earlier if it bites again):
  (1) `View::addSubView` inherits the parent's current `dirtyBits`
      onto the new child — the simplest mechanical change, no
      coupling between `View` and `UIView`. The child immediately
      participates in whatever passes the parent is dirty for.
  (2) `UIView::setLayout` / `setLayoutV2` / equivalent calls
      `markDirty(View::Layout | View::Paint)` themselves — covers
      layout authoring but not style; couples mutators to dirty
      mechanics, which is the right model.
  (3) Both. (1) handles the "child created after parent already
      dirty" case; (2) handles the "model mutated after dirty
      bits were cleared" case. Combined, they remove the need for
      app code to ever call `update()` explicitly.
  The fix is small (<30 LOC) but it's a behavior change to public
  semantics — `addSubView` propagating dirty is observable to
  anyone who relies on "new subview is clean." Worth getting in
  before too much external code builds on the current shape.

#### D9. Dependency / shipping order

- **D1, D2 ship together** — Block 2 closeout. Smallest change, no
  observable behavior shift (asserts already prove the deletions are
  unreachable).
- **D3 is already in tree** (D0 verified 2026-06-03). The former
  "D3 + D4 + D5 ship as one unit" loses its D3 leg.
- **D4, D5 ship as one unit** — animation convergence. Intermediate
  states (e.g. D4 done but D5 not) leave the resolved-style cache
  keyed by tag while the scheduler is keyed by NodeId; awkward and
  short-lived.
- **D6 ships alone** — Style Tier 2 is substantive new subsystem code.
- **D7 ships alone** — transitions + keyframe lifecycles want focused
  retargeting / cancellation tests.
- **D8 is cleanup** — runs after D7 lands cleanly.

D0 is doc-only and gates everything else.

**Risk:** Medium-High overall (down from the original High, up from
§0.2's Medium — D6's selector + cascade is the biggest single subsystem
this plan introduces, and the property-grained D5 rekey is wider than
the aggregate-cache alternative the prior wording assumed).

| Sub-phase | Risk | Why |
|---|---|---|
| D0 | None | Doc-only. |
| D1, D2 | Low | Dead-code deletion; B5 asserts already prove unreachable. |
| D3 | None | Already in tree (D0 verified 2026-06-03). |
| D4 | Low–Medium | Scalar `(tag, key)` `to`-match short-circuit is already in tree and verified. Remaining risk is the path-node `animatePropertyAt` routing — new code, but small. Header deletions are mechanical. |
| D5 | Medium | Wider than an aggregate rekey: every Paint site that read `computedStyleFor(tag).<field>` becomes a per-property `resolved(n,k,fallback)` lookup. Behavior-preserving but mechanically broad. |
| D6 | **Medium-High** | New selector matcher + cascade rules + keyframe declaration shape. Specificity ties, inherited properties, keyframe-track type erasure are the edge-case cluster. Heavy unit-test coverage on `StyleResolver` is the mitigation. |
| D7 | Medium | Transition retargeting is the standard CSS edge case. Keyframe binding lifecycle (matches → fires; un-matches → cancels; re-matches with same name → no-op) is a parallel edge-case cluster. |
| D8 | Low | Final symbol cleanup + doc. |

**Files touched (cumulative across D1–D8):** `Widget.h`,
`Widget.Paint.cpp`, `WidgetImpl.h`, `View.h`, `View.Core.cpp`,
`WidgetTreeHost.h`, `WidgetTreeHost.cpp`, `UIView.h`,
`UIView.Core.cpp`, `UIView.Style.cpp`, `UIView.Update.cpp`,
`UIView.Animation.cpp`, `UIView.Layout.cpp`, `UIViewImpl.h`,
`AnimationScheduler.h`, `AnimationScheduler.cpp` (the `friend`
`transition()` / `animateProperty<KeyframeTrack>` hooks),
`AppWindow.h`, `AppWindow.cpp`, `Application.h`, `Application.cpp`,
new `StyleSheet.{h,cpp}`, new `StyleResolver.{h,cpp}`, new
`Selector.{h,cpp}`, new `ThemeVars.{h,cpp}`, every widget subclass
(`Primatives.cpp`, `BasicWidgets.cpp`, `Containers.cpp` —
`rebuildContent()` shrinks once D7.5 lands the UA sheet), `API.rst`.

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

- **`Direct-To-Drawable-And-SDF-Plan.md`** — Owns the compositor
  backend that this plan's Paint phase emits into. The simple-shape
  parts of that plan (Phase 6.1–6.3, 6.5, 6.8) are already in:
  rect / rounded-rect / ellipse / shadow render via SDF in one draw
  per primitive, with the border consolidated into the fill draw.
  This plan inherits that contract — `Rectangle::rebuildContent`
  authors one element per shape (not one for fill + one for stroke),
  and the Paint-phase walk emits one `VisualCommand` per element
  rather than two. As that plan adds more primitives (vector-path
  edge AA in 6.4, bitmap improvements in 6.6, MSDF text in 6.7),
  the Paint phase picks them up without lifecycle changes.

- **`Animation-Scheduler-Plan.md`** (supersedes the stale
  `Animation-API-Simplification-Plan.md`) — The per-window
  `AnimationScheduler` has a `tick()` method. This plan places `tick()`
  in Phase 1 and requires that animation values are written to a
  `(NodeId,PropertyKey)` side table during Tick, read during Paint, and
  never written during Paint. The scheduler is folded into Render
  Redesign Tier 4 (Phases 4.3 / 4.4 / 4.8); it is *not* a Tier B
  prerequisite, so Tier B's Tick slot drives the existing per-view
  animator and Tier D swaps in `scheduler.tick()`.

- **`Render-Execution-Efficiency-Plan.md`** — The deferred
  invalidation model (Tier A) directly fixes the "every invalidate
  triggers a full paint" problem called out in that plan. The single
  `buildFrame()` entry point produces one display list per frame per
  window, which is what the batched compositor scheduler wants. With
  the SDF spine in, the per-frame paint cost dropped from 39–95 ms to
  ~2–8 ms; deferred invalidation now buys cleanliness more than
  emergency throughput.

---

## 8. Open questions

1. ~~**Frame pacing.** `buildFrame()` should run at most once per vsync.~~ **Resolved.** [UIView-Render-Redesign-Plan Phase H](UIView-Render-Redesign-Plan.md#phase-h-follow-up--frame-pacing-vsync-aligned-production--real-frametime--load-aware-frame-gating-folds-frame-pacing-plan) now spells out the full mechanism:

   - A **per-window `FramePacer`** (one per `AppWindow` — the pacer owns this window's frame loop and animation timeline).
   - Consuming a **per-screen vsync source**: `Native::displayLinkForScreen(currentScreen())` from [Native-API §2.9 NativeScreen](Native-API-Completion-Proposal.md#29-nativescreen-new--prerequisite-for-22s-dpi-consumer-phase-f-and-phase-h). Vsync is a property of the *display*, not the window; §2.9 owns the per-platform display-link wiring (`CADisplayLink` on macOS, `IDXGIOutput::WaitForVBlank` / `DCompositionWaitForCompositorClock` on Windows, `wl_surface.frame` + `wp_presentation_feedback` under Wayland, `GLX_INTEL_swap_event` under X11). The pacer rebinds on cross-screen transitions (driven by Phase F's `onRealize`).
   - Vsync provides two distinct goods — (a) a *gate* for frame production (one `buildFrame` per refresh, never more) and (b) a monotonic `FrameTime{monotonicNs, frameIndex}` for the `AnimationScheduler::tick` argument that replaces Phase 4.3's `steady_clock` stand-in.
   - The outer-loop vsync clock composes with the inner-loop `PaceHint` from the (now-stale) Frame-Pacing-Plan — vsync says when a build *can* happen, `PaceHint` says whether it *should*.
   - Pace-critical exceptions (resize, DPI scale change, live-animation frames, first paint) bypass throttling.
   - Refresh-rate detection is per-onVsync (handles drag between 60 Hz / 120 Hz / 144 Hz displays); ProMotion / G-Sync / FreeSync VRR is handled by `NativeScreenDesc::variableRefreshRate` plus the per-frame predicted interval from the `NativeDisplayLink`, with VRR treated as max-refresh for budgeting.

   §2.9 NativeScreen is the prerequisite that lands first; Phase H is then platform-agnostic on its side. While both are unimplemented, Tier A's "simple flag + manual trigger" remains the interim shim (Tier A is already shipped — see §0).

2. **Manual-mode widgets.** `PaintMode::Manual` widgets currently skip
   `executePaint` entirely. In the new lifecycle, manual widgets skip
   the `paint()` call in Phase 4 but still participate in Tick, Style,
   and Layout phases (since those affect their children). The manual
   widget can call `FrameBuilder::forcePaintNode(node)` to paint
   on demand.

   Sure.

3. **Warmup frames.** The current `PaintOptions::autoWarmupOnInitialPaint`
   submits the initial frame multiple times. This was a workaround for
   compositor pipeline latency. With one-frame-per-vsync and proper
   double/triple buffering in the compositor, warmup should be
   unnecessary. Verify with the single-surface rendering path before
   removing. Note: the SDF pipeline's first draw triggers shader
   compilation on some platforms; if that produces a perceived
   first-frame stall, the answer is to compile the SDF pipeline at
   `PipelineRegistry::initialize()` time (already the case) and
   pre-warm with a 1×1 dummy draw at engine init, *not* to bring back
   warmup frames at the lifecycle level.

   We have switched to loading pre-compiled shaders, so shader complilation time shouldn't be any issue.
   We can remove the Warmup frames.

4. **`invalidateNow()` callers.** Grep for all callers of
   `invalidateNow()` in the codebase. Each one is a potential
   assumption that paint completes synchronously. These must be audited
   and either converted to deferred `invalidate()` or justified as
   legitimate synchronous paint needs (e.g., screenshot capture).

   Yes.

5. **Resize path.** `WidgetTreeHost::notifyWindowResize*()` currently
   creates a `CompositeFrame`, sets it on all widgets, calls
   `handleHostResize`, and deposits. In the new lifecycle, resize
   calls `invalidate(PaintReason::Resize)` on the root widget, which
   sets `DirtyBits::Layout | Paint`, and the next `buildFrame()`
   handles it. The question is whether resize needs to be synchronous
   (to avoid one frame of stale content during a window drag) or
   whether the vsync-paced frame is fast enough. With the SDF spine
   in, the simple-primitive resize cost dropped roughly an order of
   magnitude (no per-frame triangulator round-trip, no offscreen
   texture rebuild on the no-effect path) — vsync-paced rendering is
   far more likely to be sufficient than under the original profile.
   Validate empirically on a 10-widget window resize before deciding
   whether to introduce a synchronous resize escape hatch.

   (This will be taken care of by Phase F from UIView-Render-Redesign-Plan)

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
one exception to the deferred model. With the SDF spine, the per-frame
budget that used to be 39–95 ms is now 2–8 ms, so the bar this
synchronous escape hatch has to clear is much higher than under the
original analysis.

I am assuming that the SDF backend's invariants (the bordered shape
emits a single VisualCommand; the triangulator is opened lazily) hold
for every primitive widget the migration touches. The SVGView migration
that landed alongside the SDF spine confirmed this for the public
`Canvas::draw*` API. Any in-tree widget that bypasses Canvas and
manipulates VisualCommand directly is a pre-flight checklist item for
Tier B.

---

## 11. Cross-plan execution checklist (ordered)

Synthesizes the *remaining* work across this plan,
`Style-StyleSheet-Refactor-Plan.md`, `UIView-Render-Redesign-Plan.md`,
and `Animation-Scheduler-Plan.md`, in dependency order. `[x]` = landed;
`[ ]` = pending. Reconciled 2026-05-21.

### Already landed (foundation)

- [x] Lifecycle **Tier A** — deferred invalidation (`DirtyBits`,
  deferred `invalidate()`, `paintDirty()` at the frame boundary).
- [x] Render **Tier 1** — one sync lane per window.
- [x] Render **Tier 2** (Phases 2.0–2.6) — `DisplayList` / `DrawOp`,
  `update()` builds a DisplayList, `localBoundsFromView` cache deleted,
  SVGView migrated, op-set vocabulary (`PushClip` / `NativeContent`).
- [x] Render **Tier 3** (through ≈Phase 3.8) — per-window `FrameBuilder`
  (submission orchestrator), per-view canvas collapsed, window-scoped
  composition session.

### Block 1 — Lifecycle Tier B + Style Tier 1 (COMPLETE 2026-05-29; see §5 Tier B)

- [x] **B0** pre-flight greps + `LayoutStyle` rename decision. *(done 2026-05-29 — see §0.1.)*
- [x] **B1** `StyleSheet`→`Style` rename; strip layout onto
  `UIElementLayoutSpec.layout`; animation stays on `Style`. *(done
  2026-05-29 — in-tree callers migrated to `Style`/`setStyle`/`getStyle`;
  `[[deprecated]]` `StyleSheet`/`StyleSheetPtr` typedefs + `setStyleSheet`/
  `getStyleSheet` forwarders kept one cycle for out-of-tree callers. Layout
  authoring methods + `Layout*` `Entry::Kind` + the four `layout*Value`
  fields removed from `Style`; the sheet→`convertEntriesToRules`→
  `mergeLayoutRulesIntoStyle`/`resolveLayoutTransition` layout path removed
  from `update()`. `mergeLayoutRulesIntoStyle` + `StyleRule` `Layout*`
  props kept for direct callers / Block 3. Verified: UI+Widgets libs and
  all migrated test objects compile clean.)*
- [x] **B2** `ComputedStyle` + extract `resolveStyles()`. *(done 2026-05-29
  — `UIViewInternal::ComputedStyle{brush, effects, text}` (per-element
  aggregate of the resolved sub-styles; top-level has no `Optional`,
  effect-presence `Optional`s kept inside `ResolvedEffectStyle`). Cached
  on `UIView::Impl` as `computedStyles_` (per tag) + `resolvedViewStyle_`
  (view-level). New private `UIView::resolveStyles()` rebuilds both;
  `update()` calls it then reads only the caches via
  `Impl::computedStyleFor(tag)` — no inline `resolve*`. Behavior-
  preserving (key equivalence: `entry.tag == spec.tag`; text resolved
  against `textStyleTag` and cached under the element's own tag). Rebuild
  is unconditional per-frame for now; B3 gates it on `DirtyBit::Style`.
  Full runtime verify is B5.)*
- [x] **B3** `FramePhase` + assertions on `FrameBuilder`; split `update()`
  into Tick/Style/Layout/Paint; `PaintContext`; Tick wraps
  `advanceAnimations()`; Paint layers `animatedValue` over `ComputedStyle`.
  *(done 2026-05-29 — `FramePhase{Idle,Tick,Style,Layout,Paint,Commit}` +
  `currentPhase_` + `setPhase`/`assertPhase` + `ScopedPhase` RAII +
  `phaseName()` on `FrameBuilder` (`assertPhase` defined but not yet called
  by work methods — teeth are B5). `Composition::PaintContext{displayList,
  transform=Matrix4x4::Identity, clip, opacity}` in `DisplayList.h`.
  `update()` split into `tickAnimations()`/`resolveStyles()`/`arrange()`/
  `paint(PaintContext&)`, orchestrated in order with a `ScopedPhase` around
  each (Commit wraps the FrameBuilder hand-off). `arrange()` writes
  `Impl::arranged_`/`arrangedLocalBounds_`; `paint()` reads them +
  `ComputedStyle` + `animatedValue`. Duplicate
  `start/endCompositionSession` removed from `update()` (executePaint owns
  the session). **Finding:** `advanceAnimations()` had no caller — §1.4's
  "separate timer path" is stale; the tween pump was orphaned. B3 re-homes
  it into the Tick slot (behavior-neutral: nothing starts a tween today).
  Per-view local phase flipping; the cross-tree ordered walk is Tier D.
  Builds clean incl. the full RootWidget app link; runtime verify is B5.)*
- [x] **B4** `rebuildContent()` out of `onPaint`; `Container` layout into
  the Layout phase. *(done 2026-05-29 — every primitive (Rectangle,
  RoundedRectangle, Ellipse, Path, Separator, Label, Icon, Image) gained a
  private `rebuildContent()` holding the old `lv2.clear()`→element→`setStyle`
  body; `onMount`/`setProps`/`setText`/`resize` call it (resize also
  re-runs it for the new geometry), and `onPaint(reason)` is now read-only
  (`update()` only). Removed the redundant rebuild+`update()` from `onMount`
  (executePaint(Initial) paints) — also kills a double-submit at init since
  init() runs onMount inside initWidgetRecurse's frame. Added `Image::onMount`
  (it had none). `Container`/`StackWidget` `onPaint` no longer call
  `layoutChildren()` — layout runs synchronously via `relayout()` at every
  model-change point; **the StackWidget suspicious-frame deferred-retry is
  preserved** because a valid rect arrives via `setRect → Widget::resize →
  relayout → layoutChildren` (not via paint). `layoutPending`/`needsLayout`
  are now write-only (retained for the DirtyBit::Layout integration). Builds
  clean incl. RootWidget app + Container-subclass test objects; runtime
  verify is B5.)*
- [x] **B5** assertion teeth; audit `invalidateNow()`; verify RootWidget.
  *(done 2026-05-29 — phase guards wired at the entry of each UIView phase
  method: `tickAnimations`→Tick, `resolveStyles`→Style (sole ComputedStyle
  writer), `arrange`→Layout (sole layout resolve/sort), `paint`→Paint (sole
  DisplayList appender). Realized via `FrameBuilder::assertPhase` (debug-only
  `assert`, no-op when no frame is in flight) rather than inside
  `Composition::DisplayList::append` — keeping the Composition layer free of
  a UI-layer dependency; the phase methods are the sole mutators so the
  guard is equivalent. `invalidateNow()` audit: **zero callers** (decl + def
  only); nothing to convert — it stays the `[[deprecated]]` escape hatch.
  RootWidget (Phase32 multi-UIView scene) runs with `NDEBUG` unset (asserts
  live): Metal init, window display, and render-pass command buffers all
  complete with **no phase-assertion abort** across a 7s run.)*

### Block 2 — Lifecycle Tier C cleanup (after Block 1)

Status snapshot 2026-05-31 — partial; details in §0.2.

- [ ] Delete `executePaint()`; `Widget::Impl::paintInProgress`,
  `hasPendingInvalidate`, `pendingPaintReason`. *(Still alive at
  `Widget.h:109` / `WidgetImpl.h:92-94`.)*
- [ ] Remove / retire `PaintOptions` (warmup, coalesce). *(Still alive at
  `Widget.h:57`.)*
- [x] Remove the no-op `startCompositionSession` /
  `endCompositionSession` shims from `View` / `UIView` /
  `CanvasView`. *(Done — render Phase 4.7.5 deleted them from `View`;
  the duplicate `UIView::update()` bracket went with B3;
  `CanvasView` deleted entirely via render plan §9.1.)*
- [ ] Remove the no-op `View::submitPaintFrame(int)` shim. *(Reduced
  to a no-op in render Phase 3.8 — `View.h:311`; full removal still
  pending a `Widget` call-site cleanup.)*
- [ ] Fold `WidgetTreeHost::paintDirty()` into the FrameBuilder walk.
  *(Still alive at `WidgetTreeHost.cpp:310`. `paintDirtyRecurse` is
  already marked vestigial by Phase 4.7.4.)*
- [ ] Fold / delete `WidgetTreeHost::invalidateWidgetRecurse()`.
  *(Still alive at `WidgetTreeHost.cpp:277`.)*

### ~~Block 3 — Style Tier 2~~ (retired 2026-06-01; folded into Block 4 as D6)

The 2026-06-01 Tier D scoping pass (§0.3) folds Style Tier 2 into
Tier D as **D6**. Style Tier 3 requires Tier 2; Tier D requires
Style Tier 3 to make `scheduler.transition(...)` real; so the prior
"Block 3 ships independently of Block 4" path stops making sense.
The bullets that used to live here are now D6.1–D6.5 in §5 Tier D.

### Block 4 — Tier D convergence: cleanup + animation + style cascade

Render Redesign **Tier 4** is the spine; Lifecycle **Tier D**, Style
**Tiers 2 + 3**, and Animation-Scheduler **Tiers A–E** all land inside it.
**`SceneNode` adoption was dropped 2026-05-31** (see §0.2). The 2026-06-01
pass adds a nine-sub-phase breakdown (§5 Tier D) and folds Style Tier 2
in here as **D6**.

Already-landed prerequisites:

- [x] Render 4.x — phase virtuals on `View` (4.7.0 / 4.7.2);
  `UIView::update()` retired as the monolith (split across the new
  phase methods, B3); per-view `Canvas` / `LayerTree` deleted (4.8);
  `VisualCommand` → `DrawOp` collapse in the backend tracked
  separately by the render plan.
- [x] Anim Tier A — `AnimationScheduler` lands at
  `wtk/src/UI/AnimationScheduler.{h,cpp}`; `FrameBuilder::beginFrame`
  calls `scheduler.tick()` once per outermost frame. (Render
  Phase 4.3 — verified in tree 2026-06-01.)
- [x] Anim Tier C scaffolding — `View::nodeId()` at `View.h:305`;
  `UIView::Impl::elementNodeIds_` at `UIViewImpl.h:167` and
  `animationTargets_` at `UIViewImpl.h:173` populated. Scalar
  routing through the scheduler is already in tree (D0 verified
  2026-06-03); path-node routing remains. D4 finishes it.
- [x] Anim Tier E partial — `ViewAnimator` / `LayerAnimator` /
  `LayerClip` / `ViewClip` deleted (codedb returns no symbol). D8
  finishes the residual header cleanup (`Composition/Animation.h`
  lines 21–29, 184–186 still carry stale forward decls + `friend`s;
  D0 verified 2026-06-03).
- [x] **D3 (Anim Tier B)** — `View::applyLayoutDelta` and
  `UIView::applyLayoutDelta` already route per-axis through
  `scheduler->tweenProperty<float>` (D0 verified 2026-06-03; see
  §5 Tier D `D3` for line cites).

Pending (one entry per Tier-D sub-phase from §5):

- [x] **D0** — doc-only reconciliation pass (DONE 2026-06-03; see
  §5 Tier D D0 reconciliation block).
- [x] **D1** — delete `executePaint`, `Widget::Impl` reentrancy state,
  warmup/coalesce `PaintOptions` fields; `invalidateNow()` becomes
  the lone `[[deprecated]]` sync hatch. (DONE 2026-06-03; build green.
  `Widget::executePaint` + `Widget::flushPendingPaint` deleted;
  `Widget::Impl::{paintInProgress, hasPendingInvalidate,
  pendingPaintReason, deferredReason}` removed;
  `PaintOptions::{autoWarmupOnInitialPaint, warmupFrameCount,
  coalesceInvalidates}` removed; `init()`, `invalidate()`,
  `invalidateNow()` inlined via `dirtyBitsForReason(reason)` helper;
  `WidgetTreeHost::invalidateWidgetRecurse` inlined transitionally
  pending D2 deletion. Stale cross-reference comments updated in
  `WidgetTreeHost.{h,cpp}`, `FrameBuilder.h`, `AppWindow.cpp`,
  `NativeWindow.h`.)
- [x] **D2** — delete `View::submitPaintFrame(int)`,
  `WidgetTreeHost::invalidateWidgetRecurse`, and the four no-op
  `WidgetTreeHost` recurse shims. `paintDirty()` stays (§0.3 #4).
  (DONE 2026-06-03; full-tree `ninja` builds 57/57 green.
  `View::submitPaintFrame` removed from `View.h`. Five
  `WidgetTreeHost` symbols deleted in one sweep —
  `observeWidgetLayerTreesRecurse`,
  `unobserveWidgetLayerTreesRecurse`, `invalidateWidgetRecurse`,
  `paintDirtyRecurse`, `beginResizeCoordinatorSessionRecurse` —
  along with the four observe/unobserve call sites in
  `~WidgetTreeHost`, `initWidgetTree`, and `setRoot`. The
  `compositor != nullptr` guard around `setRoot`'s observe bracket
  collapsed to nothing once the bracket was gone, so `setRoot` is
  now a plain assignment.)
- [x] **D4** — Anim Tier C: add path-node `animatePropertyAt`
  routing (`PathNodeX/Y` with `subIndex=nodeIndex`); delete dormant
  header symbols `advanceAnimations` / `PropertyAnimationState` /
  `PathNodeAnimationState`; trim unused `EffectAnimationKey*` tail
  constants (Shadow* nine kept — live read by `UIView.Update.cpp`).
  Scalar `startOrUpdateAnimation` → scheduler routing was already
  in tree. (DONE 2026-06-03; full-tree `ninja` builds 103/103
  green. New: `AnimationScheduler::tweenPropertyAt<T>` convenience
  helper; `UIView::PathNodeAxis` enum + `UIView::animatePathNode`
  public entry; `Impl::startOrUpdatePathNodeAnimation` +
  `Impl::animatedPathNodeValue` + sibling
  `pathNodeAnimationTargets_` map for the to-match short-circuit;
  anon-NS helpers `pathNodeAxisToProperty` + `packPathNodeKey`.
  Removed: `Impl::PropertyAnimationState`,
  `Impl::PathNodeAnimationState`, `Impl::advanceAnimations` decl +
  no-op body, `EffectAnimationKey{Gaussian,Directional}*` tail
  constants. D0 reconciliation correction noted: D0 grep missed
  `UIView.Update.cpp:244–266` reading `EffectAnimationKeyShadow*`,
  so the enum stays (Shadow* nine kept) instead of being fully
  deleted as the original D4 wording said.)
- [x] **D5** — split `computedStyles_` per-property into
  `Map<(NodeId, PropertyKey, subIndex), TypedValue>`; Paint reads
  via a uniform `resolved(n,k,fallback)` helper that chains
  scheduler → style table → UA default. (DONE 2026-06-03; full-tree
  `ninja` builds 81/81 green. **PropertyKey additions** in
  `AnimationScheduler.h`: `DropShadow`, `TextFont`, `TextLayout`,
  `TextLineLimit` (existing `BackgroundColor`, `FillBrush`,
  `TextColor` reused). **New machinery** in `UIViewImpl.h`:
  `StyleValue` variant (sibling of `AnimatedValue` — includes Font
  handle + `TextLayoutDescriptor` slots that have no place in the
  animation runtime); `StyleTable` class wrapping a
  `unordered_map<PropertyTableKey, StyleValue, PropertyTableKeyHash>`;
  `UIViewInternalDetail::VariantHas<T,V>` trait so `Impl::resolved<T>`
  can `if constexpr`-gate the scheduler probe at compile time when
  `T` isn't a member of `AnimatedValue` (non-animatable types like
  Font would otherwise trip the libc++ tuple `static_assert`).
  **Two reader helpers**: `Impl::resolved<T>(node, key, fallback)`
  (UA-default flavor) and `Impl::resolvedOptional<T>(node, key)`
  (used for cells that may legitimately be unset, e.g. `DropShadow`).
  **Cells written by `resolveStyles()`**: per UIView — `BackgroundColor`
  on `View::nodeId()`; per element — `FillBrush`, `DropShadow`,
  `TextFont`, `TextColor`, `TextLayout`, `TextLineLimit` on
  `ensureElementNodeId(tag)`. **Paint reader** (`UIView::paint()`)
  rewritten — every pre-D5 `computed.X.Y` field access now goes
  through `resolved<T>` / `resolvedOptional<T>`. **Deletions**:
  `Impl::resolvedViewStyle_` field, `Impl::computedStyles_` field,
  `Impl::computedStyleFor()` method, `UIViewInternal::ComputedStyle`
  aggregate. `Resolved{View,Text,Effect}Style` survive as transient
  builder types — they're the return shapes of the `resolve*Style()`
  helpers that `resolveStyles()` calls to split into cells.
  **Orphan writers** (`useBorder`, `borderColor`, `borderWidth` on
  ResolvedViewStyle; `gaussianBlur`, `directionalBlur` + the three
  `Transition` blocks on ResolvedEffectStyle) are no longer
  written into cells — pre-D5 they were aggregate fields with no
  Paint reader either, so this is behavior-preserving. D6 will
  wire them when the Style cascade lights up readers. Stale
  cross-reference comments updated in `View.h`, `View.Core.cpp`,
  `UIView.h`, `UIView.Animation.cpp`.)
- [x] **D6** — Style Tier 2: `StyleSheet` (with named keyframe
  declarations) + `Selector` + `StyleRule` + `AppWindow::styleSheets()`
  shared stack + `StyleResolver::resolve(node)` in Phase 2 + pseudo-
  class state bits + transition/keyframe binding *recording*.
  (DONE 2026-06-03; full-tree `ninja` builds 81/81 green.
  **Five sub-phases landed:**
  - **D6.1 vocabulary** — new `OmegaWTK::StyleSheets` namespace
    (the literal `Style` identifier was taken by the legacy inline
    aggregate at `omegaWTK/UI/UIView.h:19`). New public header
    `omegaWTK/UI/StyleSheet.h` with `Selector`, `StyleRule`,
    `StyleSheet`, `KeyframeAnimation`, `KeyframeAnimationProperty`,
    `TransitionSpec`, `PseudoClass` enum. `Selector::specificity()`
    uses CSS `id*100 + (class+pseudo)*10 + tag*1`.
    `StyleRule::beats()` mirrors the comparator at
    `wtk/src/UI/Layout.cpp:266`. Sheets are immutable-once-installed
    via a `Builder`. The `PropertyKey` enum, `StyleValue` variant,
    `PropertyTableKey`, `PropertyTableKeyHash`, and `AnimatedValue`
    variant were lifted to a new public header
    `omegaWTK/UI/StyleProperty.h` so sheet authoring doesn't reach
    into private scheduler internals; the private AnimationScheduler
    and UIView::Impl include the new public header.
  - **D6.2 AppWindow stack** — `addStyleSheet`, `removeStyleSheet`,
    `styleSheets()` accessors. Per-window storage in
    `AppWindow::Impl::styleSheets_`. Mutations call `requestFrame()`
    so the resolver picks up the new stack on the next frame; the
    cascade reads the stack fresh every Style phase.
  - **D6.3 StyleResolver integration** — chosen shape: **layered**
    (resolver runs FIRST in `UIView::resolveStyles()`, then the
    existing inline-`Style` writes overwrite on cell overlap so
    inline wins per §0.3 layering). New public header
    `omegaWTK/UI/StyleResolver.h` + private impl
    `wtk/src/UI/StyleResolver.cpp`. `StyleResolver::apply(view)`
    walks the AppWindow sheet stack, collects winners per
    `(NodeId, PropertyKey)` via `StyleRule::beats()`, then writes
    cells. Property-key scope (view vs element NodeId) routed via a
    `scopeOf(PropertyKey)` helper. `FrameBuilder` gained a `window()`
    accessor so the resolver can reach the stack from
    `AppWindow::activeFrameBuilder()`.
  - **D6.4 pseudo-class state** — `View::Impl::pseudoClassBits_`
    `uint8_t` field (bit layout matches `StyleSheets::PseudoClass`).
    Public `View::pseudoClassBits()` + `View::setPseudoClassBits(mask,
    on)`. The setter marks `DirtyBit::Style` only on actual change so
    mouse-stable frames don't dirty. `View::enable()` / `disable()`
    flip Disabled. `WidgetTreeHost::dispatchInputEvent` wires:
    Hover bit on hover-change (set on new target, clear on old);
    Pressed bit on LMouseDown/Up on the current target.
    `StyleResolver::selectorMatches` consults the view's bits via
    subset match — every bit required by the selector must be set
    on the view (`Hover|Pressed` requires BOTH). `:focused` is
    deferred to D7 (no focus tracker today).
  - **D6.5 transition / animation-binding recording** — new
    `ResolvedSheetBindings` struct on `UIView::Impl` holds
    `transitions: Vector<{NodeId, TransitionSpec}>` and
    `animationBindings: Vector<{NodeId, String}>`. The resolver
    clears + repopulates this every Style pass; for each winning
    rule, every node the rule won contributes the rule's
    transitions + animation name (if any) to the records. Nothing
    reads the records yet — D7.2 wires `scheduler.transition(...)`
    against transitions; D7.3 wires `scheduler.animateProperty(...)`
    against animationBindings.

  **File inventory:**
  - New public: `wtk/include/omegaWTK/UI/StyleProperty.h`,
    `omegaWTK/UI/StyleSheet.h`, `omegaWTK/UI/StyleResolver.h`.
  - New private: `wtk/src/UI/StyleSheet.cpp`, `wtk/src/UI/StyleResolver.cpp`.
  - Modified: `omegaWTK/UI/AppWindow.h` (stack API + fwd-decl),
    `omegaWTK/UI/View.h` (pseudo-class accessors),
    `omegaWTK/UI/UIView.h` (friend of StyleResolver),
    `wtk/src/UI/AppWindow.cpp` (impl bodies),
    `wtk/src/UI/AppWindowImpl.h` (storage + include),
    `wtk/src/UI/ViewImpl.h` (state field),
    `wtk/src/UI/View.Core.cpp` (pseudo-class impl + setEnabled wiring),
    `wtk/src/UI/UIViewImpl.h` (StyleSheet include +
    ResolvedSheetBindings struct + sheetBindings_ field),
    `wtk/src/UI/UIView.Style.cpp` (resolver call before inline writes),
    `wtk/src/UI/WidgetTreeHost.cpp` (hover/pressed wiring in
    dispatchInputEvent),
    `wtk/src/UI/FrameBuilder.h` (window() accessor),
    `wtk/src/UI/AnimationScheduler.h` (dedup with StyleProperty.h).

  **Verified:** full-tree build 81/81 green. The resolver runs every
  Style pass; with no sheets installed the cascade short-circuits
  to the existing inline-style behavior (zero behavior change for
  apps that don't use sheets). With sheets, inline `Style` still
  wins on cell overlap per the layered decision.

  **Risk paid:** Medium-High (the original §5 D6 estimate). The
  selector matcher is Tier-1 (single-compound, tag + pseudo only —
  id/class authoring deferred). Specificity follows CSS. The
  cascade traverses the whole stack per resolve and is O(rules ×
  elements); fine for current tree sizes.)
- [ ] **D7** — Style Tier 3 + Anim Tier D: `ThemeVars`; transitions
  wired to `scheduler.transition(...)` with retargeting; keyframe
  animations wired to `scheduler.animateProperty<T>(track, timing)`
  with bind/unbind lifecycle; `:state(name)`; UA default sheet.
- [ ] **D8** — final Anim Tier E + residual cleanup: strip dead anim
  forward decls, delete `flushPendingPaint` if D1 left it
  callerless, update `API.rst`. **Also retires the deprecated
  `Widget::onPaint(PaintReason)` virtual + every in-tree override**
  — added to D8 scope 2026-06-03 (see §5 D8 for the override list
  and the `BlueRectWidget` scheduler-pump caveat).

### Block 5 — WML front-end (last)

- [ ] Style **Tier 4** — WML parser / compiler (`omegaWTK::WML`):
  `<style>` → `StyleSheet` rules + per-node `Layout`; `.wtheme` →
  `ThemeVars`. Purely additive; the engine has no WML dependency.
