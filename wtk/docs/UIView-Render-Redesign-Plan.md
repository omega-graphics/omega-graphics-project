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
   `DrawOp` is a tagged union: `FillRect`, `FillRoundedRect`,
   `FillEllipse`, `StrokePath`, `DrawText`, `PushTransform`,
   `PopTransform`, `PushClip`, `PopClip`, `PushOpacity`, `PopOpacity`,
   `PushEffect` (shadow/blur), `PopEffect`. No per-op resource
   creation. Paint appends. Composition reads.

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
  the batched scheduler wants.
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
