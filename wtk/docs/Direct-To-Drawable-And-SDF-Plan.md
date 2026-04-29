# Direct-To-Drawable Render Path and SDF Primitives Plan

## Problem

The compositor currently has two render paths:

1. **Direct-to-drawable** — used when no canvas effects are queued. Frame
   primitives are recorded straight onto the swap chain command buffer and
   `commitAndPresent()` runs.
2. **Offscreen-then-blit** — used when any `CanvasEffect` (gaussian /
   directional blur) is queued. Frame primitives are recorded onto an
   offscreen `BGRA8Unorm` texture (`preEffectTarget`); after the draw pass
   the effect compute shaders ping-pong between `targetTexture` and
   `effectTexture`; finally a fullscreen-quad copy pipeline blits the
   offscreen result onto the swap chain drawable.

Every `BackendRenderTargetContext` allocates the offscreen texture pair
unconditionally, whether or not it ever applies an effect.

This produces several render-correctness problems and a pile of supporting
code that exists only to paper over the format/precision mismatch between
the offscreen texture and the swap chain:

- The offscreen texture is hard-coded to `BGRA8Unorm`. Swap chains on DX/VK
  routinely use other formats (`RGBA8Unorm`, `RGB10A2`, sRGB variants,
  HDR10). Every effect-path frame loses precision in the round-trip.
- `PipelineRegistry::finalCopyForFormat` exists to construct a copy
  pipeline per native pixel format because the BGRA8Unorm offscreen
  pipeline can't drive a different-format swap chain. The cache is purely
  a workaround for the offscreen format choice.
- HDR / wide-gamut swap chains can't carry their full precision through
  the frame because everything funnels through a non-HDR intermediate.
- The fullscreen-quad blit costs an extra render pass per frame on
  effect-path contexts, plus a fence handoff between the offscreen target
  and the swap chain target.
- `BackingTextureSet` forces a 64 MiB texture heap allocation up front
  (`kTextureHeapSize`) plus per-context texture pair acquisition even when
  no effects are ever used.

Effects are also conceptually misclassified in the current data model.
`CanvasEffect` (blur) is per-context queued state, but blurs semantically
apply to a whole layer. `VisualCommand::Shadow` is a per-object draw
command (correct). 3D transforms travel through `VisualCommand::SetTransform`
as per-following-draws state (also correct). The blur queue's per-context
scope is the only odd one out.

Separately, the simple-shape draw path uses CPU tessellation.
`VisualCommand::Ellipse` emits up to 4096 triangles per ellipse. Rect /
rounded-rect / shadow geometry runs through `OmegaGTE::TETriangulationParams`
and the per-context tessellation engine context. None of that is required
for shapes that have closed-form signed-distance functions, and the
tessellated output is also not resolution-independent — it's pre-AA at
fixed sub-pixel density and looks soft under 3D transforms / zoom.

Borders are doubly indirected today. `Canvas::drawRect`,
`Canvas::drawRoundedRect`, and `Canvas::drawEllipse` accept
`Core::Optional<Border> { width, brush }`. The `VisualCommand::Data`
struct already has a `Core::Optional<Border>` slot on `rectParams`,
`roundedRectParams`, and `ellipseParams` ([`Canvas.h`][canvas-h] lines
78, 84, 90). But the Canvas API never populates that slot — every
draw site passes `Core::Optional<Border>{}` and instead emits a
**second** visual built from `RectFrame` / `RoundedRectFrame` /
`EllipseFrame` (in [`Path.cpp`][path-cpp]). Each frame is an outline
constructed via `Path::addLine` and `Path::addArc` and submitted as a
`VisualCommand::VectorPath`, which then goes through the GTE
triangulation engine like any other vector path. So a "rect with a
1-pixel border" today produces:

1. one `Rect` visual (fill, no border info), tessellated to 2 triangles;
2. one `VectorPath` visual (the border outline, ~4 line segments
   widened to a stroke), tessellated to many more.

For complex paths, the triangulator already does the right thing: the
[`VisualCommand::VectorPath` case in `renderToTarget`][rt-vp] adds two
attachments to `OmegaGTE::TETriangulationParams::GraphicsPath2D` — the
stroke color first and the fill color second when present. The
triangulator emits per-vertex attachment data so each output triangle
carries either the stroke or fill color, and a single draw call
rasterizes both. This is correct today for vector paths.

[canvas-h]: ../include/omegaWTK/Composition/Canvas.h
[path-cpp]: ../src/Composition/Path.cpp
[rt-vp]: ../src/Composition/backend/RenderTarget.cpp

## Guiding Principles

- **One render path.** Every primitive renders directly to the swap chain
  drawable. Offscreen textures exist only when a layer actually has a
  blur effect applied, allocated on demand at the layer's bounds.
