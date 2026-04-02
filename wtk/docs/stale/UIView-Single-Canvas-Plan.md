# UIView Single-Canvas Rendering Plan

Collapse UIView from one-layer-per-element to a single root canvas. Replace native layer effects (CALayer shadows, CATransformLayer transforms) and CPU-side blur (CICanvasEffectProcessor) with OmegaSL shader passes that run entirely in the existing 3D pipeline.

---

## Why

UIView currently creates a separate Layer → CAMetalLayer → BackendRenderTargetContext for every element tag. Each element gets its own offscreen texture, its own tessellation context, and its own GPU commit. The compositor's blit pass then composites all textures onto the root's drawable.

This multi-layer design exists because drop shadows and transforms were originally implemented as CALayer properties (`shadowOffset`, `shadowRadius`, `CATransformLayer.transform`). A blur was implemented as a CPU readback + pixel-loop in `CICanvasEffectProcessor`. These are native-surface-level operations — they operate on an entire layer's content, so each element needed its own layer.

Problems:

| Problem | Impact |
|---------|--------|
| N+1 CAMetalLayers per UIView (root + N elements) | Each CAMetalLayer allocates a drawable, a backing texture, a fence, a tessellation context |
| Backend mirror must track every dynamic child layer | Source of the blank-content bug — `Layer::setEnabled` wasn't notifying observers |
| Software blit composites all layers per present | Extra GPU pass per UIView, linear in element count |
| CPU blur reads back the entire texture, blurs per-pixel on CPU, writes back | Blocks the render thread; O(width × height × kernel) per blur |
| Shadows use CALayer properties | Not portable; no control over shadow shape, feathering, or animation |
| Transforms use CATransformLayer | Not portable; 3D transform can't be animated through our animation system |
| z-ordering depends on layer submission order | Fragile; must match compositor's visual tree body order |

If effects are shader passes on the element's region of a single texture, all of this collapses to one Layer, one Canvas, one `sendFrame`, one `commit`, zero blit compositing.

---

## Current architecture

```
UIView.update()
  ├─ rootCanvas  →  rootLayer (CAMetalLayer)  →  BackendRenderTargetContext[root]
  ├─ element "a" →  layerA   (CAMetalLayer)   →  BackendRenderTargetContext[a]
  ├─ element "b" →  layerB   (CAMetalLayer)   →  BackendRenderTargetContext[b]
  └─ ...
  
compositeAndPresentTarget():
  blit root texture → blit a texture → blit b texture → present
```

Effects are applied per-layer:
- **Drop shadow**: `CALayer.shadowOffset/shadowRadius/shadowColor` on the element's CAMetalLayer (via `Visual::updateShadowEffect`)
- **3D transform**: `CATransformLayer.transform` wrapping the element's CAMetalLayer (via `Visual::updateTransformEffect`)
- **Gaussian blur**: CPU readback → `applyGaussianBlur` pixel loop → CPU writeback (via `CICanvasEffectProcessor`)
- **Directional blur**: Same CPU path with directional kernel

---

## Target architecture

```
UIView.update()
  └─ rootCanvas  →  rootLayer (single CAMetalLayer)  →  BackendRenderTargetContext
       draw element "a" (tessellation → colored/textured vertices)
       draw element "b"
       [optional] apply effect passes on element regions
       sendFrame → commit → present
```

One canvas, one texture, one commit. Effects are shader passes that read from the committed texture and write to an effect texture, same as the existing `preEffectTarget` / `effectTexture` pair but driven by element-scoped regions instead of whole-layer scope.

---

## Phase 0 — Single-canvas UIView rendering

**Goal:** All UIView elements draw to the root canvas. No child layers.

### 0.1 Remove `buildLayerRenderTarget` from element rendering

`UIView::update()` currently calls `buildLayerRenderTarget(tag)` per element, which creates a child Layer + Canvas. Instead, draw directly to `rootCanvas`:

```cpp
// Before (per-element layer):
auto & target = buildLayerRenderTarget(entry.tag);
target.canvas->drawRect(rect, brush);
target.canvas->sendFrame();

// After (single canvas):
rootCanvas->drawRect(rect, brush);
```

