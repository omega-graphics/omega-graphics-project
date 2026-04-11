# Unified Rendering Architecture Plan

Supersedes:
- `UIView-Single-Canvas-Plan.md`
- `Unified-Single-SwapChain-Architecture-Plan.md`
- `stale/Single-CAMetalLayer-Backend-Plan.md`

---

## Two Problems, One Design

The rendering backend has two independent structural problems that share a single solution.

**Problem 1: Per-Layer swap chains.** Every `Layer` in a `View` gets its own native present surface (`CAMetalLayer` / `IDXGISwapChain` / `VkSwapchainKHR`). Only the root's surface actually presents — child surfaces allocate drawables, fences, and backing textures that are never shown directly. The compositor already blits all layer textures onto the root's drawable in `compositeAndPresentTarget`. The child swap chains are pure overhead.

**Problem 2: Per-element Layers in UIView.** UIView creates a separate `Layer` + `Canvas` + `BackendRenderTargetContext` for every element tag. This exists because drop shadows, 3D transforms, and blurs were implemented as native layer-level operations (`CALayer.shadowOffset`, `CATransformLayer.transform`, CPU pixel-loop blur). These require each element to own its own layer surface. But the compositor already runs a 3D GPU pipeline — these effects can be handled at the right level: draw-time for some, GPU post-process for others.

**Solution: Two-tier rendering model.**

A **ViewRenderTarget** is the native present surface — one per `View`. It owns the swap chain and is the only thing that presents.

A **LayerRenderTarget** is a GPU texture — one per `Layer`. It has no swap chain, no drawable, no native surface. It is an offscreen render target that the final composition pass blits onto the ViewRenderTarget.

For UIView specifically, even multiple LayerRenderTargets become unnecessary — all elements draw to a single root LayerRenderTarget. Draw-time effects (shadow, transform, opacity) execute inline. Post-process effects (blur) operate on the committed texture via GPU compute.

---

## Effect Taxonomy

Every browser compositor and game engine UI converges on the same split. Effects divide into two categories based on whether they need previously-rendered pixels as input.

### Draw-time effects (per-object, no offscreen texture)

These execute inline during the element's draw call. They are Canvas-level operations.

| Effect | Implementation | Reference |
|--------|---------------|-----------|
| **Drop shadow** | Draw a blurred, tinted, offset copy of the element's shape *before* drawing the element itself. The shadow is geometry — a shape with a Gaussian blur mask applied during rasterization. | Chromium `box-shadow`: Skia draws a blurred rounded rect via `SkMaskFilter::MakeBlur`. Same tile, same texture, no compositor involvement. All game engines: extra geometry drawn behind the element. |
| **3D transform** | Multiply element vertices by a `float4x4` matrix in the vertex shader. No offscreen texture. | Chromium: inline matrix in transform tree, no RenderPass. Flutter: `TransformLayer` — no `saveLayer()`. All game engines: matrix on vertices. |
| **Element opacity** | Multiply fragment output by an alpha scalar in the fragment shader. | Chromium: single-quad opacity is inline (no render surface). Unity/Unreal: per-vertex color modulate. |

### Post-process effects (per-layer, requires offscreen texture)

These need to read pixels that have already been rendered. They require the element's content (or the content behind it) to exist as a committed texture before the effect can run. They are Layer-level operations — the rendering system manages the offscreen texture automatically.

| Effect | Implementation | Reference |
|--------|---------------|-----------|
| **Gaussian blur** | Render element to texture, then apply two-pass separable Gaussian blur shader (H+V) to that texture. | Chromium `filter: blur()`: subtree → RenderPass (offscreen texture) → filter. Flutter: `saveLayer()` + `ImageFilter.blur`. |
| **Directional blur** | Same as Gaussian but single-pass along an angle axis. | |
| **Backdrop blur** | Snapshot the canvas content drawn *before* the element, blur it, composite under the element. | Chromium `backdrop-filter`: copy already-drawn destination buffer, filter it. Flutter `BackdropFilter`: captures prior content. Unreal `BackgroundBlur`: reads framebuffer behind widget. |
| **Group opacity** | Render a subtree to an offscreen texture, then composite the texture with a single opacity value. Required when opacity applies to overlapping children — per-vertex alpha would let children show through each other. | Chromium: effect tree node with opacity < 1.0 on multiple children → RenderPass. Flutter `Opacity` widget → `saveLayer()`. |

### API surface

**Canvas** exposes draw-time effects as drawing operations:
- `drawShadow(shape, shadowParams)` — draws blurred shadow geometry at offset
- Per-element transform: bind `float4x4` uniform before draw calls
- Per-element opacity: bind alpha multiplier before draw calls

**UIView StyleSheet** exposes post-process effects as element style properties:
- `gaussianBlur(tag, radius)` — system renders element, then blur-shaders the result
- `directionalBlur(tag, radius, angle)` — same, along an angle axis
- `backdropBlur(tag, radius)` — system snapshots prior content, blurs behind element

