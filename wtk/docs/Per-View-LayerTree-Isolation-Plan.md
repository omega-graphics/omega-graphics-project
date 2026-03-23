# Per-View LayerTree Isolation Plan

## Problem

Every `Widget` creates a single `LayerTree`. All Views created by that widget (`rootView`, child `UIView`s, `SVGView`s, `VideoView`s, etc.) graft their `LayerTree::Limb`s onto the **same tree**, with the rootView's limb as the tree root.

**This is the same root cause** that the `Unified-Single-SwapChain-Architecture-Plan` and `Single-CAMetalLayer-Backend-Plan` attempted to address from the backend side. This plan addresses it from the frontend layer-ownership side. The two are complementary; this plan is a prerequisite for either backend fix.

### Current backend reality

The Unified-Single-SwapChain architecture has **not** been implemented. As of now:

- **Each `Layer` in the `LayerTree` gets its own `CAMetalLayer`** (`CALayerTree.mm:99` — `makeVisual` creates a fresh `[CAMetalLayer layer]` per visual).
- Each `CAMetalLayer` owns its own swapchain (drawable pool).
- **Each `View` creates its own `NSView`** (`CocoaItem`) via `make_native_item` in the View constructor.
- Child Views' NSViews are added as subviews of the parent View's NSView via `addChildNativeItem` → `[_ptr addSubview:cocoaview->_ptr]` (`CocoaItem.mm:310`).
- `MTLCALayerTree::setRootVisual` attaches the root `CAMetalLayer` to a View's NSView via `setRootLayer` → `[hostLayer addSublayer:layer]` (`CocoaItem.h:151-154`).
- Child visuals' `CAMetalLayer`s are added as sublayers of the root visual's `CAMetalLayer`: `[r->metalLayer addSublayer:v->metalLayer]` (`CALayerTree.mm:164`).
- The `compositeAndPresentTarget` pass (`RenderTarget.cpp:1218`) blits all layer textures into the root visual's `CAMetalLayer` via `commitAndPresent`, but the child `CAMetalLayer`s are **still sublayers** sitting on top with their own swapchain content.
- The same pattern applies on Windows (`IDXGISwapChain` per visual) and Linux (`VkSwapchainKHR` per visual).

### The three-layer mess (BasicAppTest full trace)

**Step 1 — Frontend wiring:**

```
Widget constructor (Widget.cpp:167-169)
  |-- Creates ONE LayerTree
  |-- Creates rootView = new View(rect, layerTree, parent=nullptr)
  |     |-- View ctor (View.cpp:256-263):
  |     |     renderTarget = make_shared<ViewRenderTarget>(
  |     |         make_native_item(rect))  → NEW CocoaItem → NEW OmegaWTKCocoaView (NSView)
  |     |-- Creates layerTreeLimb on widget's LayerTree
  |     |-- parent_ptr is null → layerTree->setRootLimb(limb)
  |     '-- rootView's limb IS the tree root

MyWidget::onMount()
  |-- makeUIView(bounds, rootView, "basic_view")
        |-- new UIView(rect, layerTree.get(), parent=rootView, tag)
        |-- View ctor runs for UIView:
        |     |-- make_native_item(rect) → NEW CocoaItem → NEW OmegaWTKCocoaView (NSView)
        |     |-- Creates NEW limb on SAME widget LayerTree
        |     |-- parent_ptr = rootView (non-null)
        |     |     → parent->addSubView(this) → addChildNativeItem
        |     |       → [rootView._ptr addSubview:uiView._ptr]
        |     |     → layerTree->addChildLimb(limb, rootView->limb)
        |     '-- UIView's limb is a CHILD of rootView's limb, UIView's NSView is a SUBVIEW of rootView's NSView
        '-- UIRenderer creates element layers on UIView's limb
```

**Step 2 — Canvas routing bypass:**

`View::makeCanvas()` (`View.cpp:366-375`) walks `parent_ptr` to the **root view**:
```cpp
auto *target = this;
while(target->parent_ptr != nullptr){
    target = target->parent_ptr;
}
return shared_ptr<Canvas>(new Canvas(target->proxy, *targetLayer));
```