- **Effects classified by their semantic scope.**
  - *Per-object* effects (drop-shadow, opacity, 3D transform) are
    expressed as draw-time state or extra draws. No offscreen surface.
  - *Per-layer* effects (blur) get a per-blurred-layer scratch surface.
    Nothing else.
- **Resolution-independent rasterization for simple shapes.** Rect,
  rounded rect, ellipse, and shadow use a single SDF fragment shader
  driven by a 6-vertex quad per primitive. No tessellation. AA via
  `smoothstep` on the signed distance.
- **Tessellation only for what cannot be expressed analytically.** Vector
  paths with arbitrary curves keep the tessellation path but gain
  per-edge distance varyings so the fragment shader smoothsteps the
  silhouette for resolution-independent AA.
- **Borders are not a separate primitive.** A rect / rounded-rect /
  ellipse with a border produces exactly one visual command and one
  draw call: the SDF fragment shader emits fill coverage *and* stroke
  coverage from the same distance evaluation. The
  `RectFrame` / `RoundedRectFrame` / `EllipseFrame` path-construction
  helpers stop being used for borders on simple shapes. They remain
  available for clients that want a stand-alone outline. For complex
  vector paths, the GTE triangulator's existing dual-attachment
  (stroke + fill) flow continues to drive a single tessellation pass
  that produces both bands of geometry.
- **No behavior changes are silent.** The existing tests
  (`EllipsePathCompositorTest`, `TextCompositorTest`,
  `SVGViewRenderTest`, `BasicAppTest`) get pixel-comparison baselines
  before each phase. Where a phase improves correctness (HDR precision,
  AA quality), the new pixels are the new baseline; the diff is reviewed
  rather than rejected.
- **`BackendResourceFactory` continues to be the single owner of every
  GPU allocation.** Per-layer scratch surfaces, the new SDF render
  pipeline state, the SDF shader library, and any new uniform/constant
  buffers all flow through it.

---

## Phase 1 — Always direct: retire the offscreen default path

**Goal:** `FrameRenderPass::begin` always records onto the native swap
chain. The `effectsQueued` parameter, the `renderingToNative_` flag, and
the offscreen branch in `end()` all retire. `commit()` simplifies to
always-just-present.

### 1.1 Remove the offscreen branch from `FrameRenderPass`

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/RenderPass.h` | `FrameRenderPass::begin(...)` drops the `bool effectsQueued` parameter. Remove `renderingToNative_`, `clearRenderingToNative()`, `renderingToNative()`. The `nativeTarget_` reference is now the only valid target the pass ever uses. |
| `wtk/src/Composition/backend/RenderPass.cpp` `begin` | Always: `frameCB_ = nativeTarget_->commandBuffer();`. Drop the `preEffectTarget` else-branch and the early-return-when-preEffectTarget-null guard. |
| `wtk/src/Composition/backend/RenderPass.cpp` `end` | Always: `nativeTarget_->submitCommandBuffer(frameCB_);`. |
| `wtk/src/Composition/backend/RenderPass.cpp` `clearOnce` | Either retire (no caller after this phase) or redirect onto the native target. Audit callers; today only `BackendRenderTargetContext::clear` calls it. |
| `wtk/src/Composition/backend/RenderPass.cpp` `beginDraw` standalone fallback | Audit. After Phase 1 the only caller path is `BackendRenderTargetContext::renderToTarget` after a successful `beginFrame`. If the standalone path is unreachable, delete it; the texture-fence handling stays but only needs the in-frame branch. |

### 1.2 Slim `commit()`

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/RenderTarget.cpp` `commit(...)` | Replace the body with: complete the present, fire the completion handler with `Completed` status. Drop the entire `canApplyEffects` / `submitCommandBuffer(_l_cb, fence)` / `imageProcessor->applyEffects(...)` / `presentBlit` block. The `Dropped` early-return when `preEffectTarget == nullptr` becomes unreachable; remove. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `applyEffectToTarget` | Stop pushing into `effectQueue`. Phase 2 reshapes the API; until then this becomes a no-op stub or compile error to flush out callers. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `beginFrame` | Drop the `!effectQueue.empty()` argument; just call `frameRenderPass_.begin(clearR, clearG, clearB, clearA)`. |