The user never explicitly manages layers or offscreen textures for effects. The rendering system promotes to an offscreen texture when the effect type requires it.

---

## Current Architecture

```
View
  └─ LayerTree
       ├─ rootLayer ──→ BackendVisualTree::Visual ──→ CAMetalLayer + BackendRenderTargetContext
       │                                               (NativeRenderTarget + TextureRenderTarget)
       ├─ childLayer ─→ BackendVisualTree::Visual ──→ CAMetalLayer + BackendRenderTargetContext
       │                                               (NativeRenderTarget + TextureRenderTarget)
       └─ childLayer ─→ ...

compositeAndPresentTarget():
  collect root + all body visuals' committedTextures
  blit each as fullscreen quad onto root's CAMetalLayer drawable
  commitAndPresent on root only
```

Each `BackendRenderTargetContext` owns:
- `targetTexture` / `effectTexture` — GPU textures for offscreen rendering
- `preEffectTarget` / `effectTarget` — texture render targets
- `renderTarget` — `GENativeRenderTarget` (swap chain wrapper)
- `tessellationEngineContext` — per-context tessellation
- `fence` — GPU sync

Child visuals allocate a `GENativeRenderTarget` (which creates a `CAMetalLayer` / `IDXGISwapChain`) even though that native target is never presented. The `compositeAndPresentTarget` blit pass reads from `committedTexture` (the GPU texture), not from the native target's drawable.

UIView additionally creates one Layer per element tag via `UIRenderer::buildLayerRenderTarget`, multiplying the overhead.

---

## Target Architecture

```
View
  └─ ViewRenderTarget ──→ single native present surface
       │                    (NSView+CAMetalLayer / HWND+DXGISwapChain / GDKWindow+VkSwapchainKHR)
       │
       ├─ LayerRenderTarget[root] ──→ GPU texture + TextureRenderTarget + fence
       ├─ LayerRenderTarget[child] ─→ GPU texture + TextureRenderTarget + fence
       └─ ...

compositeAndPresentTarget():
  for each LayerRenderTarget in z-order:
    blit committedTexture onto ViewRenderTarget's drawable
  present once
```

For UIView (single-canvas):
```
UIView
  └─ ViewRenderTarget ──→ single native present surface
       └─ LayerRenderTarget[root] ──→ single GPU texture
            for each element in z-order:
              drawShadow(shape, shadowParams)     ← draw-time, inline
              setTransform(matrix)                 ← draw-time, vertex uniform
              drawElement(shape, brush, opacity)   ← draw-time, inline
            [if blur style active]:
              commit texture
              dispatch gaussianBlurH → gaussianBlurV on element region  ← post-process
            sendFrame → commit → present
```

### Naming Map

| New Name | Old Name | What It Is |
|----------|----------|------------|
| `ViewRenderTarget` | `BackendVisualTree::root::renderTarget` (the one that presents) | Native present surface. One per View. |
| `LayerRenderTarget` | `BackendRenderTargetContext` minus the `GENativeRenderTarget` | GPU texture + texture render target + fence. One per Layer. No swap chain. |

### Platform Mapping

| Concept | macOS | Windows | Linux |
|---------|-------|---------|-------|
| ViewRenderTarget | `NSView` + one `CAMetalLayer` | `HWND` + one `IDXGISwapChain3` | `GdkWindow` + one `VkSwapchainKHR` |
| LayerRenderTarget | `GETexture` + `GETextureRenderTarget` | `GETexture` + `GETextureRenderTarget` | `GETexture` + `GETextureRenderTarget` |
| Final compose | Render pass on `CAMetalLayer` drawable | Render pass on swap chain back buffer | Render pass on swap chain image |
| Present | `commitAndPresent` on root `CAMetalLayer` | `Present()` on View's swap chain | `vkQueuePresentKHR` on View's swap chain |

---

## Render Pipeline (Per Compositor Packet)

1. **Apply layer-tree deltas** to backend mirror (rect, z-order, enabled, effects).
2. **Render dirty LayerRenderTargets** — for each dirty Layer:
   a. For each element in z-order: execute draw-time effects (shadow geometry, transform uniform, opacity uniform) and draw commands into the Layer's GPU texture.
   b. Commit the texture.
   c. For elements with post-process effects: dispatch GPU compute shaders (blur) on the committed texture's element region.
3. **Encode final composition pass** on the ViewRenderTarget's native drawable:
   - Clear background.
   - For each visible LayerRenderTarget in z-order: bind `committedTexture`, draw fullscreen quad with alpha blending.
4. **Present once** per View.

---

## Phase 0 — ViewRenderTarget Extraction

**Goal:** Separate the native present surface from individual Layer contexts. One present surface per View.

### 0.1 Introduce `ViewPresentTarget`

New struct owning the single native render target for a View:

```cpp
struct ViewPresentTarget {
    SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget;
    unsigned backingWidth = 1;
    unsigned backingHeight = 1;
};
```