ALL canvases (including UIView's element layer canvases) target the **rootView's** `CompositorClientProxy` / `ViewRenderTarget`. The `RenderTargetStore` therefore creates only ONE `BackendCompRenderTarget` with ONE `BackendVisualTree` — for the rootView's ViewRenderTarget.

All visuals (from rootView's layers AND UIView's layers) go into that one tree. `setRootVisual` is called on rootView's CocoaItem — the root `CAMetalLayer` is attached to rootView's NSView. Child CAMetalLayers are sublayers of that root CAMetalLayer.

**UIView's NSView gets nothing.** No `BackendVisualTree` is created for it. No `setRootVisual`. No `CAMetalLayer`. It sits in the NSView hierarchy as an empty subview.

**Step 3 — The resulting native hierarchy:**

```
NSWindow.contentView
  '-- OmegaWTKCocoaView [rootView's NSView]
       |
       |-- .layer = CALayer (host layer, transparent bg)
       |     '-- sublayer: CAMetalLayer #1 (rootView's root Layer visual — PRESENT TARGET)
       |           |-- sublayer: CAMetalLayer #2 (UIView's root Layer visual — own swapchain, ORPHAN)
       |           '-- sublayer: CAMetalLayer #3 ("center_rect" Layer visual — own swapchain, ORPHAN)
       |
       '-- subview: OmegaWTKCocoaView [UIView's NSView]    ← EMPTY, ON TOP
            |-- .layer = CALayer (host layer, transparent bg)
            |-- (no CAMetalLayers — setRootVisual never called for UIView's ViewRenderTarget)
            '-- has NSTrackingArea intercepting mouse events for this rect
```

### Why this is wrong — four independent problems

**Problem 1: Orphan CAMetalLayers.** CAMetalLayers #2 and #3 (UIView's layers) are sublayers of CAMetalLayer #1 (rootView's root visual). They each have their own swapchain. `compositeAndPresentTarget` blits all textures into #1's drawable and presents it. But #2 and #3 are still in the CA sublayer tree — Core Animation composites them **on top** of #1's content. If they were never presented independently (and they aren't), they show undefined/black content, occluding the correctly composited root.

**Problem 2: Ghost NSView.** UIView's `OmegaWTKCocoaView` is a subview of rootView's `OmegaWTKCocoaView`. It has a transparent background, so it doesn't visually occlude. But:
- It has an `NSTrackingArea` that intercepts mouse events for its rect, stealing them from the content-bearing CAMetalLayers underneath on rootView's NSView.
- It participates in AppKit's responder chain and layout system for no reason.
- It's an empty NSView with no rendering purpose — all Metal content was routed to rootView's visual tree.

**Problem 3: Resize desync across three layers.** On window resize:
- rootView's NSView resizes (via `CocoaItem::resize`), updating its host CALayer frame.
- rootView's root CAMetalLayer #1 resizes (via `BackendVisualTree::Visual::resize`), updating drawableSize.
- UIView's NSView resizes (if `View::resize` propagates), updating its empty host CALayer frame.
- But CAMetalLayers #2 and #3 resize only when `Layer::resize` is called, which happens when `onPaint` runs — deferred. This creates frames where the orphan sublayers have stale geometry inside the already-resized root.
- Meanwhile, all three layer resizes fire `notifyObserversOfResize` on the SAME shared LayerTree, creating notification storms.

**Problem 4: One BackendVisualTree for multiple Views.** The `RenderTargetStore` sees one ViewRenderTarget (rootView's). All visuals from all views end up in one `BackendCompRenderTarget`. The compositor cannot:
- Independently schedule or skip renders for different views.
- Apply per-view stale-frame coalescing.
- Track which CAMetalLayers belong to which view for cleanup.
- Manage per-view present timing.

---

## Proposed Solution: Per-View LayerTree

Each `View` owns its own `LayerTree`. The widget no longer owns a single shared tree. Each View's layers map to an independent `BackendVisualTree` on its own NSView, with its own set of `CAMetalLayer`s.

### Design Principles

1. **A View's LayerTree contains only that View's layers.** No cross-view layer grafting.
2. **Each View's CAMetalLayers live on that View's NSView.** No orphan sublayers on the wrong NSView's visual tree.
3. **`makeCanvas()` targets the View's own proxy.** No parent-walk bypass. Each View has its own `BackendCompRenderTarget` in the `RenderTargetStore`.
4. **Z-order between Views is managed by NSView subview ordering** (already the case via `addSubView` → `addChildNativeItem` → `[_ptr addSubview:]`). This is correct and natural.
5. **The Compositor observes each View's tree independently**, enabling per-view stale-frame coalescing, per-view resize governors, and per-view backend mirror state.
6. **The widget remains the ownership root** of its View hierarchy, but delegates layer isolation to the Views themselves.

### How it fixes the current architecture

After this change, the BasicAppTest native hierarchy becomes:

```
NSWindow.contentView
  '-- OmegaWTKCocoaView [rootView's NSView]               Z-order 0
       |-- .layer = CAMetalLayer #1 (rootView's root Layer — IS the NSView's layer)
       |     (compositeAndPresentTarget renders rootView content here)
       |
       '-- subview: OmegaWTKCocoaView [UIView's NSView]   Z-order 1
            |-- .layer = CAMetalLayer #2 (UIView's root Layer — IS the NSView's layer)
            |     '-- sublayer: CAMetalLayer #3 ("center_rect" element Layer)
            |     (compositeAndPresentTarget renders UIView content here)
            '-- NSTrackingArea correctly covers UIView's content
```

Each View's root `CAMetalLayer` **is** the NSView's `.layer` — not a sublayer of a host `CALayer`. This eliminates the intermediate host layer entirely. `setRootLayer` assigns `_ptr.layer = metalLayer` instead of creating a host `CALayer` and adding the `CAMetalLayer` as a sublayer.

- **No orphan sublayers.** Each View's CAMetalLayers live on that View's NSView. CAMetalLayers #2 and #3 are on UIView's NSView, not rootView's.
- **No ghost NSView.** UIView's NSView now has real Metal content. Its tracking area correctly covers its own visuals.
- **No intermediate host CALayer.** The root CAMetalLayer is the NSView's backing layer directly. One fewer layer in the CA hierarchy per View. `CocoaItem::resize` already handles the case where `_ptr.layer` is a `CAMetalLayer` (updates `drawableSize`), so this works with existing resize code.
- **Resize is per-view.** Resizing rootView's NSView/CAMetalLayer doesn't touch UIView's tree. Each View's `compositeAndPresentTarget` runs independently.
- **Z-order is NSView subview ordering.** UIView's NSView is a subview of rootView's NSView — AppKit/Core Animation composites it on top. This is already how the NSView hierarchy is set up; the Metal content just needs to be on the right NSView.

### Relationship to Unified-Single-SwapChain plan

| Concern | This plan (Per-View LayerTree) | Unified-Single-SwapChain |
|---------|-------------------------------|--------------------------|
| **What it fixes** | Layer ownership boundaries, orphan CAMetalLayers, ghost NSViews | Per-layer swapchain waste within a View |
| **Scope** | Frontend: LayerTree, View, Widget | Backend: BackendVisualTree, RenderTargetStore |
| **Per-layer CAMetalLayer** | Still exists within each View (layers within a View still get their own) | Eliminated (layers become textures, one swapchain per View) |
| **Cross-view entanglement** | Eliminated (each View has its own tree on its own NSView) | Not addressed (assumes trees are already per-view) |

This plan is a **prerequisite** for the Unified-Single-SwapChain plan. The unified plan assumes "one swap chain per View" — but that only makes sense when each View's layers are isolated in their own tree on their own NSView. Without per-view trees, "per-View swap chain" has no clean mapping.

The implementation order should be:
1. **This plan** — isolate LayerTrees per View, put each View's Metal content on its own NSView
2. **Unified-Single-SwapChain** — collapse per-layer swapchains within each View into one-per-View

After both:
```
rootView's NSView → one CAMetalLayer, element layers are textures composited into it
UIView's NSView   → one CAMetalLayer, element layers are textures composited into it
```

### Structural Changes

#### Phase 1: View owns its own LayerTree

**View.h**

```
Current:
  View stores Composition::LayerTree *widgetLayerTree;  // borrowed from Widget
  View creates a Limb on the widget's tree
  View ctor signature: View(rect, LayerTree*, parent)

Proposed:
  View stores SharedHandle<Composition::LayerTree> ownLayerTree;  // owned
  View creates the root Limb on its own tree
  View ctor signature: View(rect, parent)  // no LayerTree param
  View exposes LayerTree* for compositor observation
```

- `View` constructor changes: always create a new `LayerTree`, always set its own limb as the root limb. The `layerTree` pointer parameter is removed.
- `makeLayer()` operates on `ownLayerTree`.
- The parent-child View relationship (`addSubView`/`removeSubView`) is purely for event routing and NSView hierarchy — it no longer implies LayerTree limb parenting.

**Widget.h / Widget.cpp**

```
Current:
  Widget owns SharedHandle<Composition::LayerTree> layerTree;
  All make*View() methods pass layerTree.get()
  Widget::setTreeHostRecurse registers the one tree with compositor

Proposed:
  Widget removes layerTree field
  make*View() methods no longer pass a LayerTree — each View creates its own
  Widget::setTreeHostRecurse delegates to View::registerTreeRecurse
```

- `Widget::getRootPaintCanvas()` gets the root layer from `rootView->ownLayerTree->getTreeRoot()->getRootLayer()`.
- All `make*View` calls simplified: `new UIView(rect, parent, tag)` etc.

#### Phase 2: Remove makeCanvas parent-walk — each View targets its own proxy

**This is the critical change.** Currently `View::makeCanvas()` walks `parent_ptr` to the root view, funneling all Metal content to one ViewRenderTarget. After this change:

```cpp
SharedHandle<Composition::Canvas> View::makeCanvas(SharedHandle<Composition::Layer> &targetLayer){
    // Each View owns its own render target and visual tree.
    return shared_ptr<Canvas>(new Canvas(proxy, *targetLayer));
}
```

Each View's `proxy` points to its own `ViewRenderTarget`. The `RenderTargetStore` will create a separate `BackendCompRenderTarget` + `BackendVisualTree` for each View.

**Consequence on macOS:** Each View's `BackendVisualTree` calls `setRootVisual`, which calls `setRootLayer` on that View's `CocoaItem` — making the root `CAMetalLayer` the NSView's backing layer directly (see Phase 2b below). Child layer CAMetalLayers are sublayers of that root CAMetalLayer, all on the correct NSView.

#### Phase 2b: Flatten NSView layer hierarchy — `setRootLayer` assigns `_ptr.layer` directly

Currently `setRootLayer` (`CocoaItem.h:67-162`):
1. Ensures `_ptr.layer` is a plain `CALayer` (the "host layer")
2. Configures the host layer
3. Adds the passed-in `CAMetalLayer` as a **sublayer** of the host layer: `[hostLayer addSublayer:layer]`

This creates an unnecessary intermediate layer:
```
NSView._ptr.layer = CALayer (host)
  '-- sublayer: CAMetalLayer (root visual)
```

**Change:** Replace the NSView's layer with the `CAMetalLayer` directly:

```objc
void setRootLayer(CALayer *layer){
    if(_ptr != nil && layer != nil){
        NSDictionary *noActions = @{ /* ... same as current ... */ };
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        _ptr.wantsLayer = YES;

        // Remove old sublayers from the previous host layer before replacing.
        // Subviews' layers are managed by AppKit — only compositor-owned
        // sublayers need migration.

        NSRect hostBounds = _ptr.bounds;
        // ... same bounds sanitization as current ...

        CGFloat scale = /* ... same as current ... */;

        // Assign the CAMetalLayer as the NSView's layer directly.
        // No intermediate host CALayer.
        layer.actions = noActions;
        layer.autoresizingMask = kCALayerNotSizable;
        layer.masksToBounds = NO;
        layer.contentsScale = scale;
        layer.hidden = NO;
        layer.anchorPoint = CGPointMake(0.f, 0.f);
        layer.position = CGPointMake(0.f, 0.f);
        layer.bounds = hostBounds;
        layer.frame = hostBounds;

        if([layer isKindOfClass:[CAMetalLayer class]]){
            CAMetalLayer *metalLayer = (CAMetalLayer *)layer;
            metalLayer.opaque = NO;
            metalLayer.framebufferOnly = NO;
            metalLayer.presentsWithTransaction = NO;
            CGColorRef clearColor = CGColorCreateGenericRGB(0.f,0.f,0.f,0.f);
            metalLayer.backgroundColor = clearColor;
            CGColorRelease(clearColor);
            metalLayer.drawableSize = CGSizeMake(
                MIN(MAX(hostBounds.size.width * scale, 1.f), maxDrawableDimension),
                MIN(MAX(hostBounds.size.height * scale, 1.f), maxDrawableDimension));
        }

        _ptr.layer = layer;  // <-- THE KEY CHANGE: replace, don't sublayer
        _ptr.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;

        [layer setNeedsDisplay];
        [_ptr setNeedsDisplay:YES];
        [CATransaction commit];
    }
}
```

Result per View:
```
NSView._ptr.layer = CAMetalLayer (root visual — IS the backing layer)
  '-- sublayer: CAMetalLayer (child element layer, if any)
```

**Why this works:**
- `CocoaItem::resize` already handles `_ptr.layer` being a `CAMetalLayer` — the `[layer isKindOfClass:[CAMetalLayer class]]` check at `CocoaItem.mm:284` updates `drawableSize` correctly.
- AppKit manages subview layers independently of the parent's backing layer. Child NSViews (child Views) get their own layer trees via `addSubview:`, unaffected by the parent's layer type.
- `OmegaWTKCocoaView initWithFrame:` currently creates a placeholder `[CALayer layer]` as `self.layer`. After `setRootLayer`, this is replaced by the `CAMetalLayer`. The init placeholder only needs to be a valid layer until the compositor attaches the real one.
- The `BackendVisualTree::Visual::resize` method (`CALayerTree.h:57-112`) operates on its own `metalLayer` pointer directly — it does not go through `_ptr.layer`. So the backend visual resize and the `CocoaItem::resize` (which updates `_ptr.layer`) are independent and both work correctly.

#### Phase 3: Per-View compositor observation

Each View's `LayerTree` is registered with the Compositor independently:

```cpp
void View::setFrontendRecurse(Compositor *frontend){
    proxy.setFrontendPtr(frontend);
    if(frontend != nullptr && ownLayerTree != nullptr){
        frontend->observeLayerTree(ownLayerTree.get(), proxy.getSyncLaneId());
    }
    for(auto *subView : subviews){
        subView->setFrontendRecurse(frontend);
    }
}
```

`Widget::setTreeHostRecurse` no longer calls `comp->observeLayerTree(layerTree.get())` — this is handled by each View.

Each View's tree gets its own entry in:
- `targetLayerTrees`
- `layerTreeSyncState`
- `layerTreeLaneBinding`
- `backendLayerMirror`

#### Phase 4: Resize isolation

With per-view trees, resize notifications are naturally scoped:

- Resizing a layer fires `notifyObserversOfResize` only on **that View's tree**.
- Each View's `compositeAndPresentTarget` runs independently — only compositing that View's layers.
- `ViewResizeCoordinator` continues to manage parent-child View resize relationships via `addSubView`/`removeSubView`, but no longer entangles LayerTree notifications.
- Each View's CAMetalLayer drawableSize is updated by its own BackendVisualTree, not tangled with other Views' resizes.

### Migration Path

| Step | Change | Risk | Rollback |
|------|--------|------|----------|
| 1 | Add `ownLayerTree` to `View`, create it in the constructor, wire `makeLayer` to it. Keep old `widgetLayerTree` pointer alive but unused. | Low — additive | Remove `ownLayerTree` |
| 2 | **Remove `parent_ptr` walk in `View::makeCanvas()`.** Each View's canvases target its own `proxy`. This causes the `RenderTargetStore` to create per-View `BackendCompRenderTarget`s with per-View `BackendVisualTree`s. Each View's root `CAMetalLayer` gets attached to its own NSView. | **High — this is the key behavioral change.** Each View now presents independently. | Restore parent walk |
| 3 | **Flatten `setRootLayer`**: change `CocoaItem::setRootLayer` to assign `_ptr.layer = layer` directly instead of creating a host `CALayer` and adding the `CAMetalLayer` as a sublayer. | Low — `CocoaItem::resize` already handles `_ptr.layer` being a `CAMetalLayer`. | Revert to host+sublayer approach |
| 4 | Switch `View` constructor to set root limb on `ownLayerTree` instead of grafting onto the widget's tree. Stop calling `layerTree->addChildLimb`. | Medium — breaks cross-view limb parenting | Revert constructor |
| 5 | Move compositor observation from `Widget::setTreeHostRecurse` to `View::setFrontendRecurse`. Each View registers its own tree. | Medium — changes compositor state cardinality | Revert to widget-level registration |
| 6 | Remove `widgetLayerTree` from `View`. Remove `layerTree` from `Widget`. Remove `layerTree` param from all View subclass constructors. Clean up all `make*View` call sites in Widget.cpp. | High — removes backward compat | Restore both fields |
| 7 | Verify all test apps render correctly. | Verification | N/A |

### Files Touched

| File | Nature of change |
|------|-----------------|
| `include/omegaWTK/Composition/Layer.h` | No structural change to LayerTree itself |
| `include/omegaWTK/UI/View.h` | Add `ownLayerTree`; remove `widgetLayerTree` field; change ctor signatures (drop `LayerTree*` param) |
| `include/omegaWTK/UI/Widget.h` | Remove `SharedHandle<LayerTree> layerTree` field |
| `src/UI/View.cpp` | New ctor: create own tree, own root limb. **Remove parent walk in `makeCanvas()`.** `setFrontendRecurse` registers own tree with compositor. |
| `src/UI/Widget.cpp` | Remove tree creation; update `setTreeHostRecurse` (delegate to views), `getRootPaintCanvas`, all `make*View` (drop layerTree param) |
| `src/UI/UIView.cpp` | Update constructor call (drop layerTree param) |
| `src/Composition/Compositor.h` | No structural change (already supports multiple trees via `targetLayerTrees`) |
| `src/Composition/Compositor.cpp` | No structural change (observation is already per-tree) |
| `src/Composition/Layer.cpp` | No change (LayerTree API is unchanged) |
| `src/Composition/backend/RenderTarget.cpp` | `compositeAndPresentTarget` unchanged — already operates per `BackendCompRenderTarget`. With per-View trees, it gets called once per View naturally via `presentAllPending`. |
| `src/Native/private_include/NativePrivate/macos/CocoaItem.h` | **Rewrite `setRootLayer`**: assign `_ptr.layer = layer` directly instead of creating a host `CALayer` + sublayer. Remove host layer setup code. |
| `src/Composition/backend/mtl/CALayerTree.mm` | `setRootVisual` already calls `view->setRootLayer()`. No change needed — it will now be called for each View's BackendVisualTree, attaching each root CAMetalLayer as the correct NSView's backing layer. |

### Impact on Other Views

- **SVGView, VideoView, ScrollView**: All constructed with `layerTree` parameter today. After this change, their View base class creates its own tree. Constructor signatures change (drop `layerTree` param). Each gets its own NSView + own CAMetalLayer tree. Behavior is otherwise transparent.
- **UIRenderer**: `buildLayerRenderTarget` calls `view->makeLayer()` / `view->makeCanvas()`, which now operate on the UIView's own tree and own proxy. No UIRenderer logic changes needed.
- **Containers / StackWidget**: These are Widgets, not Views. They create child widgets which have their own view hierarchies. No change needed.

### Verification Criteria

1. **BasicAppTest**: rootView's CAMetalLayer **is** rootView's NSView `.layer`. UIView's root CAMetalLayer **is** UIView's NSView `.layer`. No intermediate host `CALayer`s. The red rect renders on top of the black background. No orphan sublayers. No ghost NSView.
2. **EllipsePathCompositorTest**: Drop shadows render on their own layers within each View's tree on that View's NSView, without bleeding into sibling view trees.
3. **ContainerClampAnimationTest**: Animated child widget resizes do not trigger notification storms across unrelated views' trees.
4. **LayoutResizeStressTest**: 1000 resize iterations produce deterministic results with per-view trees (no cross-tree state leakage).
5. **SVGViewRenderTest / VideoViewPlaybackTest**: Media views have their own NSView + CAMetalLayer tree, compositing at correct Z-order.
6. **Mouse events**: UIView's NSTrackingArea correctly covers UIView's NSView which now has real Metal content. No stolen events from empty NSViews.
7. **Resize skew**: Each View's CAMetalLayers resize independently via their own `compositeAndPresentTarget`. No frame shows a mix of old-sized and new-sized layers from different views.
