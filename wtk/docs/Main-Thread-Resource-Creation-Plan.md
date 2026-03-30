# Main-Thread GPU Resource Creation Plan

## Problem

GPU resources (textures, render targets, command queues, CAMetalLayers, visuals) are currently created on the compositor thread during `ensureLayerSurfaceTarget` and `makeVisual`. This causes the blit pass in `compositeAndPresentTarget` to fail silently — the drawable presents but nothing is visible on screen.

### Root Cause

The minimal Metal test proved that the blit pipeline works correctly when all resource creation and rendering happens on the main thread. When the same blit pipeline runs on a background (compositor) thread with resources created on that thread, the drawable presents but Core Animation never displays the content.

This is not a Metal API limitation — Metal command buffers, render encoders, and `presentDrawable` can all run from any thread. The issue is that **CAMetalLayer drawables acquired from a layer whose resources were allocated on a background thread** do not integrate correctly with Core Animation's display pipeline. The drawable's backing IOSurface may not be properly registered with the window server when the CAMetalLayer and its associated textures were created off the main thread.

### What Works vs What Doesn't

| Configuration | Result |
|---|---|
| All on main thread, direct render to drawable | Works |
| All on main thread, offscreen texture → blit to drawable | Works |
| Compositor thread, direct render to drawable (no blit) | Works |
| Compositor thread, offscreen texture → blit to drawable, resources created on compositor thread | **Fails** |
| Main thread resource creation, compositor thread rendering | **Expected to work** (to be verified) |

### Affected Resources

Resources currently created on the compositor thread during `ensureLayerSurfaceTarget`:

1. **`CAMetalLayer`** — created in `MTLCALayerTree::makeVisual()` on compositor thread
2. **`GENativeRenderTarget`** (wraps `CAMetalLayer` + `MTLCommandQueue`) — created in `makeNativeRenderTarget()` called from `makeVisual()`
3. **`MTLCommandQueue`** — created per native render target in `makeNativeRenderTarget()`
4. **Offscreen `MTLTexture`** — created in `BackendRenderTargetContext::rebuildBackingTarget()` via `makeTexture()`
5. **`GETextureRenderTarget`** — created in `rebuildBackingTarget()` via `makeTextureRenderTarget()`
6. **`BackendVisualTree`** (wraps `MTLCALayerTree`) — created in `executeCurrentCommand()` when a new render target is first seen

### Platform Applicability

This is not macOS-specific. The same pattern would affect:
- **Windows/DirectX 12**: `IDXGISwapChain::Present` has thread affinity to the HWND's thread. DComp visual creation may have similar requirements.
- **Linux/Vulkan**: `VkSwapchainKHR` and `VkSurfaceKHR` creation may require the thread that owns the window surface.

Moving resource creation to the main thread is the correct cross-platform architecture.

---

## Proposed Solution: Main-Thread Resource Factory

Separate GPU resource **creation** from GPU resource **usage**. The compositor thread records commands and submits GPU work. The main thread creates and owns GPU resources.

### Architecture

```
Main Thread                          Compositor Thread
-----------                          -----------------

  Resource Factory                   Compositor Scheduler
  ├── createVisualTree()             ├── processCommand()
  ├── createNativeRenderTarget()     │   ├── executeCurrentCommand()
  ├── createTexture()                │   │   ├── render to texture (preEffectTarget)
  ├── createTextureRenderTarget()    │   │   └── commit()
  └── createCAMetalLayer()           │   └── presentAllPending()
                                     │       └── compositeAndPresentTarget()
  NSApp run loop pumps               │           ├── acquire drawable
  CATransaction commits              │           ├── blit textures → drawable
                                     │           └── present
```

### Implementation Steps

#### Step 1: Introduce `BackendResourceFactory`

Create a new class that centralizes all GPU resource creation and ensures it happens on the main thread:

```cpp
class BackendResourceFactory {
public:
    // Creates a BackendVisualTree + root visual for a ViewRenderTarget.
    // Must be called before the compositor thread uses the visual tree.
    struct VisualTreeBundle {
        SharedHandle<BackendVisualTree> visualTree;
        Core::SharedPtr<BackendVisualTree::Visual> rootVisual;
        BackendRenderTargetContext *rootContext;
    };
    VisualTreeBundle createVisualTreeForView(SharedHandle<ViewRenderTarget> & renderTarget,
                                              Core::Rect & rect);

    // Creates an offscreen texture + texture render target pair.
    struct TextureTargetBundle {
        SharedHandle<GETexture> texture;
        SharedHandle<GETextureRenderTarget> renderTarget;
    };
    TextureTargetBundle createTextureTarget(unsigned width, unsigned height,
                                             PixelFormat format);

    // Creates a child visual (for element layers within a View).
    Core::SharedPtr<BackendVisualTree::Visual> createChildVisual(
            BackendVisualTree & tree, Core::Rect & rect);
};
```