`BackendCompRenderTarget` gains a `ViewPresentTarget` pointer. `compositeAndPresentTarget` uses it instead of `allContexts[0]->getNativeRenderTarget()`.

### 0.2 Root visual creates the ViewPresentTarget

`BackendVisualTree::setRootVisual` creates the native present surface and stores it in `ViewPresentTarget`. The root visual is the only one that owns a native render target. All backends follow the same pattern: one present surface per View, created at root visual time.

### 0.3 Child visuals become texture-only

`BackendVisualTree::makeVisual` for child layers allocates only a `GETexture` + `GETextureRenderTarget` + `GEFence`. No swap chain, no native surface.

Split the `BackendVisualTree` interface:

```cpp
// VisualTree.h — new interface
INTERFACE_METHOD Core::SharedPtr<Visual> makeRootVisual(Composition::Rect & rect, Composition::Point2D & pos) ABSTRACT;
INTERFACE_METHOD Core::SharedPtr<Visual> makeSurfaceVisual(Composition::Rect & rect, Composition::Point2D & pos) ABSTRACT;
```

`makeRootVisual` creates the native present surface. `makeSurfaceVisual` allocates texture-only. The old `makeVisual` is removed.

### 0.4 Per-backend changes

#### macOS — `MTLCALayerTree` (`CALayerTree.h` / `CALayerTree.mm`)

Current state:
- `makeVisual` creates a `CAMetalLayer` + `GENativeRenderTarget` for every visual (root and child).
- `setRootVisual` calls `view->setRootLayer(metalLayer)` — only the root's `CAMetalLayer` is attached to the `NSView`.
- `addVisual` pushes to `body` but does NOT call `addSublayer:` — child `CAMetalLayer`s are never added to the native view hierarchy.
- `Visual` owns `CAMetalLayer *metalLayer` and `CATransformLayer *transformLayer`.

Changes:
- `makeRootVisual`: creates one `CAMetalLayer`, one `GENativeRenderTarget`, attaches to `NSView` via `setRootLayer`. Stores in `ViewPresentTarget`.
- `makeSurfaceVisual`: allocates `GETexture` + `GETextureRenderTarget` + `GEFence` only. No `CAMetalLayer`. The `Visual` subclass for surfaces does not carry `metalLayer` or `transformLayer`.
- `addVisual` unchanged — pushes to `body` vector. No native sublayer operations.
- `Visual::resize` for surface visuals: only resizes the texture, no `CAMetalLayer` frame updates.
- Delete `CATransformLayer *transformLayer`, `attachTransformLayer` — handled by draw-time transform in Phase 2.

#### Windows — `DCVisualTree` (`DCVisualTree.h` / `DCVisualTree.cpp`)

Current state:
- `DCVisualTree` constructor calls `CreateTargetForHwnd` once per View → `IDCompositionTarget`.
- `makeVisual` creates an `IDXGISwapChain3` + `GENativeRenderTarget` + `IDCompositionVisual2` for every visual.
- `setRootVisual` calls `hwndTarget->SetRoot(v->visual)` — attaches the root DComp visual to the HWND target.
- `addVisual` calls `r->visual->AddVisual(v->visual, ...)` — child DComp visuals are added as children of the root visual, each with its own swap chain as content.
- `Visual` owns `IDXGISwapChain3 *swapChain`, `IDCompositionVisual2 *visual`, `IDCompositionShadowEffect *shadowEffect`, `IDCompositionMatrixTransform3D *transformEffect`.

Changes:
- `makeRootVisual`: creates one `IDXGISwapChain3` + `GENativeRenderTarget` + one `IDCompositionVisual2`. The swap chain is set as the visual's content. The visual is set as the HWND target root. Stores in `ViewPresentTarget`.
- `makeSurfaceVisual`: allocates `GETexture` + `GETextureRenderTarget` + `GEFence` only. No `IDXGISwapChain3`, no `IDCompositionVisual2`. No DComp visual tree wiring.
- `addVisual`: pushes to `body` only. No `AddVisual` call — child surfaces have no DComp visual representation. The blit composite pass handles compositing.
- Delete `IDCompositionShadowEffect *shadowEffect` and `IDCompositionMatrixTransform3D *transformEffect` — handled by draw-time effects in Phase 2.
- Delete `IDCompositionVisual2 *shadowVisual` and `parent` pointer — no per-child DComp visual hierarchy.
- `comp_device_desktop->Commit()` only called once after `setRootVisual` and after present, not per-`addVisual`.

#### Linux — `VKFallbackVisualTree` (`VKLayerTree.h` / `VKLayerTree.cpp`)

Current state:
- `makeVisual` calls `getOrCreateNativeTarget(rect)` which creates a single `VkSwapchainKHR` via `makeNativeRenderTarget`. The same native target is reused for all visuals (root and child share one `nativeTarget` pointer).
- `setRootVisual` just assigns `root = visual`.
- `addVisual` pushes to `body`.
- `Visual` has no platform-specific fields (no shadow/transform effects implemented — both are no-ops).
- `VKCanvasEffectProcessor::applyEffects` is a no-op stub.

