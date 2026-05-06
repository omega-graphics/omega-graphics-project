# UIView Render & Child-Layout Redesign Plan

**Status:** Proposal. Nothing below is implemented yet.
**Scope:** Gut the current `UIView` rendering path and the `View` child
rendering/layout plumbing. Replace with a smaller, more explicit model
informed by Chromium's `views::View` / `cc::LayerTreeHost` and by
retained-mode UI toolkits inside game engines (Unreal Slate, Unity
UI Toolkit).
**Non-goals:** Changing the public `Widget`, `AppWindow`, or the
compositor surface. Changing `StyleSheet` authoring syntax. Rewriting
the layout solver itself (`Layout.h` / `resolveClampedRect` stay).

**Compositor backend assumed by this plan:** the
Direct-To-Drawable / SDF backend
(see `Direct-To-Drawable-And-SDF-Plan.md`) is in for the simple
primitives — Rect / RoundedRect / Ellipse / Shadow render as
6-vertex SDF quads with their border consolidated into the same
draw call (no separate stroke path). VectorPath / Bitmap still
go through the GTE triangulator. The DrawOp design in §3.2
mirrors that consolidated reality, not the prior fill-then-frame
pair.

---

## 1. Why the current path has to go

The problems are not individual bugs. They are a design whose layers
have grown into each other. Concretely, reading the code today:

### 1.1 Every `View` owns its own `LayerTree`

`View::Impl::ownLayerTree` is a `SharedHandle<Composition::LayerTree>`
created in the `View` constructor
(`ViewImpl.h:111`, `View.Core.cpp:67`). The compositor then observes
that tree (`View.Core.cpp:249`). For a window with N views there are
N LayerTrees, each registered with the compositor under its own sync
lane (`View.Core.cpp:263`, `setSyncLaneRecurse`). Nothing in the code
actually benefits from these being independent — z-order, transforms,
and damage tracking all have to be reconstructed by walking the view
tree anyway. This is the root cause of "per-command rendering" called
out in `Render-Execution-Efficiency-Plan.md` §3.

### 1.2 Every `UIView` owns its own `Canvas`, bound to its own root layer

`UIView::update()` does
`impl_->rootCanvas = makeCanvas(getLayerTree()->getRootLayer())`
(`UIView.Update.cpp:122`). Every UIView paints into its own private
canvas, then calls `sendFrame()`. There is no traversal of children
in this path — a parent `UIView` that has child `UIView`s does not
draw them. Each child is updated independently and emits its own frame
into its own LayerTree. Child layering is emergent, not designed.

### 1.3 `update()` is a 270-line monolith that mixes layout, style, animation, and paint

Reading `UIView.Update.cpp:120-391`:

- resolves layout rects from `UIViewLayoutV2` + stylesheet
- stable-sorts by z-index
- opens a composition session
- resolves view-level background style
- for each element: resolves brush, resolves effects, resolves text
  style, *mutates the `Path` object in place* (`path.setStroke`,
  `path.close`, `path.setPathBrush` at `UIView.Update.cpp:352-356`),
  re-clamps rects inside each shape case, emits the shadow draw, emits
  the fill draw
- flushes
- closes the session
- clears five partially-overlapping dirty flags (`layoutDirty`,
  `styleDirty`, `styleDirtyGlobal`, `styleChangeRequiresCoherentFrame`,
  `firstFrameCoherentSubmit`)

None of the dirty flags are consulted to *skip* work — they exist,
they flip, they are cleared. The "skip" branch only fires when
`v2Elements.empty()`.

(Note: the per-element border-as-separate-stroked-path emission that
this monolith used to do is already retired by the SDF spine
(`Direct-To-Drawable-And-SDF-Plan` §6.5). The Border now rides on the
shape's `VisualCommand` and the compositor backend renders fill +
border as one draw call. The redesign here is about restructuring
*who emits commands and when*, not about further changing what a
single command looks like.)

### 1.4 `localBoundsFromView` caches in a file-scope `unordered_map<UIView*, …>`

`UIView.Update.cpp:61`:

```cpp
static std::unordered_map<UIView *, StableBoundsState> stableBoundsByView {};
```

Keyed by raw pointer, never erased when a `UIView` is destroyed, shared
across all instances, not thread-safe. This is a use-after-free waiting
to happen and a subtle source of state leakage between unrelated views.

### 1.5 Child layout is a union of three half-built systems

- `ViewResizeCoordinator` (per-view, `View.h:60`): holds a
  `ChildResizeSpec` per child, can resolve a parent-content rect into
  child rects. Nothing in `UIView::update()` consults it.
- `UIViewLayoutV2` (`UIView.h:253`): flat list of element specs with
  `LayoutStyle` — used by `update()` to lay out the *elements inside a
  single view*, not the child *views*.
- `Widget::Layout` (`Widget.Layout.cpp`, called from `WidgetTreeHost`):
  the actual driver used when a widget tree is attached to a window.

There is no single answer to "who lays out a UIView's children?".
Depending on the call site the answer is "nobody", "the resize
coordinator if you remembered to register", or "the widget layout
pass, but only for widgets".

### 1.6 Sync-lane fragmentation

`setSyncLaneRecurse` (`View.Core.cpp:258`) hands every subview a fresh
sync lane ID from a global atomic. The compositor then treats each
view as an independent flow-control domain. This directly undermines
the batched-compositing goal: related views on the same window cannot
coalesce into one packet because they don't share a lane.

### 1.7 Animation state lives on the rendering path

`UIView::Impl` carries `elementAnimations`, `pathNodeAnimations`,
`animationLayerAnimators`, `animationViewAnimator`, and
`lastResolvedElementColor`. `UIView.Animation.cpp` is 537 lines that
read and write this state from inside the paint/update loop. The
`Animation-API-Simplification-Plan.md` already addresses the animation
*API*; this plan addresses the fact that animation data is welded into
the paint path.

### 1.8 Summary

The current path fails the "can a new contributor read `update()` in
one sitting?" test. It fails the "does parent-paint-descends-to-child?"
test. It fails the "is there one dirty bit, not five?" test. It fails
the "is global mutable state absent from the render loop?" test.

---

## 2. What Chromium and game engines do differently

Three reference architectures, each solving a piece of the problem we
have. We should steal from all three.

### 2.1 Chromium — `views::View` + `cc::Layer`

The insight is that the **widget tree and the GPU layer tree are not
the same tree.** `views::View` is retained and hierarchical. Every
`views::View` has children, bounds, and a `OnPaint(gfx::Canvas*)`
method. But `cc::Layer`s (the actual units the compositor rasterizes
and submits) are explicit opt-ins — most Views paint into their
ancestor's backing layer. A `View` asks for its own `cc::Layer` only
when it needs a transform, clip, opacity animation, or scrolling.

- Measure / arrange is a pass owned by the **parent's** `LayoutManager`
  (`BoxLayout`, `FillLayout`, `GridLayout`). The child has no opinion
  on how it is positioned.