All elements draw to `rootCanvas` in z-order. One `sendFrame()` at the end.

### 0.2 Remove UIRenderer layer infrastructure

- Delete `UIRenderer::renderTargetStore` (the per-tag layer/canvas map)
- Delete `UIRenderer::buildLayerRenderTarget()`
- Delete `UIRenderer::handleElement()` / `handleTransition()` / `handleAnimation()`
- UIRenderer becomes unnecessary — fold any remaining logic into UIView

### 0.3 Single sendFrame

```cpp
void UIView::update(){
    // ... resolve layout, styles ...
    
    startCompositionSession();
    
    // Set background
    rootCanvas->setBackground(backgroundColor);
    
    // Draw all elements in z-order to rootCanvas
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

## Phase 1 — GPU Gaussian blur shader

**Goal:** Replace the CPU pixel-loop blur with a two-pass separable Gaussian blur in OmegaSL compute shaders.

### 1.1 OmegaSL blur shaders

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

// Horizontal pass
[in params, in srcTex, out dstTex]
compute(x=8, y=8, z=1)
void gaussianBlurH(uint3 tid : GlobalThreadID){
    // Sample horizontally within regionOrigin+regionSize
    // Write to dstTex at tid.xy
}

// Vertical pass
[in params, in srcTex, out dstTex]
compute(x=8, y=8, z=1)
void gaussianBlurV(uint3 tid : GlobalThreadID){
    // Sample vertically
    // Write to dstTex at tid.xy
}
```

The `regionOrigin` / `regionSize` fields scope the blur to the element's bounding rect within the single texture. Pixels outside the region are untouched.

### 1.2 Blur pipeline setup

Add compute pipeline states for `gaussianBlurH` and `gaussianBlurV` in `loadGlobalRenderAssets()`, alongside the existing (commented-out) gradient compute pipeline.

### 1.3 Canvas effect dispatch

Replace `CICanvasEffectProcessor::applyEffects` (CPU readback) with GPU dispatch:

```cpp
void applyEffects(...) override {
    for(auto & effect : effects){
        if(effect.type == CanvasEffect::GaussianBlur){
            // Write BlurParams to const buffer
            // Dispatch gaussianBlurH: src→tmp
            // Dispatch gaussianBlurV: tmp→dst
        }
    }
}
```

### 1.4 Delete CPU blur

Remove `applyGaussianBlur`, `applyDirectionalBlur`, `makeGaussianKernel`, `samplePremultBilinear`, `PremultPixel` from `CICanvasEffectProcessor.mm`. The entire CPU readback/writeback path goes away.

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — shader source, pipeline creation
- `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` — GPU dispatch replaces CPU loop
- `wtk/src/Composition/backend/dx/D2DCanvasEffectProcessor.cpp` — same for DX (or delete if unused)

---

## Phase 2 — GPU drop shadow shader

**Goal:** Replace CALayer shadow properties with a shader-based drop shadow.

### 2.1 Shadow generation approach

A drop shadow is: take the element's alpha mask, offset it, blur it, tint it, draw it behind the element.

1. **Alpha extract**: Compute shader reads the element's region from the canvas texture. Outputs a single-channel alpha mask offset by `(x_offset, y_offset)`.
2. **Blur**: Run the Phase 1 Gaussian blur on the alpha mask (radius = `blurAmount`).
3. **Tint + composite**: Fragment shader reads the blurred alpha, multiplies by `shadow.color * shadow.opacity`, and composites under the existing content using `dst-over` blending (or pre-draws before the element).

### 2.2 OmegaSL shadow shaders

```
struct ShadowParams {
    float2 offset;
    float radius;
    float opacity;
    float4 color;
    uint2 regionOrigin;
    uint2 regionSize;
};

// Extract alpha mask with offset
[in params, in srcTex, out maskTex]
compute(x=8, y=8, z=1)
void shadowExtractAlpha(uint3 tid : GlobalThreadID){ ... }

// After blur, composite shadow under content
[in params, in blurredMask, in mainTex, out dstTex]
compute(x=8, y=8, z=1)
void shadowComposite(uint3 tid : GlobalThreadID){ ... }
```