Changes:
- `makeRootVisual`: creates one `VkSwapchainKHR` via `makeNativeRenderTarget` from the GTK surface descriptor (Wayland `wl_surface` or X11 `Window`). Stores in `ViewPresentTarget`.
- `makeSurfaceVisual`: allocates `GETexture` + `GETextureRenderTarget` + `GEFence` only. Does NOT call `getOrCreateNativeTarget`. The current code already shares one native target, but it passes it to every `BackendRenderTargetContext` — surface visuals should pass `nullptr` instead.
- `addVisual` / `setRootVisual` unchanged.
- `VKCanvasEffectProcessor`: replace no-op with GPU compute dispatch (Phase 3/4) once OmegaSL→SPIR-V compute pipeline is available.

### 0.5 `BackendVisualTree::Visual` interface update

Remove `updateShadowEffect` and `updateTransformEffect` from the `Visual` base class — these become draw-time Canvas operations (Phase 2). The `Visual` interface reduces to:

```cpp
struct Visual {
    Composition::Point2D pos;
    BackendRenderTargetContext renderTarget;
    virtual void resize(Composition::Rect & newRect) = 0;
    virtual ~Visual() = default;
};
```

This eliminates the empty `updateShadowEffect` / `updateTransformEffect` stubs in `VKFallbackVisualTree::Visual` and the platform-specific effect implementations in `MTLCALayerTree::Visual` and `DCVisualTree::Visual`.

### Files touched

- `wtk/src/Composition/backend/RenderTarget.h` — add `ViewPresentTarget`, update `BackendCompRenderTarget`
- `wtk/src/Composition/backend/RenderTarget.cpp` — `compositeAndPresentTarget` reads from `ViewPresentTarget`
- `wtk/src/Composition/backend/VisualTree.h` — split `makeVisual` into `makeRootVisual` / `makeSurfaceVisual`, remove `updateShadowEffect` / `updateTransformEffect` from `Visual`
- `wtk/src/Composition/backend/mtl/CALayerTree.h` — root visual owns `CAMetalLayer`, surface visuals are texture-only, delete `CATransformLayer`
- `wtk/src/Composition/backend/mtl/CALayerTree.mm` — `makeRootVisual` creates `CAMetalLayer`, `makeSurfaceVisual` allocates texture-only
- `wtk/src/Composition/backend/dx/DCVisualTree.h` — root visual owns `IDXGISwapChain3` + `IDCompositionVisual2`, surface visuals are texture-only, delete DComp effect objects
- `wtk/src/Composition/backend/dx/DCVisualTree.cpp` — `makeRootVisual` creates swap chain + DComp visual, `makeSurfaceVisual` allocates texture-only, `addVisual` no longer calls `AddVisual`
- `wtk/src/Composition/backend/vk/VKLayerTree.h` — surface visuals do not share the native target
- `wtk/src/Composition/backend/vk/VKLayerTree.cpp` — `makeRootVisual` creates `VkSwapchainKHR`, `makeSurfaceVisual` allocates texture-only

---

## Phase 1 — Single-Canvas UIView

**Goal:** All UIView elements draw to the root canvas. No child Layers.

### 1.1 Remove `buildLayerRenderTarget` from element rendering

`UIView::update()` currently calls `buildLayerRenderTarget(tag)` per element, creating a child Layer + Canvas. Instead, draw directly to `rootCanvas`:

```cpp
// Before (per-element layer):
auto & target = buildLayerRenderTarget(entry.tag);
target.canvas->drawRect(rect, brush);
target.canvas->sendFrame();

// After (single canvas):
rootCanvas->drawRect(rect, brush);
```

All elements draw to `rootCanvas` in z-order. One `sendFrame()` at the end.

### 1.2 Remove UIRenderer layer infrastructure

- Delete `UIRenderer::renderTargetStore` (the per-tag layer/canvas map)
- Delete `UIRenderer::buildLayerRenderTarget()`
- Delete `UIRenderer::handleElement()` / `handleTransition()` / `handleAnimation()`
- UIRenderer becomes unnecessary — fold any remaining logic into UIView

### 1.3 Single sendFrame

```cpp
void UIView::update(){
    startCompositionSession();
    rootCanvas->setBackground(backgroundColor);
    for(const auto & entry : resolved){
        drawElement(entry, rootCanvas);
    }
    rootCanvas->sendFrame();
    endCompositionSession();
}
```

### Files touched

- `wtk/src/UI/UIView.cpp` — rewrite `update()` to single-canvas path
- `wtk/include/omegaWTK/UI/UIView.h` — remove UIRenderer dependency, remove `renderTargetStore`

---

## Phase 2 — Draw-Time Effects: Shadow, Transform, Opacity