- Paint is a tree walk producing a `cc::DisplayItemList` — an ordered,
  flat sequence of draw ops with implicit transform stack. The walk is
  pruned by the dirty region.
- `SchedulePaint(rect)` is the *only* way to request a redraw. It
  unions into a dirty region on the root view and triggers one paint
  per frame.
- The compositor thread consumes the display list as an immutable
  snapshot.

**What we take:** decoupled widget tree vs. layer tree; layerization
as an explicit choice; one display list per frame per window; one
dirty-region gate at the root; layout is a job owned by the parent.

### 2.2 Unreal Slate — `SWidget` + `FSlateWindowElementList`

Slate has the cleanest three-phase model in any C++ UI toolkit:

1. **Tick** — walk the tree, advance animations, update model data.
2. **Prepass + Arrange** — bottom-up `ComputeDesiredSize()` followed by
   top-down `ArrangeChildren()` producing `FArrangedWidget` (geometry
   snapshots) per widget per frame.
3. **Paint** — recursive `OnPaint(args, geometry, cullingRect, outDrawElements, layerId, widgetStyle, parentEnabled)`.
   Each widget appends to a single flat `FSlateWindowElementList`
   shared across the whole window. `layerId` is an integer the parent
   passes in, and the child returns the max layer it used so the
   parent can draw above it.

The important property: **paint is pure.** It reads geometry from the
arrange pass, reads model state, and writes only to the window-wide
draw list. No side effects. No mutation of the widget's own fields.
No animation ticking. The renderer then batches the flat list by
shader/texture before submitting.

**What we take:** a single shared display list per window; the
measure → arrange → paint separation; `layerId` as an integer z-index
passed *down* the walk; paint as a pure function of
(model, geometry, style).

### 2.3 Unity UI Toolkit — `VisualElement` + mesh generation context

Unity UI Toolkit keeps a `VisualElement` tree with a hierarchical
dirty flag set: `Layout`, `Style`, `Transform`, `Repaint`,
`Hierarchy`. Each frame the panel runs only the passes whose dirty
bits are set. Paint is a `generateVisualContent` callback that appends
triangles into a shared vertex buffer; the renderer batches draws
across the whole tree. The tree has one Yoga flexbox solver instance
that processes the entire dirty subtree in one shot.

**What we take:** one dirty-flag set, not five. Named phases
(Measure/Arrange/Paint/Composite). Render output is "append to a
pooled buffer", not "construct a Canvas and call sendFrame".

---

## 3. Proposed architecture

### 3.1 Overview

```
Window (AppWindow)
  └── RenderRoot (owns the single CompositionRenderTarget)
        ├── SceneNode tree (the retained widget/view scene)
        │     — bounds, transform, style handle, children
        │     — NO per-node LayerTree, NO per-node Canvas
        └── FrameBuilder
              — runs Measure → Arrange → Paint each frame
              — outputs ONE DisplayList<DrawOp>
              — hands the DisplayList to the compositor
```

A frame is always produced at the window level, never at the view
level. `UIView` becomes a node type in the scene tree — a kind of
widget that knows how to emit draw ops into the frame-global display
list — rather than a thing that owns rendering infrastructure.

### 3.2 The five collaborators (and nothing else)

1. **`SceneNode`** — replaces most of `View::Impl`. Retained tree node.
   Holds parent-relative bounds, a `Transform2D`, a `Style` handle, a
   `LayoutManager*` (optional, used to arrange *its own* children),
   and a `DirtyBits` field. Has children. Has a virtual `paint()`.
   Does **not** own a LayerTree, Canvas, sync lane, or animation state.

2. **`LayoutManager`** — replaces the `ViewResizeCoordinator` / per-view
   ad-hoc layout. A parent-owned strategy object. Has two methods:
   `measure(node, availableSize) -> desiredSize` and
   `arrange(node, finalRect)`. Default implementations: `FillLayout`,
   `StackLayout` (H/V), `AbsoluteLayout`, `FlexLayout`. Built on top
   of the existing `resolveClampedRect` / `LayoutStyle`.

3. **`DisplayList`** — a frame-scoped flat vector of `DrawOp` structs
   plus a transform stack. One instance per frame per window.
   `DrawOp` is a tagged union shaped to mirror the post-SDF
   compositor backend (one draw call per primitive, fill + border
   consolidated, vector paths still tessellated):

     - `Rect`, `RoundedRect`, `Ellipse` — each carries a fill brush
       and an optional `Border { color, width }`. The compositor
       backend renders these via the SDF pipeline; the DisplayList
       does **not** emit a separate stroke op for the border, in
       line with `Direct-To-Drawable-And-SDF-Plan.md` §6.5.
     - `Shadow` — fill brush plus blur amount; soft Gaussian-ish
       falloff handled by the SDF pipeline.
     - `Path` — arbitrary `GVectorPath2D` with stroke / fill / both
       attachments. Goes through the triangulator (lazy context).
     - `Bitmap` — texture handle + dest rect (+ tint / source-rect /
       nine-slice once `Direct-To-Drawable-And-SDF-Plan` §6.6
       lands).
     - `Text` — bitmap-text fallback today, MSDF text run after
       `Direct-To-Drawable-And-SDF-Plan` §6.7.
     - State ops: `PushTransform` / `PopTransform`, `PushClip` /
       `PopClip`, `PushOpacity` / `PopOpacity`, `PushEffect`
       (per-layer blur scratch) / `PopEffect`.

   No per-op resource creation. Paint appends. Composition reads.
   The DisplayList is the implementation-detail handoff to the
   compositor backend's `BackendRenderTargetContext::renderToTarget`,
   so its op set should track that surface — when the SDF plan
   adds a new primitive (e.g. MSDF text run), the DisplayList gains
   the matching op.

   **`DrawOp` retires `VisualCommand`.** The post-SDF `VisualCommand`
   shape — one command per primitive, fill + border consolidated
   into the same record, soft shadow as its own SDF command — is
   exactly what `DrawOp` is. They are not two parallel formats with
   a translation step between them: `DrawOp` *is* the new compositor
   op type and `VisualCommand` is **deleted**. The two prior
   producers of `VisualCommand` (per-view `Canvas` and `SVGView`'s
   internal canvas) are both removed by Tier 3 / Tier 4 of this
   plan, leaving `VisualCommand` with no upstream. The compositor
   backend's `renderToTarget()` switch is rewritten to dispatch on
   `DrawOp` directly. Backend rasterization code (the SDF pipeline,
   the triangulator path, the bitmap blit, the text run) is
   untouched — only the input type changes. `CanvasFrame` (the
   per-view recorded list of `VisualCommand`s) is similarly retired
   in favor of the per-window `DisplayList`.

