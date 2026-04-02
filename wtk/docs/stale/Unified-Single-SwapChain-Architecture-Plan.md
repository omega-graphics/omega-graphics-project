# Unified View-Level SwapChain Architecture Plan

## Motivation

The current backend creates **one swap chain / present target per visual** in the compositor visual tree. On Windows this means one `IDXGISwapChain` per `DCVisualTree::Visual`, on macOS one `CAMetalLayer` per `MTLCALayerTree::Visual`, and on Linux one `VkSwapchainKHR` per `VkVisualTree::Visual`. Since a `View` may contain multiple visuals (one per `Layer`), this creates far more swap chains than necessary.

This design causes a critical rendering failure: the `compositeAndPresentTarget` pass blits all layer textures into the **root visual's** swap chain, but child visuals still own their own swap chains which are never presented and sit on top of the root, occluding it with black/undefined content.

Beyond the immediate bug, per-visual swap chains create:
1. **Cross-visual present skew** — visuals present at different times, causing tearing between layers during resize and animation.
2. **Wasted GPU resources** — each swap chain allocates 2+ back buffers that are never used for child visuals.
3. **Unnecessary complexity** — the compositor already software-composites all layers into one final pass; having per-visual native present targets duplicates the compositing.

Views are single-function UI elements (text, shapes, images, SVG, video). Widgets are composed of many views. Even for animated SVG via `SVGView`, a single present target per window is sufficient — the views become textures blitted onto the final target.

## Target Architecture

### Core Rule
1. **One present target per `View`** — one `IDXGISwapChain` (Windows), one `CAMetalLayer` (macOS/iOS), one `VkSwapchainKHR` (Linux/Android). A `View` is the unit of presentation; a Widget is composed of one or more Views.
2. **Layers do not own swap chains.** Each Layer within a View is a logical surface: a texture + metadata (rect, z-order, opacity, transform, effects). Layers are composited together and presented through their owning View's single swap chain.
3. The final composition pass draws all of a View's layer surfaces in z-order onto the View's present target.

### Visual Node Model

```
Before (current):
  DCVisualTree::Visual                    (one per Layer)
    ├── IDCompositionVisual2
    ├── IDXGISwapChain3                   ← per-layer swap chain (PROBLEM)
    └── BackendRenderTargetContext
          ├── preEffectTarget             ← texture render target
          └── renderTarget                ← native render target (swap chain)

After (proposed):
  BackendSurface                          (one per Layer — texture only)
    ├── surfaceId
    ├── texture                           ← offscreen texture (GETexture)
    ├── textureRenderTarget               ← texture render target
    ├── rect, zOrder, opacity, transform, effects
    └── dirty flag

  ViewPresentTarget                       (one per View)
    ├── nativeRenderTarget                ← single swap chain for this View
    ├── commandQueue
    └── finalCompositorPipeline
```

### Platform Mapping