**Goal:** Implement shadow, 3D transform, and per-element opacity as Canvas draw-time operations. No offscreen textures, no native layer effects.

These are per-object effects that execute inline during the element's draw call, matching the universal pattern across Chromium (`box-shadow`, CSS transforms), Flutter (`Transform` widget, `BoxShadow`), and game engines (shadow geometry, vertex matrix).

### 2.1 Canvas `drawShadow`

Add a draw command that renders a blurred, tinted, offset copy of a shape:

```cpp
void Canvas::drawShadow(Composition::Rect & rect,
                         Composition::LayerEffect::DropShadowParams & shadow);

void Canvas::drawShadow(Composition::RoundedRect & rect,
                         Composition::LayerEffect::DropShadowParams & shadow);

void Canvas::drawShadow(Composition::Ellipse & ellipse,
                         Composition::LayerEffect::DropShadowParams & shadow);
```

Backend implementation: tessellate the shape at `(x + offset.x, y + offset.y)`, expanded by `blurAmount`, fill with `shadow.color * shadow.opacity`. The blur softens the edges.

For simple shapes (rect, rounded rect, ellipse), the blur can be analytic — a precomputed Gaussian falloff applied to the shape's signed distance field in the fragment shader:

```
struct ShadowParams {
    float2 offset;
    float blurSigma;
    float opacity;
    float4 color;
    float4 shapeRect;      // (x, y, w, h) of the shape
    float cornerRadius;
};

[in params]
fragment float4 shadowFragment(float2 fragCoord){
    float dist = roundedRectSDF(fragCoord, params.shapeRect, params.cornerRadius);
    float alpha = smoothstep(params.blurSigma, 0.0, dist) * params.opacity;
    return float4(params.color.rgb, params.color.a * alpha);
}
```

This is the Skia approach — the shadow is a single draw call with a blur mask, not a multi-pass post-process. No offscreen texture, no alpha extraction, no separate blur dispatch.

### 2.2 Per-element transform uniform

Add a `float4x4` transform uniform bound before each element's draw calls:

```
buffer<float4x4> elementTransform : 6;

[in v_buffer, in elementTransform]
vertex OmegaWTKColoredRasterData mainVertex(uint v_id : VertexID){
    OmegaWTKColoredVertex v = v_buffer[v_id];
    OmegaWTKColoredRasterData rasterData;
    rasterData.pos = elementTransform * v.pos;
    rasterData.color = v.color;
    return rasterData;
}
```

Default is identity. When an element has `TransformationParams`, construct `translate * rotate * scale` matrix and bind it. This replaces `CATransformLayer`.

For 3D transforms with non-zero Z, add an optional perspective projection uniform:

```
buffer<float4x4> projection : 7;
rasterData.pos = projection * elementTransform * v.pos;
```

### 2.3 Per-element opacity

Add an opacity scalar to the fragment shader constant buffer. Multiply final fragment alpha by it:

```
buffer<float> elementOpacity : 8;

fragment float4 mainFragment(OmegaWTKColoredRasterData rasterData){
    return float4(rasterData.color.rgb, rasterData.color.a * elementOpacity);
}
```

Default is `1.0`. This handles single-element opacity without needing an offscreen texture. Group opacity (opacity on overlapping children) is a separate, rarer case handled by explicit layer isolation if needed.

### 2.4 UIView integration

In `UIView::update()`, for each element in z-order:

```cpp
// 1. Draw shadow (if any) — inline geometry, before the element
if(hasDropShadow(entry.tag)){
    rootCanvas->drawShadow(entry.shape, shadowParams);
}

// 2. Bind transform (if any) — vertex uniform
if(hasTransform(entry.tag)){
    rootCanvas->setElementTransform(transformMatrix);
}

// 3. Bind opacity (if any) — fragment uniform
rootCanvas->setElementOpacity(opacity);

// 4. Draw the element itself
rootCanvas->drawRect(rect, brush);

// 5. Reset transform/opacity to defaults
rootCanvas->setElementTransform(identity);
rootCanvas->setElementOpacity(1.0f);
```

### 2.5 Remove native layer effects (all backends)

The `Visual` base class no longer has `updateShadowEffect` / `updateTransformEffect` (removed in Phase 0.5). This phase deletes the remaining platform-specific effect code that those methods called.

#### macOS — `MTLCALayerTree::Visual`
- Delete `updateShadowEffect` body — was setting `CALayer.shadowOpacity`, `shadowRadius`, `shadowOffset`, `shadowColor` on the `metalLayer` or `transformLayer`.
- Delete `updateTransformEffect` body — was creating/wrapping with `CATransformLayer`, building `CATransform3D` from translate × rotate × scale, setting `tLayer.transform`.
- Delete `CATransformLayer *transformLayer` field and `attachTransformLayer` flag.
- Delete `~Visual` cleanup of `transformLayer` CFRelease.