4. **`FrameBuilder`** — replaces `UIView::update()` and the
   per-view composition session dance. One per window. Runs the three
   passes:
   - **Measure** (bottom-up, only on `DirtyBits::Layout` subtrees):
     each `LayoutManager` computes `desiredSize` for its children.
   - **Arrange** (top-down): each `LayoutManager` assigns a final
     rect per child.
   - **Paint** (top-down, only on `DirtyBits::Paint | Layout` subtrees):
     each `SceneNode::paint(PaintContext&)` appends to the frame's
     `DisplayList`. Parent paints its own background/border, then
     recurses to children, then optionally emits an overlay. No node
     opens or closes a composition session — `FrameBuilder` opens one
     at the start, closes one at the end.

5. **`PaintContext`** — the argument threaded through the paint walk.
   Carries: a reference to the window's `DisplayList`, the current
   transform, the current clip, the current opacity, the current
   effect stack, and the *resolved* style for the node (already
   computed). `PaintContext` is a scratch object; nothing paints
   reaches back into it. This is directly the `FSlateWindowElementList`
   pattern from Slate.

### 3.3 Where the old concepts go

| Old                                         | New                                                |
|---------------------------------------------|----------------------------------------------------|
| `View::Impl::ownLayerTree`                  | **Deleted.** One LayerTree per window, not per view. |
| `View::Impl::proxy` (CompositorClientProxy) | **Deleted** from `View`. Lives on the window.     |
| `View::startCompositionSession/end`         | **Deleted.** `FrameBuilder` owns session lifetime. |
| `View::setFrontendRecurse`                  | **Deleted.** One frontend pointer, on the window. |
| `View::setSyncLaneRecurse` + global atomic  | **Deleted.** One sync lane per window.            |
| `UIView::Impl::rootCanvas`                  | **Deleted.** There is no per-view canvas.         |
| `UIView::Impl` animation state              | Moved to `AnimationScheduler` (see §3.6).         |
| `UIView::Impl` five dirty flags             | Collapsed into `DirtyBits` (4 bits).              |
| `UIView::update()` monolith                 | Split into `UIViewNode::measure/arrange/paint`.   |
| `UIViewLayout` (legacy)                     | **Deleted** after one migration pass.             |
| `UIViewLayoutV2` + element specs            | Kept as the *authoring* surface; feeds into the scene tree at build time, not at paint time. |
| `ViewResizeCoordinator`                     | **Deleted.** Superseded by `LayoutManager`.       |
| `localBoundsFromView` static map            | **Deleted.** Bounds live on the node.             |
| `View::computeWindowOffset` parent walk     | **Deleted.** `FrameBuilder` threads a transform.  |
| `Canvas` as a per-view paint device          | Kept **only** as the low-level API the compositor backend exposes; `DisplayList` replays into one Canvas per frame at flush time, not one per view. |
| `VisualCommand` (per-element compositor op) | **Deleted.** `DrawOp` replaces it 1:1. Backend `renderToTarget()` switch dispatches on `DrawOp`. |
| `CanvasFrame` (per-view list of `VisualCommand`s) | **Deleted.** `DisplayList` (one per window per frame) replaces it. |

The scene-tree / display-list separation is the Chromium move. The
pure-paint, shared-display-list move is the Slate move. The collapsed
dirty flags are the Unity move.

### 3.4 `DirtyBits`

Four bits, not five flags:

```
enum DirtyBit : uint8_t {
    Style   = 1 << 0,  // resolved style cache is stale
    Layout  = 1 << 1,  // desired/final rect is stale
    Content = 1 << 2,  // shape/text/element set changed
    Paint   = 1 << 3,  // needs re-walking in paint
};
```

Invalidation rules:

- Setting a stylesheet: `Style | Paint`; layout only if the style
  touches a layout property.
- Changing a `UIViewLayoutV2` element: `Content | Layout | Paint`.
- `resize()`: `Layout | Paint` on self; `Paint` on ancestors whose
  bounds depend on child size.
- Animation tick: `Paint` only. Animation must not touch `Style` or
  `Layout` bits.

Each bit *propagates upward to the root* on set. `FrameBuilder` reads
the root bits to decide whether to run each pass; the pass itself
prunes at every node whose bit is clear.

### 3.5 Layout: measure and arrange

The existing `LayoutStyle` + `resolveClampedRect` work stays. What
changes is *who calls them and when*. Today, resolution happens
inside `UIView::update()` as a side effect of paint. In the new
model:

```cpp
class LayoutManager {
public:
    virtual Size measure(SceneNode& node, Size availableSize) = 0;
    virtual void arrange(SceneNode& node, Rect finalRectLocal) = 0;
    virtual ~LayoutManager() = default;
};
```

- `FillLayout` — child fills parent content rect. Trivial.
- `StackLayout` — H or V. Uses `LayoutStyle` weights.
- `AbsoluteLayout` — child uses its own `LayoutStyle` rect, no reflow.
  This is the back-compat path for `UIViewLayoutV2`.
- `FlexLayout` — later. Wraps the current `resolveClampedRect` + a
  simple main-axis distributor.

Parents own their children's layout via a `LayoutManager*` field.
Children never position themselves. This is directly the Chromium
Views / Slate model, and removes the "who lays out my children?"
ambiguity from §1.5.

### 3.6 Animation lives outside paint

A separate `AnimationScheduler` (one per window, driven by the frame
pacer) ticks active animation tracks and writes resolved values into
a side table keyed by `(NodeId, PropertyKey)`. Paint reads the side
table when building draw ops — it never writes.

This lets `UIView.Animation.cpp` shrink dramatically and makes the
paint walk a pure function again. `Animation-API-Simplification-Plan.md`
is a prerequisite; this plan assumes that simplification lands first
or alongside.

### 3.7 The frame loop

```
FrameBuilder::buildFrame():
    if root->dirty & Style:    resolveStylesSubtree(root)
    if root->dirty & Layout:   measureSubtree(root, windowSize)
                               arrangeSubtree(root, windowRect)
    if root->dirty & Paint:    displayList.clear()
                               paintSubtree(root, PaintContext{displayList, identityXform})
                               compositor.submitDisplayList(displayList)
    root->dirty = 0
```

One entry point. Four conditional passes. One submission. The
compositor scheduler work from `Render-Execution-Efficiency-Plan.md`
§2 consumes `DisplayList` directly — no more N staggered
per-widget packets.

### 3.8 What `UIView` becomes

```cpp
class UIViewNode : public SceneNode {
    UIViewLayoutV2 content_;
    StyleSheetPtr  style_;
    ResolvedStyleCache cache_;
public:
    Size measure(Size avail) override;        // delegates to children + content
    void arrange(Rect final) override;        // positions child elements
    void paint(PaintContext& pc) override;    // appends to pc.displayList
    void setLayout(UIViewLayoutV2 l);         // sets DirtyBits::Content|Layout|Paint
    void setStyleSheet(StyleSheetPtr s);      // sets DirtyBits::Style|Paint
};
```

