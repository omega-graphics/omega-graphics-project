# Unified Composition Session

## Problem

Composition sessions can exist in many places -- `UIView::update`, `SVGView::renderNow`, `VideoView::presentCurrentFrame`, `Widget::executePaint` -- and that's fine. Sessions record composition commands. The problem is **when and how packets are sent**.

Each `View` has its own `CompositorClientProxy` with its own `ViewRenderTarget`. When a session ends (`endCompositionSession` → `proxy.endRecord()`), the proxy's `recordDepth` drops to zero and it calls `submit()`, draining its command queue into a compositor packet tagged with its own render target. The compositor creates a separate `BackendCompRenderTarget` (and separate visual tree / Vulkan surface) per render target. Content across different render targets never composites together.

A single `Widget::executePaint` cycle can trigger multiple independent submissions:

```
Widget::executePaint
  view->startCompositionSession()           // root view proxy: depth 1
    onPaint(context, reason)
      context.clear(White)
      svgView->renderNow()
        svgView->startCompositionSession()  // svgView proxy: depth 0→1
          ... draw SVG ops ...
          svgCanvas->sendFrame()
        svgView->endCompositionSession()    // svgView proxy: depth 1→0 → SUBMIT (Packet A, SVG render target)
      uiView->update()
        uiView->startCompositionSession()   // uiView proxy: depth 0→1
          rootCanvas->sendFrame()
          elementCanvas->sendFrame()
        uiView->endCompositionSession()     // uiView proxy: depth 1→0 → SUBMIT (Packet B, UIView render target)
    canvas->sendFrame()
  view->endCompositionSession()             // root view proxy: depth 1→0 → SUBMIT (Packet C, root render target)
```

Three packets, three render targets, three independent visual trees.

### Where sessions currently submit

| Call site | Proxy | Submits independently |
|-----------|-------|----------------------|
| `Widget::executePaint` | Root `View` proxy | Yes -- on `endCompositionSession` |
| `UIView::update` (2 paths) | `UIView` proxy | Yes |
| `SVGView::renderNow` | `SVGView` proxy | Yes |
| `VideoView::presentCurrentFrame` | `VideoView` proxy | Yes |
| `VideoView::flush` | `VideoView` proxy | Yes |
| `VideoView::stop` / `unbindPlaybackSource` | `VideoView` proxy | Yes |

### SVGViewRenderTest (current state)

The SVGWidget's view IS the SVGView (passed directly as the Widget's view via `SVGView::Create`). `renderNow()` is called inside `onPaint` after the clear. Since the widget's root view and the SVGView are the same object, composition goes through the same proxy/render target. This test works because there's no child view with a separate render target.

The multi-render-target problem manifests in tests like EllipsePathCompositorTest and TextCompositorTest where child UIViews have their own proxies.

## Correction: 1 Widget = 1 View = 1+ Layers

The previous version of this plan proposed converging all subview composition commands into the root view's proxy via recursive flush and command transfer. **That approach is wrong.** It violates the fundamental ownership model established by the Widget Child Ownership Refactor.

### The principle

A Widget is a wrapper around its own View. That's it.

- **1 Widget = 1 View/ViewRenderTarget = 1+ Layers/Layer render targets.**
- A Widget manages its own view's composition session and submission. It does not reach into child views.
- A Widget cannot see its children. Only a Container can have children. Even a Container does not merge child rendering into its own render target.
- Each View has its own `CompositorClientProxy`, its own `ViewRenderTarget`, and its own `LayerTree`. These are the correct isolation boundaries.
- The compositor is responsible for combining multiple render targets into the final output. That is a compositor concern, not a widget concern.

### The render target hierarchy

Views and Layers are both render targets, but at different levels:

```
Widget
  └─ View (1:1)
       ├─ ViewRenderTarget ──→ platform window surface (not directly managed)
       └─ LayerTree (1:1 with View)
            ├─ Root Layer ──→ platform animation layer (directly managed)
            ├─ Child Layer ──→ platform animation layer (directly managed)
            └─ Child Layer ──→ platform animation layer (directly managed)
```

#### ViewRenderTargets (platform window surfaces)

ViewRenderTargets wrap the platform's native window surface. We don't directly manage these — the platform owns them:

| Platform | Native type |
|----------|-------------|
| macOS | `NSView` |
| Windows | `HWND` |
| Linux/GTK | `GdkWindow` (managed by our custom animation layer) |

#### LayerRenderTargets (platform animation layers)

Layers are the render targets we directly manage. Each Layer maps to a platform animation/compositing primitive that supports effects (transforms, drop shadows, opacity):