#### Windows — `DCVisualTree::Visual`
- Delete `updateShadowEffect` body — was calling `comp_device->CreateShadowEffect`, setting `IDCompositionShadowEffect` color/standardDeviation, creating a `shadowVisual`, calling `parent->AddVisual(shadowVisual, ...)`, and `comp_device->Commit()`.
- Delete `updateTransformEffect` body — was calling `comp_device->CreateMatrixTransform3D`, building `D3DMATRIX` from `D2D1::Matrix4x4F` translate × rotate × scale, calling `visual->SetEffect(transformEffect)`, and `comp_device->Commit()`.
- Delete `IDCompositionShadowEffect *shadowEffect`, `IDCompositionMatrixTransform3D *transformEffect`, `IDCompositionVisual2 *shadowVisual` fields.

#### Linux — `VKFallbackVisualTree::Visual`
- Delete the no-op `updateShadowEffect` / `updateTransformEffect` stubs (already empty `(void)params` bodies).

#### Shared
- `LayerEffect::DropShadowParams` and `TransformationParams` structs remain as the API — they now drive Canvas draw-time operations instead of native layer properties.

### Files touched

- `wtk/include/omegaWTK/Composition/Canvas.h` — add `drawShadow`, `setElementTransform`, `setElementOpacity`
- `wtk/src/Composition/Canvas.cpp` — implement new draw commands as `VisualCommand` types
- `wtk/src/Composition/backend/RenderTarget.cpp` — SDF shadow fragment shader, transform/opacity uniforms in existing vertex/fragment shaders, pipeline variants
- `wtk/src/Composition/backend/mtl/CALayerTree.h` — delete shadow/transform effect bodies, `CATransformLayer`, `attachTransformLayer`
- `wtk/src/Composition/backend/dx/DCVisualTree.h` — delete `IDCompositionShadowEffect`, `IDCompositionMatrixTransform3D`, `shadowVisual`
- `wtk/src/Composition/backend/dx/DCVisualTree.cpp` — delete `updateShadowEffect`, `updateTransformEffect` bodies
- `wtk/src/Composition/backend/vk/VKLayerTree.cpp` — delete no-op stubs
- `wtk/src/UI/UIView.cpp` — draw shadows inline, bind transform/opacity per element

---

## Phase 3 — GPU Gaussian Blur Shader

**Goal:** Replace the CPU pixel-loop blur with a two-pass separable Gaussian blur in OmegaSL compute shaders. This is a post-process effect — it requires the element's rendered content to exist as a texture before the blur can run.

### 3.1 OmegaSL blur shaders

Two compute shaders (horizontal + vertical) for separable Gaussian blur:

```
struct BlurParams {
    float radius;
    uint2 textureSize;
    uint2 regionOrigin;
    uint2 regionSize;
};

buffer<BlurParams> params : 5;
texture2d srcTex : 3;
texture2d dstTex : 4;

[in params, in srcTex, out dstTex]
compute(x=8, y=8, z=1)
void gaussianBlurH(uint3 tid : GlobalThreadID){ ... }

[in params, in srcTex, out dstTex]
compute(x=8, y=8, z=1)
void gaussianBlurV(uint3 tid : GlobalThreadID){ ... }
```

The `regionOrigin` / `regionSize` fields scope the blur to the element's bounding rect within the single texture. Pixels outside the region are untouched.

### 3.2 Blur pipeline setup

Add compute pipeline states for `gaussianBlurH` and `gaussianBlurV` in `loadGlobalRenderAssets()`.

### 3.3 Per-backend effect processor changes

All three backends implement `BackendCanvasEffectProcessor::applyEffects`. Each needs to switch from its current approach to the OmegaSL GPU compute path.

#### macOS — `CICanvasEffectProcessor` (`CICanvasEffectProcessor.mm`)

Current state: CPU pixel-loop blur. Reads back entire texture bytes via `getBytes:`, premultiplies alpha, applies Gaussian/directional kernel per-pixel in nested loops, un-premultiplies, writes back via `replaceRegion:`. Blocks the render thread. O(width x height x kernel_size).

Changes:
- Replace `applyGaussianBlur` / `applyDirectionalBlur` / `makeGaussianKernel` / `samplePremultBilinear` / `PremultPixel` with GPU compute dispatch.
- `applyEffects` dispatches `gaussianBlurH` + `gaussianBlurV` compute pipelines via `GECommandBuffer` compute encoder.
- Delete all CPU readback/writeback code.

#### Windows — `D2DCanvasEffectProcessor` (`D2DCanvasEffectProcessor.cpp`)

Current state: Uses D3D11On12 interop to wrap D3D12 textures as D2D1 surfaces, then applies `CLSID_D2D1GaussianBlur` / `CLSID_D2D1DirectionalBlur` via the D2D1 effect pipeline. Involves: `D3D11On12CreateDevice`, `CreateWrappedResource` for source + dest, `CreateBitmapFromDxgiSurface`, `CreateEffect`, `DrawImage`, `EndDraw`, `Flush`, `Signal`.