`paint()` is the entire job, and it contains no `Canvas`, no session
management, no dirty-flag juggling, no sort, no clamp-to-parent logic
(parent already arranged the rect), no animation bookkeeping. It reads
`cache_`, it reads the already-arranged element rects, it appends
`DrawOp`s to `pc.displayList`. Estimated size: **~80 lines**, down
from ~390 in `UIView.Update.cpp`.

A bordered shape — the most common UIView element — now appends
exactly one op (`Rect` / `RoundedRect` / `Ellipse` with the border
field set), not the prior fill-op + frame-path-op pair. A drop
shadow on the same shape appends a `Shadow` op before the shape
op. The previous walk emitted 2–3 visual commands per styled
element; the new one emits 1–2.

### 3.9 Public API delta

- `UIView(const Rect&, ViewPtr parent, UIViewTag)` — unchanged.
- `UIView::setLayout(UIViewLayoutV2)` — unchanged.
- `UIView::setStyleSheet` — unchanged.
- `UIView::update()` — **removed**. Replaced by `window.requestFrame()`
  or simply by the next pacer tick. The explicit update call was
  leaking frame-building concerns into user code. If an app wants to
  force a frame, it calls `AppWindow::invalidate()`.
- `View::startCompositionSession / endCompositionSession` — **removed**.
- `View::makeLayer / makeCanvas` — **removed** from the public `View`
  surface. The low-level `Canvas` still exists but only inside the
  compositor backend as a draw-op replay target.

Breaking? Yes. The only in-tree caller of `update()` is `UIView` tests
and widget-tree hosts. Migration is a one-line replacement with
`invalidate()` or nothing at all.

---

## 4. Migration plan

Four tiers, each independently shippable and each reducing surface
area before the next.

### Tier 1 — delete the per-view sync lane fragmentation

- Remove `View::setSyncLaneRecurse` and the global atomic.
- One sync lane per window, propagated from `AppWindow` once.
- `View::Impl::proxy` keeps the frontend pointer but shares the lane.
- No behavior change expected; this lifts the block on any future
  batched-packet work.

Risk: low. Files touched: `View.Core.cpp`, `AppWindow.cpp`,
`Compositor.cpp` lane admission.

### Tier 2 — introduce `DisplayList` and replay, keep per-view Canvas

- Add `DisplayList` + `DrawOp` types under `Composition/`.
- Add `DisplayListReplay` — takes a `Canvas` and replays ops into it.
- `UIView::update()` changes internally: builds a local `DisplayList`
  instead of calling canvas directly, then replays into `rootCanvas`
  at the end. Same output, same bugs, but paint is now a pure
  function of the model.
- Delete `localBoundsFromView` static map. Bounds come from
  `getRect()` + `LayerTree` rect directly, computed per call with no
  pointer-keyed cache. The stability logic was compensating for
  resize races that the single-surface refactor
  (`Render-Execution-Efficiency-Plan.md` NV-1..NV-3) already removed.

Risk: medium. Files touched: `UIView.Update.cpp`, `UIViewImpl.h`,
new `Composition/DisplayList.{h,cpp}`.

### Tier 3 — collapse per-view LayerTree into one per window

- `View::Impl::ownLayerTree` removed.
- `AppWindow::Impl` owns one `LayerTree`.
- `View::makeLayer` / `makeCanvas` become no-ops or route to the
  window's layer tree. Deprecate in headers.
- `FrameBuilder` appears for the first time, owned by `AppWindow`.
  It walks the `View` tree (still existing) and replays each view's
  `DisplayList` into one shared `Canvas` on the window's root layer.
  Composition sessions are opened/closed **once** per frame, at the
  window level. `UIView::update()` no longer opens a session.
- `View::computeWindowOffset` stays for now but moves into
  `FrameBuilder` as a transform accumulator; the public method can
  stay as a thin wrapper until Tier 4 removes it.

Risk: high. This is the move that actually fixes the rendering path.
It must land behind a feature flag or on a branch with thorough
resize/scroll/clip testing.

### Tier 4 — introduce `SceneNode` + `LayoutManager`, retire `UIView::update`

- `UIView` becomes `UIViewNode : SceneNode`.
- `View::Impl::subviews` becomes `SceneNode::children_`.
- `ViewResizeCoordinator` deleted; replaced by
  `SceneNode::layoutManager_`.
- `UIView::update()` removed; frames are produced by the window pacer
  reading `DirtyBits` at the root.
- Animation state migrated out of `UIView::Impl` into
  `AnimationScheduler` (prereq: animation simplification plan).

Risk: high; this is the API break. Ship after Tier 3 has been in main
for at least two weeks of real use.

---

## 5. What gets deleted

At the end of Tier 4:

- `UIView.Update.cpp` (391 lines) → `UIViewNode::paint` (~80 lines)
- `UIView.Core.cpp` session/dirty plumbing (~200 lines) → gone
- `UIView.Animation.cpp` (537 lines) → ~150 lines against
  `AnimationScheduler`
- `View.Core.cpp` sync-lane + frontend recurse + session methods
  (~60 lines) → gone
- `View.ResizeCoordinator.cpp` → gone
- `ViewResizeCoordinator` class → gone
- `localBoundsFromView` + static map → gone
- `View::Impl::ownLayerTree`, per-view proxy, per-view canvas → gone
- Five overlapping dirty flags → one `DirtyBits`

**Estimated deletion: ~1200 LOC. Estimated addition: ~600 LOC.**
The net reduction is real because most of the current code is
coordinating between systems that wouldn't exist in the new model.

---

## 6. Open questions

These are the places where the developer's judgment about *this*
codebase should override anything in §3:

1. **Should `SceneNode` be the same type as `View`, or a sibling?**
   Chromium merged them (`views::View` is the scene node). Slate
   kept them separate (`SWidget` vs. `FArrangedWidget`). Merging is
   simpler. Keeping them separate lets us ship Tier 3 without a
   public API break. Recommendation: merge, and accept the break in
   Tier 4 — but only if the existing `Widget` subclass hierarchy
   can rebase onto `SceneNode` without churn. If it can't, we keep
   `View` as the public handle and `SceneNode` as the internal
   retained node, like Blink's `LayoutObject` / `Node` split.

   ANSWER: View is a scene node.

2. **Where does `Widget` fit?** `Widget` today sits above `View` and
   has its own paint lifecycle (`Widget.Paint.cpp`). The cleanest
   answer is that `Widget::executePaint` becomes the thing that asks
   the window to invalidate its subtree, and `Widget` no longer has
   its own paint method. But this touches every widget subclass, so
   it should be its own follow-up plan, not rolled into this one.

   ANSWER: Widget's are effecetively a light wrapper around View and can be removed.

3. **Layerization opt-in.** When does a `SceneNode` get its own
   composition layer (for scrolling, opacity animation, transforms)?
   Chromium's answer is "when the style requests it or a scroll
   container requires it". Unreal's answer is "almost never, because
   the flat display list is cheap to rebuild". For OmegaWTK the
   pragmatic answer is: **start with zero layerization**; add a
   `SceneNode::forceLayer()` escape hatch; let `ScrollView` be the
   first and possibly only consumer.

   ANSWER: Agreed

