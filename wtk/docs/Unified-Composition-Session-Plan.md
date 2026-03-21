# Unified Composition Session

## Problem

When a Widget paints, its rootView and any child UIViews submit compositor commands through **separate proxies** that each own a distinct `ViewRenderTarget`. This means:

1. The Widget's rootView canvas (background clear, low-level draw calls) routes through `rootView->proxy` with `ViewRenderTarget A`.
2. The UIView's canvases (element shapes, root background) route through `uiView->proxy` with `ViewRenderTarget B`.

The compositor creates a separate `BackendCompRenderTarget` (and separate `BackendVisualTree` → separate Vulkan surface) for each `ViewRenderTarget`. These two visual trees present to two independent swapchains. The compositing pass (`compositeAndPresentTarget`) only combines layers **within** a single visual tree — it never combines content across visual trees.

Result: the Widget's black background and the UIView's red rectangle render to separate Vulkan surfaces and can never appear composited together on the same swapchain image.

### Why the proxies are separate

Each `View` constructor (`View.cpp:256-277`) creates its own `ViewRenderTarget` and `CompositorClientProxy`:

```cpp
View::View(const Core::Rect & rect, Composition::LayerTree *layerTree, ViewPtr parent):
    renderTarget(std::make_shared<Composition::ViewRenderTarget>(
            Native::make_native_item(sanitizeRect(rect, ...)))),
    proxy(std::static_pointer_cast<Composition::CompositionRenderTarget>(renderTarget)),
    ...
```

The UIView is constructed as a child View, so it gets its own render target and proxy. `startCompositionSession` copies the parent's `frontend` pointer and `syncLaneId` but not the `renderTarget` — so commands are queued to the correct compositor but reference different render targets.

### Current frame submission order

During `Widget::executePaint` (Widget.cpp:228-242):

```
rootView->startCompositionSession()     // rootView proxy depth = 1
  onPaint(context, reason)
    uiView->update()
      uiView->startCompositionSession() // uiView proxy depth = 1
        rootCanvas->sendFrame()         // → queued on uiView proxy
        elementCanvas->sendFrame()      // → queued on uiView proxy
      uiView->endCompositionSession()   // uiView proxy depth = 0 → SUBMIT (Packet A)
  canvas->sendFrame()                   // → queued on rootView proxy (warmup x2 on initial)
rootView->endCompositionSession()       // rootView proxy depth = 0 → SUBMIT (Packet B)
```

Packet A and Packet B target different `CompositionRenderTarget`s, so they create different backend visual trees and present independently.

## Proposed Fix

Route all canvas frames through the **rootView's proxy** so they share a single `ViewRenderTarget` and compositor visual tree.

### Approach

1. **UIView inherits the parent's proxy for command submission.** Instead of using its own `proxy` when creating canvases, UIView passes the parent View's proxy. This means UIView canvases queue frames against the rootView's `ViewRenderTarget`.

2. **The UIView's own `ViewRenderTarget` still exists** for native item management (GTK widget hierarchy, event routing) — only the compositor command path changes.

3. **Single composition session wraps everything.** The `beginRecord/endRecord` on the rootView's proxy brackets all frames from both the rootView and its child UIViews. One `endRecord` → one `submit()` → one Packet containing all frames.

### Implementation

**Option A: Pass parent proxy to child canvas creation**

In `View::makeCanvas`, check if there's a parent view and use the parent's proxy instead:

```cpp
SharedHandle<Composition::Canvas> View::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
    // Route through parent's proxy so all layers share one render target in the compositor.
    if(parent_ptr != nullptr){
        return std::shared_ptr<Composition::Canvas>(new Composition::Canvas(parent_ptr->proxy, *targetLayer));
    }
    return std::shared_ptr<Composition::Canvas>(new Composition::Canvas(proxy, *targetLayer));
}
```

This is a one-line change but affects all View types. The `CompositionRenderTarget` in the queued command would be the parent's, so the compositor groups them under one visual tree.

**Option B: Propagate a "composition proxy" down the view hierarchy**

Add a method `View::compositionProxy()` that walks up to the root view and returns its proxy. Use this in `makeCanvas`. More explicit but same effect.

**Option C: UIView-specific override**

Override `makeCanvas` in UIView to use the Widget's rootView proxy. Most targeted but doesn't solve the general case.

### Files to modify

| File | Change |
|---|---|
| `wtk/src/UI/View.cpp` | `makeCanvas()` — route through parent proxy (Option A) |
| `wtk/src/UI/View.h` | Possibly expose `compositionProxy()` accessor (Option B) |

### Risks

- **Render target sizing**: The compositor sizes backing textures based on the `ViewRenderTarget`'s dimensions. If all layers route through the rootView's render target, child layer textures get sized to the rootView's dimensions. This is already the case for the UIView legacy path (`buildLayerRenderTarget` creates layers at `localBoundsFromView(view)` size), so no change.

- **Native item mismatch**: The `ViewRenderTarget` wraps a native item (GTK widget). The rootView's native item owns the Vulkan surface. Child views' native items are sub-widgets for event routing. If we route child canvas frames through the rootView's render target, the native item hierarchy stays correct — only the compositor's render target grouping changes.

- **Multi-window**: Each `AppWindow` has its own Widget and rootView. The proxy-sharing stays within one window's view hierarchy. No cross-window impact.

### Verification

After the change, `Widget::executePaint` should produce ONE Packet containing all frames (rootView background + UIView root + UIView elements). The compositor creates one `BackendCompRenderTarget` with one visual tree. `compositeAndPresentTarget` blits all layer textures to a single swapchain image. BasicAppTest should show a red 48x48 rectangle on a black background.