Changes:
- Replace the entire D2D1 interop path with OmegaSL compute dispatch. The blur shaders compile to HLSL compute shaders via OmegaSL. Dispatch via the existing D3D12 command queue.
- Delete `ID3D11On12Device`, `D2D1 DeviceContext`, `CLSID_D2D1GaussianBlur`, `CLSID_D2D1DirectionalBlur` usage.
- This eliminates the D3D11On12 dependency entirely — blur runs as native D3D12 compute, same as the rest of the pipeline.

#### Linux — `VKCanvasEffectProcessor` (`VKLayerTree.cpp`)

Current state: No-op stub. `applyEffects` does nothing — blur is not implemented on Vulkan.

Changes:
- Implement GPU compute dispatch using OmegaSL→SPIR-V compiled blur shaders.
- Dispatch via `VkCommandBuffer` compute pipeline. Same `gaussianBlurH` / `gaussianBlurV` shaders, compiled to SPIR-V.
- This brings blur to feature parity with Metal and DX for the first time.

### 3.4 Delete legacy blur code

- macOS: Remove `applyGaussianBlur`, `applyDirectionalBlur`, `makeGaussianKernel`, `samplePremultBilinear`, `PremultPixel` from `CICanvasEffectProcessor.mm`.
- Windows: Remove D3D11On12 interop, D2D1 effect chain from `D2DCanvasEffectProcessor.cpp`.
- Linux: Remove no-op stub body.

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — OmegaSL blur shader source, compute pipeline creation in `loadGlobalRenderAssets()`
- `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` — GPU compute dispatch replaces CPU pixel loop
- `wtk/src/Composition/backend/dx/D2DCanvasEffectProcessor.cpp` — GPU compute dispatch replaces D2D1/D3D11On12 interop
- `wtk/src/Composition/backend/vk/VKLayerTree.cpp` — GPU compute dispatch replaces no-op stub

---

## Phase 4 — GPU Directional Blur Shader

**Goal:** Replace the CPU directional blur with a single-pass compute shader. Like Gaussian blur, this is a post-process effect.

```
struct DirectionalBlurParams {
    float radius;
    float angle;
    uint2 textureSize;
    uint2 regionOrigin;
    uint2 regionSize;
};

[in params, in srcTex, out dstTex]
compute(x=8, y=8, z=1)
void directionalBlur(uint3 tid : GlobalThreadID){ ... }
```

Single pass — directional blur is 1D along the angle axis, not separable.

Same per-backend dispatch pattern as Phase 3: OmegaSL compute shader compiled to MSL/HLSL/SPIR-V, dispatched through each backend's compute encoder.

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — shader source, pipeline
- `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` — dispatch
- `wtk/src/Composition/backend/dx/D2DCanvasEffectProcessor.cpp` — dispatch (replaces `CLSID_D2D1DirectionalBlur`)
- `wtk/src/Composition/backend/vk/VKLayerTree.cpp` — dispatch (replaces no-op)

---

## Phase 5 — Cleanup

### 5.1 Remove multi-layer compositing overhead

- `compositeAndPresentTarget` simplifies: for single-canvas Views (UIView, SVGView), there is exactly one LayerRenderTarget. The blit pass draws one texture onto the ViewRenderTarget's drawable.
- For multi-Layer Views (if any remain), the blit pass still composites N textures in z-order.

### 5.2 Remove dead infrastructure

- `UIRenderer` class — delete entirely
- `buildLayerRenderTarget` — deleted in Phase 1
- `renderTargetStore` — deleted in Phase 1
- `animationLayerAnimators` — animations target root canvas directly
- `BackendVisualTree::Visual::updateShadowEffect` — deleted in Phase 2
- `BackendVisualTree::Visual::updateTransformEffect` — deleted in Phase 2
- Per-visual `CAMetalLayer` / `IDXGISwapChain` / `VkSwapchainKHR` allocation in `makeVisual` — deleted in Phase 0

### 5.3 Surface lifecycle

- Surface pool with LRU recycle for LayerRenderTarget textures to avoid resize thrash.
- Resize LayerRenderTargets on stable dimensions only.
- Resize ViewRenderTarget on View resize (existing `resizeSwapChain` path).

---

## Dependency Graph

```
Phase 0: ViewRenderTarget extraction (one present surface per View)
    └─→ Phase 1: Single-canvas UIView (no child Layers)
            ├─→ Phase 2: Draw-time effects (shadow, transform, opacity)
            ├─→ Phase 3: GPU Gaussian blur (post-process)
            ├─→ Phase 4: GPU directional blur (post-process)
            └─→ Phase 5: Cleanup
```

Phase 0 is the backend structural change. Phase 1 is the UIView frontend simplification. Phase 2 adds draw-time effects (shadow, transform, opacity) as Canvas operations. Phases 3-4 replace CPU blur with GPU compute shaders. Phase 5 is cleanup.