4. **Dirty-region culling.** §3.4 propagates dirty bits up but the
   proposal does not yet specify region-based culling (the union of
   dirty rects at the window). For slice-A this is fine — paint
   everything dirty or nothing. Region culling is a Tier-5 follow-up
   once the flat display list exists.

5. **Threading.** Chromium runs layout on one thread and compositing
   on another. OmegaWTK is currently single-threaded for paint. This
   plan keeps it single-threaded. The `DisplayList` snapshot is the
   hand-off point where a future compositor thread *could* take over,
   but nothing here forces that decision.

   

---

## 7. Relationship to existing plans

- **`Render-Execution-Efficiency-Plan.md`** — this plan fills the §3
  "per-command rendering" slot and partially the §2 "scheduler
  architecture" slot. Tier 1 removes the lane fragmentation that
  blocks §2. Tier 3 produces the one-display-list-per-frame snapshot
  the batched scheduler wants. The Tier 2 Phase 5 (SDF) work that
  lives in `Direct-To-Drawable-And-SDF-Plan.md` already shipped for
  simple primitives — the DisplayList ops in §3.2 are designed to
  pass straight through to that backend.
- **`Direct-To-Drawable-And-SDF-Plan.md`** — owns the compositor
  backend this plan's `DisplayList` replays into. Phase 6.1–6.3,
  6.5, and 6.8 of that plan are in: the simple-primitive draw ops
  this plan defines (`Rect` / `RoundedRect` / `Ellipse` / `Shadow`
  with optional border) execute as one SDF draw call each. As that
  plan adds new primitives (vector-path edge AA in 6.4, bitmap
  improvements in 6.6, MSDF text in 6.7), this plan's DrawOp set
  picks up the new types.
- **`Composition-Extension-Plan.md`** — orthogonal. The new
  `DrawOp` types are a superset of what `Canvas` exposes and will
  naturally pick up the Brush/Gradient improvements landing there.
- **`Animation-API-Simplification-Plan.md`** — prerequisite for
  Tier 4. Without it, `AnimationScheduler` has nowhere clean to sit.
- **`NativeViewHost-Adoption-Plan.md`** — unaffected. Native view
  hosts become leaf `SceneNode`s that emit a single
  `DrawOp::NativeHost` op, which the compositor turns into whatever
  the platform backend needs.
- **`Batched-Compositing-Pass-Plan.md`** — this plan is what makes
  batching possible. The batched pass consumes one display list
  per window per frame; today it cannot because there is no such
  list to consume.

---

## 8. Honest uncertainty

I have not read every caller of `View::makeLayer`, `View::makeCanvas`,
or `View::startCompositionSession`. If a widget subclass outside
`src/UI/` is calling them in a way that assumes per-view isolation,
Tier 3 will break it. Before starting Tier 3, a grep sweep of those
four symbols across `src/Widgets/` and any client code is required,
and the result should be folded back into §4 as a pre-flight
checklist.

I am also assuming that `WidgetTreeHost` and `Widget::executePaint`
can be retrofitted to drive a window-level `FrameBuilder` without
rewriting every widget. If that assumption is wrong, Tier 4 has to
become a larger plan that includes the widget paint lifecycle. That
is worth confirming before Tier 3 lands.

I am assuming the DisplayList replay maps cleanly onto the SDF
backend's `BackendRenderTargetContext::renderToTarget` switch — i.e.
that the rewrite of that switch to dispatch on `DrawOp` directly
(rather than on the now-retired `VisualCommand`) is a mechanical
rename of the case labels and a touch-up of the field accesses,
with no GPU-side change. The shape match is given by construction
(`DrawOp` was designed against the post-SDF `VisualCommand` shape),
but the mechanical rewrite still needs the same grep sweep for
out-of-tree callers of `Canvas::drawRect` / `drawRoundedRect` /
`drawEllipse` that relied on the prior fill-then-stroked-path
behavior. SVGView was the last in-tree caller that did, and was
already migrated alongside the SDF spine. Any out-of-tree consumer
is a pre-flight checklist item for Tier 3.

---

## 9. Non-`UIView` `View` subclasses

The plan above describes how `UIView` slots into the FrameBuilder
paint path. It does **not** address the other concrete `View`
subclasses currently in the tree. Each of them breaks one of the new
model's invariants in a different way, and each needs an explicit
migration story before Tier 3 lands. This section defines that story.

**Important framing.** §6 Q1 answers that **`View` *is* the
`SceneNode`** — there is no separate node type, and there is no
`CanvasViewNode` / `SVGViewNode` / `ScrollViewNode` shadow class.
Every `View` subclass already participates in the scene tree as
itself. Migration is therefore about *what each subclass's `paint()`
does* and *what state it stops owning*, not about wrapping it in a
new class.

The subclasses are:

| Class | Header | Role |
|---|---|---|
| `CanvasView` | [CanvasView.h](../include/omegaWTK/UI/CanvasView.h) | **Deleted** — see §9.1. Imperative draws are folded into `UIView`. |
| `SVGView` | [SVGView.h](../include/omegaWTK/UI/SVGView.h) | Parses SVG, builds an internal `SVGDrawOpList`, paints it |
| `ScrollView` | [ScrollView.h](../include/omegaWTK/UI/ScrollView.h) | Clips a content child, owns scroll bar overlay layers |
| `VideoView` | [VideoView.h](../include/omegaWTK/UI/VideoView.h) | **Migrates out of the View tree** — see §9.4 and [NativeViewHost-Adoption-Plan.md](NativeViewHost-Adoption-Plan.md) |

**`View::paint(PaintContext&)` is the universal contract.** Because
`View` *is* the `SceneNode`, every subclass overrides one virtual:

```cpp
class View : public Native::NativeEventEmitter {
public:
    // Default: paint nothing. Subclasses override.
    virtual void paint(PaintContext & pc) {}
    // ...
};
```

UIView, SVGView, ScrollView, and NativeViewHost each override
`paint()` with the contract specified below. There is no separate
node interface and no mixin.

### 9.1 `CanvasView` — deleted

**Current contract.** Owns a `rootCanvas_` on the view's root layer.
Exposes `clear`, `drawRect`, `drawRoundedRect`, `drawImage`, `drawText`.
Overrides `submitPaintFrame(int)`. Widgets that do imperative drawing
inherit from CanvasView and call these methods from their `onPaint`.

**Resolution.** `CanvasView` is **deleted**. Once UIView's
`UIViewLayoutV2` covers the rect/rounded-rect/ellipse/path/text/image
authoring surface declaratively, the only remaining reason for
CanvasView to exist is "I want to draw imperatively." That use case
collapses into UIView with a single declaratively-authored element.
Custom plotting, debug overlays, and ad-hoc visualization can either
build a `UIViewLayoutV2` programmatically or — in the rare case where
imperative draws into the active frame are genuinely needed — use a
`View` subclass that overrides `paint(PaintContext&)` directly and
appends `DrawOp`s. There is no public `Canvas`-style API on the
View base.