### 2.3 Integration with UIView

In `UIView::update()`, for elements with a drop shadow style:

```
1. Draw element to rootCanvas (as in Phase 0)
2. Queue shadow effect with element's resolved rect as the region
3. Effect processor runs: extract alpha → blur → composite behind
```

Alternatively, draw the shadow BEFORE the element so it naturally sits behind.

### 2.4 Remove CALayer shadow code

- Delete `Visual::updateShadowEffect` in `CALayerTree.h`
- Delete `LayerEffect::DropShadow` handling in compositor layer commands
- `LayerEffect::DropShadowParams` struct remains as the API — it just drives the shader now

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — shadow shader source, pipeline
- `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` — shadow dispatch
- `wtk/src/Composition/backend/mtl/CALayerTree.h` — delete `updateShadowEffect`
- `wtk/src/UI/UIView.cpp` — shadow effect scheduling in `update()`

---

## Phase 3 — GPU directional blur shader

**Goal:** Replace the CPU directional blur with a single-pass compute shader.

### 3.1 OmegaSL directional blur shader

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
void directionalBlur(uint3 tid : GlobalThreadID){
    // Sample along (cos(angle), sin(angle)) direction
    // Average samples weighted by distance
}
```

Single pass because directional blur is not separable. The kernel is 1D along the angle axis.

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — shader source, pipeline
- `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` — dispatch

---

## Phase 4 — GPU 3D transform

**Goal:** Replace CATransformLayer with a vertex-level transform applied during tessellation or in the vertex shader.

### 4.1 Per-element transform uniform

Add a `float4x4 transform` field to the vertex shader's constant buffer. Default is identity. When an element has a `TransformationParams` style, construct the 4×4 matrix (translate × rotate × scale) and bind it before drawing that element's vertices.

```
buffer<float4x4> elementTransform : 6;

[in v_buffer, in elementTransform]
vertex OmegaWTKColoredRasterData transformedVertex(uint v_id : VertexID){
    OmegaWTKColoredVertex v = v_buffer[v_id];
    OmegaWTKColoredRasterData rasterData;
    rasterData.pos = elementTransform * v.pos;
    rasterData.color = v.color;
    return rasterData;
}
```

### 4.2 Perspective projection

For 3D transforms with non-zero Z translation or rotation, a perspective projection matrix is needed. The current pipeline uses orthographic (NDC coordinates directly). Add an optional projection uniform:

```
buffer<float4x4> projection : 7;
rasterData.pos = projection * elementTransform * v.pos;
```

Default projection is identity (orthographic). When 3D transforms are active, set a perspective matrix based on the view's dimensions and a configurable field-of-view.

### 4.3 Remove CATransformLayer code

- Delete `Visual::updateTransformEffect` in `CALayerTree.h`
- Delete `CATransformLayer *transformLayer` from Visual
- Delete `attachTransformLayer` logic
- Delete `LayerEffect::Transformation` handling in compositor layer commands
- `LayerEffect::TransformationParams` struct remains as the API

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — shader source, transform pipeline, projection setup
- `wtk/src/Composition/backend/mtl/CALayerTree.h` — delete transform layer code
- `wtk/src/UI/UIView.cpp` — bind transform matrix per element in `update()`

---

## Phase 5 — Cleanup

### 5.1 Remove multi-layer compositing from UIView path

Once UIView uses a single canvas:
- `compositeAndPresentTarget` no longer needs to iterate `body` visuals for UIView render targets — there are none.
- The blit pass still runs (root texture → drawable), but with exactly one source.
- SVGView already uses this single-canvas pattern.

### 5.2 Remove dead layer infrastructure

- `UIRenderer` class — delete entirely
- `buildLayerRenderTarget` — deleted in Phase 0
- `renderTargetStore` — deleted in Phase 0
- `animationLayerAnimators` — animations now target the root canvas directly

### 5.3 Simplify BackendVisualTree for single-canvas views

For views that only use the root visual (SVGView, UIView after migration), the `body` vector is always empty. No functional change needed, but the `addVisual` / child visual creation path in `ensureLayerSurfaceTarget` becomes dead code for these views.

---

## Dependency graph

```
Phase 0: Single-canvas UIView
    ├─→ Phase 1: GPU Gaussian blur (replaces CPU blur)
    │       └─→ Phase 2: GPU drop shadow (uses Phase 1 blur)
    │       └─→ Phase 3: GPU directional blur
    └─→ Phase 4: GPU 3D transform (independent of blur/shadow)
            └─→ Phase 5: Cleanup