On macOS, every method dispatches to the main thread via `dispatch_sync`. On other platforms, the dispatch mechanism is platform-specific (e.g., `PostMessage` + `WaitForSingleObject` on Windows).

#### Step 2: Pre-create resources during View construction

Instead of lazily creating resources in `ensureLayerSurfaceTarget` on the compositor thread, create them eagerly during View construction (which happens on the main thread):

```
View constructor (main thread):
  ├── creates ownLayerTree
  ├── creates ViewRenderTarget (CocoaItem/NSView)
  └── NEW: calls BackendResourceFactory::createVisualTreeForView()
           → creates CAMetalLayer, GENativeRenderTarget, BackendVisualTree
           → attaches CAMetalLayer to NSView (setRootLayer)
           → creates offscreen textures for the root layer
           → stores in View's renderTarget metadata
```

The compositor thread's `ensureLayerSurfaceTarget` then just looks up the pre-created resources instead of creating them.

#### Step 3: Deferred child visual creation

For element layers (created dynamically by UIView/UIRenderer), the resource factory provides a request queue:

```
Compositor thread:                     Main thread:
  ensureLayerSurfaceTarget()
    → child layer needs visual
    → posts request to factory queue   → factory processes request
    → waits for completion             ← returns visual + context
    → uses the visual
```

This is a synchronous handoff — the compositor thread blocks briefly while the main thread creates the resource. This is safe because:
- The main thread is running the NSApp event loop which processes dispatch blocks
- The creation is fast (texture allocation + render target setup)
- It only happens when a NEW layer is first rendered, not on every frame

#### Step 4: Update `executeCurrentCommand` and `compositeAndPresentTarget`

These functions no longer create resources. They only:
- Look up pre-created resources from the `RenderTargetStore`
- Record render commands into pre-existing command buffers
- Submit and present using pre-existing native render targets

### Migration Path

| Step | Change | Risk |
|------|--------|------|
| 1 | Add `BackendResourceFactory` class with `dispatch_sync` to main thread | Low — additive |
| 2 | Move `BackendVisualTree::Create` + `makeVisual` + `setRootVisual` into factory, called from `View` constructor | Medium — changes initialization order |
| 3 | Move `rebuildBackingTarget` (offscreen texture creation) into factory | Medium — textures currently resize lazily |
| 4 | Update `ensureLayerSurfaceTarget` to use pre-created resources | Medium — removes lazy creation path |
| 5 | Move child visual creation (`addVisual`) into factory with request queue | Medium — adds sync point |
| 6 | Remove all resource creation from compositor thread code paths | High — final cutover |

### Files Touched

| File | Change |
|------|--------|
| NEW: `src/Composition/backend/ResourceFactory.h` | BackendResourceFactory class |
| NEW: `src/Composition/backend/ResourceFactory.cpp` | Implementation with platform dispatch |
| `include/omegaWTK/UI/View.h` | Store pre-created visual tree metadata |
| `src/UI/View.cpp` | Call factory during construction |
| `src/Composition/backend/Execution.cpp` | `ensureLayerSurfaceTarget` uses pre-created resources |
| `src/Composition/backend/RenderTarget.cpp` | `rebuildBackingTarget` delegates to factory |
| `src/Composition/backend/RenderTarget.h` | `BackendRenderTargetContext` accepts pre-created resources |
| `src/Composition/backend/mtl/CALayerTree.mm` | `makeVisual` / `setRootVisual` called from factory |

### Why Not Just Dispatch Individual Calls?

We considered dispatching individual Metal calls (like `compositeAndPresentTarget` or `commitAndPresent`) to the main thread. This doesn't work because:

1. **The resources themselves are tainted.** A `CAMetalLayer` created on a background thread produces drawables that don't integrate with Core Animation, regardless of which thread presents them.
2. **The offscreen textures may have the same issue.** Textures created on the background thread may not be visible to the main thread's render pipeline when used as shader inputs during the blit pass.
3. **Piecemeal dispatch creates deadlock risk.** Dispatching individual operations to the main thread from the compositor thread re-introduces the `dispatch_sync` deadlock pattern we already fixed.

The correct architecture: **create once on main, use many times on compositor.**
