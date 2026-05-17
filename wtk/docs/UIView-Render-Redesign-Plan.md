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

##### Phase 3.1 — `FrameBuilder` skeleton + window-level composition session

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

##### Phase 3.2 — `UIView` opt-in: hand its `DisplayList` to `FrameBuilder`

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

##### Phase 3.3 — `SVGView` opt-in

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

##### Phase 3.4 — `FrameBuilder` transform accumulator + `computeWindowOffset` rewire

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

##### Phase 3.5 — `DisplayListReplay` real implementation for `PushClip` / `PopClip`

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

##### Phase 3.6 — `ScrollView` migration

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

##### Phase 3.7 — `DisplayListReplay` real implementation for `NativeContent` (carve-out)

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

##### Phase 3.8 — delete per-view `LayerTree` + per-view `Canvas`; remove the flag

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

##### Phase 3.9 — `CanvasView` deletion

The Phase 2.6 `[[deprecated]]` markers gave the grep handle. Now
the in-tree callers migrate and the class is removed.

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

### Tier 4 — introduce `SceneNode` + `LayoutManager`, retire `UIView::update`

- `UIView` becomes `UIViewNode : SceneNode`.
- `View::Impl::subviews` becomes `SceneNode::children_`.
- `ViewResizeCoordinator` deleted; replaced by
  `SceneNode::layoutManager_`.
- `UIView::update()` removed; frames are produced by the window pacer
  reading `DirtyBits` at the root.
- Animation state migrated out of `UIView::Impl` into
  `AnimationScheduler` (prereq: animation simplification plan).
- `VisualCommand` and `CanvasFrame` deleted.
  `BackendRenderTargetContext::renderToTarget` rewritten to dispatch on
  `DrawOp` directly (mechanical case-label rename + field-access
  touch-up; rasterization code unchanged).
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

   ANSWER: Widget's are effectively a light wrapper around View.

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