```

Phase 0 is the structural change. Phases 1–4 are effect replacements that can proceed in parallel once Phase 0 lands. Phase 5 is cleanup after all effects are ported.

---

## Risks

1. **Z-ordering within a single texture**: With one canvas, elements are drawn in painter's-order. Later elements overwrite earlier ones. This matches the current behavior (elements are sorted by zIndex + insertion order). No change needed.

2. **Element-scoped effects on a shared texture**: Blur/shadow shaders need `regionOrigin`/`regionSize` to scope their work to one element's bounding rect. If elements overlap, a blur on element A could bleed into element B's region. Mitigation: effects read from a snapshot of the pre-effect texture (the existing `targetTexture` → `effectTexture` double-buffer pattern).

3. **Shader compilation on first use**: New compute shaders compile on first dispatch. Mitigation: compile all pipelines in `loadGlobalRenderAssets()` at startup, same as existing shaders.

4. **Animation targeting**: The current animation system targets per-element `LayerAnimator` instances. With a single canvas, animations must target the element's draw parameters (brush color, shape dimensions, transform matrix) rather than layer-level properties. The existing `PropertyAnimationState` / `advanceAnimations` system already works at this level — only the `LayerEffect` animations need rerouting.

5. **Platform parity**: The DX12/Vulkan backends have their own effect processors (`D2DCanvasEffectProcessor`, `VKLayerTree`). The shader approach (OmegaSL compiles to HLSL/SPIR-V/MSL) is inherently cross-platform, but each backend's compute dispatch path needs testing.

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/UI/UIView.cpp` | 0, 2 | Single-canvas update(), remove per-element layer creation, shadow scheduling |
| `wtk/include/omegaWTK/UI/UIView.h` | 0 | Remove UIRenderer, renderTargetStore, per-element layer members |
| `wtk/src/Composition/backend/RenderTarget.cpp` | 1, 2, 3, 4 | OmegaSL blur/shadow/directional/transform shaders, pipeline creation |
| `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm` | 1, 2, 3 | GPU compute dispatch replaces CPU pixel loops |
| `wtk/src/Composition/backend/mtl/CALayerTree.h` | 2, 4 | Delete updateShadowEffect, delete CATransformLayer code |
| `wtk/src/Composition/backend/mtl/CALayerTree.mm` | 5 | Simplify addVisual (no child visuals for single-canvas views) |
| `wtk/src/Composition/backend/dx/D2DCanvasEffectProcessor.cpp` | 1, 2, 3 | Same GPU dispatch for DX backend |

---

## References

- Current multi-layer rendering: `wtk/src/UI/UIView.cpp` (`buildLayerRenderTarget`, `update()`)
- Current CPU blur: `wtk/src/Composition/backend/mtl/CICanvasEffectProcessor.mm`
- Current CALayer effects: `wtk/src/Composition/backend/mtl/CALayerTree.h` (`updateShadowEffect`, `updateTransformEffect`)
- OmegaSL shader source: `wtk/src/Composition/backend/RenderTarget.cpp` (inline `librarySource`)
- OmegaSL compiler types: `gte/include/omegasl.h`
- Existing pipeline setup: `wtk/src/Composition/backend/RenderTarget.cpp` (`loadGlobalRenderAssets`)
- Blit composite pass: `wtk/src/Composition/backend/RenderTarget.cpp` (`compositeAndPresentTarget`)