| Platform | Native type | Notes |
|----------|-------------|-------|
| macOS | `CAMetalLayer` | Core Animation layer — GPU-backed, supports implicit animations and effects. The `MTLCALayerTree` manages the CALayer hierarchy. |
| Windows | DComp Visual | DirectComposition visual — GPU-composited, supports transforms, effects, and visual trees. The `DCVisualTree` manages the DComp visual hierarchy. |
| Linux/GTK | Custom animation layer | GTK has no advanced animation layer like Core Animation or DComp, so we invent our own on the backend. The `VKLayerTree` renders each Layer to a `GETexture` (VkImage) and blits them onto the `GdkWindow` surface. |

Each Layer gets its own `BackendRenderTargetContext` with its own GPU texture, fence, and effect pipeline. The `BackendCompRenderTarget::surfaceTargets` map holds one render context per Layer.

#### How they compose

- **Layer → ViewRenderTarget.** The platform animation layer composites Layer render targets onto the View's window surface. On macOS, Core Animation composites CAMetalLayers within the NSView. On Windows, DComp composites visuals within the HWND. On Linux, our custom backend blits VkImages onto the GdkWindow.

- **Views do not compose onto other Views.** Each View's `BackendCompRenderTarget` is independent. There is no mechanism to blit one View's output onto another View's window surface. This is why command convergence between View proxies is wrong — even if you transfer commands, the render target contexts belong to different native surfaces.

### Why command convergence is wrong

Walking the child widget/view tree to transfer commands between proxies sends the wrong packets to the wrong render targets:

1. **Each proxy is stamped with its own render target.** A `CompositorPacketCommand` carries the `ViewRenderTarget` of the proxy that submits it. If child view commands are transferred to the parent proxy, they arrive at the compositor tagged with the parent's render target. The compositor maps them to the parent's `BackendCompRenderTarget` and visual tree. The child's actual render target and visual tree receive nothing. The geometry ends up on the wrong surface.

2. **Each View's LayerTree is observed independently.** `View::setFrontendRecurse` registers each view's `ownLayerTree` with the compositor under its own sync lane. The compositor expects packets for each LayerTree to arrive through the proxy associated with that LayerTree's render target. Redirecting commands through a parent proxy breaks this correspondence.

3. **After the Widget Child Ownership Refactor, a Widget has no `children` vector.** Only Container does, and Container exposes children through `childWidgets()`. Widget is a leaf by default. There is no child tree for Widget to walk. The proposed `View::flushCompositionRecurse()` and `View::collectChildCommandsRecurse()` assume the parent widget can reach into child views — it can't and shouldn't.

## Design

### Each Widget paints itself, period

`Widget::executePaint` manages only its own view's composition session:

```cpp
void Widget::executePaint(PaintReason reason, bool immediate) {
    ...
    view->startCompositionSession();
    onPaint(context, activeReason);
    canvas->sendFrame();
    view->endCompositionSession();   // submits this widget's commands
                                      // through this view's proxy
                                      // to this view's render target
    ...
}
```

No recursive flush. No command collection. No reaching into child views. The session starts, the widget paints, the session ends and submits. That's the scope.

### Child widgets paint independently

`WidgetTreeHost::invalidateWidgetRecurse` already triggers each widget to paint independently:

```cpp
void WidgetTreeHost::invalidateWidgetRecurse(Widget *parent,
                                             PaintReason reason,
                                             bool immediate) {
    if(parent->paintMode() == PaintMode::Automatic) {
        parent->invalidate(reason);   // triggers executePaint on THIS widget
    }
    for(auto & child : parent->childWidgets()) {   // post-refactor
        invalidateWidgetRecurse(child, reason, immediate);
    }
}
```

Each widget in the tree receives its own `invalidate` call, which eventually calls `executePaint` on that widget. Each widget's `executePaint` submits through its own view's proxy. No convergence needed.

### Composition wiring is per-widget, not per-view-tree

Currently `executePaint` (lines 203–211) calls `view->setFrontendRecurse()` and `view->setSyncLaneRecurse()`, which walk the entire subview tree — including subviews that belong to child widgets (added via `addSubView` in `setParentWidgetImpl`). This is a boundary violation: the parent widget overwrites compositor wiring on child widgets' views.

After the Widget Child Ownership Refactor, compositor wiring for child widgets happens through `Container::addChild` → `child->setTreeHostRecurse(treeHost)`, which sets each child widget's own view frontend and lane. The parent's `executePaint` should only set its own view:

```cpp
void Widget::executePaint(PaintReason reason, bool immediate) {
    ...
    if(treeHost != nullptr) {
        auto desiredFrontend = treeHost->compPtr();
        auto desiredLane = treeHost->laneId();
        if(view->proxy.getFrontendPtr() != desiredFrontend ||
           view->proxy.getSyncLaneId() != desiredLane) {
            // Only set on this widget's own view. Do not recurse.
            view->proxy.setFrontendPtr(desiredFrontend);
            view->proxy.setSyncLaneId(desiredLane);
        }
    }
    ...
}
```