**Migration of existing callers.** The CanvasView subclasses in tree
(if any) move to UIView with their imperative draws translated to
`UIElementLayoutSpec` element declarations. The
`submitPaintFrame(int)` override is removed; the
`rootCanvas()` accessor is removed. The header file is deleted.

**Tier alignment.** Tier 2 stops emitting frames through CanvasView's
canvas (the imperative methods become deprecated stubs that route to
a temporary `UIViewLayoutV2` if any caller still needs them). Tier 3
deletes the class. No callers in `wtk/src/Widgets/` or test code
should remain by the end of Tier 3 — confirmed by the §8 grep sweep.

**Tier alignment.** Tier 2 here, alongside the DisplayList introduction.
CanvasView's `submitPaintFrame` override is removed in Tier 3 when
sessions move to the window level. The imperative API survives
unchanged for callers.

### 9.2 `SVGView` — internal display list, already aligned

**Current contract.** Parses an SVG document into an internal
`SVGDrawOpList` ([SVGView.h:38](../include/omegaWTK/UI/SVGView.h)),
owns its own `svgCanvas`, rebuilds on `setSourceDocument` /
`setSourceString` / `setSourceStream`, paints to its canvas via
`renderNow` or implicitly during the paint walk.

**Breakage.** Less than the others. SVGView is structurally already
the model this plan proposes — a node whose paint output is a pre-
built ordered list of draw ops. It just builds the list into its own
canvas instead of into a shared display list.

**Resolution.** Replace `SVGDrawOpList` with the engine-wide
`DisplayList`/`DrawOp` type, or keep `SVGDrawOpList` as the cached
form and have `paint()` copy/move-append it into `pc.displayList`:

```cpp
class SVGView : public View {  // View == SceneNode
    Optional<XMLDocument>     sourceDoc_;
    UniquePtr<DisplayList>    cachedOps_;   // built from sourceDoc_
    SVGViewRenderOptions      options_;
    bool                      needsRebuild_ = true;

    void rebuildDisplayList();              // sourceDoc_ → cachedOps_

public:
    void paint(PaintContext & pc) override {
        if (needsRebuild_) rebuildDisplayList();
        pc.displayList.append(*cachedOps_);  // bulk copy
    }

    bool setSourceDocument(XMLDocument doc); // sets DirtyBit::Content|Paint
    void setRenderOptions(const SVGViewRenderOptions &);
};
```

`renderNow()` is removed. `DirtyBit::Content` set by source-document
mutators triggers a rebuild during the next FrameBuilder pass, in
the order Style → Layout → Paint. The SVG render options
(`scaleMode`, `antialias`, `enableAnimation`) feed into
`rebuildDisplayList`, not into runtime paint.

**Resize.** SVGView's `resize()` override stays, but instead of
re-rendering directly it sets `DirtyBit::Layout | Content | Paint`
so the next FrameBuilder pass handles it.

**Animation.** `enableAnimation` becomes a flag the rebuild consults
(SMIL/SVG animations expand into multiple display-list snapshots
keyed by time, or into AnimationScheduler tracks — out of scope for
this plan; tracked separately).

**Tier alignment.** Tier 2 (DisplayList introduction). SVGView is the
first non-trivial validation of the DisplayList model: if a
several-hundred-op SVG document replays correctly through the new
path, the abstraction holds.

### 9.3 `ScrollView` — the layerization opt-in case

**Current contract.** Owns a single content child View. Tracks a
`scrollOffset`. Owns two scroll bar overlay layers
(`vScrollBarLayer`, `hScrollBarLayer`) with their own canvases.
Overrides `scrollOffsetContribution()` so descendants of the content
child can subtract the scroll amount when computing their window
offset (`View::computeWindowOffset`). Compositor scissor clips the
content to the ScrollView's visible bounds.

**Breakage.** Three things break:

1. `View::computeWindowOffset` is deleted (§3.3 table) in favor of
   FrameBuilder's transform accumulator. The hook
   `scrollOffsetContribution()` no longer has a caller.
2. The two overlay layers + their canvases are exactly the per-view
   `LayerTree` / `Canvas` pattern this plan kills.
3. Scissor-based clipping at the compositor level still works, but
   the *decision* to clip moves into the DisplayList as a
   `PushClip`/`PopClip` op pair, not a long-lived layer attribute.

**Resolution.** ScrollView becomes the canonical example of §6 Q3
("layerization opt-in"). It is the first SceneNode that requests its
own composition layer, via:

```cpp
class ScrollView : public View {  // View == SceneNode
    ViewPtr        contentChild_;
    Point2D        scrollOffset_ {0.f, 0.f};
    bool           hasV_, hasH_;
    ScrollBarStyle vBar_, hBar_;   // resolved style for the bars

    bool wantsLayer() const override { return true; }  // forceLayer()

public:
    const Point2D & scrollOffset() const { return scrollOffset_; }
    void setScrollOffset(const Point2D & o);  // sets DirtyBit::Paint

    void paint(PaintContext & pc) override {
        // 1. Clip to ScrollView's visible bounds.
        pc.pushClip(finalRect());

        // 2. Translate by -scrollOffset for the content subtree.
        pc.pushTransform(Transform2D::translate(-scrollOffset_));
        // (FrameBuilder recurses to contentChild_ after this returns.)

        // 3. Overlay scroll bars on top, OUTSIDE the scroll transform.
        // The FrameBuilder pops the transform/clip after children
        // paint; bars are emitted as a post-children overlay.
    }

    void paintOverlay(PaintContext & pc) override {
        // Called by FrameBuilder after children paint and after
        // pushClip/pushTransform are popped.
        if (hasV_) appendVerticalBar(pc, vBar_, scrollOffset_, contentSize_);
        if (hasH_) appendHorizontalBar(pc, hBar_, scrollOffset_, contentSize_);
    }
};
```

The `scrollOffsetContribution()` hook is replaced by the
`pc.pushTransform(translate(-scrollOffset))` call in ScrollView's
own `paint()`. The FrameBuilder's transform accumulator is what
makes descendant paint correct without each descendant having to know
about scroll. The two overlay layers and their per-canvas painting
collapse into two `RoundedRect` DrawOps (the bars themselves) emitted
in `paintOverlay`.

**Layerization.** `wantsLayer()` returning true tells the compositor
that this subtree's DisplayList output should be tagged for a
separate composition layer. This is what enables future scroll
optimizations (compositor-thread scrolling, retained content
texture) without the ScrollView itself caring about the mechanism.
For Tier 3 the layer tag is a no-op — content re-rasterizes every
frame — but the surface is in place.

