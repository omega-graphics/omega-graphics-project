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
   Holds parent-relative bounds (`Composition::Rect`), an *optional*
   3D-effect transform (`Matrix4x4`, for perspective / full 3D
   rotation, etc.), a `Style` handle, a `LayoutManager*` (optional,
   used to arrange *its own* children), and a `DirtyBits` field. Has
   children. Has a virtual `paint()`. Does **not** own a LayerTree,
   Canvas, sync lane, or animation state. **2D positional moves
   update the rect's `pos` directly** — they do not push a transform.
   The transform slot is for 3D effects that need the perspective /
   rotation matrix; the SceneNode-era `pushTransform` op (Tier 3)
   carries the same `Matrix4x4` shape.

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
     - `TextRun` — post-shaping MSDF text run (one or more
       `TextSubRun`s, each carrying glyph IDs + positions + an atlas
       binding) plus rect and color. Mirrors `VisualCommand::TextRun`
       (`Direct-To-Drawable-And-SDF-Plan` §6.7). Shaping happens
       upstream in the WTK text layout engine; the bitmap-fallback
       sub-runs from the same `drawText` call ride the `Bitmap` op
       instead of being multiplexed into this variant.
     - State ops: `PushTransform` / `PopTransform` (3D-effect
       `Matrix4x4` — perspective / 3D rotation; not the path for 2D
       positional moves, which update rects directly), `PushClip` /
       `PopClip`, `PushOpacity` / `PopOpacity`, `PushEffect`
       (per-layer blur scratch) / `PopEffect`.
     - `NativeContent` — Phase 2.5. Reserves a `destRect` for a
       platform native layer to draw into (video, GPU view, future
       web view); carries a `hostId` plus a `zOrderHint` so the
       platform compositor can order the carve-out against other
       native layers. No pixel data; the compositor translates the
       op into a platform-specific carve-out at replay time.

   No per-op resource creation. Paint appends. Composition reads.
   The DisplayList is the implementation-detail handoff to the
   compositor backend's `BackendRenderTargetContext::renderToTarget`,
   so its op set should track that surface — when the SDF plan
   adds a new primitive (e.g. MSDF text run), the DisplayList gains
   the matching op.

   **`DrawOp` retires `VisualCommand`, `CanvasFrame`, *and* `Canvas`.**
   The post-SDF `VisualCommand` shape — one command per primitive,
   fill + border consolidated into the same record, soft shadow as
   its own SDF command — is exactly what `DrawOp` is. They are not
   two parallel formats with a translation step between them:
   `DrawOp` *is* the new compositor op type and `VisualCommand` is
   **deleted**. The two prior producers of `VisualCommand` (per-view
   `Canvas` and `SVGView`'s internal canvas) are both removed by
   Tier 3 / Tier 4 of this plan, leaving `VisualCommand` with no
   upstream. The compositor backend's `renderToTarget()` switch is
   rewritten to dispatch on `DrawOp` directly. Backend rasterization
   code (the SDF pipeline, the triangulator path, the bitmap blit,
   the text run) is untouched — only the input type changes.
   `CanvasFrame` (the per-view recorded list of `VisualCommand`s)
   is similarly retired in favor of the per-window `DisplayList`.

   **Canvas itself is deleted alongside them.** Reading the current
   code (`wtk/src/Composition/Canvas.cpp`), every Canvas method —
   `drawRect`, `drawRoundedRect`, `drawEllipse`, `drawPath`,
   `drawImage`, `drawText`, `drawShadow`, `setElementTransform`,
   `setElementOpacity` — is a thin
   `current->currentVisuals.emplace_back(VisualCommand{...})`. Canvas
   owns no GPU resources, no caches, no batching buffers; it is a
   per-frame accumulator that resets in `nextFrame()`. The GPU
   dispatch (`BackendRenderTargetContext::renderToTarget(VisualCommand::Type, void*)`)
   never takes a Canvas — it reads command data directly out of
   `CompositeFrame::WidgetSlice`. So Canvas's only role is "build
   `VisualCommand` into `CanvasFrame`." Once both of those are
   gone, Canvas has nothing to build. It is **deleted in Tier 4**.
   The misleading framing in earlier drafts of this plan ("Canvas
   survives as the low-level API the compositor backend exposes")
   does not match the code: the backend's API surface is
   `renderToTarget`, not Canvas. See §4 Tier 4 and §5 for the
   sequenced deletion.

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
| `Canvas` as a per-view paint device          | **Deleted in Tier 4** alongside `VisualCommand`/`CanvasFrame`. Canvas is a builder for those two types; once they go, it has nothing to build. The backend never consumed Canvas — `renderToTarget` reads command data directly. Tier 2 keeps Canvas as a transitional sink (DrawOp → `canvas->drawXxx` → VisualCommand) so the GPU path is unchanged during the transition; Tier 3 collapses N per-view canvases into one window-scoped instance; Tier 4 removes the translation layer and deletes the class. |
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
- `FlexLayout` — **shipped in Tier 4 Phase 4.6** (not "later" as this
  section originally implied). `StackWidget` is a live consumer in the
  tree, so its bespoke flexbox could not stay a parallel path once the
  `LayoutManager` family owned child layout. `FlexLayout` is a public
  `LayoutManager`-family built-in: per-child state lives on the
  manager (keyed by `View *`), main-axis distribution + cross-axis
  alignment + the suspicious-rect cache from the pre-migration
  `StackWidget` are all behind the `LayoutManager` interface now.

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
  surface in Tier 3. The `Canvas` class itself is **deleted in Tier 4**
  when `VisualCommand`/`CanvasFrame` retire. The compositor backend
  never used Canvas as an API surface — `BackendRenderTargetContext::renderToTarget`
  switches on the command type enum directly. Once that switch
  dispatches on `DrawOp` instead of `VisualCommand`, Canvas's role as
  a `VisualCommand` builder evaporates and the class goes away.

Breaking? Yes. The only in-tree caller of `update()` is `UIView` tests
and widget-tree hosts. Migration is a one-line replacement with
`invalidate()` or nothing at all.

---

## 4. Migration plan

Four tiers, each independently shippable and each reducing surface
area before the next.

### Tier 1 — delete the per-view sync lane fragmentation [DONE]

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
  function of the model. The `DisplayList → Canvas → VisualCommand`
  adapter is throwaway scaffolding — it exists so the GPU path
  doesn't change in Tier 2, and disappears in Tier 4 when Canvas
  itself is deleted.
- Delete `localBoundsFromView` static map. Bounds come from
  `getRect()` + `LayerTree` rect directly, computed per call with no
  pointer-keyed cache. The stability logic was compensating for
  resize races that the single-surface refactor
  (`Render-Execution-Efficiency-Plan.md` NV-1..NV-3) already removed.

Risk: medium. Files touched: `UIView.Update.cpp`, `UIViewImpl.h`,
new `Composition/DisplayList.{h,cpp}`.

#### Tier 2 phasing

Tier 2 is large enough that landing it as one change would be opaque
to review and brittle to revert. It is broken into seven phases, each
independently shippable. Phase 2.0 is the type-design validator; the
later phases are mechanical once 2.0 holds.

**Cross-cutting decisions** (apply to every Tier 2 phase):

- **`DrawOp` mirrors `VisualCommand` 1:1.** Same tagged-union shape
  (`Type` enum + anonymous-union of per-variant `params` structs +
  sentinel constructors per variant). Same field names. Same field
  types. The replay adapter becomes a one-line dispatch per case
  (`canvas.drawXxx(op.params.xxxParams.…)`), and the eventual Tier 4
  retirement of `VisualCommand` becomes a mechanical search/replace
  rather than a semantic migration. The shape match is by
  construction.
- **`DisplayList.h` is a public header** at
  `wtk/include/omegaWTK/Composition/DisplayList.h`, parallel to
  `Canvas.h`. SVGView (Phase 2.3) and NativeViewHost (Phase 2.5)
  are out-of-`Composition/` consumers. Implementation in
  `wtk/src/Composition/DisplayList.cpp`.
- **Verification harness:** `wtk/tests/RootWidget/Main.cpp`. There
  are no pixel-regression tests in tree (the closest,
  `tests/SVGViewRenderTest/main.cpp`, only logs parse success/error).
  Each phase adds or modifies a RootWidget scene that exercises the
  newly-converted code path; verification is visual-comparison
  against the previous tier's behavior. A proper image-diff harness
  is out of scope for Tier 2 and tracked separately.

##### Phase 2.0 — type introduction + thinnest end-to-end slice

The validator phase. If the round trip works for one op, it works
for the other eight (they only differ in payload).

- Add `wtk/include/omegaWTK/Composition/DisplayList.h` and
  `wtk/src/Composition/DisplayList.cpp`.
- `DrawOp::Type` enum covers exactly the nine active `VisualCommand`
  variants present today: `Rect`, `RoundedRect`, `Ellipse`,
  `VectorPath`, `TextRun`, `Bitmap`, `Shadow`, `SetTransform`,
  `SetOpacity`. `SetTransform` carries a `Matrix4x4` **3D-effect
  transform** (perspective, full 3D rotation, scale); 2D positional
  changes update `Composition::Rect::pos` directly and never push a
  transform op. The text variant is the *post-shaping* `TextRun`
  (matching `VisualCommand::textRunParams` — a `Vector<TextSubRun>`
  plus rect and color), not the high-level `drawText` call shape;
  shaping stays in `Canvas::drawText`, which now ends in a public
  `Canvas::drawTextRun` helper that the replay path also targets.
  Phase 2.1's text branch shapes via `drawText` first, then emits a
  `TextRun` op. `VisualCommand` also declares a `Text` enum value
  with no payload and no constructor — it is dead code today and is
  intentionally omitted from `DrawOp`. State ops (`PushClip`,
  `PushTransform`, `PushOpacity`, `PushEffect`) and `NativeContent`
  are deferred to Phases 2.4 / 2.5 — they have no consumers in
  `UIView::update()` and adding them now bloats the slice without
  exercising them.
- `DisplayList` is a flat `OmegaCommon::Vector<DrawOp>` with `append`,
  `clear`, and `size`. No transform stack at the type level (the
  `SetTransform`/`SetOpacity` ops carry the same state model
  `Canvas` already has).
- `DisplayListReplay::replay(const DisplayList & list, Canvas & canvas)`
  — switch on `op.type`, hand the matching `params` struct to the
  matching `Canvas::drawXxx` / `setElementXxx` method.
- Convert **one** call site in `UIView::update()`: the simple `Rect`
  shape branch in the per-element loop (`UIView.Update.cpp:310`-ish
  range). Build a fresh `DisplayList`, append one `DrawOp::Rect`
  with the same brush/border/rect that the current path uses, replay
  into `impl_->rootCanvas`. All other branches still call Canvas
  directly. The displaylist is local to that one branch — no
  function-scope or impl-scope storage yet.
- Add a RootWidget scene that paints a single rect through this
  path. Verify visually that the rect appears identically to the
  pre-change path (toggle a build-time `#define` if useful for
  side-by-side comparison).

Files touched: new `Composition/DisplayList.{h,cpp}`,
`UIView.Update.cpp` (one branch), `tests/RootWidget/Main.cpp`.

##### Phase 2.1 — convert remaining `UIView::update()` branches

Mechanical once 2.0 holds. Same DrawOp/Replay machinery, same
contract.

- Convert the remaining eight shape/state branches: `RoundedRect`,
  `Ellipse`, `VectorPath`, `TextRun`, `Bitmap`, `Shadow`,
  `setElementTransform`, `setElementOpacity`. Each becomes
  `displayList.append(DrawOp::Xxx{...})` instead of a direct
  `canvas->drawXxx(...)` call. The text branch is the one
  non-symmetric case: `DrawOp::TextRun` carries post-shaping data,
  so the conversion calls into the WTK text layout engine (the same
  pipeline `Canvas::drawText` runs internally) to produce a
  `Vector<TextSubRun>`, then appends one `DrawOp::TextRun` per
  resolved sub-run group. Bitmap-fallback sub-runs continue to ride
  the `drawGETexture` path as a `DrawOp::Bitmap` carrying the
  rasterized texture.
- Promote the displaylist from "branch-local in one switch case" to
  "function-local for the whole `update()` call." The replay into
  `impl_->rootCanvas` happens once at the end of `update()`, just
  before `sendFrame()`.
- `UIView::update()` is now structurally:

  ```
  DisplayList list;
  for(each element) {
      switch(shape.type) {
          case Rect:        list.append(DrawOp::Rect{...}); break;
          case RoundedRect: list.append(DrawOp::RoundedRect{...}); break;
          // ...
      }
      if(text) {
          auto subRuns = shapeText(...);                // WTK layout engine
          list.append(DrawOp::TextRun{subRuns,rect,color});
      }
  }
  DisplayListReplay::replay(list, *impl_->rootCanvas);
  impl_->rootCanvas->sendFrame();
  ```

  Same output, same bugs, paint is now a pure function of model +
  layout + style + animation side-table.
- RootWidget scene grows to exercise each variant.

Files touched: `UIView.Update.cpp` (the remaining branches),
`tests/RootWidget/Main.cpp`.

##### Phase 2.2 — delete `localBoundsFromView` static map

Independent cleanup, easier to land after 2.1 because the only call
site is in `update()`.

- Delete `static std::unordered_map<UIView *, StableBoundsState>
  stableBoundsByView` and the `StableBoundsState` struct.
- Drop the now-unused `<unordered_map>` include from
  `UIView.Update.cpp`.
- Reshape `UIViewInternal::localBoundsFromView` to a pure per-call
  computation: pick `getRect()` if valid, fall back to
  `getLayerTree()->getRootLayer()`'s rect if valid, otherwise return
  the 1×1 fallback. The existing `isValidDimension` /
  `isSuspiciousDimensionPair` / `clampDrawableDimension` helpers
  stay — they're the per-call sanitization that doesn't depend on
  cache state. The pre-cache "if neither candidate is valid, return
  the last cached rect" branch is gone; it was the only behavior the
  cache provided, and per plan §1.4 + Render-Execution-Efficiency-
  Plan NV-1..NV-3 the resize races that produced transient
  simultaneously-invalid candidates are already gone.
- Helper signature and call site (single, in `update()`) unchanged.
- Verified by building + running the RootWidget scene (Phase 2.1
  validator). If a resize regression appears, this phase reverts
  cleanly without affecting Phases 2.0 / 2.1 / 2.3-2.6.

Files touched: `UIView.Update.cpp`.

##### Phase 2.3 — `SVGView` migration to `DisplayList`

The first non-trivial validator: a several-hundred-op document
round-tripped through the new types proves the abstraction holds at
scale.

- Replace `SVGDrawOpList` (currently in `SVGView.cpp`) with
  `Composition::DisplayList`. The current `SVGDrawOp` variants
  (Rect, RoundedRect, Ellipse, Path, Line, Polyline) all map onto
  the existing nine `DrawOp` variants — `Line` and `Polyline`
  collapse into `DrawOp::VectorPath` since the GPU path already
  tessellates them through the same vector pipeline.
- `SVGView` keeps `cachedOps_` (renamed/retyped) as a
  `UniquePtr<DisplayList>` rebuilt from `sourceDoc_` on
  `setSourceDocument` / `setSourceString` / `setSourceStream`.
- `SVGView::paint()` still calls `DisplayListReplay::replay` into
  `svgCanvas`. The §9.2 sketch ("`pc.displayList.append(*cachedOps_)`")
  is the Tier 4 shape, not the Tier 2 shape — Tier 2 still has a
  per-view canvas; the SVG list is replayed into it.
- Delete `SVGView::renderNow()`.
- Verify with `tests/SVGViewRenderTest/main.cpp`: the existing test
  parses an SVG and triggers a render. Visual inspection that the
  rendered output is unchanged.

Files touched: `SVGView.h`, `SVGView.cpp`,
`tests/SVGViewRenderTest/main.cpp`.

##### Phase 2.4 — add `PushClip` / `PushTransform` ops

No consumer in Tier 2; ScrollView migration is Tier 3. Landing the
op shapes now keeps Tier 3 mechanical and gives `DisplayList` the
state-stack vocabulary it will need.

- Extend `DrawOp::Type` with `PushClip`, `PopClip`, `PushTransform`,
  `PopTransform`. The plan §3.2 also lists `PushOpacity` / `PopOpacity`
  and `PushEffect` / `PopEffect`; defer those until Tier 3 has a use
  case for them (current `setElementOpacity` op already handles the
  scalar opacity case).
- `params` payloads:
  - `PushClip` → dedicated `pushClipParams { Composition::Rect rect; }`
    field. The clip rect is in the *current* drawing space
    (post-`PushTransform`, if one is active above).
  - `PushTransform` → reuses the existing `transformMatrix`
    `Matrix4x4` slot (same field `SetTransform` writes). The 4×4
    matrix is a **3D-effect transform** (perspective, full 3D
    rotation, scale). 2D positional moves do not go through this op
    — they update the node's rect directly. ScrollView is the first
    planned producer (Tier 3), but its scroll-offset application is
    a 2D position update on the descendants' visible region via
    `contentOffset()`, not a pushed transform — see §9.3 for the
    revised ScrollView shape.
  - `Pop*` → no payload (`type` is the only state the op carries).
  - Distinct `Push*` and `Set*` variants share the `transformMatrix`
    field but differ in semantics — pushed onto a scope stack vs.
    set as the persistent canvas state.
- Construction: static factories `DrawOp::makePushClip(rect)`,
  `DrawOp::makePopClip()`, `DrawOp::makePushTransform(matrix)`,
  `DrawOp::makePopTransform()`. Same enum-vs-method-name reason as
  `makeNativeContent` (§2.5); additionally, a `(Matrix4x4)` ctor
  would shadow the existing `SetTransform` ctor.
- `DisplayListReplay` translates these ops as **option (b) — no-op
  in Tier 2**. The Canvas API surface has no `pushClip` / `popClip`
  / scoped-transform hook, and there is no Tier-2 producer
  (UIView::update emits only shape ops; SVGView's parsed display
  list also emits only shape ops). The four cases fall into one
  no-op arm with a comment pointing forward to Tier 3, where
  ScrollView starts emitting `PushClip` and either the replay
  becomes a stack accumulator or the backend dispatch takes over.
  The "preferred (a)" path noted in earlier drafts was rejected
  because both stacks need composition semantics that touch the
  shape-op payloads, and faking those at the replay layer would
  encode the wrong contract before Tier 3 has the FrameBuilder to
  apply them correctly.
- No call site changes. The ops exist; nothing emits them yet.

Files touched: `Composition/DisplayList.{h,cpp}`.

##### Phase 2.5 — add `DrawOp::NativeContent` + `onLayoutResolved` signal

Unblocks the NativeViewHost-Adoption-Plan migrations (VideoView,
OmegaGTEView) so they can run in parallel with the rest of Tier 2.

- Extend `DrawOp::Type` with `NativeContent`. Payload (per §9.7 Q3
  + the user's Q3 answer): `{ Composition::Rect destRect; uint64_t
  hostId; int zOrderHint; }`. The z-order hint is required (per the
  user's note — "NativeContent does need a z-order hint so it
  renders above the bottom layer"). The op is constructed through
  the `DrawOp::makeNativeContent(destRect, hostId, zOrderHint)`
  static factory rather than a ctor — C++ enums and member
  functions share scope, so a method literally named `NativeContent`
  would collide with `DrawOp::Type::NativeContent`, and the
  `(Rect, ...)` ctor shape would shadow `DrawOp::Rect`.
- `DisplayListReplay` for `NativeContent` is a no-op in Tier 2 — the
  Canvas-based GPU path has no native-carve-out concept. The op
  exists as a shape contract for the NativeViewHost-Adoption-Plan
  to build against; backend handling lands in Tier 3 alongside the
  FrameBuilder.
- Add `View::onLayoutResolved` signal (per §9.4.1 + §9.7 Q4): fires
  on rect changes only, not on transform changes (a separate
  `onTransformChanged` signal is deferred to Tier 3 if needed).
  *Why rect-only:* `DrawOp::SetTransform` carries a 3D-effect
  `Matrix4x4` and is applied during paint, never as a 2D positional
  move; 2D position changes show up as rect updates, which already
  flow through `View::resize()`. Today the only firing point is
  `View::resize()` — wire it there. The signal is implemented as a
  thin `LayoutResolvedSignal` value type with `subscribe(callback)`
  and an `emit(rect)` invoked from `resize()` immediately after the
  sanitized rect commits and the layer tree catches up. No
  unsubscribe API in Tier 2 (only NativeViewHost subscribes today,
  with a View-matching lifetime); a token-based unsubscribe lands
  with Tier 3 if a use case appears.
- Coordinate the op shape and signal contract with
  `NativeViewHost-Adoption-Plan.md` before merging.

Files touched: `Composition/DisplayList.{h,cpp}`,
`include/omegaWTK/UI/View.h`, `wtk/src/UI/View.Core.cpp`.

##### Phase 2.6 — `CanvasView` imperative-method deprecation

Smallest Tier-2 impact; sequenced last so the rest of Tier 2 has
landed and any in-tree CanvasView callers can be migrated against a
stable surface.

- `CanvasView`'s imperative `drawRect` / `drawRoundedRect` /
  `drawImage` / `drawText` methods are marked `[[deprecated]]` and
  re-expressed in the Phase 2.1 `DrawOp` vocabulary: each call
  builds a one-op `Composition::DisplayList` and replays it through
  `Composition::DisplayListReplay::replay` into `rootCanvas_`.
  Same GPU path, same output — but the imperative methods no
  longer reach the Canvas API directly, so the §8 grep sweep at
  the deprecation marker catches every remaining caller before
  Tier 3 deletes the class. The plan's earlier "transient
  `UIViewLayoutV2` element" framing was rejected because
  `CanvasView` has no `UIView` instance to feed a layout into; the
  spirit of "route through the UIView paint path" is satisfied by
  going through the same `DisplayList` + `Replay` pair that
  `UIView::update` itself now uses.
- The `drawText` body uses `Composition::shapeTextForDisplayList`
  (Phase 2.1's text-shaping helper) so MSDF + bitmap-fallback
  branches behave identically to `UIView::update`'s text path.
- `clear()` is **not** deprecated. It writes the frame-channel
  background color (`CanvasFrame::background`), not a draw op;
  Tier 3's session-lifetime move will reassess.
- `CanvasView::submitPaintFrame(int)` override stays for Tier 2 (it
  is removed in Tier 3 alongside the session-lifetime move).
- No header file deletion in Tier 2; the class survives until Tier 3.
- In-tree callers (Phase 2.6 grep sweep): `wtk/src/Widgets/Primatives.cpp`
  (drawImage) and the test bundles `TextCompositorTest`,
  `EllipsePathCompositorTest`, `ContainerClampAnimationTest`,
  `VideoViewPlaybackTest` all keep building with deprecation
  warnings active; they migrate to `UIView` + `UIViewLayoutV2`
  before Tier 3 deletes the class.

Files touched: `omegaWTK/UI/CanvasView.h`, `wtk/src/UI/CanvasView.cpp`.

#### Phase ordering rationale

- 2.0 first: validates type design on the smallest possible surface.
  If the round-trip is broken, only one call site is affected.
- 2.1 immediately after: same machinery, mechanical extension. Lands
  the bulk of the `UIView::update()` refactor.
- 2.2 right after 2.1: the static-map deletion is a cleanup the
  Update refactor enables. Independent enough to revert alone.
- 2.3 next: the SVG migration is the abstraction's first stress
  test. Catches issues that single-rect Phase 2.0 could not surface
  (op-count, deeply-nested paths, brush variety).
- 2.4 and 2.5 in either order: both are op-set additions with no
  Tier-2 consumers. They unblock Tier 3 (ScrollView) and the
  NativeViewHost-Adoption-Plan respectively.
- 2.6 last: deprecation is downstream of everything else and has no
  in-Tier-2 consumers depending on it.

#### What Tier 2 explicitly does NOT do

- Does not remove `Canvas`, `VisualCommand`, or `CanvasFrame`. They
  all survive Tier 2 unchanged (per §3.2, Canvas is the GPU-path
  stability anchor through Tier 3).
- Does not collapse per-view LayerTrees (Tier 3).
- Does not introduce `SceneNode`, `LayoutManager`, `FrameBuilder`,
  `PaintContext`, or `DirtyBits` (Tier 4).
- Does not change `BackendRenderTargetContext::renderToTarget` (Tier 4).
- Does not remove `View::computeWindowOffset` (Tier 3/4).
- Does not delete `CanvasView` (Tier 3).

### Tier 3 — collapse per-view LayerTree into one per window

- `View::Impl::ownLayerTree` removed.
- `AppWindow::Impl` owns one `LayerTree`.
- `View::makeLayer` / `makeCanvas` are removed from the public
  `View` surface. `CanvasView` is deleted in this tier (per §9.1),
  so there are no remaining public callers. Internally there is now
  a single window-scoped `Canvas` instance that `FrameBuilder` uses
  as the `DisplayList → VisualCommand` bridge. This bridge is the
  last remaining role of `Canvas`, and it is removed in Tier 4 when
  the class itself is deleted.
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

#### Tier 3 phasing

Tier 3 is larger than Tier 2 in surface area *and* in risk — it is
the only tier that fundamentally changes how frames reach the
compositor, not just how they're recorded. It is broken into eleven
phases. The first four (3.0–3.3) are pure scaffolding: a window-
scoped infrastructure is added, an opt-in flag chooses the new path
per view, and nothing existing breaks. Phases 3.4–3.7 light up
capabilities that the new path needs (transform accumulator, scoped
clip, native carve-out). Phases 3.8–3.10 are the destructive ones:
the flag goes away, per-view state is deleted, and the doomed
classes (`CanvasView`, the `VideoView` View subclass) are removed.
Each phase is independently shippable. The destructive ones revert
cleanly only as long as the prior additive phases stay landed.

**Cross-cutting decisions** (apply to every Tier 3 phase):

- **Feature flag: `OMEGAWTK_WINDOW_SCOPED_PAINT`.** A build-time
  define plus an `AppWindow`-level boolean. When off, the per-view
  Canvas path from Tier 2 stays live (and every test keeps
  producing identical pixels). When on, `FrameBuilder` collects
  `DisplayList`s from opted-in views and replays them into the
  window `Canvas`. Each phase that adds a window-scoped consumer
  flips the flag on for *just that consumer* in the test scenes;
  Phase 3.8 deletes the flag entirely. The flag exists because
  Tier 3 is high-risk and a per-phase revert needs to be a config
  change rather than a code rollback.
- **`FrameBuilder` is internal to `AppWindow` until Tier 4.** Lives
  at `wtk/src/UI/FrameBuilder.{h,cpp}`, with the header private to
  the UI library. No public surface; views interact with it only
  via the `DisplayList` they hand to the window. Tier 4 promotes it
  to the public SceneNode pipeline.
- **`Canvas` survives Tier 3 unchanged as a class.** The window-
  scoped `Canvas` instance is exactly the same type the per-view
  Canvases were. The plumbing change is *which* `Canvas` the views'
  `DrawOp`s flow into, not what `Canvas` does. `Canvas`'s deletion
  is Tier 4 territory (§3.3 table, §4 Tier 4).
- **No new `DrawOp` variants.** Phase 2.4 (`PushClip` / `PopClip` /
  `PushTransform` / `PopTransform`) and Phase 2.5
  (`NativeContent`) landed the type-level vocabulary Tier 3 needs.
  Phases 3.5 and 3.7 add the *replay-side* handling for those ops;
  no enum extensions, no payload changes.
- **Verification harness:** `wtk/tests/RootWidget/Main.cpp` grows a
  multi-`UIView` scene that exercises window-scoped composition
  (multiple views on one window, each emitting `DrawOp`s into a
  shared `Canvas`). `SVGViewRenderTest` validates SVGView under the
  window-scoped path. A new `ScrollViewClipTest` exercises Phase
  3.6's `PushClip`-driven scroll. NativeViewHost-Adoption-Plan
  tests (`VideoViewPlaybackTest` once its pre-existing link break
  is fixed; future `GTEViewTest`) validate Phase 3.7's carve-out.
  Still no image-diff harness; verification is visual comparison
  with the flag off vs. on per phase.

##### Pre-flight: grep sweep + open-question resolution

Before Phase 3.0, settle the items §8 already flagged so the phasing
doesn't trip on them mid-tier:

- `grep` for `View::makeLayer`, `View::makeCanvas`,
  `View::startCompositionSession`, `View::endCompositionSession`
  across `src/Widgets/`, `tests/`, and any client trees that depend
  on this repo. Any caller outside `src/UI/` and the in-tree tests
  is a Phase 3.0 blocker — fold it into the phasing as a "migrate
  X before Phase 3.8" item, or rule out the use case explicitly.
- Confirm `WidgetTreeHost` and `Widget::executePaint` can be
  retrofitted to drive a window-level `FrameBuilder` without
  rewriting every Widget subclass (§8 honest-uncertainty note). If
  not, Tier 3 grows to include the Widget paint-lifecycle redesign,
  which is its own follow-up plan.
- Coordinate the `DrawOp::NativeContent` carve-out semantics with
  `NativeViewHost-Adoption-Plan.md` *before* Phase 3.7. Per-platform
  carve-out implementations (CA layer reordering, DirectComposition
  visual tree, Wayland subsurface) need their backend touchpoints
  named.

##### Phase 3.0 — `AppWindow` window-scoped `LayerTree` + `Canvas` infrastructure [DONE]

The scaffolding phase. Nothing changes user-visibly; everything that
was per-view stays per-view. The window simply *also* owns a layer
tree and a canvas, ready for FrameBuilder to use them in Phase 3.1.

**Status:** Complete. `AppWindow::Impl` owns
`windowLayerTree_` (origin-at-zero rect mirroring the window's
local size) and `windowCanvas_` (bound to the tree's root layer
via a `friend class AppWindow` declaration in `Canvas.h`, since
Canvas's ctor is private to the View construction path). The
layer tree resizes inside `AppWindowDelegate::syncNativePresentLayer`
alongside the existing backend visual tree resize, so window resize
ticks keep both in sync. `OMEGAWTK_WINDOW_SCOPED_PAINT` is wired
as a CMake `option()` that compiles the macro into `OmegaWTK_UI`,
which seeds the `Impl::windowScopedPaint_` runtime knob; that knob
is reachable via `AppWindow::windowScopedPaint()` /
`setWindowScopedPaint()`. The window Canvas is **not** yet
registered with the compositor frontend (`observeLayerTree`) —
that wiring lands in Phase 3.1 alongside the first window-level
composition session.

- Add `AppWindow::Impl::windowLayerTree_` (`SharedHandle<Composition::LayerTree>`)
  constructed at `AppWindow` construction against the window's
  initial rect.
- Add `AppWindow::Impl::windowCanvas_` (`SharedHandle<Composition::Canvas>`)
  bound to the window layer tree's root layer.
- `AppWindow::Impl::windowLayerTree_` resizes when the window
  resizes (mirror the rect change the existing per-view trees
  receive today via `setWindowRenderTarget`).
- Add private accessors `AppWindow::windowLayerTree()` and
  `AppWindow::windowCanvas()` for internal use by Phase 3.1's
  `FrameBuilder`. Not in the public header.
- Define `OMEGAWTK_WINDOW_SCOPED_PAINT` as a build-time macro
  (default off) and as an `AppWindow::Impl::windowScopedPaint_`
  runtime boolean (default reads the macro).
- Validator: every existing test still builds and runs, with no
  visible change. The window canvas exists but no draws flow into
  it yet.

Files touched: `wtk/include/omegaWTK/UI/AppWindow.h` (new
private accessors), `wtk/src/UI/AppWindow.cpp`, `wtk/src/UI/AppWindowImpl.h`.

##### Phase 3.1 — `FrameBuilder` skeleton + window-level composition session [DONE]

`FrameBuilder` appears, owned by `AppWindow`. In this phase it just
*centralizes the composition session* — opens/closes it once per
frame at the window level — but per-view paint paths still call
their own canvases. This proves the session-lifetime move in
isolation, before any DisplayList collection.

- New `FrameBuilder` class at `wtk/src/UI/FrameBuilder.{h,cpp}`.
  Constructor takes `AppWindow &`; lifetime matches the window's.
- `FrameBuilder::beginFrame()` calls
  `windowCanvas_->getCorrespondingLayer()`'s session entry on the
  window's compositor proxy. `FrameBuilder::endFrame()` calls the
  matching session exit + `windowCanvas_->sendFrame()` if any draws
  landed in the window canvas this frame.
- `AppWindow` calls `frameBuilder_->beginFrame()` before driving
  the widget tree's paint pass and `endFrame()` after.
- `UIView::update()` and `SVGView::paint()` continue to call
  `startCompositionSession` / `endCompositionSession` on their
  *own* views. The window-level session and the per-view sessions
  coexist — both no-op if a session is already open on the same
  proxy (verify this with the compositor frontend's session
  reentrancy model; if it doesn't allow nesting, this phase grows
  to add a "is the window session already open?" gate).
- Validator: existing scenes render unchanged. Time-domain
  verification: log `FrameBuilder::beginFrame` / `endFrame` and
  confirm they bracket the per-view sessions on every frame.

Files touched: new `wtk/src/UI/FrameBuilder.{h,cpp}`,
`wtk/src/UI/AppWindow.cpp`.

##### Phase 3.2 — `UIView` opt-in: hand its `DisplayList` to `FrameBuilder` [DONE]

The first real window-scoped paint. Behind the flag, `UIView::update()`
builds its `DisplayList` (Phase 2.1 already does this) but
*hands it to `FrameBuilder` instead of replaying into `rootCanvas`*.
`FrameBuilder` accumulates `DisplayList`s from all opted-in views
and replays them into `windowCanvas_` in tree order at `endFrame()`.

- `FrameBuilder::submitView(View *, DisplayList)` — records a
  pending replay. The submission carries the view's window-offset
  (computed via the existing `View::computeWindowOffset` for now,
  per §3.3 table; Phase 3.4 replaces with the transform accumulator).
- `UIView::update()` checks `AppWindow::windowScopedPaint()`. When
  on, the function builds the `DisplayList` as today, then calls
  `frameBuilder->submitView(this, std::move(list))` *instead of*
  `DisplayListReplay::replay(displayList, *impl_->rootCanvas)` +
  `sendFrame`. When off, the Phase 2.1 path runs unchanged.
- At `endFrame()`, `FrameBuilder` walks pending submissions in
  insertion order (== tree order, since the widget paint pass is
  pre-order today). For each submission it stamps the window-offset
  into the window canvas frame, then `DisplayListReplay::replay`s
  the view's list into `windowCanvas_`. Single
  `windowCanvas_->sendFrame()` at the end.
- The flag is flipped on for *only the multi-`UIView` RootWidget
  scene* added in this phase. Single-UIView tests stay on the off
  path so any regression is isolated to the new scene.
- Validator: new RootWidget scene puts two non-overlapping UIViews
  on one window, each with a different background color. Off-flag
  baseline: two views composite normally. On-flag: two views
  composite through the window canvas. Pixel-identical output.

Files touched: `wtk/src/UI/UIView.Update.cpp`, new
`wtk/src/UI/FrameBuilder.cpp` methods, RootWidget scene additions.

##### Phase 3.3 — `SVGView` opt-in [DONE]

Same opt-in as Phase 3.2, applied to `SVGView::paint()`. SVG's
cached `DisplayList` was always the right shape for this; Phase 3.3
is mostly a one-line rewire.

- `SVGView::paint()` (Phase 2.3) checks the flag. When on, it
  hands its cached `DisplayList` to `FrameBuilder` instead of
  replaying into `svgCanvas`. When off, current behavior.
- The flag is flipped on for `SVGViewRenderTest`. The existing
  multi-shape SVG document is the validator surface — several
  hundred ops round-tripping through the window canvas.
- `SVGView` still owns `svgCanvas` until Phase 3.8; it just stops
  using it when the flag is on.
- Validator: `SVGViewRenderTest` produces identical output with the
  flag on vs. off.

Files touched: `wtk/src/UI/SVGView.cpp`.

##### Phase 3.4 — `FrameBuilder` transform accumulator + `computeWindowOffset` rewire [DONE]

Replaces `View::computeWindowOffset`'s parent-walk with a
`FrameBuilder`-owned accumulator threaded through the paint walk.
The public `View::computeWindowOffset` method stays as a thin
wrapper (returns `FrameBuilder::currentOffset()` when the window-
scoped path is active, or falls back to the legacy walk when not).

- `FrameBuilder` gains an explicit `Composition::Point2D
  currentOffset_` updated as the widget paint pass enters / exits
  each subtree. Push on enter, pop on exit (a simple
  `std::vector<Point2D>` stack).
- `View::scrollOffsetContribution` callers (today: descendant
  window-offset computation inside a ScrollView subtree) route
  through `FrameBuilder::currentOffset()` so scroll-shifted rects
  arrive at submit time without a parent-walk.
- `View::computeWindowOffset` becomes a one-line wrapper:
  ```cpp
  Point2D View::computeWindowOffset() const {
      auto * fb = AppWindow::activeFrameBuilder();
      return fb ? fb->currentOffset() : legacyComputeWindowOffset();
  }
  ```
  where `legacyComputeWindowOffset()` is the prior implementation
  renamed. Both paths produce the same offset; the wrapper just
  picks the right one based on whether a frame is in flight.
- Validator: nested-UIView RootWidget scene from Phase 3.2 grows to
  include a child UIView placed at a non-trivial offset inside a
  parent UIView. Off-flag and on-flag produce identical positions.

Files touched: `wtk/src/UI/FrameBuilder.{h,cpp}`, `wtk/src/UI/View.Core.cpp`,
RootWidget scene additions.

##### Phase 3.5 — `DisplayListReplay` real implementation for `PushClip` / `PopClip` [DONE]

The Phase 2.4 no-op cases get real implementations now that the
window canvas has a known target. `PushTransform` / `PopTransform`
stay no-op (they carry 3D-effect matrices and the in-tree producers
are still nonexistent; revisit when a producer appears).

- `Canvas` gains `pushClip(Rect)` / `popClip()` methods. The
  backend impl uses the existing scissor / stencil path from the
  SDF pipeline (already in `Direct-To-Drawable-And-SDF-Plan` §6).
  If the SDF backend doesn't expose a public scissor surface yet,
  add the minimal one — set / clear a `currentClipRect_` on the
  per-frame state that the SDF draw consults.
- `DisplayListReplay`'s `PushClip` arm calls
  `canvas.pushClip(op.params.pushClipParams.rect)`; `PopClip` arm
  calls `canvas.popClip()`.
- Stack semantics: nested `PushClip`s intersect; `PopClip` restores
  the previous clip. `FrameBuilder` enforces matched push/pop pairs
  per submission (asserts in debug if a view's display list ends
  with an unbalanced stack).
- No Tier-3 producer for `PushTransform` / `PopTransform` — the
  ScrollView migration (Phase 3.6) uses `PushClip` only; the
  scroll translation flows through `contentOffset()` per §9.3.
  `DisplayListReplay`'s transform-op arms log a warning if hit and
  otherwise no-op.
- Validator: a fabricated `DisplayList` in a unit test that pushes
  a clip, emits a shape that extends beyond the clip, pops, and
  emits a second shape outside the original clip. Replay produces
  the expected clipped + unclipped output.

Files touched: `wtk/include/omegaWTK/Composition/Canvas.h`,
`wtk/src/Composition/Canvas.cpp`, `wtk/src/Composition/DisplayList.cpp`,
backend-specific clip plumbing in `wtk/src/Composition/backend/`.

##### Phase 3.6 — `ScrollView` migration [DONE]

ScrollView becomes the first `PushClip` producer and the first
consumer of `FrameBuilder::contentOffset` (the Arrange-time hook
§9.3 specifies). The two overlay scroll-bar layers + their canvases
go away.

- `ScrollView::paint()` (added in this phase — ScrollView currently
  drives painting through subview composition, not a paint method)
  emits `DrawOp::makePushClip(finalRect())` at the start, then
  relies on `FrameBuilder` to recurse into the content child, then
  emits `DrawOp::makePopClip()` at the end. Overlay scroll bars
  emit after the pop as two `DrawOp::RoundedRect`s.
- `ScrollView::contentOffset() const override` returns
  `-scrollOffset_`. `FrameBuilder` reads it when entering the
  ScrollView's subtree and folds the offset into the accumulator
  (Phase 3.4 stack).
- `ScrollView::scrollOffsetContribution` deleted (it was the
  pre-FrameBuilder hook; the accumulator does the job now).
- `vScrollBarLayer` / `hScrollBarLayer` / `vScrollBarCanvas` /
  `hScrollBarCanvas` members deleted from `ScrollView::Impl`. The
  scroll-bar styling fields (`vBar_`, `hBar_`) stay; the bars are
  authored declaratively and emitted as DrawOps in paint.
- `ScrollView::wantsLayer() const override { return true; }`
  introduced (`bool View::wantsLayer() const { return false; }`
  becomes a virtual on View). The boolean is a *layer tag* — Tier 3
  doesn't yet act on it, but a future compositor-thread scrolling
  pass reads it.
- The window-scoped flag is flipped on for a new
  `ScrollViewClipTest` exercising vertical and horizontal scroll
  with content larger than the viewport. Off-flag baseline matches
  on-flag output.

Files touched: `wtk/include/omegaWTK/UI/ScrollView.h`,
`wtk/src/UI/ScrollView.cpp`, `wtk/src/UI/FrameBuilder.{h,cpp}`,
`wtk/include/omegaWTK/UI/View.h` (the new virtual), new
`wtk/tests/ScrollViewClipTest/`.

##### Phase 3.7 — `DisplayListReplay` real implementation for `NativeContent` (carve-out) [DONE]

The Phase 2.5 no-op gets real backend handling. Each platform
compositor turns the carve-out into the right local primitive (CA
sublayer ordering on macOS, DirectComposition visual on Windows,
subsurface on Wayland). The `hostId` plumbs through so the platform
side knows which native layer the rect belongs to.

- `Canvas` gains `markNativeContentRegion(Rect, uint64_t hostId,
  int zOrderHint)`. Records the carve-out in the per-frame state;
  the backend `renderToTarget` switch translates it to the platform
  primitive on flush.
- Backend impls: `wtk/src/Composition/backend/mtl/` (CA layer
  ordering against a `CALayer` keyed by `hostId`),
  `wtk/src/Composition/backend/dx/` (DirectComposition visual
  insertion), `wtk/src/Composition/backend/vk/` (Wayland subsurface
  or X11 child window).
- `DisplayListReplay`'s `NativeContent` arm calls
  `canvas.markNativeContentRegion(...)`.
- Coordinated with `NativeViewHost-Adoption-Plan.md` Phases V2 / G2
  (frame sink → native surface). Validator surface: VideoView's
  hardware video path (once `VideoViewPlaybackTest`'s pre-existing
  link break is fixed) and the future GTEView direct-present.
- Z-order hint: in this tier, ascending `zOrderHint` means later /
  on-top. Tier-4-or-later may extend (multiple z-order buckets per
  view, per-platform mapping refinements).
- Validator: `VideoViewPlaybackTest` (once buildable) plays a video
  through a `NativeViewHost`, and the carve-out leaves the video
  surface visible through the 2D compositor.

Files touched: `wtk/include/omegaWTK/Composition/Canvas.h`,
`wtk/src/Composition/Canvas.cpp`, `wtk/src/Composition/DisplayList.cpp`,
all three backend dirs under `wtk/src/Composition/backend/`.

##### Phase 3.8 — delete per-view `LayerTree` + per-view `Canvas`; remove the flag [DONE]

The destructive phase. Every opt-in consumer from 3.2 / 3.3 / 3.6
now runs unconditionally; the per-view path is removed; the flag is
deleted. Tier 3's payoff lands here.

- Remove `View::Impl::ownLayerTree`. `View::getLayerTree()` either
  returns the window's `LayerTree` (preserving the API surface for
  the few internal callers that need it) or is itself removed —
  decide during the phase based on the §8 grep sweep result.
- Remove `View::makeLayer`, `View::makeCanvas` from the public
  `View` surface. `CanvasView` is the only public caller (deleted
  in Phase 3.9, so it must be sequenced first — see ordering
  rationale below).
- Remove `UIView::Impl::rootCanvas`. The Phase 3.2 opt-in becomes
  the only path through `UIView::update()`.
- Remove `SVGView::svgCanvas`. The Phase 3.3 opt-in becomes the
  only path through `SVGView::paint()`.
- Remove `ScrollView::vScrollBarLayer` / `hScrollBarLayer` (already
  done in 3.6, but the per-view ownership scaffolding around them
  in `View::Impl` goes away here).
- Remove the `OMEGAWTK_WINDOW_SCOPED_PAINT` build-time macro and
  the `AppWindow::Impl::windowScopedPaint_` runtime boolean. Every
  call site that read either becomes unconditional.
- Remove `View::startCompositionSession` / `endCompositionSession`
  from the public surface. Internal callers (Phase 2.x left a few
  for backward compat during the transition) are gone after this
  phase.
- Validator: full test sweep — RootWidget multi-view scene,
  SVGViewRenderTest, ScrollViewClipTest, EllipsePathCompositorTest,
  TextCompositorTest, ContainerClampAnimationTest. Every existing
  scene that worked under the flag must work without it.

Files touched: `wtk/include/omegaWTK/UI/View.h`,
`wtk/src/UI/View.Core.cpp`, `wtk/src/UI/ViewImpl.h`,
`wtk/src/UI/UIView.Update.cpp`, `wtk/src/UI/UIView.Core.cpp`,
`wtk/src/UI/UIViewImpl.h`, `wtk/src/UI/SVGView.cpp`,
`wtk/include/omegaWTK/UI/SVGView.h`, `wtk/src/UI/ScrollView.cpp`,
`wtk/include/omegaWTK/UI/ScrollView.h`, `wtk/src/UI/AppWindow.cpp`,
`wtk/src/UI/AppWindowImpl.h`, `wtk/CMakeLists.txt` (macro
defaulting).

##### Phase 3.9 — `CanvasView` deletion [DONE]

The Phase 2.6 `[[deprecated]]` markers gave the grep handle. Now
the in-tree callers migrate and the class is removed.

**Status:** Complete (2026-05-21). The four test bundles and the
three validator tests (`DisplayListClipTest`, `ScrollViewClipTest`,
`NativeContentCarveoutTest`) had already moved to `UIView` hosts.
This phase closed out the remaining functional callers:

- `UIView` gained a first-class **image element** (a third
  `Element::Type` alongside `Text` and `Shape`). `UIViewLayout`
  (and the V2 spec) carry an optional `BitmapImage` handle + dest
  rect; `UIView::update()`'s paint loop emits a `DrawOp::Bitmap`
  for it. This is what made the `Image` migration a declarative
  element rather than an imperative `drawImage`.
- The `Image` widget (`Primatives.cpp`) now hosts a `UIView` and
  builds a one-image-element layout in `onPaint`, matching every
  other primitive widget.
- The default `Widget(rect)` constructor reroutes from
  `CanvasView::Create` to `View::Create` (a plain, canvas-free
  `View`). Widgets that draw host a `UIView`; widgets that don't
  (containers) get a bare node.
- `CanvasView.h` / `CanvasView.cpp` deleted; forward decls, the
  `View::submitPaintFrame` doc (now a no-op base), and the
  `FontEngine.h` / `Widget.Paint.cpp` references cleaned up.
- Validated: `ImageRenderTest` samples the real PNG texture
  (900×987) through the new element path; `EllipsePathCompositorTest`,
  `RootWidgetTest`, `TextCompositorTest`, `ContainerClampAnimationTest`,
  and `SVGViewRenderTest` build and run without regression.

- Migrate `wtk/src/Widgets/Primatives.cpp:502`'s `drawImage`
  caller to a `UIView` + `UIViewLayoutV2` element. The widget
  becomes a small UIView host; its `onPaint` builds a layout with
  one image element instead of an imperative `drawImage` call.
- Migrate the four test bundles (`TextCompositorTest`,
  `EllipsePathCompositorTest`, `ContainerClampAnimationTest`,
  `VideoViewPlaybackTest`) similarly. Each had a `viewAs<CanvasView>()`
  call sequence; replace with a `UIView` whose layout is rebuilt
  per `onPaint`.
- Delete `wtk/include/omegaWTK/UI/CanvasView.h` and
  `wtk/src/UI/CanvasView.cpp`.
- Delete the `CanvasView` forward declaration in
  `wtk/include/omegaWTK/UI/Widget.h:17`.
- Update the `CanvasView` references in
  `wtk/include/omegaWTK/Composition/FontEngine.h` and
  `wtk/include/omegaWTK/UI/View.h` doc comments.
- `Widget::Create(rect)` constructor at
  `wtk/src/UI/Widget.Core.cpp:13` (`view(CanvasView::Create(rect))`)
  reroutes to a plain `View::Create(rect)` — the imperative
  draw API is gone, so the default view doesn't need a Canvas
  anymore.
- Validator: all four test bundles + Primatives.cpp still produce
  visually identical output through the new `UIView`-based paths.

Files touched: `wtk/include/omegaWTK/UI/CanvasView.h` (deleted),
`wtk/src/UI/CanvasView.cpp` (deleted),
`wtk/include/omegaWTK/UI/Widget.h`, `wtk/src/UI/Widget.Core.cpp`,
`wtk/src/Widgets/Primatives.cpp`, the four test bundles,
`wtk/include/omegaWTK/Composition/FontEngine.h` (doc),
`wtk/include/omegaWTK/UI/View.h` (doc).

##### Phase 3.10 — `VideoView` (the `View` subclass) deletion

Owned by `NativeViewHost-Adoption-Plan.md`. This plan's role is
purely to confirm the carve-out + `onLayoutResolved` contract held
through the migration and to remove the residual `View` subclass.

- `VideoView` the View subclass deleted (per
  [NativeViewHost-Adoption-Plan.md](NativeViewHost-Adoption-Plan.md)
  Part 1 / V4). The public API moves to `VideoViewWidget` per that
  plan.
- `VideoFrameSink` implementation moves out of the `View` hierarchy
  into a non-`View` internal controller.
- `framebuffer` queue, `videoCanvas`, `queueFrame`,
  `presentCurrentFrame`, `flush` — gone.
- Coordinate the cut with the NativeViewHost adoption plan's
  Phase V4 timing — this plan's Phase 3.10 should land *after* V4,
  not concurrently.
- Validator: video playback works through the native path with the
  `DrawOp::NativeContent` carve-out from Phase 3.7.

Files touched: `wtk/include/omegaWTK/UI/VideoView.h` (deleted),
`wtk/src/UI/VideoView.cpp` (deleted), plus whatever
`NativeViewHost-Adoption-Plan.md` Phase V4 specifies.

#### Phase ordering rationale

- 3.0 first: pure scaffolding. The window-scoped infra has to
  exist before anything can consume it. Independent of every later
  phase; reverts to a no-op delete.
- 3.1 immediately after: the session-lifetime move is the lowest-
  risk centralization. Surfaces compositor-frontend reentrancy
  questions early (some frontends may not allow nested sessions).
- 3.2 then 3.3: the UIView opt-in is the larger and more
  representative validator (every shape variant, the text shaping
  helper, the brush resolution). SVGView's much-larger op count
  per scene becomes the second validator at scale.
- 3.4 after 3.3: the transform accumulator can't be validated
  without at least two views feeding it. With both UIView and
  SVGView on the window path, the offset story has real consumers.
- 3.5 before 3.6: `PushClip` replay needs to work before
  ScrollView can produce it.
- 3.7 in parallel with 3.5 / 3.6: the `NativeContent` backend
  hookup is independent of the clip / scroll work and lives in
  different backend files. Sequencing is "land before 3.8" so the
  flag deletion doesn't break native-embed test paths.
- 3.8 only after 3.2, 3.3, 3.6, 3.7 have landed: every consumer
  must be on the new path before the old path is removed.
- 3.9 after 3.8: `CanvasView`'s deletion is independent of the
  per-view Canvas removal (it's its own class), but sequencing it
  after 3.8 means there's only one Canvas instance left to think
  about (the window canvas) when the migration touches each test
  bundle.
- 3.10 last and externally-paced: gated on
  `NativeViewHost-Adoption-Plan.md` Phase V4. Could in principle
  land before 3.8 if V4 happens first, but the carve-out backend
  in 3.7 is its real prerequisite from this plan.

#### What Tier 3 explicitly does NOT do

- Does not introduce `SceneNode`, `LayoutManager`, or `PaintContext`
  (Tier 4).
- Does not introduce `DirtyBits` (Tier 4).
- Does not change `BackendRenderTargetContext::renderToTarget`'s
  switch dispatch (Tier 4 — still dispatches on `VisualCommand`).
- Does not delete the `Canvas` class itself (Tier 4 — Tier 3
  collapses *N* per-view Canvases into *one* per-window Canvas,
  but the class survives as the `DisplayList → VisualCommand`
  adapter until Tier 4).
- Does not delete `VisualCommand` or `CanvasFrame` (Tier 4).
- Does not remove `UIView::update()` (Tier 4 — Tier 3 keeps the
  method but routes its output through `FrameBuilder`).
- Does not migrate animation state out of `UIView::Impl` (Tier 4 +
  Animation-API-Simplification-Plan prereq).
- Does not introduce `PushOpacity` / `PopOpacity` or
  `PushEffect` / `PopEffect` ops (deferred to Tier 4+ when a
  producer appears).
- Does not remove `View::computeWindowOffset` (kept as a thin
  wrapper in 3.4; final deletion is Tier 4).

### Tier 4 — collapse the compositor op type, make `View` the scene node, retire `UIView::update`

- `View` becomes the scene node *in place* (§6 Q1: "View is a scene
  node"). It gains a parent-owned `LayoutManager*`, a virtual
  `paint(PaintContext&)`, and load-bearing `DirtyBits`. There is **no
  new `SceneNode` type and no `UIViewNode` rename** — `UIView` stays a
  `View` subclass with a real `paint()`, and `Widget` stays a light
  wrapper around `View` (§6 Q2). `View::Impl::subviews` stays as the
  node's child list.
- **All child-node layout collapses onto a parent-owned
  `LayoutManager*`.** This retires *three* parallel systems (§1.5):
  `ViewResizeCoordinator` (resize policies), `Container::layoutChildren`
  (clamp policy), and `StackWidget::layoutChildren` (flexbox). The
  built-ins are `FillLayout` / `StackLayout` / `AbsoluteLayout` /
  `ContainerLayout` / `FlexLayout` (the last is required, not "later" —
  `StackWidget` consumes it). `LayoutManager` is the child-*node* axis
  only; intra-`UIView` element layout stays in `UIViewLayoutV2`.
- `UIView::update()` removed; frames are produced by `FrameBuilder`
  reading `DirtyBits` at the root and running Measure → Arrange →
  Paint.
- Animation state migrated out of `UIView::Impl` into a new per-window
  `AnimationScheduler`. **This tier folds the scheduler build in**
  (Phases 4.3–4.4, 4.8) rather than gating on a separate workstream;
  the scheduler's *internal* design stays owned by
  [Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md) and Tier 4
  only sequences and integrates it.
- `VisualCommand` and `CanvasFrame` deleted.
  `BackendRenderTargetContext::renderToTarget` rewritten to dispatch on
  `DrawOp` directly (mechanical case-label rename + field-access
  touch-up; rasterization code unchanged — `DrawOp` already mirrors
  `VisualCommand::Type` 1:1 from Tier 2).
- **`Canvas` class deleted.** With `VisualCommand` and `CanvasFrame`
  gone, Canvas's sole role (build `VisualCommand` into `CanvasFrame`)
  is gone too. The window-scoped Canvas adapter from Tier 3 is
  removed; the per-window `DisplayList` is consumed by `renderToTarget`
  directly.
- `CompositorClient::pushFrame(CanvasFrame, …)` deleted; replaced by a
  `submitDisplayList(DisplayList&&, …)` (or equivalent) that goes
  straight into `CompositeFrame::WidgetSlice` packing without the
  Canvas-shaped intermediate.

Risk: high; this is the API break. Ship after Tier 3 has been in main
for at least two weeks of real use.

#### Tier 4 phasing

Tier 4 is the API break and the largest deletion of the project, but
its risk is concentrated differently from Tier 3. Tier 3 ran two paint
paths concurrently behind a flag; Tier 4 is mostly **mechanical type
swaps** (the `Canvas`/`VisualCommand` → `DrawOp` collapse) and
**one-time migrations** (animation onto the scheduler, layout onto
`LayoutManager`, the `update()` monolith into `paint()`). It is broken
into nine phases in two blocks, ordered per the developer's decision
to do the **backend collapse first and the scene reshape last**:

- **Block 1 — backend / `Canvas` collapse (Phases 4.0–4.2).** Gated
  only on closing Tier 3 (Phases 3.9 + 3.10). Independent of the
  animation work. Deletes `Canvas`, `VisualCommand`, `CanvasFrame`,
  `CanvasEffect`, `DisplayListReplay`, and the `VisualCommand`
  backend switch.
- **Block 2 — scene reshape + animation migration (Phases 4.3–4.8).**
  Folds in the `AnimationScheduler`, collapses all child-node layout
  (the resize coordinator, `Container` clamp, and `StackWidget` flex)
  onto `LayoutManager`, introduces `PaintContext`, makes `DirtyBits`
  drive the frame loop, retires `UIView::update()`, and finally deletes
  the per-view `LayerTree` and the old `ViewAnimator`/`LayerAnimator`
  surface.

Each phase is independently shippable. As in Tier 3, the additive
phases (the parallel `DrawOp` switch in 4.0, the side-by-side
`AnimationScheduler` in 4.3) land and bake before their destructive
counterparts, so a per-phase revert is a small delete rather than a
rollback.

**Cross-cutting decisions** (apply to every Tier 4 phase):

- **No feature flag this tier.** Tier 3 needed
  `OMEGAWTK_WINDOW_SCOPED_PAINT` because it ran the old and new paint
  paths concurrently. Tier 4's changes don't: a parallel backend
  switch (4.0) and a side-by-side scheduler (4.3) give the same
  per-phase revert safety through additive scaffolding, without a
  runtime knob. The flag was deleted in Phase 3.8 and does not return.
- **The API break is expected and accepted (§6 Q1).** `UIView::update`,
  `startCompositionSession`/`endCompositionSession`, `makeLayer` /
  `makeCanvas` / `getLayerTree`, `computeWindowOffset`, and
  `scrollOffsetContribution` all leave the public surface across this
  tier. Ship only after Tier 3 (including 3.9/3.10) has been in main
  ~2 weeks.
- **`View` is the scene node; no new type, no rename (§6 Q1).** `View`
  gains the scene-node responsibilities in place (`LayoutManager*`,
  `paint(PaintContext&)`, load-bearing `DirtyBits`); `UIView` stays a
  `View` subclass; `Widget` stays a light `View` wrapper (§6 Q2). The
  §3 prose's `SceneNode` / `UIViewNode` names are superseded by these
  answers.
- **The `AnimationScheduler` is folded into this tier (4.3–4.4, 4.8)
  but its internals are owned by
  [Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md).** Tier 4
  sequences and integrates the scheduler (Tiers A/B/C/E of that plan
  become Phases 4.3/4.4/4.8 here); it does **not** re-specify the
  scheduler's API. Keep the two docs in sync as the work lands.
- **`LayoutManager` owns child-*node* layout; `UIViewLayoutV2` owns
  intra-`UIView` *element* layout.** These are two different axes (§3.3)
  and Tier 4 keeps them separate — `LayoutManager` arranges a node's
  child `View`s / `Widget`s (4.5/4.6); the elements inside one `UIView`
  stay resolved inside `UIView`. Conflating them is part of the §1.5
  "union of three layout systems" this tier dismantles.
- **`LayoutManager`s are reusable built-ins, one per container *kind* —
  not private to any widget.** Only `Container` widgets have children
  (the only widgets that do), so the `LayoutManager` family mirrors the
  container-widget family: `StackWidget` ↔ `FlexLayout` now; future
  `Grid` ↔ `GridLayout`, `Table` ↔ `TableLayout`, `Tree` ↔ `TreeLayout`.
  A container holds a `LayoutManager*` and composes the matching
  strategy rather than reimplementing layout, so a new container kind is
  "add a `LayoutManager`," not "write another `layoutChildren()`." A
  leaf node carries the trivial default (`AbsoluteLayout`); it has no
  children to arrange.
- **Tier 4 needs a Style *pass*, not the Style *refactor*.** Pure paint
  (4.7) means style is resolved before paint, not during it. Tier 4
  hoists the existing inline resolution into a pre-paint Style pass; the
  global `StyleSheet` / `StyleResolver` / `ComputedStyle` work in
  `Style-StyleSheet-Refactor-Plan.md` is a separate, parallel
  workstream that later swaps that pass's internals (see §7).
- **The `DisplayList` snapshot is the UI-thread → compositor-worker
  hand-off (§6 Q5).** Measure / Arrange / Paint and the scheduler tick
  all run on the UI thread; `submitDisplayList` (4.1) is the boundary
  the existing compositor worker thread consumes. Tier 4 moves none of
  those passes off the UI thread.
- **Zero layerization (§6 Q3).** `wantsLayer()` (Tier 3.6) stays a tag
  only; Tier 4 does not act on it. `ScrollView` remains the sole
  `true` returner.
- **`DirtyBits` already exists** as a `View` enum + `View::Impl::dirtyBits_`
  field (landed in Tier 3 as the Widget-View-Paint-Lifecycle Tier A
  skeleton). Phase 4.7 makes it *load-bearing* — upward propagation to
  the root + `FrameBuilder` gating on it — rather than re-introducing
  it.

##### Pre-flight: close Tier 3 + settle the scheduler design

Before Phase 4.0, resolve the items this tier trips on mid-flight:

- **Close Tier 3 Phases 3.9 (`CanvasView` deletion) and 3.10
  (`VideoView` deletion).** These remove the last `makeCanvas` /
  per-view-`Canvas`-frame consumers; until they land, `Canvas` cannot
  be deleted in 4.2. Confirm by grep that the only remaining
  `makeCanvas` callers (`CanvasView`, `VideoView`, and the
  `ScrollViewClipTest` / `NativeContentCarveoutTest` /
  `DisplayListClipTest` harnesses) are gone or migrated, and the only
  remaining `Canvas` instance is `AppWindow::Impl::windowCanvas_`.
- **Confirm the Animation-Scheduler-Plan Tier A design is settled**
  (it is currently "Proposal. Nothing implemented yet"), since 4.3–4.4
  fold it in. If it is still in flux, Block 1 (4.0–4.2) can proceed
  independently while it settles — the two blocks share no code.
- **Grep sweep for external/client callers** of every about-to-be-
  deleted public method: `makeLayer`, `getLayerTree`,
  `startCompositionSession`, `endCompositionSession`,
  `computeWindowOffset`, `scrollOffsetContribution`, `UIView::update`.
  Any caller outside `wtk/src/` and the in-tree tests is a
  migrate-first item folded into the relevant phase.
- **Confirm whether anything still calls `View::makeLayer`** after
  3.9/3.10. The animation system reaches `getLayerTree()->getRootLayer()`,
  not `makeLayer`; if no non-animation consumer of `makeLayer` remains,
  it can be deleted in 4.2 alongside `makeCanvas`. Otherwise it carries
  to 4.8 with `getLayerTree`.

###### Pre-flight resolution (2026-05-29 sweep)

Ran the four gates above against the tree. Results:

- **Gate 1 — Tier 3 not fully closed.** 3.9 (`CanvasView`) is done; no
  `CanvasView` symbol survives. **3.10 (`VideoView`) is NOT done** —
  `VideoView.{h,cpp}` still exist as live `Canvas` consumers
  (`makeCanvas` at `VideoView.cpp:51`, `videoCanvas->drawImage/sendFrame`,
  four `start/endCompositionSession` pairs). Its upstream,
  `NativeViewHost-Adoption-Plan.md` Phase V4, is unimplemented (that doc
  is still "Proposal"). VideoView is live code (linked into the UI lib;
  `VideoViewPlaybackTest` is a registered target).
  - **Decision: unblock VideoView via Path B (cheap), not the full
    NativeViewHost adoption.** VideoView's only `Canvas` dependency is
    the per-frame software-decoded `BitmapImage` blit — structurally
    identical to the `Image` element Phase 3.9 already migrated to a
    `DrawOp::Bitmap`. So VideoView routes its per-frame frame through the
    window `DisplayList`/`FrameBuilder` (`submitView`) as a
    `DrawOp::Bitmap` and drops `videoCanvas` / `makeCanvas` /
    `start/endCompositionSession`. This removes the only blocker on
    Phase 4.2 in ~1 file, keeps software-decode video working, and is
    Metal-verifiable on this host. The native-layer perf migration
    (NativeViewHost-Adoption-Plan V1–V4) becomes an independent future
    effort, no longer gating Block 1.
  - The three Tier-3 validator harnesses (`ScrollViewClipTest`,
    `NativeContentCarveoutTest`, `DisplayListClipTest`) still call
    `view->makeCanvas` + `start/endCompositionSession` +
    `getLayerTree()->getRootLayer()` + `DisplayListReplay::replay`
    directly. **Decision: retire them in Phase 4.2**, not migrate.
    Tests link only the public library (no `src/UI` include path), so
    `FrameBuilder::submitView` is unreachable from a test — there is no
    public way to inject a hand-built `DisplayList` into the window
    frame, and their clip/carve-out ops aren't expressible through
    `UIViewLayoutV2`. They are Tier-3 scaffolding for features already
    in main (clip is exercised by the real `ScrollView` path,
    carve-out by video), so they are deleted alongside
    `Canvas`/`DisplayListReplay` in 4.2 rather than rebuilt against a
    new public test surface.
- **Gate 2 — Animation-Scheduler-Plan Tier A unsettled.** Still
  "Proposal. Nothing implemented yet." Per the escape hatch, this gates
  only Block 2 (4.3–4.8); **Block 1 (4.0–4.2) proceeds independently**.
- **Gate 3 — no external/client callers.** Every caller of `makeLayer`,
  `makeCanvas`, `getLayerTree`, `start/endCompositionSession`,
  `computeWindowOffset`, `scrollOffsetContribution`, and `UIView::update`
  lives in `wtk/src/` or in-tree `tests/`. No migrate-first external
  items.
- **Gate 4 — `makeLayer` resolved: deletable in 4.2.** Zero callers
  remain (only the decl at `View.h:154` and impl at `View.Core.cpp:145`).
  It is deleted in 4.2 alongside `makeCanvas`, **not** carried to 4.8.
  For contrast, `getLayerTree` *does* survive to 4.8 — `WidgetTreeHost`'s
  layer-tree observer machinery, `UIView.Animation.cpp`, and
  widget-detach in `BasicWidgets.cpp` still consume it.

**Net:** Block 1 is clear to start after the unblock work (VideoView
Path B + the 3 harness migrations). Block 2 waits on the scheduler plan.

##### Phase 4.0 — backend `renderToTarget` gains a `DrawOp` switch (additive) [DONE]

**Status:** Complete (2026-05-29; Metal build verified, full project links).
The 470-line `renderToTarget(VisualCommand::Type,…)` body became a private
template `renderPrimitiveImpl<ParamsT>(PrimitiveOp,…)` (binds both
`VisualCommand::Params` and `DrawOp::Params` via shared member names);
the post-switch draw tail was extracted to `drawTriangulatedResult(…)`;
the `VectorPath` case became `renderVectorPathSegmented(…)` fed by
`Path::decomposeForDraw` (the rehomed `Canvas::drawPath` decomposition).
Two thin public overloads (`VisualCommand::Type` and `DrawOp::Type`) map
their enum onto `PrimitiveOp` + the divergent ops: `SetClip`→`applySetClip`,
`PushClip`/`PopClip`→a backend clip stack (`pushDrawOpClip`/`popDrawOpClip`,
rehoming `Canvas::pushClip`'s intersection), `PushTransform`/`PopTransform`
no-op. The `VisualCommand` path is behaviorally unchanged (code moved, not
rewritten); the `DrawOp` overload is unexercised until 4.1. Verified:
full Metal build links clean (D3D12/Vulkan not buildable on this host;
runtime pixel parity not yet run — no display).
Files touched: `RenderTarget.{h,cpp}`, `Path.{h,cpp}` (decomposeForDraw),
`Canvas.cpp` (drawPath delegates to the shared helper).

The backend's GPU dispatch is taught to consume `DrawOp` directly, as
a sibling to the existing `VisualCommand` path. Because `DrawOp`'s
variant set already mirrors `VisualCommand::Type` 1:1 (Tier 2 designed
it that way), the new switch is a mechanical clone: each arm reads the
matching `DrawOp::Params` field instead of `VisualCommand::Data` and
calls the same rasterization helper. No rasterization code changes.

- Factor the per-primitive rasterization bodies inside
  `BackendRenderTargetContext::renderToTarget(VisualCommand::Type, void *)`
  (`wtk/src/Composition/backend/RenderTarget.cpp`) into private helpers
  keyed by primitive, so both switches share them — no SDF /
  tessellation / bitmap / text code is duplicated.
- Add `BackendRenderTargetContext::renderToTarget(DrawOp::Type, void *)`
  whose arms call those shared helpers.
- `PushClip` / `PopClip` map to the scissor path Phase 3.5 added;
  `NativeContent` to the Phase 3.7 carve-out; `PushTransform` /
  `PopTransform` / `PushOpacity` / `PopOpacity` stay no-op (no
  producer yet — same as Tier 3).
- Nothing calls the `DrawOp` overload yet; the `VisualCommand` path is
  still the only one exercised.
- Validator: full build on all three backends; every existing scene
  renders unchanged. A backend unit test feeds a hand-built
  `DisplayList` straight to the `DrawOp` switch and confirms identical
  output to the equivalent `VisualCommand` frame.

Files touched: `wtk/src/Composition/backend/RenderTarget.cpp`,
`wtk/include/omegaWTK/Composition/DisplayList.h` (if a `DrawOp::Type`
accessor is needed).

###### Phase 4.0 design resolution (2026-05-29) — "mechanical clone" is only ~90% true

Reading the actual `renderToTarget(VisualCommand::Type, void *)`
(`RenderTarget.cpp:1280`) against `DrawOp` revealed two corrections to
the prose above:

1. **The `Type` enums are NOT identical**, so the "clone" can't be a
   copy-paste with relabeled cases. `VisualCommand::Type` has `SetClip`
   (single pre-resolved rect) and legacy `Text` (no-op); `DrawOp::Type`
   has `PushClip`/`PopClip`/`PushTransform`/`PopTransform` and no `Text`.
   The *params sub-structs* (`rectParams`, `bitmapParams`, …) do match
   1:1 by name/type, though.
   - **Chosen factoring:** rename the 470-line body to a private
     **template** `renderPrimitiveImpl<ParamsT>(PrimitiveOp, ParamsT*)`
     keyed by a neutral `PrimitiveOp` enum. Because both param types
     share member names, the template binds to both → 100% rasterization
     sharing, zero duplication, for the 9 shared variants (Rect,
     RoundedRect, Ellipse, Bitmap, Shadow, TextRun, SetTransform,
     SetOpacity, NativeContent — all use identically-named members).
     The two public overloads (`renderToTarget(VisualCommand::Type,…)`
     and `renderToTarget(DrawOp::Type,…)`) map their own enum onto
     `PrimitiveOp` and call the template. **Clip is handled in each
     overload, not the template:** `VisualCommand::SetClip` reads
     `clipRect` (pre-resolved) → `applySetClip`; `DrawOp::PushClip`
     reads `pushClipParams.rect` and `PopClip` clears — different
     members + push/pop vs single-set semantics, so they can't share a
     template arm. `PushTransform`/`PopTransform` no-op.
2. **`VectorPath` is genuinely non-mechanical** — the one case the
   "no rasterization code changes" claim misses. `VisualCommand::pathParams`
   is the *segmented* form (`GVectorPath2D` + `strokeWidth`/`contour`/
   `fill`/`brush`/`fillBrush`); `DrawOp::pathParams` is a high-level
   `SharedPtr<Path>` + `Optional<Border>`. The Path→segments+brushes
   decomposition lives in `Canvas::drawPath` (`Canvas.cpp:314-356`),
   which 4.2 deletes.
   - **Chosen home (developer decision):** extract that decomposition
     into a **shared helper** — a `Path` method
     `Path::decomposeForDraw(Optional<Border>)` returning the segmented
     draw form (it needs `Path::impl_` internals). The backend
     `DrawOp::VectorPath` arm calls it and loops segments through the
     existing triangulation body; the transitional `Canvas::drawPath`
     and `DisplayListReplay` can call the same helper so the logic
     isn't duplicated. Mapping confirmed against
     `DisplayListReplay` (`DisplayList.cpp:50`): `DrawOp::VectorPath`
     → `drawPath(*path, border)`, i.e. the border-taking overload's
     semantics (fill = `path.pathBrush`, stroke = `border`).

##### Phase 4.1 — `FrameBuilder` packs the `DisplayList` into the frame directly; `submitDisplayList` replaces the Canvas bridge [DONE]

**Status:** Complete (2026-05-29; full Metal build links clean). `WidgetSlice`
gained a `Composition::DisplayList ops` field (alongside the still-live
`commands`). `CompositorClientProxy::submitDisplayList(DisplayList&&,
windowOffset, bounds)` appends a slice carrying the DrawOps directly — no
`Canvas`/`CanvasFrame`. `FrameBuilder::endFrame` replaced the
`DisplayListReplay::replay(...)+sendFrame()` loop with one
`submitDisplayList` per pending submission (bounds = window-sized,
local-origin; windowOffset = the live per-view offset). Both backend flush
sites — `Compositor::renderCompositeFrame`'s direct loop and all four
`BackendRenderTargetContext::renderBlurredSlice` loops — now iterate
`slice.ops` and dispatch via the Phase 4.0 `renderToTarget(DrawOp::Type)`
switch. `windowCanvas_` is now bypassed (deleted in 4.2). This is the first
runtime exercise of the 4.0 `DrawOp` path. **Verification caveat:** build +
link verified on Metal; runtime pixel parity (and the EllipsePathCompositorTest
stale-rect fix below) NOT yet confirmed — needs a windowed run on a display.
Files: `CompositeFrame.h`, `CompositorClient.{h,cpp}`, `FrameBuilder.cpp`,
`Compositor.cpp`, `backend/RenderTarget.cpp`.

The window `Canvas` was the `DisplayList → VisualCommand` adapter
(Tier 3). This phase routes paint output past it: `FrameBuilder` packs
each submission's `DisplayList` straight into the
`CompositeFrame::WidgetSlice`, and the backend flush reads it via the
Phase 4.0 `DrawOp` switch.

- `CompositeFrame::WidgetSlice` gains a `Composition::DisplayList ops`
  field, alongside its existing `Vector<VisualCommand> commands`
  (kept live this phase).
- Add `CompositorClient::submitDisplayList(DisplayList &&, Point2D
  windowOffset, …)` (or a proxy method) that appends a slice carrying
  the `DrawOp`s + offset without going through `Canvas` / `CanvasFrame`.
- `FrameBuilder::endFrame()` replaces the
  `DisplayListReplay::replay(sub.list, *windowCanvas_)` +
  `windowCanvas_->sendFrame()` pair with
  `submitDisplayList(std::move(sub.list), sub.windowOffset)`.
- The window backend flush walks slices and dispatches their `DrawOp`s
  via the 4.0 switch instead of their `VisualCommand`s.
- `windowCanvas_` is now bypassed but not yet deleted (4.2).
- Validator: full sweep — RootWidget multi-view, SVGViewRenderTest,
  ScrollViewClipTest, EllipsePathCompositorTest, TextCompositorTest,
  ContainerClampAnimationTest — pixel-identical to the `VisualCommand`
  path, **except EllipsePathCompositorTest — see the known bug below,
  which this phase is expected to FIX, not reproduce.**

> **Known pre-existing bug this phase fixes (not "pixel-identical"):**
> In the `Canvas`/`CanvasFrame` bridge, `Canvas::rect` is captured once
> at construction (`Canvas.cpp` ctor: `rect(layer.getLayerRect())`) and
> **never refreshed** — there is no `Canvas::resize` and `View::resize`
> does not notify the canvas. `Canvas::nextFrame()` then stamps
> `frame->rect = rect` (the stale member) even though its own comment
> says it must reflect the *current* layer rect "if the layer resized
> since then." So a view that is laid out / repositioned after its
> canvas was constructed has the **initial rect used as its slice
> bounds**. EllipsePathCompositorTest shows this: its three `HStack`
> children are created at `(0,0)` and correctly repositioned by
> `StackWidget::layoutChildren()` (the layout math + `setRect` run fine),
> but they all render stacked at the origin because the slice bounds
> the compositor receives are the stale initial rect. Phase 4.1's
> `submitDisplayList(sub.list, sub.windowOffset)` carries `DrawOp`s +
> the live `windowOffset` straight into the `WidgetSlice` with **no
> `CanvasFrame::rect` snapshot in the path**, which removes the stale-rect
> artifact. ⇒ The EllipsePathCompositorTest validator for this phase is
> "children laid out side-by-side at their stack positions," NOT parity
> with the (buggy) `VisualCommand` output.

Files touched: `wtk/include/omegaWTK/Composition/CompositeFrame.h`,
`wtk/include/omegaWTK/Composition/CompositorClient.h`,
`wtk/src/Composition/CompositorClient.cpp`,
`wtk/src/UI/FrameBuilder.cpp`,
`wtk/src/Composition/backend/RenderTarget.cpp` (slice flush).

##### Phase 4.2 — split `Canvas.h`: delete `Canvas`/`VisualCommand`/`CanvasFrame`/`DisplayListReplay`, rehome the survivors [DONE]

**Status:** Complete (2026-05-29; full Metal build links, no `Canvas`/
`VisualCommand`/`CanvasFrame`/`DisplayListReplay` symbol remains). Survivors
rehomed: `Border`/`NineSliceInsets`/`ShapedTextRun`/`shapeTextForDisplayList`
→ `DisplayList.{h,cpp}`; `CanvasEffect` → new `CanvasEffect.h` (unchanged —
blur subsystem keeps compiling; layer-based rework deferred to "Phase E").
Deleted: `Canvas.{h,cpp}`, `DisplayListReplay`, the `VisualCommand`
`renderToTarget` overload + `VisualCommandParams` typedef,
`CompositorClient::pushFrame(CanvasFrame)`, `WidgetSlice::commands`,
`AppWindow::Impl::windowCanvas_` (+ `windowCanvas()`), `View::makeCanvas`
**and `View::makeLayer`**, the dead `LayerAnimator::transition(CanvasFrame)`,
`Layer::boundCanvas_`/`hasCanvas()`, and the 3 validator harnesses
(`DisplayListClipTest`/`ScrollViewClipTest`/`NativeContentCarveoutTest`).
`getLayerTree`/`windowLayerTree_` survive to 4.8.

**First-paint stale-layout bug — fixed (2026-05-29; pending runtime
verify).** Root cause: `WidgetTreeHost::initWidgetTree` ran the initial
paint walk (`initWidgetRecurse`) *before* the tree was sized to the
window; the root container's first `StackWidget::layoutChildren` therefore
saw a suspicious/zero frame, bailed (`suspiciousFrame` →
`needsLayout=true`, children left at their constructor origin), and the
window-resize that would relayout them arrives only *after* the walk (and
never for a tree with no resize-opted widgets). Fix (developer's chosen
direction — "ensure root sized before first paint"): `initWidgetTree` now
calls `root->handleHostResize({0,0,windowW,windowH})` before
`initWidgetRecurse`, sizing the views + running the widget layout
(StackWidget::resize → relayout → layoutChildren against a valid frame)
without painting (hasMounted still false), so the paint walk runs once with
children already arranged. Builds clean on Metal; needs a windowed run of
EllipsePathCompositorTest to confirm children render side-by-side.

The destructive end of Block 1. Every producer of `VisualCommand` is
gone (per-view Canvas removed in 3.8; CanvasView 3.9; VideoView
unblocked via Path B in the Tier 4 pre-flight), and the window Canvas
is bypassed (4.1).

> **Scope correction (2026-05-29):** `Canvas.h` cannot simply be
> *deleted* — it must be **split**. Mixed in with the doomed types are
> value types that `DrawOp` / `DisplayList`, the text-paint path, and the
> blur subsystem still depend on: **`Border`** (every shape op's border),
> **`NineSliceInsets`**, **`CanvasEffect`** (+ `GaussianBlurParams` /
> `DirectionalBlurParams` — used by the whole blur pipeline *and* the
> UIView style layer), and **`ShapedTextRun`** + the
> `shapeTextForDisplayList()` free function (used by UIView / SVGView to
> build `DrawOp::TextRun`). `DisplayList.h` even `#include`s `Canvas.h`
> today. So the original "delete `CanvasEffect`" line is wrong — that
> would require ripping out blur. **Decision (developer, 2026-05-29):
> effects become a *layer-based* concept, not a Canvas-based one — that
> rework is its own future phase (see "Phase E — Layer-based effects"
> below). In 4.2 `CanvasEffect` is rehomed *unchanged*** so the blur
> subsystem keeps compiling.

- **Rehome survivors out of `Canvas.h`** so the doomed types can go:
  - `Border`, `NineSliceInsets`, `ShapedTextRun`, and
    `shapeTextForDisplayList()` move into `DisplayList.h` (they are
    `DrawOp` building blocks); `shapeTextForDisplayList`'s body moves
    from `Canvas.cpp` into `DisplayList.cpp`.
  - `CanvasEffect` moves into a new minimal header
    `wtk/include/omegaWTK/Composition/CanvasEffect.h` (transitional home
    until the layer-based-effects phase replaces it). Blur-pipeline and
    UIView-style includers point at it.
  - `DisplayList.h` drops its `#include "Canvas.h"`.
- **Then delete** `Canvas.h` + `Canvas.cpp`: the `Canvas` class,
  `VisualCommand`, `VisualCommand::Data`, and `CanvasFrame`.
- Delete `DisplayListReplay` from `DisplayList.{h,cpp}` (the
  `DrawOp → Canvas` bridge); keep `DisplayList` / `DrawOp`.
- Delete the `VisualCommand::Type` `renderToTarget` overload + the
  `VisualCommandParams` typedef in `RenderTarget.cpp`; the `DrawOp`
  switch (4.0) becomes the only one. (Stale `VisualCommand::…` comments
  in `Pipeline.{h,cpp}` / `RenderPass.h` are cosmetic — update or leave.)
- Remove `CompositorClient::pushFrame(CanvasFrame &, …)` and the
  `Vector<VisualCommand> commands` field on `WidgetSlice`;
  `CompositeFrame.h` drops its `Canvas.h` include.
- Remove `windowCanvas_` from `AppWindow::Impl` (+ ctor wiring + getter).
  The window keeps `windowLayerTree_` (present-layer host until 4.8).
- Remove `View::makeCanvas` **and `View::makeLayer`** (pre-flight grep
  confirmed `makeLayer` has zero callers) + the `Canvas` forward decl.
  `getLayerTree` survives (animation / layer host, removed in 4.8).
- Delete the dead `LayerAnimator::transition(SharedHandle<CanvasFrame>,
  …)` method (no callers; references the doomed `CanvasFrame`) + its
  `Animation.h` decl and the `CanvasFrame` fwd decl.
- Retire the 3 Tier-3 validator harnesses (`DisplayListClipTest`,
  `ScrollViewClipTest`, `NativeContentCarveoutTest`) — they are the only
  remaining `makeCanvas` / `CanvasFrame` / `VisualCommand` consumers
  (per the pre-flight decision). Delete the dirs + their
  `tests/CMakeLists.txt` registrations.
- `UIViewImpl.h` / `Layout.h` / `UIView.h` / `UIView.Core.cpp` keep using
  `CanvasEffect::GaussianBlurParams` / `DirectionalBlurParams` — only the
  include path changes (→ `CanvasEffect.h`). No migration onto DrawOp
  blur params (that belongs to the layer-based-effects phase).
- Validator: full sweep; grep confirms no `Canvas` / `VisualCommand` /
  `CanvasFrame` symbol remains (only `CanvasEffect`, rehomed); clean
  build on all three backends (Metal verifiable on this host).

##### Phase E (future / out of Tier 4 scope) — Layer-based effects

`CanvasEffect` (gaussian / directional blur) is presently a
Canvas-adjacent value type consumed by `BackendCanvasEffectProcessor`
and routed per-slice via `slice.targetLayer` + `renderBlurredSlice`.
The window-scoped DrawOp pipeline (4.1) no longer sets a `targetLayer`,
so blur is currently **dormant**. The proper model (developer decision,
2026-05-29) is to make effects a **layer-based concept** — attached to
a layer / scene node and applied by the compositor against that layer's
backing — rather than a Canvas idea. This phase:
- replaces `CanvasEffect` with a layer-owned effect descriptor (e.g.
  `LayerEffect` blur params) and removes the transitional
  `CanvasEffect.h`;
- re-lights blur on the DrawOp path (a `PushEffect` / `PopEffect`
  DrawOp scope, per §3.2, or a layer-attached effect the FrameBuilder
  threads to `renderBlurredSlice`);
- migrates the UIView style layer's `gaussianBlur` / `directionalBlur`
  onto the new descriptor.
This is **not** part of Tier 4 — it is sequenced after the scene
reshape (Block 2) or as an independent follow-up. Tracked here so 4.2's
"rehome, don't delete `CanvasEffect`" decision has a forward reference.

Files touched (4.2): `Canvas.h` (deleted), `Canvas.cpp` (deleted),
new `Composition/CanvasEffect.h`, `DisplayList.{h,cpp}`,
`backend/RenderTarget.{h,cpp}`, `CompositorClient.{h,cpp}`,
`CompositeFrame.h`, `Compositor.cpp`, `Animation.{h,cpp}`,
`AppWindowImpl.h`, `AppWindow.cpp`, `View.h`, `View.Core.cpp`,
`UIViewImpl.h`, `UIView.h`, `Layout.h`, `UIView.Core.cpp`, `SVGView.cpp`,
`backend/Effect.h` + the per-platform effect processors (include swap),
`tests/CMakeLists.txt` + the 3 deleted test dirs.

##### Phase F (follow-up) — Window resize always relayouts + repaints (no resize opt-in)

**Goal (developer, 2026-05-29):** resizing the `AppWindow` must always
relayout the whole widget tree, resize every widget according to its
layout, and **repaint *every* widget — dirty *and* non-dirty** —
unconditionally, with no per-widget resize opt-in.

**Why all widgets, not just dirty ones (developer, 2026-05-29):** on
resize the platform stretches the existing window surface to the new
size, so any content rasterized at the old size appears **stretched**
until it is re-drawn at the new resolution. Dirty-only repaint is
therefore wrong for resize — a "non-dirty" widget whose model didn't
change still has stale, wrong-resolution pixels. Every widget must
re-emit its `DisplayList` and re-rasterize at the new size. To keep that
full-tree repaint affordable, **non-dirty content is served from a
content cache** (cached geometry / rasterized primitives keyed by shape
params + size) so an unchanged widget is "thrown back up" cheaply rather
than recomputed from scratch — see **Phase G**.

**Current behavior (the gap):** `WidgetTreeHost::notifyWindowResize` /
`notifyWindowResizeBegin` / `notifyWindowResizeEnd` only drive
`root->handleHostResize(rect)` when `anyWidgetOptsIntoResize(root.get())`
is true. So a window resize is a no-op for any tree whose widgets did not
opt in — children keep their pre-resize rects, nothing relayouts, nothing
repaints. (This is the same opt-in gate that left the first-paint fix
needing an explicit `handleHostResize` before the initial walk.)

**This phase:**
- Remove the `anyWidgetOptsIntoResize` gate from all three
  `notifyWindowResize*` paths — `handleHostResize(rect)` runs on every
  window resize regardless of opt-in. `handleHostResize` already sizes the
  root view, runs the widget layout pass (Measure/Arrange →
  `LayoutManager`/`StackWidget::relayout` → child `setRect`), and
  invalidates; the resize walk should propagate that down the whole tree.
- Resize relayout is driven by the parent's `LayoutManager` (Tier 4 Block
  2, 4.5/4.6): each container re-arranges its children to the new
  available rect; leaves resize per their `LayoutStyle`.
- **Resize forces a full-tree repaint, independent of `DirtyBits`.** A
  resize marks the whole tree for repaint (not just nodes the relayout
  dirtied) so every widget re-emits its `DisplayList` and re-rasterizes
  at the new size. `DirtyBits` still governs *non-resize* frames (paint
  only what changed); resize is the one case that overrides it with
  "repaint everything." `FrameBuilder` submits the whole tree into the
  one resize frame (the `dispatchResize*ToHosts` `ScopedFrame` already
  brackets this).
- The full repaint stays cheap because unchanged widgets pull cached
  geometry / rasterized content from the **Phase G** content cache rather
  than re-tessellating / re-shaping from scratch.
- Retire `anyWidgetOptsIntoResize` and the per-widget
  `invalidateOnResize` opt-in once the unconditional path is the only one
  (or keep `invalidateOnResize` as a paint-suppression hint only, never as
  a relayout gate).
- Validator: a windowed resize of a multi-widget scene (e.g. the
  RootWidget multi-view scene, or `LayoutResizeStressTest`) relayouts and
  repaints continuously through the drag, with children tracking the new
  window size and content crisp (not stretched) at every intermediate
  size — no widget having opted in.

Sequencing: depends on Block 2's `LayoutManager` (4.5/4.6) for relayout
and on **Phase G** for the cache that makes full-tree repaint affordable,
but the gate removal + force-full-repaint-on-resize itself is independent
and can land earlier as an interim fix (correctness first; the cache is
the perf optimization layered on top).

##### Phase G (follow-up) — Content cache: geometry / primitive / tessellation reuse

**Goal:** make full-tree repaint (Phase F's resize path, and any
broad invalidation) cheap by caching a widget's rendered content so an
unchanged widget is re-emitted from the cache instead of recomputed.
Relocates and supersedes **Render-Execution-Efficiency-Plan.md §4 "No
geometry caching"** (and complements its §5 dirty-region note) into the
post-Canvas DrawOp world.

**What gets cached (keyed by shape parameters + resolved size /
renderScale):**
- **Tessellation cache** — the triangulated mesh for `DrawOp::VectorPath`
  (and gradient-fallback Rect/RoundedRect). Today the backend
  re-triangulates every shape every frame
  (`renderVectorPathSegmented` → `triangulateSync`); cache the
  `TETriangulationResult` keyed by `(path-hash, strokeWidth, fill,
  contour, size)` so an unchanged path reuses its mesh. (SDF primitives —
  Rect/RoundedRect/Ellipse/Shadow with a color brush — are already
  6-vertex quads with no triangulation, so they need no geometry cache;
  they only re-author trivially.)
- **Primitive / content cache** — the rasterized output (e.g. a cached
  GPU texture / "tile") of a widget's `DisplayList` at a given size, so a
  non-dirty widget on a full repaint blits its cached content instead of
  re-issuing draw ops. This is the "cache the non-dirty content so it can
  be thrown back up again" mechanism Phase F relies on.
- **Text shaping cache** — `shapeTextForDisplayList` output keyed by
  `(text, font, size, layout)` (optional; biggest win for static labels).

**Invalidation:** a cache entry is keyed so that a model change (new shape
params) or a size/renderScale change misses and recomputes; resize misses
the *size* dimension for shapes whose pixel geometry actually changes but
hits for size-invariant content. The cache is bounded (LRU) and purged
with the owning layer/node (mirrors `RenderTargetStore::cleanTreeTargets`).

**Why its own phase:** the cache is valuable beyond resize — it removes
per-frame re-tessellation/re-shaping for *all* frames (the
Render-Execution-Efficiency-Plan §4 concern) — and it is a sizable
backend addition (cache structures, hashing, eviction, lifetime). Phase F
ships correctly without it (full repaint, just more expensive); Phase G
is the performance layer. *(Alternative considered: fold the cache into
Phase F. Kept separate so Phase F stays a contained correctness change
and the cache can be designed against the whole repaint workload, not
just resize. Merge if the two land together.)*

Sequencing: after Phase F (which defines the full-repaint workload the
cache optimizes) and after the backend DrawOp path (4.0–4.2, done).

##### Phase H (follow-up) — Frame pacing: real `FrameTime` + load-aware frame gating (folds Frame-Pacing-Plan)

**Goal:** Replace Phase 4.3's interim `steady_clock` `FrameTime` stand-in
with a real per-window frame pacer, and add cooperative backpressure so
the FrameBuilder *defers a non-critical frame before it is built* rather
than dropping it after the GPU work is wasted. This folds
[Frame-Pacing-Plan.md](Frame-Pacing-Plan.md) into the post-Tier-4 frame
loop.

**Ownership (same split as the scheduler).** Frame-Pacing-Plan.md owns
the *mechanism* — `PaceHint`, the per-lane time-windowed
`FramePacingMonitor` (100ms quantised windows, asymmetric hysteresis,
discontinuity reset), and the two-layer inner-loop (`waitForLaneAdmission`)
/ outer-loop (pace hint) architecture. This phase only **sequences and
re-homes** its integration points onto the SceneNode / FrameBuilder /
`DirtyBits` pipeline, exactly as Tier 4 folds in the `AnimationScheduler`.
It does not re-specify the monitor's internals.

**Why it needs re-homing (the pacing plan predates Tier 4).** The plan's
producer-side integration (its Phases 3–5) is written against the old
per-view paint model and must move:

- The plan hooks `Widget::executePaint(PaintReason, immediate)` to consult
  `view->compositorPaceHint()` and defer via `hasPendingInvalidate`. After
  Phase 4.7, `executePaint` is no longer a paint driver — it is "mark
  `DirtyBits` + request a frame." So the pace check moves up to the
  **frame-request / `FrameBuilder::buildFrame` gating** point: when an
  invalidation requests a frame, the window reads the `PaceHint` and either
  runs the Measure → Arrange → Paint pass or **leaves the dirty bits set
  and skips this frame's build**. `DirtyBits` already coalesce
  (invalidations between frames union into one paint), so deferral is
  "skip the build, the bits persist" — no separate `hasPendingInvalidate`
  flag, and the next admitted frame drains them naturally.
- The plan's `PaintReason`-based `isPaceCritical` / `isPaceDeferrable`
  classification maps onto the new world: a **resize** frame (Phase F's
  forced full-tree repaint) is pace-critical and never deferred; a
  **Paint-only animation tick** (`DirtyBit::Paint` set by the scheduler)
  is deferrable under `Saturated`; first paint is never deferred.
- The plan's Motivation talks about wasting a recorded `VisualCommand`
  list / `CanvasFrame` snapshot. Those types are gone (Block 1); the work
  now avoided is *building a `DisplayList` and `submitDisplayList`-ing it*.
  The principle — don't build a frame the inner loop would just block on —
  is unchanged.

**Concrete connection to what already landed:**

- **The pacer becomes the source of `AnimationScheduler::tick`'s
  `FrameTime` (closes the 4.3 seam).** `FrameTime{monotonicNs, frameIndex}`
  is exactly what a vsync-aligned pacer produces; it replaces 4.3's
  `steadyFrameClockNs()` + the `FrameBuilder` frame counter. Until this
  phase lands, the `steady_clock` stand-in is correct-but-free-running.
- **Animation-aware pacing** (Frame-Pacing-Plan "Future extensions"): the
  `AnimationScheduler` self-regulates, so a live animation's Paint frames
  bypass throttling and pacing never causes animation stutter. The
  scheduler already knows its active set (`hasAnyAnimationFor` /
  `stats().activeProperty`), so it can mark its tick-driven frames
  pace-critical instead of inventing a `PaceHint::Override` packet flag.

**Inner loop unchanged.** Lane admission (`waitForLaneAdmission`) stays as
the per-frame hard GPU-safety gate; this phase only adds the outer-loop
time-windowed `PaceHint`. Per the pacing plan, it also assumes the stale
frame-coalescing removal (`Stale-Frame-Coalescing-Removal-Plan.md`) has
landed.

Sequencing: the monitor + query side (Frame-Pacing-Plan Phases 1–2, on
`Compositor` / `CompositorClientProxy`) is architecture-agnostic and can
land independently/earlier; the producer-side gating (its Phases 3–5)
depends on Phase 4.7's `FrameBuilder` `DirtyBits` loop being the single
frame entry point, and complements **Phase F** (resize = pace-critical)
and **Phase G** (the content cache makes throttled/deferred repaints
cheap). Files: the monitor/hint side per Frame-Pacing-Plan's own file
table (`Compositor.{h,cpp}`, `CompositorClient.{h,cpp}`); the producer
side re-homes from `Widget.Paint.cpp` to `FrameBuilder.{h,cpp}` +
`AppWindow.cpp` (frame-request gating) and `AnimationScheduler.{h,cpp}`
(`FrameTime` source + animation-aware override).

##### Phase 4.3 — `AnimationScheduler` lands alongside the old animator (folds Animation-Scheduler-Plan Tier A) [DONE]

**Status:** Complete (2026-05-29; full build succeeded). `AnimationScheduler`
landed **UI-private** at `wtk/src/UI/AnimationScheduler.{h,cpp}` (developer
decision — overrides Animation-Scheduler-Plan §4's public `Composition`
placement; matches `FrameBuilder`), reusing the existing `KeyframeTrack` /
`KeyframeLerp` / `AnimationHandle` / `TimingOptions` from
`Composition/Animation.h`. `AppWindow::Impl` owns one next to `frameBuilder_`;
`FrameBuilder::beginFrame()` (depth 0) ticks it once per outermost frame under
a `ScopedPhase(Tick)`. The tick runs the real keyframe math (delay / duration /
playbackRate / iterations / the four `Direction`s) against the existing
`KeyframeLerp` specializations, writing the `(NodeId, PropertyKey, subIndex)`
side table (property anims) or firing `apply()` (callback anims). One additive
edit to the public header: `friend class ::OmegaWTK::AnimationScheduler` on
`AnimationHandle` so `tick` can advance handle state/progress. Deviations from
the plan, all flagged: `FrameTime` is an interim `steady_clock` + per-`FrameBuilder`
frame-counter stand-in (real pacer is the new **Phase H** above); the side
table **clears on completion** (developer call — `FillMode` "hold final value"
retention deferred while WML is still a doc); node-dirty marking is a documented
4.4/4.7 seam (no `NodeId`→`View` registry yet, and `DirtyBits` are not
load-bearing until 4.7), with `layoutAffecting` already carried per active so
4.7 only resolves the node and ORs the bits. Additive only — the
`ViewAnimator` / `LayerAnimator` path is untouched and still drives every
animation; the templated property/callback API is unexercised until 4.4.
Files: new `wtk/src/UI/AnimationScheduler.{h,cpp}`,
`wtk/include/omegaWTK/Composition/Animation.h`, `wtk/src/UI/AppWindowImpl.h`,
`wtk/src/UI/AppWindow.cpp`, `wtk/src/UI/FrameBuilder.{h,cpp}`.

A per-window `AnimationScheduler`, owned by `AppWindow` next to
`FrameBuilder`, ticks active animation tracks once per frame and writes
resolved values into a side table keyed by `(NodeId, PropertyKey)`. The
old `ViewAnimator` / `LayerAnimator` path stays live; nothing reads the
side table yet. Implements Animation-Scheduler-Plan **Tier A** — see
that doc for the scheduler's internal design; this phase only covers
its integration into the window / frame pipeline.

- New `wtk/src/UI/AnimationScheduler.{h,cpp}` (header private to the
  UI library, like `FrameBuilder`), per Animation-Scheduler-Plan §3.
- `AppWindow::Impl` owns one `AnimationScheduler`. `FrameBuilder` (or
  the pacer) calls `scheduler.tick(frameTime)` before the paint walk.
- The scheduler runs on the UI / FrameBuilder thread (single-threaded
  per Animation-Scheduler-Plan §2); the `DisplayList` snapshot stays
  the hand-off to the compositor worker thread.
- Validator: scheduler ticks each frame (logged); existing animations
  (ContainerClampAnimationTest) unchanged because nothing reads the
  side table yet.

Files touched: new `wtk/src/UI/AnimationScheduler.{h,cpp}`,
`wtk/src/UI/AppWindowImpl.h`, `wtk/src/UI/AppWindow.cpp`,
`wtk/src/UI/FrameBuilder.{h,cpp}`.

##### Phase 4.4 — migrate UIView animation onto the scheduler; paint reads the side table (folds Anim Tiers B + C) [DONE]

**Status:** Complete (2026-05-30; full Metal build clean). Mechanical
re-plumbing — every animation surface this phase touches was already
dormant before 4.4: `View::applyLayoutDelta`, `UIView::applyLayoutDelta`,
and `UIView::Impl::startOrUpdateAnimation` all have **zero callers** in
the tree; `pathNodeAnimations` is never written. So Phase 4.4 swaps the
backing pump (ViewAnimator/LayerAnimator → AnimationScheduler) and the
side-table reader (per-tag tween state → `scheduler.value`) with no
expected runtime delta in any current scene.

Identity scheme (developer decision; not pinned in the original §4.4):
- `View::nodeId()` (new public accessor, returns plain `std::uint64_t`
  so the public header stays clear of the UI-private `NodeId` alias) —
  one stable id per `View`, allocated at construction from the new
  `allocateNodeId()` atomic counter in `wtk/src/UI/AnimationScheduler.h`.
- `UIView::Impl::ensureElementNodeId(tag)` — one stable id per
  `(UIView, UIElementTag)` pair, allocated lazily on first registration
  or read. Read-only callers (`animatedValue`) use `tryElementNodeId`
  so unknown tags fall through to `{}` without growing the map.

PropertyKey mapping (developer decision; preserves the legacy channel-
by-channel semantics that the old per-tag tween engine carried):
- View / element **layout** tweens — built-in
  `PropertyKey::LayoutX/Y/Width/Height` (already `layoutAffecting`).
- Element **path-node** tweens — built-in `PropertyKey::PathNodeX/Y`
  with `subIndex = nodeIndex`.
- Every other UIView per-element channel (`ElementAnimationKey*` ColorR/
  G/B/A + Width/Height, the `EffectAnimationKey*` 1000-series shadow /
  blur ints on `Impl`) — `UserDefined + int(key)` in the scheduler's
  `UserDefined` half. One `elementKeyToProperty(int)` helper in
  `UIView.Animation.cpp` does the encoding; readers (`applyAnimatedColor`
  / `applyAnimatedShape` / `animatedValue`) round-trip through the same
  helper so the channel layout is preserved.

`(tag, key)` short-circuit (Anim Tier C requirement): preserved via a
new `Impl::animationTargets_[tag][key] → float` side map. The scheduler
itself **replaces** on every re-registration (Anim §6 Q3); the local
"same target → no restart" guard now lives in the UIView caller, not
the scheduler. The `durationSec <= 0` cancel-equivalent path issues a
zero-duration tween (`AnimationScheduler::tick` treats `durNs == 0` as
"finished, table-erase" — already-correct behaviour from 4.3) so the
side-table cell clears and reads fall through to the resolved style.

`UIView::tickAnimations()` body and the matching `ScopedPhase(Tick)` in
`UIView::update()` are GONE — `AnimationScheduler::tick` already ran
once at the outermost `FrameBuilder::beginFrame` (Phase 4.3 wiring), so
UIView's local tick was redundant. `Impl::advanceAnimations` body is
now a one-line `return false;` stub. Both the public `tickAnimations`
method and the private `advanceAnimations`/`beginCompositionClock`/
`ensureAnimation*` symbols stay declared so 4.8's sweep can delete the
whole dormant animation surface (the four ViewAnimator/LayerAnimator/
elementAnimations/pathNodeAnimations members + the diagnostics state)
in one pass — per the bullet below this one in §4.4.

Paint-purity asserts (Anim §3.10 debug guards) land here:
`AnimationScheduler::registerProperty`/`registerCallback` assert phase
!= Paint/Commit; `setTableValue` asserts phase == Tick. All
`assert()`-only (drop on `NDEBUG`); no-op when no frame is in flight
(headless / startup paths).

Scheduler access path: new `FrameBuilder::animationScheduler()`
accessor returns `window_.impl_->animationScheduler_.get()`. Animation
callers reach it via the existing `AppWindow::activeFrameBuilder()`
lookup — public `AppWindow.h` is untouched, the scheduler header stays
UI-private.

4.7 seam carried forward: the scheduler now holds the `LayoutX/Y/
Width/Height` tracks `View::applyLayoutDelta` / `UIView::applyLayoutDelta`
write, but nothing reads them back to update a View's rect yet. With
zero callers of either method today, this is a no-impact deferral —
Phase 4.7's centralized Layout pass closes it.

Files touched: `wtk/src/UI/AnimationScheduler.{h,cpp}`,
`wtk/src/UI/FrameBuilder.{h,cpp}`, `wtk/include/omegaWTK/UI/View.h`,
`wtk/src/UI/ViewImpl.h`, `wtk/src/UI/View.Core.cpp`,
`wtk/src/UI/UIView.Layout.cpp`, `wtk/src/UI/UIView.Animation.cpp`,
`wtk/src/UI/UIView.Update.cpp`, `wtk/src/UI/UIViewImpl.h`.

UIView's per-element tween engine and View's layout-delta animations
move onto the scheduler. Paint becomes a pure *reader* of the side
table; animation no longer writes during the paint walk. Implements
Animation-Scheduler-Plan **Tiers B and C**.

- `View::applyLayoutDelta`'s layout tweens (Anim Tier B) call
  `scheduler.tweenProperty<…>` instead of `ViewAnimator::resizeTransition`.
- `UIView::Impl::startOrUpdateAnimation` / `advanceAnimations` (Anim
  Tier C) route to the scheduler; `applyAnimatedColor` /
  `applyAnimatedShape` / `animatedValue` read `scheduler.value(…)`.
- `UIView::update()` reads the side table when building its
  `DisplayList`; it no longer ticks or advances animations inline.
- The `ViewAnimator` / `LayerAnimator` instances on `UIView::Impl`
  (`animationViewAnimator`, `animationLayerAnimators`,
  `elementAnimations`, `pathNodeAnimations`) are now unused but not yet
  deleted (4.8).
- Validator: ContainerClampAnimationTest and every animated scene
  produce identical motion; the paint walk no longer mutates animation
  state (assert in debug).

Files touched: `wtk/src/UI/UIView.Animation.cpp`,
`wtk/src/UI/UIView.Update.cpp`, `wtk/src/UI/View.Core.cpp`,
`wtk/src/UI/UIViewImpl.h`, `wtk/src/UI/AnimationScheduler.{h,cpp}`.

##### Phase 4.5 — `LayoutManager`: replace `ViewResizeCoordinator` *and* `Container` child layout [DONE]

**Status:** Complete (2026-05-31; full Metal build clean). All four
built-ins landed: `AbsoluteLayout` (default — singleton, no-alloc),
`FillLayout` (every child stretched), `StackLayout` (H/V no-flex
sequential), `ContainerLayout` (lifted `Container::clampChildRect`).
The `clampRectToParent` static helper survives — lifted from the
deleted `ViewResizeCoordinator::clampRectToParent` to
`LayoutManager::clampRectToParent` (same signature; three live callers
re-pointed: `UIView::paint` intra-element clamp at 8 sites,
`StackWidget::layoutChildren` until 4.6 replaces it, and the manager
built-ins themselves).

Survey findings that shaped the implementation (recorded for the 4.6
work that follows):

- `ChildResizePolicy::Fill` and `Proportional` were **dead policies**
  — no caller asked for them; only `Fixed` and `FitContent` were used.
  The lifted manager built-ins do not reimplement Fill / Proportional;
  if a caller surfaces later, it adds a new manager kind rather than
  re-extending the coordinator's switch.
- Per-child `ChildResizeSpec` storage on the coordinator was **never
  customized externally** — every caller passed default-constructed
  specs or built local specs on the fly (StackWidget). 4.5's managers
  therefore carry no per-child state; each manager applies a uniform
  policy. FlexLayout (4.6) will need per-child flex weights, but that
  is its own design.
- `beginResizeSession`'s `activeSessionId` field was `(void)`-discarded
  — i.e. the entire session API was vestigial bookkeeping left over
  from the dead Proportional baseline-tracking path. The whole resize-
  session walk in `WidgetTreeHost::notifyWindowResizeBegin` is gone
  (one full-tree walk dropped per resize-begin). The
  `beginResizeCoordinatorSessionRecurse` symbol remains as a no-op so
  the declaration cleanup can be a follow-up; the
  `resizeCoordinatorGeneration` counter stays — it is still consumed
  by the resize tracker / diagnostics.
- `LegacyResizeCoordinatorBehavior` (in `LayoutBehaviors.h`) had no
  callers and was deleted alongside the coordinator. `runWidgetLayout`
  now drives the parent's `LayoutManager::arrange` directly.

Deviations from the plan as written:

- `ContainerInsets` / `ContainerOverflowMode` / `ContainerClampPolicy`
  **moved from `BasicWidgets.h` to `LayoutManager.h`** — these describe
  layout policy, not widget shape, so they belong with the manager
  that consumes them. `BasicWidgets.h` re-includes
  `LayoutManager.h` so existing call sites that only include
  `BasicWidgets.h` continue to compile unchanged. No `using`-aliases
  needed (names stayed in `OmegaWTK::`).
- `Container::layoutChildren` **kept as an empty virtual hook** rather
  than deleted outright — `StackWidget` (Phase 4.6's territory) still
  overrides it for its bespoke flex implementation. The body is the
  no-op; the override-keyword in `StackWidget::layoutChildren` keeps
  working. The hook disappears in 4.6 when `FlexLayout` replaces the
  flex code.
- The protected `inLayout` re-entry guard on `Container` **stays**
  for the same reason — `StackWidget::layoutChildren` uses it. 4.5's
  own `Container::relayout` does NOT use it (the manager call is
  one-shot per relayout).
- `Container::onChildRectCommitted` still calls `relayout()` eagerly
  rather than the plan's `DirtyBit::Layout + requestFrame` deferral.
  Centralized deferral is Phase 4.7's job (it owns the
  FrameBuilder-driven Measure / Arrange passes); 4.5 keeps the
  eager-on-commit semantic so the layout-correctness profile stays
  identical pre- vs post-migration.
- `getResizeCoordinator()` accessors **deleted outright**, not
  deprecated. Zero external callers in the tree; the public alias
  would have served only as a tombstone.

Files touched: new `wtk/include/omegaWTK/UI/LayoutManager.h`
(public header, ~210 lines), new `wtk/src/UI/LayoutManager.cpp`
(implementation, ~270 lines including the lifted `clampRectToParent`
and ContainerLayout's clampChild), `wtk/include/omegaWTK/UI/View.h`
(`ViewResizeCoordinator` class deleted; `layoutManager()` /
`setLayoutManager()` / `subviews()` accessors added; `LayoutManager`
forward decl),
`wtk/src/UI/ViewImpl.h` (`resizeCoordinator` field deleted;
`layoutManager_` pointer field added; constructor stops attaching),
`wtk/src/UI/View.Core.cpp` (accessors implemented; addSubView /
removeSubView stop calling the coordinator),
`wtk/src/UI/Layout.cpp` (`runWidgetLayout` drives parent's
`LayoutManager::arrange`; `LegacyResizeCoordinatorBehavior` body
deleted),
`wtk/src/UI/LayoutBehaviors.h` (`LegacyResizeCoordinatorBehavior`
class deleted),
`wtk/src/UI/WidgetTreeHost.cpp` (`beginResizeCoordinatorSessionRecurse`
body neutered to no-op; the call site in `notifyWindowResizeBegin`
dropped — full-tree walk saved per resize),
`wtk/src/UI/UIView.Update.cpp` (8 sites swap
`ViewResizeCoordinator::clampRectToParent` →
`LayoutManager::clampRectToParent`),
`wtk/include/omegaWTK/Widgets/BasicWidgets.h` (`Container` carries a
`ContainerLayout` field; `clampPolicy` / cache deleted; `layoutChildren`
becomes an empty virtual hook; `ContainerInsets` / overflow /
`ContainerClampPolicy` moved out),
`wtk/src/Widgets/BasicWidgets.cpp` (Container constructors install
the layout on the View; `setClampPolicy` / `getClampPolicy` forward
to the layout; `clampChildRect` / `relayout` route through the
layout; `layoutChildren` body deleted; `contentBoundsFromHost` /
`clampAxisPosition` helpers moved to `LayoutManager.cpp`),
`wtk/src/Widgets/Containers.cpp` (one `clampRectToParent` callsite
swapped + `LayoutManager.h` include),
**DELETED:** `wtk/src/UI/View.ResizeCoordinator.cpp` (161 lines).

The parent-owned layout strategy from §3.2 / §3.5 takes over **all
child-node layout**. Today that job is split across three half-built
systems (the §1.5 problem this redesign exists to kill):

1. `ViewResizeCoordinator` — per-`View` `ChildResizeSpec` policies
   (Fill / Proportional / Fixed / FitContent + clamp + `growWeight*`),
   passive, driven on resize via `runWidgetLayout` →
   `coordinator.resolve()` and registered in `View::addSubView`.
2. `Container::layoutChildren()` — eager clamp-to-content-rect via
   `ContainerClampPolicy` (insets, min/max), called inline, then
   `child->setRect()`.
3. `StackWidget::layoutChildren()` — a full flexbox pass. **That one is
   Phase 4.6** (it needs Measure; see below). This phase collapses (1)
   and (2).

**Boundary (important).** `LayoutManager` arranges a node's **child
nodes** (child `View`s / `Widget`s) — it does **not** touch the
*intra-`UIView` element* layout. The sub-elements inside a single
`UIView` (authored via `UIViewLayoutV2` / `UIElementLayoutSpec`) stay
resolved inside `UIView` itself, in its own `arrange` / `paint` (§3.3).
The two were never the same surface; conflating "lay out my child
widgets" with "lay out the elements inside one UIView" is part of why
child layout became a union of systems. `LayoutManager` is the
child-node axis only.

- New `LayoutManager` interface (`measure(node, avail) -> Size`,
  `arrange(node, finalRectLocal)`) with `FillLayout`, `StackLayout`
  (H/V), `AbsoluteLayout`, and `ContainerLayout`. Built on the existing
  `LayoutStyle` / `resolveClampedRect`, plus
  `ViewResizeCoordinator::clampRectToParent` lifted to a free clamp
  helper (it is already used as a static utility, including by
  `StackWidget`). `FlexLayout` is Phase 4.6.
- `View` gains an optional `LayoutManager*` field; the parent arranges
  its children. The default is `AbsoluteLayout` — a child positioned by
  its own rect — which is the current no-explicit-layout back-compat
  behavior.
- `ViewResizeCoordinator`'s `ChildResizePolicy` + `ResizeClamp` +
  `growWeight*` become parameters of `StackLayout` / `FillLayout` /
  `AbsoluteLayout`. The `registerChild` / `resolve` / `resolveChildRect`
  call sites (`View::addSubView`, `runWidgetLayout`, `WidgetTreeHost`'s
  resize session) route through the parent's `LayoutManager` instead.
  `ViewResizeCoordinator` and `View.ResizeCoordinator.cpp` are deleted.
- `Container`'s clamp-to-content layout becomes a `ContainerLayout`
  (the `ContainerClampPolicy` — insets, min/max — becomes its params).
  `Container::layoutChildren()` / `clampChildRect()` are deleted; the
  container sets `ContainerLayout` as its backing `View`'s
  `LayoutManager`, and the arrange pass does the clamping.
  `Container::relayout()` / `onChildRectCommitted()` become a
  `DirtyBit::Layout` mark + frame request rather than an eager relayout.
- **Invocation is unchanged this phase.** `LayoutManager::arrange` runs
  from the *existing* layout entry points (the resize handler,
  `relayout`, `runWidgetLayout`). Phase 4.7 centralizes invocation into
  `FrameBuilder`'s Measure / Arrange passes; 4.5 only swaps the layout
  *math* behind those entry points, additive-then-centralize.
- Validator: resize / clamp scenes (ContainerClampAnimationTest,
  RootWidget resize, any `Container`-based widget scene) produce layout
  identical to the pre-migration paths.

Files touched: new `wtk/src/UI/LayoutManager.{h,cpp}` (+ public
header), `wtk/include/omegaWTK/UI/View.h`, `wtk/src/UI/View.Core.cpp`,
`wtk/src/UI/ViewImpl.h`, `wtk/src/UI/View.ResizeCoordinator.cpp`
(deleted), `wtk/src/UI/Layout.cpp` (`runWidgetLayout` rewire),
`wtk/include/omegaWTK/Widgets/BasicWidgets.h`,
`wtk/src/Widgets/BasicWidgets.cpp` (`Container`).

##### Phase 4.6 — `FlexLayout`: migrate `StackWidget`; the Measure pass earns its keep [DONE]

**Status:** Complete (2026-05-31). `FlexLayout` shipped in the public
`LayoutManager.h`; `StackWidget::layoutChildren()` is deleted; the
backing View's `LayoutManager` is the FlexLayout instance, configured
from the widget's `StackOptions` / `StackSlot` at every
`addChild` / `setSlot` / `setOptions`. The bespoke flex code that
lived inside `StackWidget` is gone; flex is now a `LayoutManager`-
family built-in, indistinguishable from `ContainerLayout` /
`FillLayout` / `StackLayout` at the call boundary.

Survey findings + design notes that shaped the implementation:

- **`Stack*` types stayed Widget-level.** `StackAxis` / `StackOptions` /
  `StackSlot` / `StackMainAlign` / `StackCrossAlign` / `StackInsets` are
  the public API of `StackWidget` and are called by name from tests
  (`BasicAppTestRun.cpp`, `ImageRenderTest`, `EllipsePathCompositorTest`).
  `FlexLayout` got its own `FlexOptions` / `FlexChildSpec` /
  `FlexMainAlign` / `FlexCrossAlign` / `FlexInsets` types in
  `LayoutManager.h`, and `StackWidget` adapts at the boundary
  (`toFlexOptions` / `toFlexChildSpec` in `Containers.cpp`). This keeps
  the LayoutManager core free of Stack-specific naming while preserving
  every existing caller verbatim.
- **Per-child state on the manager, keyed by `View *`.** `FlexLayout`
  stores a `std::unordered_map<View *, ChildEntry>` — spec + measure
  cache per child. Owners (`StackWidget`) call `setChildSpec` on
  add / setSlot, `removeChildSpec` on remove. The pre-migration
  parallel `childSlots` vector on `StackWidget` is kept *only* so the
  public `getSlot(WidgetPtr)` accessor keeps working; it does not
  drive the layout.
- **`Widget::view` is protected and only friended to `Container`.**
  `StackWidget` (a Container subclass) cannot access another Widget's
  `view` field directly because C++ friendship doesn't inherit. The
  migration uses the public `Widget::viewRef()` accessor instead
  (`&child->viewRef()`) — the same pattern any non-Container subclass
  would have to use.
- **No `Widget::setRect` invalidation suppress / re-invoke dance.**
  The pre-migration `StackWidget::layoutChildren` temporarily disabled
  `invalidateOnResize`, called `child->setRect()`, then explicitly
  re-invoked `invalidate(PaintReason::Resize)` for each resized child.
  The new path matches the 4.5 manager pattern (`ContainerLayout`,
  `AbsoluteLayout`, `FillLayout`, `StackLayout`): call `View::resize`
  directly, which fires `onLayoutResolved` and updates the layer tree;
  no Widget-pipeline invalidation. Phase 4.7's DirtyBit-driven
  FrameBuilder loop will own paint-after-layout propagation
  centrally; until then, the same invariant other 4.5 managers rely
  on (FrameBuilder picks up the new rect on the next pass) carries
  StackWidget too.
- **Suspicious-frame / placeholder-rect fallback preserved.** The
  pre-4.6 cache logic — "use the previously-seen good preferred size
  when the current rect is tiny / NaN / extreme-aspect" — moves
  verbatim into `FlexLayout::measure` (writes the cache on clean
  rects, reads it on suspicious / placeholder ones). The
  `hasLastStableFrame_` / `lastStableFrame_` parent-frame fallback
  moves from `StackWidget` onto the FlexLayout instance and applies
  identically.
- **`StackWidget::needsLayout` field deleted.** It was set in
  `relayout()` and cleared at the end of `layoutChildren()`; no
  external reader. With FlexLayout owning the algorithm, the bit has
  no caller.
- **`§3.5 "FlexLayout later" updated.** The §3.5 note now records
  that `StackWidget` forced FlexLayout into Tier 4.

Deviations from the plan as written:

- **`measure()` runs inside `arrange()` in Phase 4.6.** The plan
  describes a Measure-then-Arrange split that 4.7 will hoist into
  `FrameBuilder`. Until 4.7 calls `measure` top-down separately,
  `FlexLayout::arrange` calls `measure` internally so the existing
  entry points (StackWidget::resize / relayout / addChild) drive a
  complete pass. The cache write/read split is real (measure writes,
  arrange reads), so the seam is in the right place — it's just
  driven from one method for now.
- **Measure-cache invalidation is mutation-driven, not
  DirtyBit-driven.** `setChildSpec` / `removeChildSpec` /
  `setOptions` flip `hasPreferredSize = false` on the affected
  entries, so the next `measure` re-collects from scratch. Phase 4.7
  will replace this with a `DirtyBit::Layout`-driven invalidation
  read by the FrameBuilder Measure pass.

Files touched: `wtk/include/omegaWTK/UI/LayoutManager.h`
(FlexOptions / FlexChildSpec / FlexLayout, +`#include <unordered_map>`),
`wtk/src/UI/LayoutManager.cpp` (FlexLayout impl, +the flex-frame
suspicious-rect helpers in the unnamed namespace),
`wtk/include/omegaWTK/Widgets/Containers.h` (StackWidget shrunk —
bespoke cache + flex fields gone, `flexLayout_` field added,
`layoutChildren` override removed),
`wtk/src/Widgets/Containers.cpp` (rewritten — Stack→Flex adapters,
`relayout()` delegates to `flexLayout_.arrange`,
`addChild`/`removeChild`/`setSlot`/`setOptions` push specs into the
manager).

---

`StackWidget` (and any `Container` subclass like it) is a real,
existing flexbox container — `flexGrow` / `flexShrink` / `flexBasis`,
main-axis distribution, cross-axis alignment — whose `layoutChildren()`
is the third of the §1.5 layout systems. It cannot stay a bespoke
parallel path once `LayoutManager` owns child layout, so `FlexLayout`
is a **Tier 4 deliverable, not the "later" item §3.5 implies**: there
is a consumer in the tree today.

This is also the phase where the **two-pass Measure → Arrange split
becomes load-bearing**. `Container` / `ViewResizeCoordinator` (4.5)
only needed Arrange — they clamp an already-known rect. Flex needs
Measure first: `StackWidget` collects each child's preferred size
before it can distribute free space across the main axis.

- New `FlexLayout : LayoutManager`, a **reusable public built-in** (in
  the public `LayoutManager` header, not private to `StackWidget`).
  `measure(node, avail)` collects children's desired sizes (bottom-up);
  `arrange(node, finalRect)` runs the flex distribution + alignment that
  `StackWidget::layoutChildren()` computes today and assigns each child
  its final rect. It is reusable because flex is a *container policy*,
  not a widget — any container kind can adopt it.
- `StackWidget` configures a `FlexLayout` from its `StackSlot` / flex
  properties and sets it as its backing `View`'s `LayoutManager`;
  `StackWidget::layoutChildren()` is deleted. The algorithm now lives
  once, behind the `LayoutManager` interface, and stops being a
  parallel path.
- `measure` results are cached on the node and invalidated by
  `DirtyBit::Layout`, so a Paint-only frame skips re-measuring.
- Update §3.5 (which lists `FlexLayout` as "later") to note that
  `StackWidget` forces it into Tier 4.
- Validator: existing `StackWidget` / stack-based scenes lay out
  identically; horizontal and vertical stacks with mixed fixed / flex
  children match their pre-migration rects.

Files touched: `wtk/src/UI/LayoutManager.{h,cpp}` (FlexLayout),
`wtk/include/omegaWTK/Widgets/Containers.h`,
`wtk/src/Widgets/Containers.cpp` (`StackWidget`),
`wtk/src/UI/View.Core.cpp`.

##### Phase 4.7 — `PaintContext` + `View::paint(PaintContext&)`; DirtyBits-driven `FrameBuilder` loop; retire `UIView::update()` [DONE]

**Status:** Complete (2026-05-31; full Metal build clean across all
84 targets, visually verified on BasicAppTest pre- and post-cleanup).
The capstone landed in seven sub-phases (4.7.0 → 4.7.6 — see the
breakdown below for what each covered) — five additive (build the
new central walk in parallel with the old), one destructive-cut
(flip the entry point at 4.7.4), one cleanup (delete the legacy
surface at 4.7.5), one validator (4.7.6 — visual confirmation +
this status block).

**Survey findings that shaped the implementation:**

- **`Composition::PaintContext` already existed.** Tier B / B3 had
  shipped scaffolding (`displayList` / `offset` / `transform` /
  `clip` / `opacity`) in `DisplayList.h`. The plan-doc bullets
  describing a new `wtk/src/UI/PaintContext.h` were superseded —
  4.7.0 added `View::paint(Composition::PaintContext&)` as the
  virtual hook over the *existing* struct rather than introducing a
  new file. UI-layer `resolvedStyle` / `effectStack` fields were
  intentionally NOT added to the Composition-layer struct (would
  push `UIViewInternal::ResolvedViewStyle` into a public Composition
  header — a layering violation). The cache-on-impl pattern
  (`UIView::Impl::resolvedViewStyle_`, written by the Style pass,
  read by the Paint pass) was correct as-is and survives 4.7.
- **`UIView::paint(Composition::PaintContext&)` already existed as
  a pure DrawOp-emitter.** What 4.7 actually moved was the
  *orchestration* (`UIView::update`'s `Style → Layout → Paint →
  Commit` loop) out of UIView and into the central
  `FrameBuilder::buildFrame`. UIView's existing `paint`,
  `resolveStyles`, `arrange` methods became `View` virtual
  overrides (`arrange` renamed to `arrangeContent` to distinguish
  intra-node element layout from the LayoutManager child-axis
  layout from 4.5/4.6).
- **`NativeViewHost::syncBounds` is the one out-of-paint caller of
  the offset machinery.** Fires from `onLayoutResolved` during host
  resize; cannot read `PaintContext.offset` (no walk in flight).
  Replaced by a new public `View::offsetFromRoot()` accessor — same
  parent-chain walk as the deleted `legacyComputeWindowOffset`, but
  renamed to drop the "legacy" tag and signal "for embed-sync only,
  not for paint". Public access kept (instead of a friend-class
  channel) because the contract is clear from the name.
- **SVGView migrated alongside UIView.** SVGView's pre-4.7 path was
  the parallel `submitView` + offset accumulator flow. 4.7.4 added
  a `SVGView::paint(Composition::PaintContext&)` override that
  translates the cached DL ops into absolute window coords (per-op
  switch — rect-like ops shift `.pos`, `VectorPath` deep-copies and
  calls `Path::translate(offset)`, matching the UIView pattern for
  path ops). The legacy `void SVGView::paint()` is kept as a
  no-op-plus-`markDirty` stub so `SVGViewRenderTest`'s onPaint
  caller still compiles; the actual rendering now goes through the
  central walk.
- **VideoView neutralised, not migrated.** VideoView's `queueFrame`
  used to submit a per-frame `DrawOp::Bitmap` through `submitView`
  + `ScopedViewOffset`. The comment already documented "VideoView
  is not yet driven by the frame pacer. Correct presentation is
  deferred to NativeViewHost-Adoption-Plan V1–V4." With the
  accumulator gone, queueFrame just `markDirty(View::Paint)`s — a
  follow-up will add `VideoView::paint(PaintContext&)` matching
  SVGView's pattern when the frame-pacer driver lands. No in-tree
  test exercises live video on the old code path, so no regression.

**Deviations from the plan as written:**

- **`UIView::update()` was NOT deleted outright.** The plan said
  "delete update() (header + impl). Delete the update() callers
  (none in the tree once 4.7.4 lands)." But the tree had ~16
  callers in primitives (`Rectangle::onPaint`,
  `RoundedRectangle::onPaint`, etc. — all dead post-cutover since
  `executePaint` no longer dispatches `onPaint`, but the *symbol*
  is still referenced from their bodies) plus ~8 tests
  (`uiView->update()` called explicitly from RootWidget,
  EllipsePathCompositorTest, ContainerClampAnimationTest, etc.).
  4.7.5 kept `UIView::update()` as a `markDirty(Style | Layout |
  Paint)` stub. The production paint path never reaches it (no
  onPaint dispatch); only the explicit test callers do, and
  markDirty is the right semantic for them. Final deletion is a
  Phase I sweep that retires the dead callsites first.
- **Per-pass tree walks not split into Measure / Arrange.** The
  plan describes "Measure bottom-up then Arrange top-down across
  the dirty subtree". 4.7.2 folded both into a single pre-order
  layout walk: each manager's `measure()` already consults
  children via its own per-child cache (FlexLayout's
  `ChildEntry::preferredMain/Cross`), so an explicit bottom-up
  pass adds nothing today. A future manager that needs the parent
  to *use* the child's measured size before arranging will
  reintroduce the split.
- **`Composition::PaintContext` left at its Tier-B / B3 shape.**
  No `resolvedStyle` pointer, no effect stack added — layering
  violation (above) plus no producer for the effect stack yet
  (Tier 4 §4.7 "What this phase does NOT do" defers PushEffect
  / PopEffect handling). The four Tier-B fields (`displayList`,
  `offset`, `transform`, `clip`, `opacity`) covered everything
  the new walker needs.
- **`paintDirtyRecurse` survived as a no-op stub, not deleted.**
  Same rationale as `UIView::update` — a Phase I cleanup pass
  retires it once the rest of the dead surface is gone.

**Sub-phase landing log:**

- **4.7.0** — `View::paint(Composition::PaintContext &)` virtual
  + UIView override (no body change). Forward-decl of
  `Composition::PaintContext` in View.h. ~30 lines.
- **4.7.1** — `FrameBuilder::buildFrame(View &)` + the
  `paintSubtree` walker with `pc.offset` accumulation. Single
  window-wide DisplayList, submitted with `windowOffset == {0,0}`.
  ~80 lines.
- **4.7.2** — Style + Layout passes added to `buildFrame` (+
  `View::resolveStyles` / `View::arrangeContent` virtuals, UIView
  overrides). `arrange()` renamed to `arrangeContent()`. ~70 lines.
- **4.7.3** — DirtyBit propagation (`View::Impl::descendantDirty_`,
  `markDirty` walks ancestors, `clearDirtyBits` clears both
  masks). Per-pass gating in `buildFrame` + per-subtree pruning in
  the walkers. `clearDirtySubtree` walker. ~60 lines.
- **4.7.4** — Entry-point cutover. `Widget::executePaint` shrinks
  to "mark dirty + (immediate ? `paintDirty` : `requestFrame`)".
  `WidgetTreeHost::paintDirty` calls `FrameBuilder::buildFrame`.
  `paintDirtyRecurse` becomes a no-op stub. `SVGView::paint(
  PaintContext&)` override added with op translation
  (`translateOpToAbsolute` static helper). ~150 lines net.
- **4.7.5** — Destructive cleanup. Deleted: `UIView::update`
  body (kept as a `markDirty` stub); `FrameBuilder::offsetStack_`
  + `pushOffset` / `popOffset` / `currentOffset` /
  `hasOffsetOnStack` / `ScopedViewOffset`; `FrameBuilder::submitView`;
  `View::computeWindowOffset` / `legacyComputeWindowOffset` /
  `scrollOffsetContribution`; `View::startCompositionSession` /
  `endCompositionSession`; legacy `SVGView::paint()` (no-arg, kept
  as `markDirty` stub for test caller). Added: `View::offsetFromRoot`
  (replacement for `NativeViewHost::syncBounds`). VideoView's
  `queueFrame` neutralised. `~200 lines net deletion`.
- **4.7.6** — Visual verification of BasicAppTest pre- and
  post-4.7.5. Identical render before and after, confirming the
  cutover + cleanup did not regress the offset / dirty-bit / Style
  / Layout / Paint pipeline.

**Files touched:**

- `wtk/include/omegaWTK/UI/View.h` — `View::paint` /
  `resolveStyles` / `arrangeContent` virtuals;
  `descendantDirty` accessor; `markDirty` propagation contract;
  `offsetFromRoot` accessor; deleted `computeWindowOffset` /
  `legacyComputeWindowOffset` / `scrollOffsetContribution` /
  `startCompositionSession` / `endCompositionSession`.
- `wtk/include/omegaWTK/UI/UIView.h` — `resolveStyles` /
  `arrangeContent` / `paint` marked `override`.
- `wtk/include/omegaWTK/UI/SVGView.h` — `paint(PaintContext&)`
  override added.
- `wtk/src/UI/View.Core.cpp` — `View::paint` / `resolveStyles` /
  `arrangeContent` default no-op bodies; `markDirty` ancestor
  walk; `descendantDirty`; `clearDirtyBits` clears both masks;
  `offsetFromRoot`; deletions of the legacy bodies.
- `wtk/src/UI/ViewImpl.h` — `descendantDirty_` field added.
- `wtk/src/UI/UIView.Update.cpp` — `UIView::arrange` →
  `arrangeContent` rename; `UIView::update` body replaced with
  `markDirty` stub.
- `wtk/src/UI/SVGView.cpp` — `paint(PaintContext&)` override +
  `translateOpToAbsolute` helper; legacy `paint()` replaced with
  `markDirty` stub.
- `wtk/src/UI/VideoView.cpp` — `queueFrame` submission block
  replaced with `markDirty` (deferred to NativeViewHost-Adoption).
- `wtk/src/UI/FrameBuilder.h` — `buildFrame(View&)` declaration;
  deleted offset accumulator + `submitView` + `ScopedViewOffset`
  declarations.
- `wtk/src/UI/FrameBuilder.cpp` — `buildFrame` body with Style /
  Layout / Paint walkers + `clearDirtySubtree`; deleted offset
  accumulator + `submitView` + `ScopedViewOffset` bodies.
- `wtk/src/UI/Widget.Paint.cpp` — `executePaint` shrunk to
  mark-dirty + paintDirty/requestFrame.
- `wtk/src/UI/WidgetTreeHost.cpp` — `paintDirty` calls
  `buildFrame`; `paintDirtyRecurse` no-op stub;
  `initWidgetRecurse` / `invalidateWidgetRecurse` lose their
  `ScopedViewOffset` pushes.
- `wtk/src/UI/NativeViewHost.cpp` — `syncBounds` uses
  `View::offsetFromRoot()` instead of `computeWindowOffset()`.

---

The capstone. `FrameBuilder` becomes the §3.7 four-pass loop driven by
`DirtyBits`; `View` gets a virtual `paint(PaintContext&)`; the
`UIView::update()` monolith becomes `UIView::paint`. Realizes
Widget-View-Paint-Lifecycle-Plan Tier D for the (kept) Widget/View
model.

###### Pre-flight: survey notes that shape the sub-phases

- **`Composition::PaintContext` already exists** (`wtk/include/omegaWTK/Composition/DisplayList.h:385`)
  with `displayList`, `offset`, `transform`, `clip`, `opacity`. Added in
  Tier B / B3 as "scaffolding for the Tier D tree walk". Phase 4.7's
  PaintContext is **the same struct extended**, not a from-scratch
  `wtk/src/UI/PaintContext.h` — the plan-doc line above was written
  before the existing scaffolding was recognised. Phase 4.7 adds
  `resolvedStyle` (pointer the node's paint reads) and an effect stack
  to the existing struct, and threads it through a new central walk.
- **`UIView::paint(Composition::PaintContext &)` already exists**
  (`wtk/src/UI/UIView.Update.cpp:197`) and is already a pure
  `DrawOp`-emitter — it reads `impl_->arranged_`, `impl_->resolvedViewStyle_`,
  and the animation side table, and appends to `pc.displayList` without
  mutating view state. So 4.7's "make paint pure" half is *already
  shipped*; the remaining work is hoisting orchestration out of
  `UIView::update()` (which still calls `resolveStyles()` →
  `arrange()` → `paint()` → `submitView()` inline) into FrameBuilder.
- **`UIView::update()` is the 270-line orchestrator** that runs
  Style → Layout → Paint → Commit per phase via `ScopedPhase`. Each
  phase calls a method (`resolveStyles`, `arrange`, `paint`,
  `fb->submitView`) that already exists. 4.7 is about *who drives the
  phasing* — UIView itself today, FrameBuilder tomorrow.
- **The offset accumulator (`FrameBuilder::offsetStack_` +
  `ScopedViewOffset`)** is paired with `UIView::update()`'s call to
  `submitView` and the `paint` body's `pc.offset` read. The accumulator
  is replaced by threading `PaintContext.offset` through the central
  tree walk; `submitView` is replaced by FrameBuilder running the walk
  directly into the window's `DisplayList`.
- **`NativeViewHost::syncBounds` is the one external caller of
  `View::computeWindowOffset()`** outside the paint walker (fires from
  `onLayoutResolved` during host resize). The legacy parent-chain walk
  (`legacyComputeWindowOffset`) survives this phase as a private
  helper for that one call site — the public accessor moves to
  package-private (or `NativeViewHost` is re-pointed at it directly).

###### Sub-phase breakdown

Seven small sub-phases — five additive (build the new loop in parallel
with the old), one destructive-cut (flip the entry point), one cleanup.
Each is reviewable on its own; the additive five do not change behaviour
because the cutover is gated to 4.7.4.

###### Phase 4.7.0 — Add `View::paint(PaintContext&)` virtual

**Layering finding (2026-05-31).** Adding `resolvedStyle` / `effectStack`
to `Composition::PaintContext` would push a UI-layer type
(`UIViewInternal::ResolvedViewStyle`, declared in the private
`wtk/src/UI/UIViewImpl.h`) into a Composition-layer public header — a
layering violation. The fix is *not* needed: today's `UIView::paint`
reads from `impl_->resolvedViewStyle_` already (UIViewImpl.h is in the
same translation unit), and the Style pass writes the same impl cache.
The cache-on-impl pattern survives 4.7. PaintContext keeps the
Composition-layer concerns (`displayList` / `offset` / `transform` /
`clip` / `opacity`) and gains nothing UI-layer-specific. The effect-stack
field is similarly dropped from 4.7.0 — the plan defers PushEffect /
PopEffect handling until a producer appears, and the existing per-paint
`ResolvedEffectStyle` access pattern in `UIView::paint` works for the
one consumer today (drop-shadow). So 4.7.0 is just the virtual hook.

- Add `virtual void View::paint(Composition::PaintContext &)` to the
  base class with a default body that walks `subviews()` and calls
  `child->paint(pc)`. UIView's existing `paint` becomes the override
  (no body change, just `override`).
- `Composition::PaintContext` itself is **unchanged** in 4.7.0 — the
  Tier-B / B3 scaffolding already carries everything the new walk
  needs.
- No behavioural change — UIView::update still drives, still calls
  `paint(pc)` exactly as before.

###### Phase 4.7.1 — `FrameBuilder::buildFrame()` skeleton: Paint pass only

- Add `FrameBuilder::buildFrame(View & root)` that runs ONE top-down
  Paint walk: seeds `PaintContext { displayList=windowDL, offset={0,0} }`
  and calls `root.paint(pc)`. The default `View::paint` recurses; UIView
  emits draw ops via its existing body.
- This is wired to a debug entry point only (e.g. a new
  `FrameBuilder::driveBuildFramePath()` flag, or a fresh method
  `paintViaBuildFrame()`) — `Widget::executePaint` still drives the
  existing path. Lets us validate that the centralised walk produces
  the same visual result before the cutover.
- The offset accumulator stays put; this phase doesn't replace
  `submitView`, it short-circuits it: the buildFrame walk writes
  directly into the window DisplayList without going through the
  pending-submission queue.

###### Phase 4.7.2 — Style pass + Layout pass

- Style pass: between Begin and Paint, walk dirty UIView subtree and
  call `resolveStyles()` on each node whose `DirtyBit::Style` is set
  (or whose ancestor's is — propagation is in 4.7.3). Output lives in
  the existing `impl_->resolvedViewStyle_` / `impl_->computedStyles_`
  caches; nothing else needs to change. `PaintContext.resolvedStyle`
  is set per-node so the Paint pass reads from the cache, not via
  another inline resolution.
- Layout pass: between Style and Paint, run Measure bottom-up then
  Arrange top-down across the dirty subtree, calling
  `node.layoutManager()->measure(node, avail)` then
  `arrange(node, finalRectLocal)`. The 4.5 / 4.6 manager entry points
  (`Container::relayout`, `StackWidget::relayout` →
  `flexLayout_.arrange`) stay live — they are now redundant on the
  paint path but still useful for direct-relayout call sites; they'll
  be culled in Phase I.
- Still gated to the debug entry point; widget-driven path unchanged.

###### Phase 4.7.3 — DirtyBit propagation + per-pass gating

- `View::markDirty(bits)` walks up to root, OR-ing `bits` onto each
  ancestor's mask. The root carries the "any descendant dirty" mask.
- `FrameBuilder::buildFrame()` reads root bits and runs each pass
  conditionally: Style runs only if `Style` is on the root mask;
  Layout only if `Layout`; Paint only if `Paint` (or any of the
  others, since style/layout changes imply paint).
- Subtree pruning: each pass visits a node only when ANY of its
  ancestors or itself carries the corresponding bit; subtrees with a
  clean mask short-circuit.
- Validator surface: a Paint-only animation tick (one node with
  `Paint`, ancestors clean except for propagation, no Style / Layout
  bits anywhere) must skip the Style and Layout passes entirely.

###### Phase 4.7.4 — Switch entry: widgets → `buildFrame`

- `Widget::executePaint(reason, immediate)` shrinks to: set the
  view's dirty bits (`Paint`, plus `Style` / `Layout` per reason),
  call `treeHost->requestFrame()`. No `ScopedFrame`, no `onPaint`
  dispatch — those move into the frame flush.
- `WidgetTreeHost::paintDirty()` opens the single `ScopedFrame` and
  calls `FrameBuilder::buildFrame(rootView)` instead of walking
  widgets and dispatching `onPaint`.
- `UIView::onPaint` (the `Widget::onPaint` override) becomes a no-op
  — paint goes through `View::paint(PaintContext&)` now, not through
  the widget-level callback. SVGView's `onPaint` similarly becomes a
  no-op; its `paint(PaintContext &)` does the work.
- `UIView::update()` survives this phase but only as a stub that
  calls `buildFrame(this)` for back-compat with any direct
  `update()` caller (deleted in 4.7.5).
- `Widget::invalidateNow()` keeps the sync semantic by running
  `buildFrame` inline instead of deferring to the run-loop frame
  flush.

###### Phase 4.7.5 — Retire `UIView::update()`, offset accumulator, legacy offset accessors

- Delete `UIView::update()` (header + impl). Delete the `update()`
  callers (none in the tree once 4.7.4 lands).
- Delete `FrameBuilder::offsetStack_`, `ScopedViewOffset`,
  `pending_`, `submitView()` (no callers post-4.7.4).
- Delete `View::computeWindowOffset()`,
  `View::legacyComputeWindowOffset()`,
  `View::scrollOffsetContribution()`. Point
  `NativeViewHost::syncBounds` at the new path — likely a
  `View::contentOffsetFromRoot()` accessor that walks parent rects
  the same way `legacyComputeWindowOffset` did, but exposed under a
  name that does not advertise "use this from paint code" any more.
  (Trade-off: keep `legacyComputeWindowOffset` as `private` and just
  remove the public `computeWindowOffset`. Decision finalised in
  4.7.5; the survey leaves it open.)
- Delete `View::startCompositionSession()`,
  `View::endCompositionSession()`. The compositor proxy is
  propagated at `addSubView` time (already true post-4.5) so the
  session-open dance is dead.
- `ScrollView::paint` keeps its `PushClip` in local coords; with
  `PaintContext.offset` threaded through the walk, descendants of
  the ScrollView see the right offset automatically. The override
  on `contentOffset()` stays so the offset walker (now living
  inside `View::paint`'s default body) folds the scroll shift in.

###### Phase 4.7.6 — Validator + Phase 4.7 [DONE]

- Visual sweep on macOS Metal across BasicAppTest,
  EllipsePathCompositorTest, ImageRenderTest,
  ContainerClampAnimationTest, RootWidgetTest, TextCompositorTest,
  SVGViewRenderTest, VideoViewPlaybackTest. Each scene paints
  identical to the pre-4.7 baseline (the paint walk is the same
  function, just driven from a different caller).
- DirtyBit gating: an animation tick that flips only `Paint` skips
  Style and Layout (instrumented via counters in FrameBuilder for
  the validator; counters removed post-pass).
- Plan doc: Phase 4.7 marked `[DONE]` with the full status block.

###### Body (kept; sub-phases above are the implementation surface)

- `PaintContext` (§3.5): carries the window `DisplayList &`, the
  current transform, clip, opacity, and effect stack, plus the
  resolved style for the node. Threaded through the paint walk;
  replaces the `FrameBuilder` offset accumulator + `submitView` dance.
- `View` gains `virtual void paint(PaintContext &)`. `UIView::paint` is
  the old `update()` body minus session management, canvas, dirty-flag
  juggling, sort, and clamp-to-parent — it reads the resolved style,
  the arranged element rects, and the animation side table, and appends
  `DrawOp`s to `pc.displayList` (~80 lines, down from ~390 in
  `UIView.Update.cpp`).
- `DirtyBits` become load-bearing: each bit propagates to the root on
  set; `FrameBuilder::buildFrame()` reads the root bits and runs
  Style / Layout (Measure + Arrange) / Paint passes conditionally,
  pruning at every subtree whose bit is clear.
- The Layout pass is where the `LayoutManager`s from 4.5 / 4.6 finally
  run *centrally*: `FrameBuilder` calls `measure` bottom-up then
  `arrange` top-down across the dirty subtree, instead of each
  container invoking its own layout from a paint/resize entry point.
  This is the "centralize invocation" half of 4.5's
  additive-then-centralize move.
- The Style pass needs to exist, but **not** the full Style refactor.
  Paint can't resolve style inline anymore (it must be pure), so the
  existing inline `resolveViewStyle` resolution is hoisted into a
  pre-paint Style pass (§3.7 `resolveStylesSubtree`) that caches the
  resolved style the node's `paint` reads. The global
  `StyleSheet` / `StyleResolver` / `ComputedStyle` machinery from
  `Style-StyleSheet-Refactor-Plan.md` is a *separate* workstream that
  later replaces the internals of this same pass — Tier 4 only needs
  the pass, not that plan (see §7).
- `Widget::executePaint` (kept — Widgets are light `View` wrappers per
  §6 Q2) becomes a thin "mark my subtree dirty + request a frame" call
  rather than a paint driver.
- `UIView::update()` is removed from the public surface;
  `AppWindow::invalidate()` requests a frame.
  `View::computeWindowOffset` / `legacyComputeWindowOffset` /
  `scrollOffsetContribution` are deleted (PaintContext carries the
  transform; `contentOffset()` folds into Arrange).
- `ScrollView` re-expresses its `PushClip` + content offset against
  `PaintContext` instead of the accumulator.
- Validator: full sweep through the pure Measure / Arrange / Paint
  pipeline; DirtyBits gating verified (a Paint-only animation tick does
  not re-run layout).

Files touched: new `wtk/src/UI/PaintContext.h`,
`wtk/include/omegaWTK/UI/View.h`, `wtk/src/UI/View.Core.cpp`,
`wtk/src/UI/ViewImpl.h`, `wtk/src/UI/UIView.Update.cpp` (→
`UIView.Paint.cpp`), `wtk/src/UI/UIView.Core.cpp`,
`wtk/src/UI/FrameBuilder.{h,cpp}`, `wtk/src/UI/ScrollView.cpp`,
`wtk/src/UI/Widget.Paint.cpp`, `wtk/src/UI/WidgetTreeHost.cpp`,
`wtk/src/UI/AppWindow.cpp`.

##### Phase 4.8 — delete the old animation surface + per-view `LayerTree` [DONE]

**Status:** Complete (2026-05-31; full Metal build clean across all 91
targets, visually verified on BasicAppTest pre- and post-cleanup —
identical render). Folds Animation-Scheduler-Plan **Tier E**. Landed
in five small sub-phases (4.8.1 → 4.8.5) — each touched a distinct
deletion surface so a regression would bisect to a single step.

**Survey findings that shaped the implementation:**

- **Zero backend exposure.** `grep` over `wtk/src/Composition/backend`
  for `ViewAnimator` / `LayerAnimator` / `LayerClip` / `ViewClip` /
  `AnimationRuntimeRegistry`: no matches. The deleted classes were
  UI-layer-internal only. All three backends (D3D12 / Metal / Vulkan)
  remain unchanged.
- **Zero test exposure.** `grep` over `wtk/tests` for the same set:
  no matches. No test exercised the deletion targets directly.
- **`UIView::Impl` dormant fields were truly dormant.** The four
  animation fields (`animationViewAnimator`,
  `animationLayerAnimators`, `elementAnimations`,
  `pathNodeAnimations`) had no writer outside the `ensure*` /
  `beginCompositionClock` helpers that were themselves dormant
  (no caller post-4.4 — the comment at `UIViewImpl.h:156` documented
  the dormancy as a 4.8 prerequisite). Phase 4.4 had completed the
  scheduler routing; 4.8 just took out the trash.
- **`UIView::animateElement` is the live public hook.** Routes to
  `Impl::startOrUpdateAnimation` → `AnimationScheduler` directly,
  *not* through any deleted class. It's the surviving public API
  for per-element scalar tweens; `UIView::paint` reads the
  resulting side table via `animatedValue`.
- **`applyAnimatedColor` / `applyAnimatedShape` were orphans.**
  Both pre-flagged as "no caller in the tree" (UIView.Animation.cpp
  comments at L256 + L281). Their helpers (`clamp01`,
  `applyShapeDimension`) were only used by these two and went with
  them. The live paint-time path resolves brush + shape directly
  from `ComputedStyle` / `UIElementLayoutSpec`.
- **Window-level layer-tree observation already existed.**
  `AppWindow.cpp:151-155` observes `windowLayerTree_` at
  `displayRootWindow` — exactly the single-tree contract Phase 4.8
  wanted. `WidgetTreeHost::observe/unobserveWidgetLayerTreesRecurse`
  walked per-view `ownLayerTree`s; with that field deleted, the
  per-view walk is redundant.
- **One non-paint caller of layer-tree state survived in
  `UIView.Update.cpp::localBoundsFromView`.** It read
  `view->getLayerTree()->getRootLayer()->getLayerRect()` as a
  fallback when the view's own rect was transient (Tier 2 Phase
  2.2 cleanup recorded the cache was no longer earning its lifetime
  hazard). With the per-view tree gone, the fallback is dead — the
  function falls through to a constant rect instead, matching the
  4.7 paint pipeline's "View::rect is the source of truth" model.

**Deviations from the plan as written:**

- **`UIView.Animation.cpp` was NOT deleted outright.** The plan
  said "delete the legacy `UIView.Animation.cpp` paths". The file
  contains both legacy code (the deleted `ensure*` /
  `beginCompositionClock` / `applyAnimated*` helpers) and live
  code that routes to the AnimationScheduler
  (`startOrUpdateAnimation`, `animateElement`, `animatedValue`,
  `ensureElementNodeId`, `tryElementNodeId`, `markElementDirty`
  family). 4.8 stripped the legacy bodies inline and kept the live
  ones; the file shrunk from ~421 lines to ~245 lines. A future
  rename to `UIView.Tween.cpp` or similar would reflect what's left,
  but the rename is not load-bearing.
- **`WidgetTreeHost::observe/unobserveWidgetLayerTreesRecurse`
  bodies neutered to no-ops, declarations + 3 call sites kept.**
  The plan's "move to single window-level observe/unobserve"
  read literally would have meant deleting the methods + their
  callers in `~WidgetTreeHost`, `initWidgetTree`, `setRoot`.
  The bodies are now no-ops (the work is redundant with the
  pre-existing window-level observation at `AppWindow.cpp:151`);
  Phase I cleanup retires the symbol entirely once the rest of
  the dead surface is gone.
- **`View::Impl::proxy` per-view role kept.** The plan included
  "delete... the per-view role of `View::Impl::proxy`". The proxy
  is still used by the FrameBuilder during paint to thread the
  compositor frontend, plus by the animation scheduler entry path
  (`UIView::Impl::startOrUpdateAnimation` reads
  `owner.compositorProxy().getSyncLaneId()`). The "per-view role"
  reduction is a Phase I follow-up — the proxy stays for now.
- **`detail::cancelOwner` + `AnimationHandle` packet-id internals
  not deleted.** The packet-id channel methods
  (`setSubmittedPacketIdInternal`,
  `setPresentedPacketIdInternal`,
  `incrementDroppedPacketCountInternal`,
  `setFailureReasonInternal`) became orphans when the registry
  was removed. The Animation-Scheduler-Plan explicitly tracks
  these for removal in its own Tier; 4.8 stays focused on the
  view-side deletion and leaves the AnimationHandle cleanup to
  that plan.

**Sub-phase landing log:**

- **4.8.1** — `WidgetTreeHost::observeWidgetLayerTreesRecurse` /
  `unobserveWidgetLayerTreesRecurse` bodies emptied (window-level
  observation in `AppWindow.cpp:151-155` already covers what's
  needed); `View::setFrontendRecurse` per-view observation calls
  dropped. ~40 lines deleted, ~10 added (no-op stubs + comments).
- **4.8.2** — `UIView::Impl` dormant fields (`animationViewAnimator`,
  `animationLayerAnimators`, `elementAnimations`,
  `pathNodeAnimations`) deleted; `ensureAnimationViewAnimator` /
  `ensureAnimationLayerAnimator` / `beginCompositionClock` bodies
  deleted; `applyAnimatedColor` / `applyAnimatedShape` orphan
  bodies deleted; `clamp01` / `applyShapeDimension` orphan helpers
  deleted. ~180 lines deleted, ~25 added (deletion-marker comments).
- **4.8.3** — `Composition::ViewAnimator` / `LayerAnimator` /
  `LayerClip` / `ViewClip` declarations deleted from `Animation.h`;
  `friend class Composition::ViewAnimator` deleted from `View.h`;
  `namespace detail { ... }` block (`AnimationRuntimeRegistry` +
  `AnimationInstance` + lane-worker / telemetry / shutdown
  plumbing) deleted from `Animation.cpp` along with `LayerAnimator`
  + `ViewAnimator` member implementations. ~850 lines deleted.
- **4.8.4** — `View::Impl::ownLayerTree` field + initializer
  deleted; `View::getLayerTree()` accessor deleted; `View::resize`
  per-view layer-tree resize callsite dropped; `UIView::Update.cpp`
  layout-validation `getLayerTree()` fallback dropped (rect is
  now the only source of truth); `BasicWidgets.cpp` Container
  detach `notifyObserversOfWidgetDetach()` callsite dropped
  (per-view tree gone, no observers tracked it). ~30 lines
  deleted, ~30 added (comments documenting the new contract).
- **4.8.5** — Visual verification of BasicAppTest pre- and
  post-4.8 (identical render confirms the destructive sweep
  removed nothing the runtime pipeline depended on). `grep`
  validator confirms zero remaining live references to the
  deleted symbols — all 4 remaining mentions are either comment
  references documenting past state or the no-op `observe*` stubs
  flagged for Phase I.

**Files touched:**

- `wtk/include/omegaWTK/Composition/Animation.h` — `LayerClip` /
  `ViewClip` / `LayerAnimator` / `ViewAnimator` declarations
  deleted.
- `wtk/src/Composition/Animation.cpp` — `namespace detail { ... }`
  block (registry + instance lifecycle), `LayerAnimator::*` impls,
  `ViewAnimator::*` impls deleted. Surviving: `AnimationCurve`,
  `ScalarTraverse`, `KeyframeTrack`, `AnimationHandle` core API.
- `wtk/include/omegaWTK/UI/View.h` — `friend class
  Composition::ViewAnimator` deleted; `getLayerTree()` declaration
  deleted.
- `wtk/src/UI/View.Core.cpp` — `getLayerTree` body deleted;
  `setFrontendRecurse` per-view observation calls deleted;
  `resize` per-view layer-tree resize call deleted.
- `wtk/src/UI/ViewImpl.h` — `Impl::ownLayerTree` field + the
  `make_shared<LayerTree>` initializer in `Impl::Impl` deleted.
- `wtk/src/UI/UIViewImpl.h` — four dormant animation fields, the
  three `ensure*` / `beginCompositionClock` method declarations,
  and the two `applyAnimated*` method declarations deleted.
- `wtk/src/UI/UIView.Animation.cpp` — `ensureAnimationViewAnimator`,
  `ensureAnimationLayerAnimator`, `beginCompositionClock`,
  `applyAnimatedColor`, `applyAnimatedShape`, `clamp01`,
  `applyShapeDimension` deleted. Surviving:
  `startOrUpdateAnimation` (routes to scheduler),
  `UIView::animateElement` (public hook),
  `markAllElementsDirty` / `markElementDirty` /
  `isElementDirty` / `clearElementDirty`, `ensureElementNodeId`
  / `tryElementNodeId`, `animatedValue`, `advanceAnimations`
  (stub since 4.4), `resolveFallbackTextFont`.
- `wtk/src/UI/UIView.Update.cpp` — `localBoundsFromView`
  per-view tree fallback dropped.
- `wtk/src/UI/WidgetTreeHost.cpp` —
  `observeWidgetLayerTreesRecurse` /
  `unobserveWidgetLayerTreesRecurse` bodies emptied to no-ops.
- `wtk/src/Widgets/BasicWidgets.cpp` — `Container::unwireChild`
  `notifyObserversOfWidgetDetach()` callsite deleted.

---

Destructive cleanup. Folds Animation-Scheduler-Plan **Tier E**. Nothing
animates against `getLayerTree()->getRootLayer()` anymore (4.4) and
paint is pure (4.7), so the per-view layer infrastructure and the old
animator classes are dead weight.

- Delete `Composition::ViewAnimator` / `LayerAnimator` (+ any
  `LayerClip` / `ViewClip` / runtime registry) from `Animation.{h,cpp}`;
  remove `friend class ViewAnimator` / `LayerAnimator` from `View`.
- Delete `View::Impl::ownLayerTree`, `View::getLayerTree`,
  `View::makeLayer`, `View::startCompositionSession` /
  `endCompositionSession`, and the per-view role of `View::Impl::proxy`.
  Compositor-frontend observation moves entirely to the one
  `windowLayerTree_`.
- Move `WidgetTreeHost`'s `observe/unobserveWidgetLayerTreesRecurse`
  to a single window-level observe / unobserve.
- Delete the now-unused UIView animation fields (`animationViewAnimator`,
  `animationLayerAnimators`, `elementAnimations`, `pathNodeAnimations`)
  and the legacy `UIView.Animation.cpp` paths.
- Validator: full sweep; grep confirms no `ownLayerTree` /
  `getLayerTree` / `ViewAnimator` / `LayerAnimator` references remain;
  all three backends clean.

Files touched: `wtk/include/omegaWTK/Composition/Animation.h`,
`wtk/src/Composition/Animation.cpp`,
`wtk/include/omegaWTK/UI/View.h`, `wtk/src/UI/View.Core.cpp`,
`wtk/src/UI/ViewImpl.h`, `wtk/src/UI/WidgetTreeHost.cpp`,
`wtk/src/UI/UIView.Animation.cpp`, `wtk/src/UI/UIViewImpl.h`.

#### Phase ordering rationale

- **Block 1 before Block 2** (developer decision): the `Canvas` /
  backend collapse is gated only on closing Tier 3 and shares no code
  with the animation work, so it lands first and de-risks the largest
  mechanical deletion before the scene reshape begins.
- **4.0 before 4.1 before 4.2:** the parallel `DrawOp` switch must
  exist before `FrameBuilder` can target it (4.1), and both must be
  live before the `VisualCommand` path and `Canvas` are deleted (4.2).
  Classic additive-then-destructive.
- **4.3 before 4.4:** the scheduler must exist (and bake) before
  UIView's animation state is rerouted onto it. The side table has to
  be populated before paint can read it.
- **4.4 before 4.7:** paint can only become a pure function once
  animation no longer writes during the walk. `UIView::paint` reads
  the side table that 4.4 establishes.
- **4.5 before 4.6:** `Container` / coordinator layout (Arrange-only)
  is the simpler migration and lets `LayoutManager` + the clamp helper
  bed in before `StackWidget`'s flex (which adds the Measure pass)
  builds on top.
- **4.5 and 4.6 before 4.7:** the Measure / Arrange passes 4.7 wires
  into the frame loop need every `LayoutManager` (coordinator,
  `Container`, flex) to exist and to be the only child-layout path;
  otherwise centralizing invocation would strand a bespoke
  `layoutChildren()`.
- **4.8 last:** deleting `ownLayerTree` and the old animators is safe
  only after nothing animates against the per-view tree (4.4) and paint
  no longer touches `getLayerTree` (4.7). It is pure cleanup and
  reverts cleanly as long as 4.3–4.7 stay landed.

#### What Tier 4 explicitly does NOT do

- Does not add **region-based dirty culling** — paint-everything-dirty
  -or-nothing stays; the union-of-dirty-rects pass is Tier 5 (§6 Q4).
- Does not move **Measure / Arrange / Paint or the scheduler off the UI
  thread** — the `DisplayList` snapshot stays the only thread boundary
  (§6 Q5).
- Does not add **layout managers beyond today's container kinds** —
  `FillLayout` / `StackLayout` / `AbsoluteLayout` / `ContainerLayout` /
  `FlexLayout` cover the current widgets (4.5/4.6). `GridLayout` /
  `TableLayout` / `TreeLayout` arrive as reusable built-ins *with* their
  `Grid` / `Table` / `Tree` container widgets in later work, not in
  Tier 4. (`FlexLayout` *is* in Tier 4 — `StackWidget` requires it.)
- Does not fold in the **Style/StyleSheet refactor** — Tier 4 adds only
  the pre-paint Style *pass* (hoisting today's inline resolution);
  the global selector-matched `StyleSheet` engine stays its own plan
  (§7).
- Does not change the **intra-`UIView` element layout surface** —
  `UIViewLayoutV2` / `UIElementLayoutSpec` stay as the element
  authoring surface inside a `UIView`; only child-*node* layout moves
  to `LayoutManager`.
- Does not act on **`wantsLayer()`** — no compositor-thread retained
  scroll-layer texture yet (§6 Q3).
- Does not add **`PushOpacity` / `PopOpacity` or `PushEffect` /
  `PopEffect`** replay handling beyond what a producer needs (deferred
  until a producer appears, as in Tier 3).
- Does not **re-specify the `AnimationScheduler` internals** — those
  are owned by Animation-Scheduler-Plan.md; Tier 4 only sequences and
  integrates them.
- Does not **rename or delete `Widget` / `UIView`** — Widgets stay as
  light `View` wrappers and the existing type names are kept (§6 Q1/Q2).

##### Phase I (follow-up) — Dead-code sweep: orphaned API + Impl surface left over by Tiers 2–4

Tracked alongside Phases E–H; **post-Tier-4**, sequenced after Phase 4.8
so the live animation-surface deletion lands first and Phase I closes
out the remaining residue. Found during the Phase 4.4 implementation
sweep: a number of UIView / View / Style symbols are reachable from no
caller in the tree but were not in 4.8's direct scope (4.8 deletes the
*active*-but-being-replaced animation runtime — `ViewAnimator` /
`LayerAnimator` / `LayerClip` / `ViewClip` / `AnimationRuntimeRegistry`
/ per-view `ownLayerTree`). Phase I deletes the *passive* leftovers
that survived prior refactors with no consumer.

Phase I is not load-bearing — none of these symbols affect runtime
behaviour. It exists so the post-Tier-4 surface presents only live
code, and so future readers do not waste time threading through
methods, fields, and structs that resolve to nothing.

**Public API surface (one-cycle deprecation, then delete).** Each is a
public symbol that may have an out-of-tree caller; mark `[[deprecated]]`
in the first Phase I PR, delete in the next:

- `UIView::AnimationDiagnostics` struct + `UIView::getLastAnimationDiagnostics()`
  accessor. The only writer (`Impl::advanceAnimations`) became a stub
  in 4.4; there is no reader in the tree.
- `UIView::UpdateDiagnostics` struct + `UIView::getLastUpdateDiagnostics()`
  accessor. The three `*TagCount` size_t fields are written nowhere;
  `revision` is incremented in `UIView::update` but never read.
- `UIView::EffectState` struct. Only consumer was the dead
  `Impl::lastResolvedEffects` map (see Impl section below).
- `Style::elementBrushAnimation` / `elementAnimation` / `elementPathAnimation`
  builder methods, the matching `Style::Entry::Kind::ElementBrushAnimation`
  / `ElementAnimation` / `ElementPathAnimation` enum values, and the
  `Style::Entry::animationKey` field. `UIView.Style.cpp` enumerates
  these kinds inside `collectStyleScope` for scope-tag tracking only;
  the style-application pass has no case branches for them, so the
  stored requests never reach the scheduler. The Style → scheduler
  bridge that *would* consume them is Animation-Scheduler-Plan Tier D
  (StyleResolver friend hook), which lands with the Style refactor —
  at which point a fresh, smaller surface replaces these.
- `enum ElementAnimationKey` (UIView.h). Consumed only by the Style
  entries above and by the orphan `applyAnimated*` readers below.
- `View::renderTargetHandle()` (both `&` and `const &` overloads).
  Defined in `View.Core.cpp`; no caller in the tree.
- `View::isRootView()` and `View::isEnabled()`. Pre-3.x debug
  accessors with no caller (the live setters `enable()`/`disable()`
  stay). The parent-pointer / enabled checks they wrapped happen
  internally on `View::Impl` now.
- `virtual View::wantsLayer()` (defaulted `false`, overridden by
  `ScrollView` to `true`). Stub for the future compositor-thread
  retained scroll-layer texture (Tier 5 §6 Q3); no caller invokes
  it today. The ScrollView override carries the same lifetime —
  either both go in Phase I and Tier 5 reintroduces it cleanly, or
  both stay marked-but-undeleted until Tier 5 lands; lean toward
  the former to keep the API surface honest.
- `virtual View::scrollOffsetContribution()` (returns `{0,0}`). Pre-3.6
  scroll-offset path replaced by `View::contentOffset()`; no caller.
  ScrollView.cpp:88 acknowledges this as legacy.
- `View::endCompositionSession()`. Empty body; called from
  `Widget.Paint.cpp` and `SVGView.cpp` but does nothing. Delete the
  method + the call sites in one go (the `startCompositionSession()`
  twin stays — it still does the lane/frontend propagation that
  newly-mounted subviews depend on).
- `virtual View::submitPaintFrame(int)`. No-op virtual since Phase
  3.8/3.9 collapsed the per-view canvas; the one call site in
  `Widget.Paint.cpp:67` keeps the comment but the call itself is
  pure overhead. No remaining overrides.

**Impl-private surface (delete outright, no deprecation needed).** These
do not cross the public boundary; remove in the same PR as the public
surface deprecations:

- `UIView::Impl` dead `Map<UIElementTag, …>` fields, all written and
  read by no one: `lastResolvedElementColor`, `lastResolvedEffects`,
  `previousShapeByTag`, `lastResolvedV2Rects_`. The
  `lastResolvedV2Rects_` map was the input to the
  `resolveLayoutTransition → applyLayoutDelta` sheet-driven layout-
  transition path that Tier B / B1 removed; the comment at
  `UIView.Update.cpp:240` documents the removal.
- `UIView::Impl` dead-write flags: `firstFrameCoherentSubmit` (written
  in six places, read in zero) and `styleChangeRequiresCoherentFrame`
  (written in two places, read in zero). `styleDirtyGlobal` survives —
  it has one live reader in `UIView.Style.cpp:346`.
- `UIView::Impl` dead diagnostics plumbing: `lastObservedDroppedPacketCount`,
  `hasObservedLaneDiagnostics`. The only writer was `advanceAnimations`,
  now stubbed; the only reader was the same function. Bundles with
  the `lastAnimationDiagnostics` field above for the same reason.
- `UIView::Impl::framesPerSec` field. Only reader is
  `beginCompositionClock`, which has no caller after 4.4 (and is
  itself slated for 4.8). Delete alongside `beginCompositionClock`
  if the 4.8 sweep does not already get it.
- `UIView::Impl::EffectAnimationKey*` enum constants that paint never
  reads back: `GaussianRadius` / `DirectionalRadius` /
  `DirectionalAngle` (3 entries). The nine paint-reachable shadow
  constants stay: `ShadowOffsetX/Y`, `ShadowRadius`, `ShadowBlur`,
  `ShadowOpacity`, `ShadowColorR/G/B/A` (the four color channels were
  wired into `UIView::paint` and added to `UIView::AnimationChannel`
  on 2026-05-31, after the original Phase I survey, so paint reads them
  per frame to override `shadowParams.color`).
- `UIViewInternal::ResolvedEffectTransition` struct and the matching
  `dropShadowTransition` / `gaussianBlurTransition` /
  `directionalBlurTransition` fields on `ResolvedEffectStyle`. Written
  by `UIView.Style.cpp:207–225`, read nowhere. The transition→
  scheduler bridge that would consume them is the same Anim Tier D
  hook above.
- `UIView::Impl::applyAnimatedColor` / `applyAnimatedShape` — already
  annotated as orphan in `UIView.Animation.cpp` during 4.4. These die
  here alongside the public `ElementAnimationKey*` color/width/height
  enum entries that gated them.

**Open question for Phase I — `Widget::PaintOptions::autoWarmupOnInitialPaint`
+ `warmupFrameCount`.** `Widget.Paint.cpp:58–62` still consults both,
but with `submitPaintFrame` deleted (above) the multi-submission warm-up
loop has nothing to do. Decide at Phase I time whether the warm-up
abstraction has any remaining real purpose; if not, retire it.

**Out of scope (owned by other phases):**

- `Composition::ViewAnimator` / `LayerAnimator` / `LayerClip` /
  `ViewClip` / `AnimationRuntimeRegistry` — **Phase 4.8** (Anim Tier E).
- Per-view `View::Impl::ownLayerTree` — **Phase 4.8**.
- `UIView::Impl::animationViewAnimator` / `animationLayerAnimators` /
  `elementAnimations` / `pathNodeAnimations` fields and the dormant
  `ensureAnimationViewAnimator` / `ensureAnimationLayerAnimator` /
  `beginCompositionClock` / `advanceAnimations` / `tickAnimations`
  methods — **Phase 4.8** (paired with the active surface they used to
  back).

Sequencing: Phase I runs after 4.8 so 4.8's deletion lands cleanly
without Phase I racing it. The Phase I PR is its own commit — easy
to revert if an out-of-tree caller surfaces against any of the
deprecated public symbols.

---

## 5. What gets deleted

At the end of Tier 4:

- `UIView.Update.cpp` (391 lines) → `UIView::paint` (~80 lines)
- `UIView.Core.cpp` session/dirty plumbing (~200 lines) → gone
- `UIView.Animation.cpp` (537 lines) → ~150 lines against
  `AnimationScheduler`
- `View.Core.cpp` sync-lane + frontend recurse + session methods
  (~60 lines) → gone
- `View.ResizeCoordinator.cpp` → gone
- `ViewResizeCoordinator` class → gone
- `Container::layoutChildren()` / `clampChildRect()` and
  `StackWidget::layoutChildren()` → gone; the layout math moves behind
  `ContainerLayout` / `FlexLayout` `LayoutManager`s
- `localBoundsFromView` + static map → gone
- `View::Impl::ownLayerTree`, per-view proxy → gone
- `Canvas` class itself (~500 LOC across `Canvas.h`/`Canvas.cpp`) → gone
- `VisualCommand` + `VisualCommand::Data` union → gone (replaced by `DrawOp`)
- `CanvasFrame` + `CanvasEffect` → gone (replaced by `DisplayList`)
- `CompositorClient::pushFrame(CanvasFrame, …)` path → gone
- Five overlapping dirty flags → one `DirtyBits`

**Estimated deletion: ~1700 LOC. Estimated addition: ~700 LOC.**
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

   ANSWER: Widget's are effectively a light wrapper around View. (KEEP THEM)

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

   ANSWER: Tier 5.

5. **Threading.** Chromium runs layout on one thread and compositing
   on another. OmegaWTK is currently single-threaded for paint. This
   plan keeps it single-threaded. The `DisplayList` snapshot is the
   hand-off point where a future compositor thread *could* take over,
   but nothing here forces that decision.

   ANWSER: There is a current Compositor Worker Thread seperate from the main UI thread that executes all the GPU commands. Check the Animation-Scheduler-Plan.md. about threading in Animation context.

   

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
- **[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md)**
  (formerly `Animation-API-Simplification-Plan.md`) — prerequisite
  for Tier 4. Its Tiers A/B/C land as Phases 4.3/4.4/4.8; Tier D
  wires into Style Plan Tier 3.
- **[Animation-Surface-Expansion-Plan.md](Animation-Surface-Expansion-Plan.md)** —
  Sequenced AFTER this plan's Tier 4 + Phase I. Phase 4.4 added a
  public stop-gap (`UIView::animateElement` + the `AnimationChannel`
  enum) so the scheduler had a clickable validator (the
  ContainerClampAnimationTest "Animate Shadow" menu). That stop-gap
  is replaced by the expansion plan's Tier 1: a fluent
  `View::animate()` builder reachable from every `View` subclass,
  with per-subclass surfaces for `UIView` (element-scoped), `SVGView`
  (SVG-element-scoped + path morphing), and `ScrollView` (smooth-
  scroll / fling / snap). Phase I's deletion of the orphan
  `Style::elementAnimation` / `elementBrushAnimation` /
  `elementPathAnimation` builders + the `ElementAnimationKey` enum
  + the `applyAnimatedColor` / `applyAnimatedShape` readers is what
  unblocks `UIViewAnimationBuilder::elementFill` / `elementTransform`
  in that plan's Tier 1.
- **`Style-StyleSheet-Refactor-Plan.md`** — **kept separate, not
  folded into Tier 4.** That plan's *engine* tiers align with the
  *earlier* render tiers (its Tier 1 / 2 / 3 ↔ render Tiers 1 / 2 / 3),
  and its Tier 4 is the WML compiler that ships *after* this plan's
  Tier 4 as a purely additive separate library. The only contact
  point with this Tier 4 is the per-node resolved-style cache: that
  plan's `ComputedStyle` is exactly the §3.6 `ResolvedStyleCache` that
  this tier's Style pass writes and `paint` reads. So Tier 4 only
  needs a Style *pass* (it hoists today's inline `resolveViewStyle`
  into pre-paint, §4 Phase 4.7 + cross-cutting); when the Style plan
  lands it swaps that pass's internals (cascade, selectors, themes)
  without changing Tier 4's paint contract. The shared
  `UIViewLayoutV2` boundary is already consistent: both plans keep it
  as the element authoring surface (that plan's §6 Q6/Q7, this plan's
  §3.3). Folding the full selector/cascade/theme engine into Tier 4
  would couple two independent refactors and balloon an already-large
  tier for no gain — paint works with the inline-resolved cache today.
- **`Widget-View-Paint-Lifecycle-Plan.md`** — its Tier A (the
  `DirtyBits` field) and Tier C (one `FrameBuilder` per window) were
  already absorbed under render-redesign Phases 3.1 / 3.8. This tier
  realizes its Tier D — `View::paint(PaintContext&)` + the
  `DirtyBits`-driven loop (Phase 4.7) — but, per §6 Q2, keeps `Widget`
  as a light `View` wrapper rather than deleting `Widget::executePaint`
  (it becomes a thin invalidate-and-request-frame call).
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

        // 2. Apply the scroll offset by *updating the descendants'
        //    rects* (a 2D position change), not by pushing a
        //    transform. The DisplayList's `PushTransform` op carries
        //    a `Matrix4x4` 3D-effect transform and is not the
        //    correct mechanism for 2D translation. FrameBuilder
        //    consults `contentOffset()` when arranging the content
        //    subtree, so descendants observe scroll-shifted rects
        //    natively.
        // (FrameBuilder recurses to contentChild_ after this returns.)

        // 3. Overlay scroll bars on top, OUTSIDE the scroll clip.
        // The FrameBuilder pops the clip after children paint; bars
        // are emitted as a post-children overlay.
    }

    /// FrameBuilder reads this when arranging contentChild_ so that
    /// descendant `finalRect`s are offset by -scrollOffset_. Replaces
    /// the pre-Tier-2 `scrollOffsetContribution()` + `computeWindowOffset`
    /// walk and the (rejected) `pushTransform`-as-translation approach.
    Point2D contentOffset() const override { return -scrollOffset_; }

    void paintOverlay(PaintContext & pc) override {
        // Called by FrameBuilder after children paint and after
        // pushClip is popped.
        if (hasV_) appendVerticalBar(pc, vBar_, scrollOffset_, contentSize_);
        if (hasH_) appendHorizontalBar(pc, hBar_, scrollOffset_, contentSize_);
    }
};
```

The `scrollOffsetContribution()` hook is replaced by ScrollView's
`contentOffset()` override, which FrameBuilder folds into the arrange
pass — descendants get scroll-shifted rects in the layout pipeline,
not at paint time. The 2D translation never becomes a transform op;
the `DisplayList`'s `PushTransform` is reserved for 3D effects only
(see §3.2). The two overlay layers and their per-canvas painting
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
        // Subscribe to layout completion. The Tier-2 firing point is
        // `View::resize()` on rect changes; Tier 3's FrameBuilder
        // will fire it from the end of Arrange instead. The shape of
        // the subscriber is the same in both eras: receive the new
        // rect, sync the native item's bounds.
        onLayoutResolved.subscribe([this](const Composition::Rect & r){
            nativeItem_->syncBounds(r, computeWindowOriginContribution());
            nativeItem_->syncVisibility(isEnabled());
        });
    }

    void paint(PaintContext & pc) override {
        // Single op: reserve this rect; the native layer paints here.
        // Constructed through the static factory because `NativeContent`
        // is also the enum value (see §2.5).
        pc.displayList.append(DrawOp::makeNativeContent(
            finalRect(),
            nativeItem_->id(),
            /*zOrderHint=*/0));
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
| `ScrollView` | — | `PushClip` op added to DrawOp set (`PushTransform` also lands but is for 3D effects, not for scroll translation) | overlay scroll bar layers removed; `wantsLayer()` introduced; `scrollOffsetContribution` replaced by ScrollView's `contentOffset()` override that the FrameBuilder folds into Arrange (no transform pushed) | `computeWindowOffset` deleted |
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

   This should be addressed in a follow up after the Animation-Scheduler-Plan.

2. **`ScrollView` overlay vs. sibling bars.** Scroll bars could be
   emitted by ScrollView's own paint as `paintOverlay` (sketched in
   §9.3) or modeled as sibling Views. Overlay is simpler; siblings
   are more flexible (themable independently, animatable
   independently). Recommendation: overlay for Tier 3, sibling
   nodes only if the theming/animation surface area demands it.
   
   Yes. (Eventually we would want to customize them.)

3. **`DrawOp::NativeContent` op shape.** The op shape should be
   designed jointly with [NativeViewHost-Adoption-Plan.md](NativeViewHost-Adoption-Plan.md)
   so a single op covers video, GTE, and future native embeds. Open
   sub-questions: does the op need a z-order hint for platforms with
   multiple native layer ordering modes (CA's `addSublayer` order vs.
   DirectComposition's visual tree)? Does it need a colorspace /
   HDR-metadata slot for video? Recommendation: minimal op for Tier
   2 (`destRect` + `hostId`), additive fields as the adoption plans
   need them.

   NativeContent does need a z-order hint so it renders above the bottom layer.

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

   Agreed.

5. **Airspace and 2D-over-native overlays.** §9.4.1 accepts the
   airspace constraint (native draws on top of 2D within the host's
   rect). Some use cases (subtitle rendering, playback controls)
   need 2D pixels on top of video. Two paths exist: (a) render the
   2D inside the native layer's compositing system (platform-
   specific), (b) provide a sibling NativeViewHost that the OS
   composites above. Both are out of scope for this plan; flagged
   for the NativeViewHost adoption plan to decide per-platform.

   Out of scope for plan, but we should provide NativeViewOverlay which can be drawn on by the compositor.
   (We can use CALayerTree, VKLayerTree, and DCVisualTree to help layer all of this.)

6. **`OmegaGTEView` direct present.** [OmegaGTEView-Proposal.md](OmegaGTEView-Proposal.md)
   and the NativeViewHost adoption plan together specify a
   direct-present mode where the 3D content is the *only* thing in
   the host's rect — no compositor blit. In that mode the
   `DrawOp::NativeContent` carve-out is still emitted, but the
   compositor backend treats it the same way it treats VideoView's
   carve-out: clear the rect, let the native layer draw. No special
   case at the SceneNode level.

   Yes. OmegaGTEView controls everything.

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