### 1.3 Stop allocating the offscreen pair

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/Texture.cpp` `BackingTextureSet::rebuild` | Stop calling `texturePool()->acquire(poolKey)` for `targetTexture_` / `effectTexture_`. Stop creating `preEffectTarget_` / `effectTarget_`. The `tessellationEngineContext_` creation moves to Phase 6 (it currently depends on `preEffectTarget_`); for Phase 1, keep tessellation working by having it construct from a temporary or by routing through the native target. |
| `wtk/src/Composition/backend/Texture.cpp` `~BackingTextureSet` | The pool-release dance retires for `targetTexture_` / `effectTexture_`. Keep `nativeTarget_` shared handle reset. |

### 1.4 Verification

- All four compositor tests render the same shapes at the same positions.
  Pixels may differ on DX/VK where the swap chain format ≠ BGRA8Unorm
  (this is a correctness improvement; new pixels become the baseline).
- `OMEGAWTK_TRACE_RENDER=1` shows zero `[WTK Diag] commit: applyEffects`
  lines. Direct path traces only.
- `ResourceTrace` `Create`/`Destroy` events for `TextureTarget` still fire
  per `BackendRenderTargetContext` (the trace ID and label are
  context-bound, not texture-bound — they survive the texture-pair
  retirement).
- No frame should hold the offscreen texture pair. Confirm via memory
  inspection on a build with one window and no blur layers.

---

## Phase 2 — Per-layer blur scratch

**Goal:** Move blur from a per-context queue to a per-layer property.
Layers that have a blur effect get a scratch render target sized to the
layer's bounds. The layer's primitives render into the scratch instead of
the drawable; the blur compute pass runs on the scratch; a textured-quad
draw composites the result onto the drawable at the layer's position.

### 2.1 Express blur as a layer property

| File | Change |
|------|--------|
| `wtk/include/omegaWTK/Composition/Layer.h` | Add `OmegaCommon::Vector<CanvasEffect> blurEffects;` (or a dedicated `LayerBlur` struct holding radius / direction / angle). The list is set by client code at layer-construction or via a new `Layer::setBlur(CanvasEffect)`. |
| `wtk/src/Composition/Layer.cpp` | Trivial accessors + a `bool hasBlur() const { return !blurEffects.empty(); }`. |
| `wtk/src/Composition/Compositor.cpp` (the layer-tree walker that issues compositor commands) | When traversing a layer with blur, emit a "render layer to scratch + composite" command pair instead of recording its primitives directly into the frame's command buffer. |

### 2.2 Per-layer scratch surface

| File | Change |
|------|--------|
| New: `wtk/src/Composition/backend/BlurScratch.{h,cpp}` | Declare `class LayerBlurScratch`. Owns: `SharedHandle<GETexture> source` (rendered into), `SharedHandle<GETexture> pingPong` (effect output), `SharedHandle<GETextureRenderTarget> sourceTarget`, `SharedHandle<GEFence> fence`. Constructed from the layer's bounds rect; rebuild on bounds change. Pool acquisition via `BackendResourceFactory::texturePool()`. |
| `wtk/src/Composition/backend/Effect.h` | The `BackendCanvasEffectProcessor::applyEffects` signature is unchanged. Inputs become "the scratch's source + pingPong, the layer's effects, the layer's bounds dimensions" instead of "the context's preEffectTarget + effectTexture + the global queue + the backing dimensions." |

### 2.3 Reshape the rendering loop

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/RenderTarget.cpp` `renderToTarget` (or its compositor-thread caller) | When entering a blurred layer: redirect `frameRenderPass_.beginDraw(...)` so its `cb` is the scratch's `sourceTarget->commandBuffer()` for the duration of the layer. When exiting the blurred layer: run `imageProcessor->applyEffects(scratch.pingPong, scratch.sourceTarget, layer.blurEffects, layer.bounds.w, layer.bounds.h)`, then issue a textured-quad draw onto `frameCB_` at the layer's bounds rect, sampling the blurred result. |
| `wtk/src/Composition/backend/RenderPass.cpp` `beginDraw` | Add an optional override target (`SharedHandle<GETextureRenderTarget> *`) so the draw scope can be redirected for blurred layers. The default (null) keeps recording onto `frameCB_`. |

The textured-quad composite reuses the existing **texture render
pipeline** (`PipelineRegistry::texture()`) — no separate "final copy"
pipeline. Dynamically positioned at layer bounds; no fullscreen quad.

### 2.4 Verification

- Construct a synthetic test that places a 200×200 colored rect inside a
  layer with `CanvasEffect::GaussianBlur { radius = 8 }`. Confirm the
  blur extent matches the layer's bounds (not the frame viewport).
- Confirm only the blurred layer allocates a scratch via `ResourceTrace`.
- A frame with N non-blurred layers + 1 blurred layer holds exactly one
  scratch surface in memory.

---

## Phase 3 — Per-object effects (drop-shadow, 3D transform, opacity)

These are largely already correct. This phase audits them and confirms
no leakage from the retired offscreen path remains.

### 3.1 Audit

| Effect | Current site | Action |
|--------|--------------|--------|
| 3D transform | `VisualCommand::SetTransform` → `currentTransform`, applied per-vertex in `writeColorVertexToBuffer` / `writeTexVertexToBuffer` | Already direct. No change. |
| Opacity | `VisualCommand::SetOpacity` → `currentOpacity`, applied per-vertex (alpha multiply) | Already direct. No change. |
| Drop-shadow (geometric) | `VisualCommand::Shadow` case in `renderToTarget`: offset+expand the shape, fill with shadow color | Already direct. Phase 6 upgrades this to true Gaussian falloff via SDF without changing the data model. |