Phases 2, 3, and 4 are independent of each other and can proceed in parallel once Phase 1 lands.

---

## Risks

1. **SDF shadow quality for complex shapes**: The analytic SDF approach works well for rects, rounded rects, and ellipses. For arbitrary vector paths, the SDF is harder to compute. Fallback: rasterize the path's alpha mask to a small texture, blur it with the Phase 3 Gaussian blur shader, composite as shadow. This is the `filter: drop-shadow()` path — more expensive, but only needed for non-analytic shapes.

2. **Shader compilation on first use**: New shaders (shadow SDF, blur compute) compile on first dispatch. Mitigation: compile all pipelines in `loadGlobalRenderAssets()` at startup.

3. **Animation rerouting**: Current animation system targets per-element `LayerAnimator` instances. With a single canvas, animations must target draw parameters (brush color, shape dimensions, transform matrix, shadow params). The existing `PropertyAnimationState` / `advanceAnimations` system already works at this level — only `LayerEffect` animations need rerouting.

4. **Platform parity**: OmegaSL compiles to MSL/HLSL/SPIR-V, so shaders are cross-platform. Each backend's compute dispatch path needs testing.

5. **Memory from per-Layer textures**: Mitigation: pooled textures with LRU eviction, shared format/size classes.

6. **Group opacity edge case**: Per-element opacity (Phase 2) handles single-element transparency. If overlapping children need group opacity (the CSS `opacity` on a container problem), the subtree must render to an offscreen texture first. This is rare in practice and can be handled by explicit layer isolation if needed — not in scope for this plan.

---

## File Change Summary

### Backend-agnostic

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/Composition/backend/RenderTarget.h` | 0 | Add `ViewPresentTarget`, update `BackendCompRenderTarget` |
| `wtk/src/Composition/backend/RenderTarget.cpp` | 0, 2, 3, 4 | `compositeAndPresentTarget` uses `ViewPresentTarget`; OmegaSL SDF shadow shader, transform/opacity uniforms, blur/directional compute shaders, pipeline creation |
| `wtk/src/Composition/backend/VisualTree.h` | 0 | Split `makeVisual` into `makeRootVisual` / `makeSurfaceVisual`; remove `updateShadowEffect` / `updateTransformEffect` from `Visual` base class |
| `wtk/include/omegaWTK/Composition/Canvas.h` | 2 | Add `drawShadow`, `setElementTransform`, `setElementOpacity` |
| `wtk/src/Composition/Canvas.cpp` | 2 | Implement new draw commands as `VisualCommand` types |
| `wtk/src/UI/UIView.cpp` | 1, 2 | Single-canvas `update()`, draw-time shadow/transform/opacity per element |
| `wtk/include/omegaWTK/UI/UIView.h` | 1 | Remove UIRenderer, `renderTargetStore` |

### macOS (Metal)

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/Composition/backend/mtl/CALayerTree.h` | 0, 2 | Root visual owns `CAMetalLayer`; surface visuals texture-only; delete `CATransformLayer`, `attachTransformLayer`, `updateShadowEffect`, `updateTransformEffect` |
| `wtk/src/Composition/backend/mtl/CALayerTree.mm` | 0 | `makeRootVisual` creates `CAMetalLayer` + native target; `makeSurfaceVisual` allocates texture-only |
| `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` | 3, 4 | GPU compute dispatch replaces CPU pixel-loop blur; delete `applyGaussianBlur`, `applyDirectionalBlur`, `makeGaussianKernel`, `samplePremultBilinear`, `PremultPixel` |

### Windows (D3D12 / DComp)

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/Composition/backend/dx/DCVisualTree.h` | 0, 2 | Root visual owns `IDXGISwapChain3` + `IDCompositionVisual2`; surface visuals texture-only; delete `IDCompositionShadowEffect`, `IDCompositionMatrixTransform3D`, `shadowVisual` |
| `wtk/src/Composition/backend/dx/DCVisualTree.cpp` | 0, 2 | `makeRootVisual` creates swap chain + DComp visual + HWND target; `makeSurfaceVisual` allocates texture-only; `addVisual` no longer calls `AddVisual`; delete `updateShadowEffect`/`updateTransformEffect` bodies |
| `wtk/src/Composition/backend/dx/D2DCanvasEffectProcessor.cpp` | 3, 4 | GPU compute dispatch replaces D2D1/D3D11On12 interop; delete `ID3D11On12Device`, `CLSID_D2D1GaussianBlur`, `CLSID_D2D1DirectionalBlur` usage |

### Linux (Vulkan / GTK)

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/Composition/backend/vk/VKLayerTree.h` | 0 | Surface visuals do not share native target |
| `wtk/src/Composition/backend/vk/VKLayerTree.cpp` | 0, 2, 3, 4 | `makeRootVisual` creates `VkSwapchainKHR`; `makeSurfaceVisual` allocates texture-only; delete no-op effect stubs; implement GPU compute blur dispatch (first blur implementation on Vulkan) |