**Input.** `DefaultScrollHandler` and the wheel event path are
unchanged. Scroll wheel deltas mutate `scrollOffset_` and call
`markDirty(DirtyBit::Paint)`. That's the entire input loop.

**Tier alignment.** Tier 3, alongside the FrameBuilder transform
accumulator. ScrollView's two overlay layers are removed in Tier 3;
its `scrollOffsetContribution` is removed in Tier 4 along with the
rest of `View::computeWindowOffset`.

### 9.4 `VideoView` — migrates out of the View tree

**Current contract.** A `View` subclass that also implements
`Media::VideoFrameSink` ([VideoView.h:35](../include/omegaWTK/UI/VideoView.h)).
Frames arrive on a media thread via `pushFrame(VideoFrame)`. Frames
queue in `framebuffer`. `presentCurrentFrame()` (called from the
playback dispatch queue, not the UI loop) draws the head frame to
`videoCanvas`. The view also owns playback and capture sessions.

**Breakage.** VideoView fundamentally violates the
"paint is a pure function of model + layout + style + animation
side table" rule. The model changes from a thread the FrameBuilder
doesn't own, at a cadence (video frame rate) that may differ from
vsync. There is no clean `paint()` that derives the current texture
from authored state — the texture is *handed* to the view from
outside.

**Resolution.** This isn't a problem this plan solves directly.
[NativeViewHost-Adoption-Plan.md](NativeViewHost-Adoption-Plan.md)
already specifies the migration: VideoView the *View subclass* is
**deleted**, and the public surface moves to `VideoViewWidget`
which owns a `NativeViewHost`. The native layer
(`AVSampleBufferDisplayLayer` on macOS, DXGI video swap chain on
Windows, GStreamer video sink on Linux) presents the decoded frames
directly through the platform's hardware video path — zero-copy,
out of the FrameBuilder loop entirely.

From the SceneNode/DisplayList perspective, the only thing this plan
needs to handle is **how `NativeViewHost` participates in the scene
tree**, which is the same question for video, GTE, and any future
native embed. See §9.4.1.

**Where the existing logic goes** (recap of the NativeViewHost plan):

- `VideoView` becomes a non-`View` internal controller class that
  manages playback/capture sessions and pushes frames into the
  native layer. It is no longer a SceneNode.
- `VideoFrameSink` interface is unchanged; the implementation
  pushes to the native surface instead of a `Canvas`.
- `framebuffer` queue, `videoCanvas`, `queueFrame`,
  `presentCurrentFrame`, `flush` — gone (NativeViewHost plan
  Phase V4).
- `VideoViewDelegate` and the playback/capture API stay intact on
  `VideoViewWidget`.

**Tier alignment.** This plan does not gate on the VideoView
migration and the VideoView migration does not gate on Tier 3 of
this plan. They are independent. The shared dependency is §9.4.1
below — both plans need the NativeViewHost SceneNode contract
defined before either ships.

#### 9.4.1 How `NativeViewHost` paints in the new model

NativeViewHost is the umbrella case for *every* embed of native
content into the WTK scene tree — video, OmegaGTEView, future
WebView, OS-native form controls, etc. From the FrameBuilder's
perspective, NativeViewHost has the same shape:

- It is a `View`, therefore a `SceneNode`.
- Its bounds participate in layout normally (Phase 3).
- Its `paint()` does **not** emit visual `DrawOp`s. Instead it emits
  a single **carve-out** op that tells the compositor "leave this
  rectangle alone — a native layer is going to draw on top of it."
- The native layer's position/clip/visibility is synced from the
  resolved rect via the **`onLayoutResolved` signal** the
  NativeViewHost-Adoption-Plan already specifies — not via a
  per-node `commit()` callback. **Layout and paint are kept
  separate**; NativeViewHost's bounds sync rides on the layout
  signal, not on the paint phase.

```cpp
class NativeViewHost : public View {  // View == SceneNode
    NativeItemPtr nativeItem_;
public:
    NativeViewHost(NativeItemPtr item) : nativeItem_(std::move(item)) {
        // Subscribe to layout completion. Fires after Phase 3 (Arrange)
        // resolves this node's finalRect.
        onLayoutResolved.subscribe([this](const Rect & r){
            nativeItem_->syncBounds(r, computeWindowOriginContribution());
            nativeItem_->syncVisibility(isEnabled());
        });
    }

    void paint(PaintContext & pc) override {
        // Single op: reserve this rect; the native layer paints here.
        pc.displayList.append(DrawOp::NativeContent{
            .destRect = finalRect(),
            .hostId   = nativeItem_->id(),
        });
    }
};
```

`onLayoutResolved` is a per-node signal fired by the FrameBuilder at
the end of Phase 3 (Arrange) for any node whose `finalRect` changed.
NativeViewHost is the canonical subscriber today; future nodes that
need "tell me when my geometry is settled" hook the same signal
without the FrameBuilder gaining a new phase callback. Style-driven
visibility changes fire the same signal because visibility is a
layout-relevant input.

This keeps the §3.2 invariant intact: there is no `commit()` hook
that runs alongside paint. Paint is a pure read of resolved state;
side-effecting work that the *native side* needs to know about
(bounds, visibility) rides on the layout signal that already exists.

`DrawOp::NativeContent` carries no pixel data. Its job at
DisplayList replay time is purely to:

1. Establish a hole in any 2D content the compositor would draw on
   top of this rect (clear / clip / blend-mode-discard, depending
   on backend).