### 3.2 Out of scope (this phase)

- True per-object Gaussian drop-shadow via per-shadow scratch. This is
  expensive for typical UI and unnecessary once Phase 6 lands — the SDF
  shader produces real Gaussian-ish falloff via `smoothstep` on the
  shadow distance for free.

---

## Phase 4 — Retire dead surface area

After Phases 1–3, large blocks of code are unreachable. Remove them.

### 4.1 Pipeline registry

| Field / method | Action |
|----------------|--------|
| `PipelineRegistry::finalCopy_` | Delete. |
| `PipelineRegistry::finalCopyByFormat_` | Delete. |
| `PipelineRegistry::finalCopyForFormat(PixelFormat)` | Delete. |
| `PipelineRegistry::fullscreenQuadBuffer_` | Delete. |
| `Pipeline.cpp::initialize` | Delete the fullscreen-quad-buffer authoring loop (the six `bufferWriter->writeFloat4 / writeFloat2` calls that populate the unit quad). Delete the copy-pipeline construction block. |
| `Pipeline.cpp::shutdown` | Drop `finalCopy_.reset();` etc. |
| `compositor.omegasl` | Delete the `copyVertex` / `copyFragment` shader functions. |

### 4.2 Texture set

| Field / method | Action |
|----------------|--------|
| `BackingTextureSet::targetTexture_` | Delete. |
| `BackingTextureSet::effectTexture_` | Delete. |
| `BackingTextureSet::preEffectTarget_` | Delete. |
| `BackingTextureSet::effectTarget_` | Delete. |
| `BackingTextureSet::releaseTexturesToPool` | Delete. |
| `BackingTextureSet::presentBlit` | Delete. |
| `BackingTextureSet::uploadGradientTexture` | Move to a free function in `Pipeline.cpp` (or a new `Gradient.cpp`) — it had no real coupling to the texture set; only used `nativeTarget_->commandBuffer()`. |
| `BackingTextureSet` itself | After the deletions, the class holds only `renderTargetSize_`, `renderScale_`, `backingWidth_`, `backingHeight_`, and `nativeTarget_`. Either: (a) collapse into `BackendRenderTargetContext` directly, or (b) rename to `RenderTargetSizing` and keep as a small value type. Recommend (a) — the extracted module loses its reason to exist. |
| `Texture.h` / `Texture.cpp` | After (a), delete both files. After (b), they shrink by ~70%. |

### 4.3 Render pass

| Field / method | Action |
|----------------|--------|
| `FrameRenderPass::renderingToNative_` | Already removed in 1.1. Confirm. |
| `FrameRenderPass::clearOnce` | Audit callers. If `BackendRenderTargetContext::clear` is the only one and it has no remaining users (the `beginFrame` clear color subsumes it), delete both. |
| `FrameRenderPass::DrawScope::standalone` | Audit. If the standalone fallback path was deleted in 1.1, drop the field. |
| `FrameRenderPass::beginDraw` standalone branch | Delete if confirmed unreachable. |
| `FrameRenderPass::endDraw` standalone branch | Delete if confirmed unreachable. |

### 4.4 Effect

| Field / method | Action |
|----------------|--------|
| `Effect.cpp::GPUCanvasEffectProcessor::applyEffects` fence-tail | The current `submitCommandBuffer(cb, fence); commit();` was for handing off to `BackingTextureSet::presentBlit`. Replace with: signal the scratch's own fence so the textured-quad composite onto the drawable waits for blur completion. |
| `BackendCanvasEffectProcessor::fence` | Audit ownership. The fence is currently per-context (acquired from `FencePool` in the context ctor). With per-layer scratch, the fence should live on the scratch, not on the effect processor. Move it to `LayerBlurScratch`; the processor is stateless. |

### 4.5 Render target

| Field / method | Action |
|----------------|--------|
| `BackendRenderTargetContext::imageProcessor` | Move to per-blurred-layer (`LayerBlurScratch`) or keep one factory-created instance shared across all blur work in the process. Recommend the latter — `GPUCanvasEffectProcessor` is stateless after the fence move in 4.4. |
| `BackendRenderTargetContext::effectQueue` | Delete. |
| `BackendRenderTargetContext::applyEffectToTarget` | Delete. Layer-level blur replaces it. |
| `BackendResourceFactory::createEffectProcessor` | Either remove (single shared processor) or keep for a future CPU fallback. Document the choice. |

### 4.6 Verification

- The bytes deleted in this phase should net out to several hundred lines
  removed; the additions in Phase 2 (~150 lines for `LayerBlurScratch`
  and the layer-blur orchestration in the compositor) keep the total
  net-negative.