`setFrontendRecurse` and `setSyncLaneRecurse` remain available for `setTreeHostRecurse` to use (wiring a widget's internal subview tree when the widget first attaches to a tree host), but `executePaint` must not re-walk the full subview tree every paint cycle.

### Async content (VideoView, animations)

VideoView's `presentCurrentFrame` / `flush` happen outside the paint cycle (playback timer, decode callback). These already work correctly under the per-widget model: the VideoView submits through its own proxy to its own render target. No change needed.

### What stays the same

- **Sessions stay where they are.** `UIView::update`, `SVGView::renderNow`, `VideoView` methods all keep their `startCompositionSession` / `endCompositionSession` calls. Each records commands into its own view's proxy and submits independently.
- **The session depth mechanism is unchanged.** Nested sessions still work via `recordDepth`.
- **The compositor observes each View's LayerTree independently** via `observeWidgetLayerTreesRecurse` → `compositor->observeLayerTree(rootTree, syncLaneId)`. Each LayerTree maps to one render target, one visual tree.
- **`View::setSyncLaneRecurse`** already assigns each View its own sync lane (via `g_viewSyncLaneSeed`), so per-view packets don't block each other's budget/inFlight counters.

## Methods that violate the 1:1 principle (to fix)

### Existing code

| Method | File | Problem |
|--------|------|---------|
| `Widget::executePaint` L203–211 | Widget.cpp | Calls `view->setFrontendRecurse()` / `view->setSyncLaneRecurse()` every paint cycle, walking into child widgets' subviews. Should set only its own view's proxy. |
| `View::setFrontendRecurse` | View.cpp:452–466 | Walks all subviews including child widget views. When called from `executePaint`, this overwrites wiring that child widgets own. Acceptable when called from `setTreeHostRecurse` during tree attachment (one-time wiring of a widget's own internal subview tree). |
| `View::setSyncLaneRecurse` | View.cpp:468–479 | Same issue. Walks all subviews. When called from a parent widget's `executePaint`, it overwrites lanes on child widget views. |

### Deleted from this plan (previously proposed, now rejected)

| Method | Reason |
|--------|--------|
| `View::flushCompositionRecurse()` | Walked child views to flush composition — Widget cannot reach into child views. |
| `View::collectChildCommandsRecurse()` | Transferred child proxy commands to parent proxy — sends commands to wrong render target. |
| `CompositorClientProxy::transferCommandsTo(proxy)` | Enabled command convergence between proxies — violates per-view render target isolation. |
| `CompositorClientProxy::flushRecordedCommands()` | Deferred flush for cross-view batching — no cross-view batching exists. |
| `CompositorClientProxy::deferSubmission` flag | Controlled deferred vs. immediate submission for batching — not needed when each widget submits independently. |

## File Change Summary

| File | Change |
|------|--------|
| `Widget.cpp` | `executePaint` stops calling `view->setFrontendRecurse()` / `view->setSyncLaneRecurse()`. Sets only `view->proxy.setFrontendPtr()` / `view->proxy.setSyncLaneId()` directly. |
| `View.h` | No new methods. No `flushCompositionRecurse`, no `collectChildCommandsRecurse`. |
| `View.cpp` | No changes to `setFrontendRecurse` / `setSyncLaneRecurse` signatures, but their use from `executePaint` is replaced by direct proxy calls. They continue to be used by `setTreeHostRecurse` for one-time wiring. |
| `CompositorClient.h` | No new methods. No `flushRecordedCommands`, no `transferCommandsTo`, no `deferSubmission` flag. |
| `CompositorClient.cpp` | No changes. `endRecord()` continues to call `submit()` when `recordDepth` drops to zero. |

No changes to UIView, SVGView, or VideoView session call sites.

## Verification

- `Widget::executePaint` submits one packet through its own view's proxy to its own `ViewRenderTarget`. Does not touch child views.
- `WidgetTreeHost::invalidateWidgetRecurse` triggers independent paint on each widget in the tree.
- Each View's `CompositorClientProxy` submits to its own `ViewRenderTarget`. No command transfer between proxies. The compositor maps each `ViewRenderTarget` to a `BackendCompRenderTarget` with per-Layer `BackendRenderTargetContext`s. Layer textures blit onto the View's native surface.
- EllipsePathCompositorTest: UIView geometry submits through UIView's proxy to the UIView's native render target; root view background submits through root proxy to the root native render target. Each View's LayerTree composites its own Layers independently.
- TextCompositorTest: text and accent layers are separate Layers within the same View's LayerTree. They render to separate `BackendRenderTargetContext`s and composite onto the same ViewRenderTarget.
- SVGViewRenderTest: SVG shapes on white background (same view — unchanged).
- VideoView async playback: submits through its own proxy to its own ViewRenderTarget independently (already correct).