2. Carry the `hostId` so the compositor knows which native layer
   owns this region (relevant when the platform compositor needs
   ordering hints, e.g. Core Animation's layer ordering or
   Direct Composition's visual tree).

Whether 2D widgets can layer *on top of* a NativeViewHost is the
**airspace** question. The NativeViewHost plan accepts the airspace
constraint (native draws on top of virtual content within its
rect). This plan inherits that constraint without modification —
2D `DrawOp`s emitted by descendants of a NativeViewHost still
append to the DisplayList, but on most platforms the native layer
will obscure them. Designs that need 2D-over-native (subtitles,
playback controls) handle it inside the native layer (e.g.
AVPlayerViewController's overlay path) or by rendering 2D into a
sibling NativeViewHost that the OS composites above the video
layer.

This is the §9.5 cross-cutting answer for *all* native embeds:
emit `DrawOp::NativeContent`, sync bounds in commit, accept
airspace. There is no per-subclass design for native-backed
views — they all use this contract.

### 9.5 Cross-cutting — what `View` provides and what it stops providing

After all four migrations, the methods on `View` that survive at the
public surface are:

- `getRect()`, `resize(Rect)` — geometry, kept.
- `enable()` / `disable()` / `isEnabled()` — visibility, kept.
- `setDelegate(ViewDelegate*)` — input, kept (input plan is separate).
- `containsPoint(Point2D)` — hit testing, kept.
- `getResizeCoordinator()` — **deleted** (§3.3 table).
- `makeLayer` / `makeCanvas` — **deleted** (§3.3 table).
- `startCompositionSession` / `endCompositionSession` — **deleted**.
- `submitPaintFrame(int)` — **deleted** (lifecycle plan §4).
- `setFrontendRecurse` / `setSyncLaneRecurse` — **deleted** (§3.3).
- `computeWindowOffset` / `scrollOffsetContribution` — **deleted**;
  the FrameBuilder transform accumulator replaces both.
- `applyLayoutDelta` — moves to AnimationScheduler.

The base class shrinks from "view + render-target + canvas-factory +
session-owner + offset-resolver" to "view + geometry + visibility +
hit-test + delegate." Everything else is the SceneNode/FrameBuilder/
DisplayList pipeline.

### 9.6 Migration order

The subclasses are not equally urgent. Recommended order, interleaved
with the tiers in §4:

| Subclass | Tier 1 | Tier 2 | Tier 3 | Tier 4 |
|---|---|---|---|---|
| `SVGView` | — | `SVGDrawOpList` becomes a cached `DisplayList` | per-view Canvas removed; `renderNow` deleted | dirty-bit-driven rebuild only |
| `ScrollView` | — | `PushClip` / `PushTransform` ops added to DrawOp set | overlay scroll bar layers removed; `wantsLayer()` introduced; `scrollOffsetContribution` replaced by `pushTransform` in own paint | `computeWindowOffset` deleted |
| `NativeViewHost` (covers `VideoView`, `OmegaGTEView`, future) | — | `DrawOp::NativeContent` added; `onLayoutResolved` signal wired | airspace contract documented | — |
| `CanvasView` | — | imperative methods become deprecated stubs routing to a temporary `UIViewLayoutV2` | **deleted** (header + class) | — |
| `VideoView` (the View) | — | — | **deleted**; replaced by `VideoViewWidget` per [NativeViewHost-Adoption-Plan.md](NativeViewHost-Adoption-Plan.md) | — |

`SVGView` is the cheapest migration and the best early validator of
the DisplayList model — it already produces an ordered op stream.
Do it first in Tier 2.

`ScrollView` is the validator for the transform accumulator and the
layerization opt-in — do it in Tier 3 alongside the FrameBuilder
appearing.

`NativeViewHost` is the validator for native embeds. Its
`DrawOp::NativeContent` op needs to land in Tier 2 so the
NativeViewHost-Adoption-Plan migrations (VideoView, OmegaGTEView)
can proceed in parallel. The op shape and the `onLayoutResolved`
signal contract should be reviewed against both adoption plans
before Tier 2 ships.

`CanvasView` is **deleted**, not migrated. Tier 2 deprecates the
imperative methods; Tier 3 deletes the class.

`VideoView` the View subclass is **not** migrated by this plan —
the NativeViewHost-Adoption-Plan deletes it. This plan only owes
that adoption plan the `DrawOp::NativeContent` op and the §9.4.1
contract.

### 9.7 Open questions specific to this section

1. **`SVGView` animation.** SVG SMIL animations are not addressed
   here. If the WML / animation roadmap needs them, the rebuild loop
   becomes time-keyed and the FrameBuilder Tick phase has to drive
   it. Out of scope for this plan; flagged for the animation
   simplification follow-up.

2. **`ScrollView` overlay vs. sibling bars.** Scroll bars could be
   emitted by ScrollView's own paint as `paintOverlay` (sketched in
   §9.3) or modeled as sibling Views. Overlay is simpler; siblings
   are more flexible (themable independently, animatable
   independently). Recommendation: overlay for Tier 3, sibling
   nodes only if the theming/animation surface area demands it.

3. **`DrawOp::NativeContent` op shape.** The op shape should be
   designed jointly with [NativeViewHost-Adoption-Plan.md](NativeViewHost-Adoption-Plan.md)
   so a single op covers video, GTE, and future native embeds. Open
   sub-questions: does the op need a z-order hint for platforms with
   multiple native layer ordering modes (CA's `addSublayer` order vs.
   DirectComposition's visual tree)? Does it need a colorspace /
   HDR-metadata slot for video? Recommendation: minimal op for Tier
   2 (`destRect` + `hostId`), additive fields as the adoption plans
   need them.

4. **`onLayoutResolved` signal scope.** The signal exists on every
   `View`, but only NativeViewHost subscribes today. Open question:
   does the signal also fire for transform changes (a parent
   ScrollView scrolling, a parent Animator translating), or only
   for layout-rect changes? Recommendation: fire on rect changes
   only for Tier 2; add a separate `onTransformChanged` signal in
   Tier 3 if and when a use case appears. Conflating the two would
   make every animated transform invoke every native sync, which
   is wasteful for the common case (no native embeds in the
   subtree).

5. **Airspace and 2D-over-native overlays.** §9.4.1 accepts the
   airspace constraint (native draws on top of 2D within the host's
   rect). Some use cases (subtitle rendering, playback controls)
   need 2D pixels on top of video. Two paths exist: (a) render the
   2D inside the native layer's compositing system (platform-
   specific), (b) provide a sibling NativeViewHost that the OS
   composites above. Both are out of scope for this plan; flagged
   for the NativeViewHost adoption plan to decide per-platform.

6. **`OmegaGTEView` direct present.** [OmegaGTEView-Proposal.md](OmegaGTEView-Proposal.md)
   and the NativeViewHost adoption plan together specify a
   direct-present mode where the 3D content is the *only* thing in
   the host's rect — no compositor blit. In that mode the
   `DrawOp::NativeContent` carve-out is still emitted, but the
   compositor backend treats it the same way it treats VideoView's
   carve-out: clear the rect, let the native layer draw. No special
   case at the SceneNode level.

### 9.8 What this section did not address

- **Future `View` subclasses** that are not native-backed
  (hypothetical PDFView, ChartView, etc.). These will appear and
  will each pick from the same toolbox: cached display list like
  `SVGView`, transform/clip like `ScrollView`, or native carve-out
  like `NativeViewHost`. (Imperative `CanvasView`-style drawing is
  not in the toolbox — see §9.1.) The pattern is established here;
  new subclasses do not require revisiting the core plan.

- **Native-backed Views beyond video and GTE.** WebView, MapView,
  OS-native form controls, and the rest of the
  NativeViewHost-Adoption-Plan's Phase 6+ candidates all use the
  §9.4.1 contract unchanged. This plan owes them only the
  `DrawOp::NativeContent` op and the commit-phase bounds sync.

- **Widget subclasses that inherit from these views.** Most widgets
  use `UIView` already; the few that use `CanvasView` or specialized
  views inherit the migration path of their base. If a widget
  reaches into `makeCanvas` or `submitPaintFrame` directly, the
  Tier-3 grep sweep (§8) catches it. Same checklist, no new
  machinery.

- **Test coverage.** Each subclass migration should ship with a
  before/after rendering test that captures the same pixel output
  through the old path and the new path. This is mechanical work
  per subclass and should be tracked alongside the migration tier
  it belongs to.