- `compositor.omegasl` shrinks. Compile both the macOS and DX/VK builds
  after each shader edit; the OmegaSL compiler runs at engine init.
- `BackendResourceFactory::initializePools` still allocates the texture
  heap (now used by per-layer scratch) and the buffer / fence pools;
  heap size may be tunable down if profiling shows non-blur use is now
  buffer-only.

---

## Phase 5 — Post-direct verification

Before starting the SDF migration, confirm the compositor backend is
internally consistent on the always-direct path.

### 5.1 Smoke

- All compositor tests on macOS / Metal, Win32 / DX, Linux / VK.
- `BasicAppTest` wall-clock per frame on each platform — direct path
  should have no regression vs the pre-Phase-1 direct branch.
- Visual diff: any test that exercised the effect path historically
  (today: none in `wtk/tests/`) is replaced by the new layer-blur test
  introduced in Phase 2.4.

### 5.2 Color / format correctness

This is the headline correctness benefit and warrants a dedicated check.

- On Win32 with an `RGB10A2` swap chain, render a tall vertical gradient
  spanning [0, 1] luminance. Compare pixels to the pre-Phase-1 build.
  The direct-path build should preserve more of the gradient's precision
  in the swap chain because no `BGRA8Unorm` round-trip happened.
- On macOS with an HDR-capable swap chain (if available in the test
  harness), repeat with values > 1. Same expectation.

### 5.3 Memory

- A frame with 50 non-blurred layers should allocate zero offscreen
  textures. Inspect via `ResourceTrace` `Create / Texture` events.
- A frame with 5 blurred layers should allocate exactly 5 scratch
  surface pairs (or 5 single textures if the blur compute can use the
  pool's same-size scratch directly).

---

## Phase 6 — SDF rasterization for simple primitives

**Goal:** Rect, rounded rect, ellipse, and shadow stop tessellating. Each
becomes a 6-vertex quad covering the primitive's expanded bounds; a
single SDF fragment shader evaluates the signed distance and `smoothstep`s
the boundary for resolution-independent AA. Vector paths keep the
tessellation path but gain per-vertex edge-distance varyings so the
fragment shader smoothsteps the silhouette identically.

This phase is independent of Phases 1–5 in principle but dramatically
cheaper to land *after* the always-direct refactor because there is one
render path to retarget instead of two, and one pixel format to author
shaders for instead of negotiating between offscreen and swap chain.

### 6.1 SDF shader library

| File | Change |
|------|--------|
| New: `wtk/src/Composition/backend/shaders/sdf.omegasl` | Vertex function `sdfVertex`: pass the quad position through the current 4×4 transform, interpolate shape-local coordinates and shape parameters as varyings. Fragment function `sdfFragment`: read the shape-kind tag from a varying, compute the distance via the appropriate closed-form SDF (`sdRect`, `sdRoundedRect`, `sdEllipse`, `sdShadow`), `smoothstep(-1.f, 0.f, dist)` for fill coverage, multiply by the brush color and current opacity. Output color premultiplied. |
| `wtk/src/Composition/backend/shaders/sdf.omegasl` | Provide one uber-shader rather than per-shape variants. Branching on a small integer tag in the fragment shader is cheap on modern GPUs. Document the tag values: `0=Rect`, `1=RoundedRect`, `2=Ellipse`, `3=Shadow`. |

The closed-form SDFs are standard:

- Rect: `vec2 q = abs(p) - b; return length(max(q, 0)) + min(max(q.x, q.y), 0);`
- Rounded rect: `vec2 q = abs(p) - b + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;`
- Ellipse: classic 4-step Newton iteration on the ellipse equation; see
  Inigo Quilez's reference implementation. (Or a cheaper bounding-box
  approximation if profiling shows the iteration is expensive.)
- Shadow: same as the underlying shape (rect / rounded rect / ellipse)
  but with `smoothstep(-blurAmount, 0, dist)` for a Gaussian-ish falloff.
  This subsumes the current geometric drop-shadow's offset+expand
  behavior and *adds* real blur without an offscreen pass.