| Concept | Windows (D3D12) | macOS (Metal) | Linux (Vulkan) |
|---|---|---|---|
| View present target | `IDXGISwapChain3` via `CreateSwapChainForComposition` | `CAMetalLayer` | `VkSwapchainKHR` |
| Layer surface | `GETexture` + `GETextureRenderTarget` | `GETexture` + `GETextureRenderTarget` | `GETexture` + `GETextureRenderTarget` |
| DComp integration | `IDCompositionTarget` + one `IDCompositionVisual2` per View (swap chain as content) | N/A (`CAMetalLayer` on View's `NSView`) | N/A (`VkSurfaceKHR` per View) |
| Final compose | Render pass on View's swap chain back buffer | Render pass on View's `CAMetalLayer` drawable | Render pass on View's swap chain image |

## Render Pipeline (Per Compositor Packet, Per View)

1. **Apply layer-tree deltas** — update surface metadata (rect, z-order, opacity, transform, effects, dirty flags) for layers within this View.
2. **Render dirty layer surfaces** — for each dirty layer, execute render commands into its offscreen texture via `TextureRenderTarget`.
3. **Run per-surface effects** — blur, shadow, etc. into effect textures.
4. **Encode final View pass** on the View's present target:
   - Clear View background color.
   - For each visible layer surface in z-order:
     - Wait on surface's fence (cross-queue sync).
     - Bind surface texture as shader input.
     - Apply opacity, transform, clip as shader/viewport parameters.
     - Draw full-surface quad via copy/blit shader.
   - End render pass.
5. **Present** — single `Present()` / `presentDrawable` / `vkQueuePresentKHR` for this View.
6. **Emit telemetry** for the packet.

## Changes Required

### Backend-Agnostic (Composition Layer)

#### `BackendVisualTree` Refactor
- `makeVisual` no longer creates a swap chain per visual.
- Root visual creation (`setRootVisual`) creates the View's single swap chain and associates it with the `ViewPresentTarget`.
- `addVisual` → `addSurface` (registers a layer surface in z-order; no per-layer native visual/swap chain creation).

#### `BackendRenderTargetContext` Simplification
- Remove `renderTarget` (native render target) from per-layer contexts.
- Each layer context only owns `preEffectTarget` (texture render target) + `fence`.
- The `ViewPresentTarget` owns the single native render target for the View.

#### `compositeAndPresentTarget` Consolidation
- Already composites all layer textures into one target — keep this logic.
- Remove the assumption that `allContexts[0]->getNativeRenderTarget()` is the present target.
- Instead, use the `ViewPresentTarget` directly.

#### `RenderTargetStore` Update
- Track `ViewPresentTarget` per View.
- `presentAllPending` presents each View's target once after compositing its layer surfaces.

### Windows (D3D12 / DComp)

#### `DCVisualTree` Changes
- Create **one** `IDXGISwapChain3` via `CreateSwapChainForComposition` per View.
- Create **one** `IDCompositionVisual2` per View, with the View's swap chain as content.
- Remove per-layer `IDCompositionVisual2` creation and `AddVisual` calls.
- Layer surfaces are textures only — no DComp visual representation.
- After present, `comp_device_desktop->Commit()` once.

#### Swap Chain Creation
- Enable `AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED` for correct DComp transparency between Views.
- One swap chain per View, sized to the View's backing dimensions.

### macOS (Metal)

See [Single-CAMetalLayer-Backend-Plan.md](Single-CAMetalLayer-Backend-Plan.md) — this plan generalizes that Metal-specific approach to all backends, with the refinement that the unit of swap chain ownership is the View, not the window.

### Linux (Vulkan)

- One `VkSwapchainKHR` per View surface.
- Layer surfaces are `VkImage` render targets.
- Final pass renders all layer surfaces into the View's swap chain image.

## Migration Slices

### Slice 1: `ViewPresentTarget` Extraction
- Introduce `ViewPresentTarget` owning the single native render target per View.
- `DCVisualTree` / `MTLCALayerTree` / `VkVisualTree` create one present target per View at construction.
- `compositeAndPresentTarget` reads from `ViewPresentTarget` instead of `allContexts[0]`.
- **Immediate fix**: stop creating per-layer swap chains; layers become texture-only surfaces.

### Slice 2: Surface-Only Layers
- `makeVisual` for child layers → `makeSurface` — allocates texture + texture render target, no swap chain.
- Remove `IDCompositionVisual2` per child layer (Windows), sublayer addition (macOS), extra `VkSwapchainKHR` (Linux).
- Existing `BackendRenderTargetContext` retains `preEffectTarget` for offscreen rendering.
- Root visual retains swap chain ownership as the View's present target.

### Slice 3: DComp / Native View Integration
- Windows: one `IDCompositionVisual2` per View + one swap chain per View. `CreateTargetForHwnd` once per window.
- macOS: one `CAMetalLayer` per View on `NSView`.
- Linux: one `VkSurfaceKHR` + `VkSwapchainKHR` per View.

### Slice 4: Surface Lifecycle + Resize
- Surface pool with LRU recycle to avoid resize thrash.
- Resize layer surfaces on stable dimensions only.
- Resize View swap chain on View resize (already handled by `resizeSwapChain`).

### Slice 5: Validation
- `BasicAppTest` — red rect visible on black background.
- `EllipsePathCompositorTest` — ellipse rendering.
- `TextCompositorTest` — text rendering.
- Live resize stress — no skew, no flicker, no mixed-epoch frames.

## Risks and Mitigations

1. **Risk**: Extra GPU cost from final composition pass.
   **Mitigation**: Dirty-region tracking — only re-composite when a surface changes. Skip unchanged surfaces.

2. **Risk**: Memory growth from per-surface textures.
   **Mitigation**: Pooled textures with LRU eviction. Share texture format/size classes.

3. **Risk**: Regression in DComp effects (shadow, transform).
   **Mitigation**: Map DComp effects to shader parameters in the final composition pass. Phase out `IDCompositionEffect` usage.

4. **Risk**: Increased latency from two-pass rendering (surface + final).
   **Mitigation**: Already the current architecture — surfaces are rendered to textures then blitted. No additional pass.

## Acceptance Criteria

1. Exactly one swap chain / present target exists per `View` on every backend.
2. One present call per compositor packet per window.
3. `BasicAppTest` displays a red rectangle on a black background (all backends).
4. No cross-visual skew artifacts during live resize.
5. Existing test apps render correctly before and after the change.