### 6.2 Pipeline registry

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/Pipeline.h` | Add `SharedHandle<OmegaGTE::GERenderPipelineState> sdf_;` and accessor `sdf() const`. |
| `wtk/src/Composition/backend/Pipeline.cpp` `initialize` | Compile `sdf.omegasl` alongside `compositor.omegasl` (or merge the file). Construct the SDF render pipeline state with the same `colorPixelFormats` as the swap chain (resolved per platform; on Phase 1 always-direct, this is the swap chain's native format). Vertex layout: position (`float4`), shape-local coord (`float2`), shape params (`float4` × N depending on which shape uses the most params; pad unused). |
| `wtk/src/Composition/backend/Pipeline.cpp` `shutdown` | `sdf_.reset();`. |

### 6.3 Replace the simple-primitive paths in `renderToTarget`

| `VisualCommand::Type` | Before | After |
|-----------------------|--------|-------|
| `Rect` | `TETriangulationParams::Rect` → tessellation → color/texture pipeline draw. Border (if any) emitted as a *separate* `VectorPath` visual built by `RectFrame`. | Emit a 6-vertex quad covering the rect with padding for AA + stroke half-width. Shape params: half-extents, `cornerRadius = 0`, `strokeWidth` (0 if no border), fill color, stroke color. Fragment shader's tag = `Rect`. **Single visual; no separate border path.** |
| `RoundedRect` | Same as Rect, with rounding params. Border emitted as a separate path via `RoundedRectFrame`. | Same quad pattern; tag = `RoundedRect`; shape params include `cornerRadius`. Border handled in the same draw call. **No separate border path.** |
| `Ellipse` | Manually-built triangle fan with up to 4096 triangles. Border emitted as a separate path via `EllipseFrame`. | 6-vertex quad covering the ellipse bounding box; tag = `Ellipse`; shape params include `(rx, ry)`. Border handled in the same draw call. **No separate border path.** |
| `Shadow` | Geometric expansion + tessellation, no blur | 6-vertex quad covering the shadow's expanded bounds; tag = `Shadow`; carry the underlying shape kind, its dimensions, and `blurAmount`. The fragment shader does `smoothstep(-blurAmount, 0, dist)`. |
| `Bitmap` | Tessellated quad with `texturePaint` bound at fragment slot 2 | Unchanged. Bitmaps are pixel data; SDF doesn't apply. |
| `VectorPath` | Dual-attachment tessellation (stroke + fill) via `TETriangulationParams::GraphicsPath2D` + flat color pipeline | See 6.4 — keeps the existing dual-attachment tessellation for fill + stroke; gains edge-smoothstep AA for resolution-independent edges. |
| `SetTransform` / `SetOpacity` | Per-context state, applied per-vertex in lambdas | Unchanged. The SDF vertex shader applies the same `currentTransform` / `currentOpacity` exactly as today's color pipeline does. |
| `Text` | DWrite / CT / Harfbuzz | Out of scope. Text rendering is a separate, large migration (typically to SDF glyph atlases following Valve's seminal paper). Not addressed in this plan. |

The SDF fragment shader for simple primitives evaluates the signed
distance once per pixel, then composes both coverage bands:

```
dist = signedDistance(p, shapeKind, halfExtents, cornerRadius);
fillCov   = smoothstep( 1.f, -1.f, dist);              // dist <= 0 → 1
strokeCov = smoothstep( 1.f,  0.f, abs(dist) - half) * stepInside;
result    = mix(fill, stroke, strokeCov) * fillCov + stroke * (strokeCov * (1 - fillCov));
```

(That's a sketch — the final blend math has to handle all three cases:
fill-only pixels, stroke-only pixels, and the AA transition zone where
both contribute. `strokeWidth = 0` collapses the stroke term to zero
and the result reduces to today's fill-only behavior.)

The SDF draws still go through `FrameRenderPass::beginDraw` / `endDraw`
and `bindTexturePipeline` is replaced by a new `bindSDFPipeline(scope)`
on `FrameRenderPass` (same suppression-and-rebind contract as the color
and texture binders).

### 6.4 Vector paths: keep dual-attachment tessellation, add edge smoothstep

Arbitrary curves don't have closed-form SDFs. Vector paths keep the
tessellation engine and the existing fill+stroke dual-attachment flow
that the engine already produces. The phase adds resolution-independent
AA via per-vertex edge-distance varyings.

The current behavior to preserve, from
[`renderToTarget` `VisualCommand::VectorPath` case][rt-vp-2]:

```
auto te_params = TETriangulationParams::GraphicsPath2D(
        path, strokeWidth, contour, fill);
te_params.addAttachment(Attachment::makeColor(strokeColor));
if (fill && fillBrush) te_params.addAttachment(Attachment::makeColor(fillColor));
```

The triangulator emits stroke triangles and fill triangles in one pass,
each vertex tagged with which attachment it belongs to. One draw call
rasterizes both. **Don't change this contract.**

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/shaders/sdf.omegasl` | Add a fragment function `pathFragment` that reads a `signed-edge-distance` varying interpolated across the triangle and `smoothstep(-1, 0, edgeDist)` for coverage. The fill/stroke color is selected by an `attachmentTag` varying that the vertex shader passes through unchanged from the per-vertex tag the triangulator already emits. Vertex function `pathVertex` passes both varyings through. |
| `wtk/src/Composition/backend/Pipeline.h` / `Pipeline.cpp` | Add `SharedHandle<...> path_;` for the path render pipeline. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::VectorPath` case | The tessellator output gains a per-vertex distance-to-silhouette `float`. If `OmegaGTE::TETriangulationResult` doesn't already carry this, extend the tessellator (one `float` per vertex) — or compute it CPU-side from the path's outline after triangulation. The per-vertex attachment tag is already produced by the engine; just thread it through to the vertex buffer authoring. |
| `wtk/src/Composition/backend/RenderTarget.cpp` vertex authoring lambdas | `writeColorVertexToBuffer` for the path case writes `(position, color, edgeDistance, attachmentTag)`. The fragment shader uses `attachmentTag` to pick fill or stroke color and `edgeDistance` for AA. |

[rt-vp-2]: ../src/Composition/backend/RenderTarget.cpp

### 6.5 Stop emitting separate border visuals from `Canvas`

This is the Canvas-API-side change that pairs with the SDF stroke band.

| File | Change |
|------|--------|
| `wtk/src/Composition/Canvas.cpp` `drawRect` | Pass the caller's `border` argument straight into the `VisualCommand::Data` constructor instead of `Core::Optional<Border>{}`. Delete the `if(border.has_value()) { auto frame = RectFrame(...); ... drawPath(*frame); }` branch. |
| `wtk/src/Composition/Canvas.cpp` `drawRoundedRect` | Same change. Delete the `RoundedRectFrame` branch. |
| `wtk/src/Composition/Canvas.cpp` `drawEllipse` | Same change. Delete the `EllipseFrame` branch. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::Rect` case | Read `_params.border`. If present, populate the SDF shape-params with `strokeWidth = border->width` and the stroke color from `border->brush`; otherwise `strokeWidth = 0`. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::RoundedRect` case | Same. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::Ellipse` case | Same. |
| `wtk/include/omegaWTK/Composition/Path.h` | `RectFrame` / `RoundedRectFrame` / `EllipseFrame` remain in the public API for clients that want a standalone outline path (independent of any rect / rrect / ellipse fill). Mark them as such in a doc comment; remove the implicit "we always call these for borders" coupling. |

After this sub-phase, "rect with border" produces **one** visual command
and **one** draw call. Alpha blending across the fill / stroke
transition becomes correct because both bands are evaluated in the same
fragment-shader invocation rather than as two separate draws issued
back-to-back.

The `Border::brush` field today is `Core::SharedPtr<Brush>`. Phase 6
supports `Brush::Type::Color` for the stroke side; gradient strokes on
simple shapes are a follow-up (the SDF fragment shader can sample a
gradient by computing the local stroke parameter from `dist /
strokeWidth`, but that's not a Phase 6 requirement).

### 6.6 Tessellation engine context lifecycle

Once Phase 6 lands, only `VisualCommand::VectorPath` calls
`textures_.tessellationContext()` (or whatever its post-Phase-4 home is).
`Rect` / `RoundedRect` / `Ellipse` / `Shadow` / `Bitmap` no longer need
the tessellation context. Audit:

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/Texture.cpp` `BackingTextureSet::rebuild` (or wherever it landed after Phase 4 collapse) | The tessellation context is now created lazily on first `VectorPath` draw rather than at rebuild time. For frames that contain no vector paths (the common case), no tessellation context exists. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `renderToTarget` | The early-out `if(... textures_.tessellationContext() == nullptr) return;` becomes path-specific rather than blanket. |

### 6.7 Verification

- Visual: `EllipsePathCompositorTest` produces a *crisper* ellipse under
  zoom and 3D transform than the tessellated baseline. The pixel diff is
  significant; the new pixels become the baseline.
- Visual: shadow tests show real blur falloff instead of a hard-edged
  expanded silhouette.
- Visual: a `drawRect` with a 1-pixel border at sub-pixel position
  shows a single, crisp band — no double-AA artifact from the previous
  fill-then-separate-path overlap.
- Performance: `BasicAppTest` per-frame wall-clock should improve on
  ellipse-heavy frames (the 4096-triangle fan is replaced by 2
  triangles) and on bordered-rect-heavy frames (one draw instead of
  two per bordered rect).
- Visual command count: a frame with N rects, all bordered, should
  produce N visual commands after Phase 6 (vs 2N before). Inspect via
  `currentVisuals.size()` on the canvas.
- Tessellation context is null on a frame with no vector paths. Confirm
  via a debug log line in `renderToTarget`. Crucially, a frame with
  bordered rects / rrects / ellipses but no actual `drawPath` calls
  should now have `tessellationContext()` null — it had triangulated
  border paths before.

---

## Out of Scope

- **Text rasterization.** Migrating glyph rendering to SDF atlases is a
  separate, large project. Phase 6 leaves `VisualCommand::Text` on the
  current platform-specific path (DWrite / Core Text / Harfbuzz).
- **Backdrop blur.** Blurring what is *behind* a layer (CSS
  `backdrop-filter`) requires reading the swap chain back into a scratch
  texture before the layer's own draws are recorded. Different mechanism
  from layer blur. Not addressed.
- **MSAA.** SDF rasterization makes hardware MSAA largely redundant for
  the simple-primitive path. Whether to enable MSAA on the swap chain
  for the path-tessellation case is a separate decision, driven by
  empirical AA quality of the edge-smoothstep approach.
- **Depth testing for 3D transforms.** Painter's algorithm (back-to-front
  draw order, set by the compositor) handles non-intersecting
  3D-transformed flat layers. If overlapping 3D-transformed geometry
  needs proper depth resolution, the swap chain gains a depth attachment
  — outside this plan.
- **CPU fallback for the SDF and effect paths.** The plan assumes
  compute pipelines (for blur) and a programmable fragment stage (for
  SDF) are available. Targets without one or both keep the current
  tessellation + offscreen path; that fallback is gated behind
  `BackendResourceFactory` runtime feature detection in a follow-up.

## Risks

- **Format negotiation in Phase 1.** The SDF / direct path renders into
  the swap chain's pixel format. Pipeline state objects must be created
  with the correct `colorPixelFormats` per platform (today the offscreen
  pipelines are hard-coded to `BGRA8Unorm`). Test on every backend; the
  Vulkan validation layer will reject mismatches loudly.
- **Premultiplied vs straight alpha.** Today's draws render into a
  BGRA8Unorm texture and copy to the swap chain via the copy pipeline.
  The copy may be doing implicit alpha conversion. Going direct, the
  draws need to emit alpha in the swap chain's expected convention
  (premultiplied is the safe default; verify on each platform).
- **Compositing order across layers.** Direct-to-drawable means the
  swap chain accumulates layer draws in the order the compositor issues
  them. Blurred layers' composite-quad draws must be issued at the right
  z-order relative to non-blurred layers around them. The compositor's
  layer-tree traversal already orders layers; confirm the per-layer
  blur scratch's composite step lands at the correct point in the
  command stream.
- **SDF ellipse at extreme aspect ratios.** Newton iteration for the
  exact ellipse SDF can be unstable for aspect ratios > ~50:1. Cap the
  iteration count and fall back to a bounding-box approximation
  (`length(p / r) - 1`, scaled) when iteration diverges.
- **`compositor.omegasl` source path resolution.** `Pipeline.cpp` already
  resolves the shader source path from the executable's location on each
  platform. The new `sdf.omegasl` (if separate from `compositor.omegasl`)
  needs the same resolution; mirror the existing logic. Or merge into
  the single existing file to avoid the path-resolution duplication.
- **Trace label drift.** `ResourceTrace::emit` events for `TextureTarget`
  fire from `BackendRenderTargetContext` ctor / dtor / rebuild. After
  Phase 4 collapses `BackingTextureSet`, the trace still fires from the
  same owner with the same label and payload. Audit at the end of each
  phase that touches resource lifecycle.
- **Border AA at the fill / stroke boundary.** The current "fill rect
  + tessellated border path" approach produces a thin band of
  double-AA at the fill-stroke transition because both draws are
  independently antialiased and then alpha-composited. The SDF
  evaluation in 6.3 / 6.5 fixes this — both bands are computed from
  one distance evaluation — but the *new* pixels at that boundary
  will differ from the baseline. Treat as a correctness improvement,
  not a regression. Worth a side-by-side screenshot in the PR
  description so reviewers see the before/after.
- **Stroke width = 0 vs no border.** `Path::setStroke` asserts
  `newStroke > 0`, and `Canvas::drawPath` treats stroke `0` as a fill
  request (see [`Canvas.cpp`][canvas-cpp-2]). The SDF data flow needs
  a clear convention: `Border` is `Core::Optional<Border>` so the
  *absence* of a border is `nullopt`, not `width = 0`. Make sure the
  shape-params attribute on the SDF draw uses `strokeWidth = 0` only
  when no border was specified — and that the fragment shader's
  stroke band collapses cleanly in that case (it does, since
  `abs(dist) < 0` is never true).

[canvas-cpp-2]: ../src/Composition/Canvas.cpp

## Sequencing Notes

Phases 1 → 5 are tightly coupled (each leaves the tree green for tests but
the always-direct path is incomplete until Phase 5 finishes). Phase 6 is
independent and can land months later if desired; it requires Phase 4 to
have shrunk the texture / pipeline surface so the SDF additions don't
collide with retired offscreen state.

Recommended order: 1 → 2 → 3 → 4 → 5 → 6.

Phases 1 and 2 should not be combined into one PR — Phase 1 alone is a
significant correctness change (HDR / format precision) and benefits from
landing under its own bisect-able commit. Phase 2 is the larger code
change; Phase 1 buys time to validate format correctness in production
before the layer-blur code lands.
