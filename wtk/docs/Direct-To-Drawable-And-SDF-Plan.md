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

## Phase 1 — Always direct: retire the offscreen default path [DONE]

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

## Phase 2 — Per-layer blur scratch [DONE]

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

## Phase 3 — Per-object effects (drop-shadow, 3D transform, opacity) [DONE]

These are largely already correct. This phase audits them and confirms
no leakage from the retired offscreen path remains.

### 3.1 Audit

| Effect | Current site | Audit finding |
|--------|--------------|---------------|
| 3D transform | `VisualCommand::SetTransform` → `currentTransform`, applied per-vertex in `writeColorVertexToBuffer` / `writeTexVertexToBuffer` | Already direct. No change. |
| Opacity | `VisualCommand::SetOpacity` → `currentOpacity`, applied per-vertex (alpha multiply) | Direct on the color path; **was silently dropped on the textured path** (gradient brushes / bitmaps ignored opacity). Phase 3 fix: the texture vertex's trailing two floats now carry `(currentOpacity, _)` — repackaged on the GPU side as the high half of a `float4 texCoordTint` varying, and the texture/copy fragment shaders multiply the sampled alpha by it. Buffer stride is unchanged. |
| Drop-shadow (geometric) | `VisualCommand::Shadow` case in `renderToTarget`: offset+expand the shape, fill with shadow color | Already direct. Geometry tessellates through the standard color path so transform + opacity already apply. Phase 6 upgrades this to true Gaussian falloff via SDF without changing the data model. |
| Per-slice state leakage | `currentTransform` / `currentOpacity` persisted across slice boundaries — a `SetTransform` / `SetOpacity` left dangling at the end of one slice's command stream silently bled into the next. | Phase 3 fix: `BackendRenderTargetContext::resetElementState()` is invoked at every slice boundary in `Compositor::renderCompositeFrame` (and once at frame begin), so each slice starts from identity / opaque. |
| Dead offscreen state (`effectQueue`, `preEffectTarget`, `effectTexture`, `effectTarget`) | Declared but no readers / writers on the live path. | Confirmed inert — Phase 4 deletes. |

### 3.2 Out of scope (this phase)

- True per-object Gaussian drop-shadow via per-shadow scratch. This is
  expensive for typical UI and unnecessary once Phase 6 lands — the SDF
  shader produces real Gaussian-ish falloff via `smoothstep` on the
  shadow distance for free.

---

## Phase 4 — Retire dead surface area [DONE]

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

**Status:** Phase 6.1, 6.2, 6.3, 6.5, and 6.8 landed in the SDF spine
commit. Remaining sub-phases — 6.4 (vector-path edge-distance AA),
6.6 (bitmap quad / sampler / tint / source-rect / nine-slice), 6.7
(MSDF text), and 6.9 (post-merge visual / perf verification) — are
follow-ups.

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

### 6.1 SDF shader library [DONE]

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

### 6.2 Pipeline registry [DONE]

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/Pipeline.h` | Add `SharedHandle<OmegaGTE::GERenderPipelineState> sdf_;` and accessor `sdf() const`. |
| `wtk/src/Composition/backend/Pipeline.cpp` `initialize` | Compile `sdf.omegasl` alongside `compositor.omegasl` (or merge the file). Construct the SDF render pipeline state with the same `colorPixelFormats` as the swap chain (resolved per platform; on Phase 1 always-direct, this is the swap chain's native format). Vertex layout: position (`float4`), shape-local coord (`float2`), shape params (`float4` × N depending on which shape uses the most params; pad unused). |
| `wtk/src/Composition/backend/Pipeline.cpp` `shutdown` | `sdf_.reset();`. |

### 6.3 Replace the simple-primitive paths in `renderToTarget` [DONE]

| `VisualCommand::Type` | Before | After |
|-----------------------|--------|-------|
| `Rect` | `TETriangulationParams::Rect` → tessellation → color/texture pipeline draw. Border (if any) emitted as a *separate* `VectorPath` visual built by `RectFrame`. | Emit a 6-vertex quad covering the rect with padding for AA + stroke half-width. Shape params: half-extents, `cornerRadius = 0`, `strokeWidth` (0 if no border), fill color, stroke color. Fragment shader's tag = `Rect`. **Single visual; no separate border path.** |
| `RoundedRect` | Same as Rect, with rounding params. Border emitted as a separate path via `RoundedRectFrame`. | Same quad pattern; tag = `RoundedRect`; shape params include `cornerRadius`. Border handled in the same draw call. **No separate border path.** |
| `Ellipse` | Manually-built triangle fan with up to 4096 triangles. Border emitted as a separate path via `EllipseFrame`. | 6-vertex quad covering the ellipse bounding box; tag = `Ellipse`; shape params include `(rx, ry)`. Border handled in the same draw call. **No separate border path.** |
| `Shadow` | Geometric expansion + tessellation, no blur | 6-vertex quad covering the shadow's expanded bounds; tag = `Shadow`; carry the underlying shape kind, its dimensions, and `blurAmount`. The fragment shader does `smoothstep(-blurAmount, 0, dist)`. |
| `Bitmap` | Tessellated quad through the GTE triangulator + texture pipeline at slot 2 | See 6.6 — hardcoded 6-vertex quad (no triangulator round-trip), high-quality sampler with mipmaps, optional tint / source-rect / nine-slice. |
| `VectorPath` | Dual-attachment tessellation (stroke + fill) via `TETriangulationParams::GraphicsPath2D` + flat color pipeline | See 6.4 — keeps the existing dual-attachment tessellation for fill + stroke; gains edge-smoothstep AA for resolution-independent edges. |
| `SetTransform` / `SetOpacity` | Per-context state, applied per-vertex in lambdas | Unchanged. The SDF vertex shader applies the same `currentTransform` / `currentOpacity` exactly as today's color pipeline does. |
| `Text` | Per-platform CPU rasterization (DWrite / Core Text / Harfbuzz + FreeType) → upload as `GETexture` → drawn as a single textured quad via `drawGETexture` | See 6.7 — per-font MSDF glyph atlas built lazily; one quad per glyph in the run; resolution-independent. Existing CPU-bitmap path stays as a fallback for fonts whose outlines can't be extracted. |

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

### 6.5 Stop emitting separate border visuals from `Canvas` [DONE]

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

### 6.6 Bitmap rendering improvements [DONE]
Sampler / orientation: investigated. Bitmaps rendered upside-down on all
three backends because OmegaCommon's PNG / JPEG / TIFF decoders deliver
pixel rows bottom-up (legacy GL convention) while GTE samplers treat
texture row 0 as the top. SDF rects and tessellated paths weren't
affected because their geometry doesn't carry a vertical orientation
signal. Fixed in `BitmapTextureCache::acquire` by row-flipping during
upload through the new GTE §4.5 region-aware `copyBytes` overload
(`gte/docs/Pipeline-Completion-Extension-Plan.md` §4.5): each source row
at `h - 1 - row` writes into destination row `row`. The codec
convention stays bottom-up so non-WTK consumers are unaffected; GTE
texture buffers end up correctly oriented for sampling.

Bitmaps still use a textured quad with sampling — the data is opaque
pixel data, not analytic geometry — but the current path has three
avoidable problems:

1. **Tessellation round-trip.** The `Bitmap` case in `renderToTarget`
   builds a `TETriangulationParams::Rect` and runs the GTE triangulator
   to produce two triangles for a unit quad. After Phase 6.3 the SDF
   primitives all use a hardcoded 6-vertex quad; the bitmap path
   should use the same quad authoring helper. No tessellation context,
   no per-frame round-trip.
2. **Default sampling, no mipmaps.** Whatever sampler the texture
   render pipeline binds today is the only sampler bitmaps see.
   Bitmaps drawn at small scale (icons under DPR=1) shimmer because
   there is no minification filter beyond bilinear; bitmaps under 3D
   transforms or zoom go soft because there is no anisotropic
   filtering or mipmap LOD. Both get worse once 3D transforms become
   routine (Phase 6 makes them crisper for shapes; bitmaps shouldn't
   regress relatively).
3. **No tint / sub-region / nine-slice.** Common UI patterns —
   recoloring an SVG icon, sampling a region of a sprite atlas, or
   stretching a button background's edges differently from its corners
   — currently require either pre-baking the texture or issuing
   multiple draws. The visual command grew the slot for some of these
   already (the `BitmapParamsData` struct in
   [`Canvas.h`][canvas-h-2]); they should land here.

[canvas-h-2]: ../include/omegaWTK/Composition/Canvas.h

#### 6.6.1 Quad authoring + sampler upgrade

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::Bitmap` case | Replace the `TETriangulationParams::Rect(gteBmpRect)` + `tessellationContext()->triangulateSync(...)` call with the same hardcoded 6-vertex quad helper used by the SDF primitives. Vertex attributes: `(position, texCoord)` (no shape kind, no SDF params). The triangulation context is no longer touched on Bitmap draws. |
| `wtk/src/Composition/backend/Pipeline.{h,cpp}` `texture` pipeline state | Construct with a sampler descriptor that requests linear minification, linear magnification, and (when supported) anisotropic filtering at a sane default (4×). Mipmap mode: `Linear` so trilinear sampling kicks in when the texture has mips. Wrap mode: `ClampToEdge` (the default — bitmaps draw bounded; nine-slice sub-regions specify their own UV ranges). |
| `wtk/src/Composition/Canvas.cpp` / wherever `BitmapImage → GETexture` upload happens | At texture upload time, if the texture is larger than 64×64 (heuristic — tunable), generate a mipmap chain. The GTE engine's `GETexture` likely has a `generateMipmaps` API; if not, that's a one-method addition on the `OmegaGTE::GraphicsEngine` interface. Pre-generated mipmaps cost roughly +33% memory but pay for themselves on the first minified or 3D-transformed draw. |
| `wtk/src/Composition/backend/Pipeline.cpp` `texture` pipeline state | When the swap chain format is sRGB-encoded (common on Win32 / Linux), the pipeline state's `colorPixelFormats` should be the sRGB variant so the GPU does linearize-on-sample / encode-on-write automatically. After Phase 1's always-direct path this is straightforward — the pipeline targets the swap chain format directly, no offscreen intermediate to break the convention. |

#### 6.6.2 Color tint and source-rect sampling

| File | Change |
|------|--------|
| `wtk/include/omegaWTK/Composition/Canvas.h` `BitmapParams` | Add optional fields: `Core::Optional<Composition::Color> tintColor;` and `Core::Optional<Composition::Rect> sourceRect;`. The source rect is in *texture pixel* coordinates (so a 256×256 atlas can be sampled with `{ 64, 0, 32, 32 }` for the second-row first icon). |
| `wtk/src/Composition/Canvas.cpp` `drawImage` overloads | Add overloads that accept tint and source rect. Existing overloads keep current behavior (no tint, full-texture UV). |
| `wtk/src/Composition/backend/shaders/compositor.omegasl` `textureFragment` (or a new `bitmapFragment`) | Sample the texture at the UV range derived from `sourceRect` (computed in the vertex shader from the bitmap's dimensions) and multiply by `tintColor.rgba` × `currentOpacity` before output. When `tintColor` is the identity color (`1,1,1,1`), the multiply collapses cleanly. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::Bitmap` case | Pass `tintColor` and the UV range as per-vertex attributes (or as a small uniform block per draw). |

#### 6.6.3 Nine-slice (resizable bitmap)

Lower priority — flag for inclusion or follow-up depending on consumer
demand.

| File | Change |
|------|--------|
| `wtk/include/omegaWTK/Composition/Canvas.h` `BitmapParams` | Add optional `Core::Optional<NineSliceInsets>` field — `{ left, top, right, bottom }` in texture pixels. When present, the bitmap renders as 9 quads (corners, edges, center) with the corners at fixed size and the edges / center stretched to fill the destination rect. |
| `wtk/src/Composition/Canvas.cpp` `drawImage` overload | Compute the 9 quad rects + UV ranges from the source-rect (or full texture) divided by the insets. Emit 9 `Bitmap` visuals (or one `Bitmap` visual with 9 quads worth of vertex data). |

Nine-slice doesn't require any backend shader work — each slice is just
a `Bitmap` draw with its own destination rect and source UV range,
which 6.6.2 already handles. The work is purely in
`Canvas::drawImage`'s rect math.

### 6.7 Text rendering: MSDF glyph atlases

**Implementation chunks (Linux-first vertical slice).** Section 6.7 is
landing in four chunks against the Pango/FT (Linux) backend. DWrite
and Core Text get stub `rasterizeGlyphMSDF` callbacks that return false
so those `Font`s stay on `BitmapFallback` (today's bitmap text path
keeps working unchanged on macOS / Windows) — they get their own
chunks once the Linux path is verified.

| Chunk | Status | What it lands |
|-------|--------|---------------|
| **6.7-c1** Plumbing | DONE | `Font::Mode` (`MSDF` / `BitmapFallback`) + forward-declared `GlyphAtlas` + out-of-line `Font` ctor/dtor with `unique_ptr<GlyphAtlas>` member; new `wtk/src/Composition/backend/GlyphAtlas.{h,cpp}` (skeleton: `lookup`, `ensureGlyph` stub returning false, `RasterizeFn` typedef); new `wtk/src/Composition/Font.cpp` for the out-of-line definitions; `VisualCommand::TextRun` enum value + `TextSubRun` struct + `textRunParams` Data field + ctors; `text_` render pipeline state in `PipelineRegistry` (alpha-over blend) + `bindTextPipeline` + `PipelineKind::Text`; magenta-stub `textVertex` / `msdfTextFragment` shader pair in `compositor.omegasl` (slots 12/13/14); no-op `TextRun` case in `renderToTarget`'s switch. **No behavior change** — `drawText` still uses today's bitmap path; nothing emits `TextRun`; the new pipeline is built but never bound. |
| **6.7-c2** Linux atlas | DONE | msdfgen added as an `AUTOMDEPS` git dep + `add_subdirectory` with `MSDFGEN_CORE_ONLY=ON`, linked privately into `OmegaWTK_Composition`; pangoft2 + freetype2 added to the Linux `pkg_check_modules` set; `GlyphAtlas` lazy-allocates a 1024×1024 `RGBA8Unorm` texture, runs a naive shelf packer, sub-rect-uploads via `GETexture::copyBytes(bytes, bpr, region)`; `HarfBuzzFont` holds a ref-counted resolved `PangoFont*` and exposes `setMode`; `HarfBuzzFontEngine::CreateFont` resolves the description via `pango_cairo_font_map_get_default()` + `pango_font_map_load_font`, locks the FT face for an `FT_FACE_FLAG_SCALABLE` probe, installs a `RasterizeFn` lambda (PangoFc lock → `FT_Set_Pixel_Sizes(0, descSize)` → `FT_Load_Glyph(NO_BITMAP \| NO_HINTING)` → `FT_Outline_Decompose` into a msdfgen `Shape` → `edgeColoringSimple(shape, 3.0)` → `generateMSDF` into `Bitmap<float,3>` → quantize-and-Y-flip into 8-bit RGB → unlock); flips to `Mode::MSDF` on success; `OMEGAWTK_TRACE_TEXT=1` env-gates trace logs at probe + `ensureGlyph` first-touch; smoke `ensureGlyph` for the `'A'` glyph runs at construction so any FT/msdfgen breakage surfaces at startup. **Verified live** (Helvetica/28 in `TextCompositorTest` traces `MSDF mode` + 32×32 tile at (0,0), advance≈20.2 px). Still no behavior change — no caller emits `TextRun` yet. |
| **6.7-c3** Render path | DONE | `GlyphRun` grew a `shape(rect, layoutDesc)` virtual returning a `ShapedTextRun` (`requiresFallback` + parallel `glyphIds` / `positions`); `HarfBuzzGlyphRun::shape()` builds a `PangoLayout` at the *unscaled* design size (DPR is applied downstream by the render viewport), walks `pango_layout_get_iter` → `pango_layout_iter_get_run_readonly` runs, family-compares each run's resolved face against the requested face (mismatch ⇒ `requiresFallback`, whole string to bitmap path — the chunk-3 single-sub-run contract), and emits `(glyphId, baseline-pen-position)` per visible glyph with the bitmap path's vertical-alignment offset folded in; `AtlasGlyph` gained `tileOriginX/tileOriginY/tileScale` (the padded `shape.bound` origin + tile-px-per-font-px) and the chunk-2 rasterize lambda now stores them; `Canvas::drawText` switches on `font->mode()` — `MSDF` shapes, pre-warms the atlas via the new out-of-line `Font::ensureGlyphsResident` (atlas uploads must run on the paint-recording thread, **not** inside the compositor frame render pass — doing it in the render path tripped a `vkCmdPipelineBarrier2` validation error), and emits a `TextRun` visual command; `RenderTarget::emitTextSubRun` authors one 6-vertex quad per *resident* glyph (lookup-only, silent-skip for absent glyphs) into a single shared vertex buffer + one draw call against `pipelines.text()` (`v_buffer_text` 12, `textParams` 13, `textAtlasTex` 14); `msdfTextFragment` swapped the magenta stub for `median(s.r,s.g,s.b)` → `fwidth` → `smoothstep(0.5±aa)` → `textColor × coverage` (per-element opacity pre-folded into `textColor.a`). **Verified live** (`TextCompositorTest`: `Helvetica/28` probes to MSDF, `shape()` traces 24/52-glyph runs with no fallback, all glyphs rasterize into the atlas, no Vulkan validation errors). Pixel-level crispness + DPR=2 no-re-rasterize still want a human visual check. |
| **6.7-c4** Fallback faces | TODO | `FontEngine::adoptResolvedFace(nativeHandle)` cache keyed by raw `PangoFont*` (DWrite `IDWriteFontFace*`, Core Text `CTFontRef` once those backends light up) so repeated fallback to the same substitute face shares one `Font` and one atlas across the process. `HarfBuzzGlyphRun::shape()` walks `PangoLayoutRun::item->analysis.font` per run, groups by resolved `PangoFont*`, surfaces one sub-run per group; the rasterize lambda installed by `adoptResolvedFace` runs the same FT-probe-and-MSDF path against the substitute face. `RenderTarget::TextRun` case iterates sub-runs and issues one draw per resolved atlas (no coalescing across faces). Fallback-vs-MSDF probe runs at adoption time too, so a Latin-MSDF primary face plus a color-emoji fallback face cleanly produces a `TextRun` with one MSDF sub-run plus one bitmap-fallback sub-run in the same string (the latter routes the per-sub-run draw through the existing `TextRect`/`drawGETexture` path against a per-glyph cached bitmap). |

After 6.7-c4 the Linux path matches §6.7.2 in full. The DWrite and
Core Text backends then each become their own chunk (replicating
6.7-c2 + 6.7-c3 surface area against `IDWriteFontFace::GetGlyphRunOutline`
and `CTFontCreatePathForGlyph` respectively); these are deliberately
sequenced after the Linux path is shown to behave under the visual
checks in §6.9 because the macOS / Windows builds can only be
verified outside the day-to-day Linux dev loop.

Please review important note in the following section!!

#### Chunk-2 lessons + open gotchas

These came out of landing 6.7-c2 and apply to chunks 3+:

- **Tile origin vs FT layout metrics.** `AtlasGlyph::bearingX/bearingY`
  in 6.7-c2 are the FT pen-bearing values (correct for *layout* — i.e.,
  where the glyph sits relative to the text baseline). They are *not*
  the offset from the tile's top-left into the glyph's silhouette inside
  the MSDF tile, which is what the chunk-3 quad authoring needs to
  position the tile correctly. Chunk 3 must extend `AtlasGlyph` to
  capture the `(l, b)` bound-box origin from `shape.bound(...)` (or
  switch to baking-in a fixed padding offset and restoring it at draw
  time) so quad authoring can compute `quadXY = penXY + tileOffset`
  cleanly.
- **Y-flip direction is unverified.** The chunk-2 quantizer reads
  `msdf(x, kMsdfTileSize - 1 - y)` so atlas-row-0 is the top of the
  glyph. This matches the standard image convention but has not been
  visually confirmed — chunk-3 verify is the first frame where the
  atlas is sampled, so an upside-down glyph there means the flip was
  reversed. Cheap to fix; flag if it bites.

  IMPORTANT NOTE: GTE sampler treats top as 0, so make sure to apply y-flip to all text engine backends. (Unless using existing bitmap cache which now automatically flips texture uploads, assuming they are bottom up acording to the GL standard)

- **Pango lock/unlock deprecation.** `pango_fc_font_lock_face` and
  `pango_fc_font_unlock_face` are deprecated as of Pango 1.44 in
  favor of `pango_font_get_hb_font`. The plan's per-platform notes
  call for the lock/unlock pair, and they still function on the
  reference Pango version, so chunk-2 keeps them and accepts the
  warning. A follow-up may switch to the HarfBuzz-fronted accessor
  once the Pango baseline allows it across all supported distros.
- **Scope of font fallback in chunk 3.** The original 6.7.2
  description treats multi-sub-run fallback as native to the chunk-3
  render path. The chunked breakdown defers per-face `adoptResolvedFace`
  to chunk 4, so chunk 3 ships a *single* sub-run per `TextRun` and
  routes any string whose layout requires fallback to the bitmap
  path entirely. This trades some chunk-3 visual richness for a
  smaller blast radius; chunk 4 then lights up the multi-atlas case
  end-to-end, including atlas adoption keyed by the platform handle.
- **Atlas eviction is out of scope.** The chunk-2 shelf packer is
  append-only — when a 1024×1024 atlas fills, `ensureGlyph` returns
  false and the affected draw silently skips the glyph. LRU paging
  is a Phase-6.7 follow-up the original plan acknowledges. Chunks
  3 and 4 should keep this contract (silent skip, no resize, no
  panic) so the eviction work can drop in without reshaping the
  draw path.



Today text follows this path (in [`Canvas::drawText`][canvas-cpp-3]):

1. `TextRect::Create(rect, layoutDesc, renderScale)` allocates a
   per-platform `TextRect` (DWrite / Core Text / Harfbuzz).
2. `GlyphRun::fromUStringAndFont(text, font)` shapes the unicode string.
3. `textRect->drawRun(glyphRun, color)` rasterizes glyphs to a
   CPU bitmap on the platform-specific backend.
4. `textRect->toBitmap()` uploads the bitmap as a `GETexture`.
5. `drawGETexture(bitmap.s, rect, bitmap.textureFence)` draws the
   rasterized text as a single textured quad via the `Bitmap` path.

Three problems:

1. **Resolution-dependent.** The bitmap is rasterized at one DPR; under
   zoom or 3D transform the text goes soft. After Phase 6 every other
   primitive is resolution-independent; text becomes the visibly
   weakest element on screen.
2. **Re-rasterized on DPR change.** Every render scale change (window
   moved between displays, zoom, etc.) re-rasterizes the text and
   re-uploads a new texture.
3. **One texture per text run.** Memory cost scales with how much
   distinct text is on screen. A list of 100 unique labels is 100
   textures, none of them shareable.

[canvas-cpp-3]: ../src/Composition/Canvas.cpp

The fix: per-font **MSDF glyph atlases**. Glyph outlines are extracted
from the platform font and rasterized into a multi-channel signed
distance field (RGB encodes 3 distance channels; the fragment shader
takes the median for fill coverage, which preserves sharp corners that
single-channel SDF rounds off — Chlumsky's msdfgen approach).

Per-platform outline extraction:

- **DX (Windows).** `IDWriteFontFace::GetGlyphRunOutline` into a custom
  `IDWriteGeometrySink` that records the contour for msdfgen. Direct
  path; the existing `DWriteFontEngine` already holds the font face.
- **Metal (macOS).** `CTFontCreatePathForGlyph` returns a `CGPathRef`;
  walk it via `CGPathApply` and translate the elements into msdfgen
  contours. Direct path; the existing `CTFontEngine` already holds the
  `CTFontRef`.
- **VK (Linux).** Despite the file name, the Linux backend
  ([`vk/HarfbuzzFontEngine.cpp`](../src/Composition/backend/vk/HarfbuzzFontEngine.cpp))
  is actually **Pango + Cairo + FontConfig**. `HarfBuzzFont` wraps a
  `PangoFontDescription`, not a HarfBuzz `hb_font_t`. To get to
  outlines we descend through PangoFc to FreeType:
  `PangoFontMap` (default Cairo font map) → `pango_context_load_font(desc)`
  → `pango_fc_font_lock_face(PANGO_FC_FONT(...))` → `FT_Face`. Then per
  glyph: `FT_Load_Glyph(face, glyphId, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)`
  → `FT_Outline_Decompose` with a callback set that emits msdfgen
  contour primitives. Release with `pango_fc_font_unlock_face` after
  the rasterization completes. The HarfBuzz/FreeType pair lives inside
  Pango; we don't need to construct an `hb_font_t` ourselves.

#### 6.7.1 Atlas

| File | Change |
|------|--------|
| New: `wtk/src/Composition/backend/GlyphAtlas.{h,cpp}` | Declare `class GlyphAtlas`. One atlas per `Font`. Lazy-populated: glyph IDs not yet present in the atlas trigger an MSDF rasterization on the *main thread* (atlas update has to coordinate with GPU sampling) and a sub-region update of the atlas texture. Fields: `SharedHandle<GETexture> texture`, `Map<glyphId, AtlasGlyph>` (each `AtlasGlyph` carries the UV rect and the metric offsets needed for layout). |
| `wtk/include/omegaWTK/Composition/FontEngine.h` `Font` | Add `GlyphAtlas & atlas()`. The atlas lives on the font; the platform `FontEngine` constructs the font with an empty atlas and MSDF rasterization function pointer (per-platform). |
| `wtk/src/Composition/backend/dx/DWriteFontEngine.cpp` | `DWriteFont::rasterizeGlyphMSDF`: `IDWriteFontFace::GetGlyphRunOutline` into an `IDWriteGeometrySink` adapter that emits msdfgen contour primitives; run msdfgen; copy the resulting RGB distance field into `outBuffer`. |
| `wtk/src/Composition/backend/mtl/CTFontEngine.mm` | `CTFont::rasterizeGlyphMSDF`: `CTFontCreatePathForGlyph` → `CGPathApply` to walk path elements → msdfgen contour primitives; run msdfgen; copy into `outBuffer`. |
| `wtk/src/Composition/backend/vk/HarfbuzzFontEngine.cpp` | `HarfBuzzFont::rasterizeGlyphMSDF`: load the `FT_Face` for this `PangoFontDescription` via the PangoFc descent above, `FT_Load_Glyph(... FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)`, `FT_Outline_Decompose` with a callback that emits msdfgen contour primitives; run msdfgen; `pango_fc_font_unlock_face`; copy into `outBuffer`. The `FT_Face` lock/unlock pair must wrap each rasterization; the face is shared with Pango's own rendering. |

Atlas size: start at 1024×1024 (room for ~1000 glyphs at 32×32 cells)
and grow / page out via LRU when full.

#### 6.7.2 Render path

| File | Change |
|------|--------|
| `wtk/include/omegaWTK/Composition/Canvas.h` | Add a new `VisualCommand::Type::TextRun` variant whose params carry **a list of sub-runs**, each `{ resolvedFont, glyphIds, perGlyphPositions }`. One sub-run per resolved face after the layout engine's font fallback (see note below). Keep `Text` as an alias / fallback for the bitmap path. |
| `wtk/src/Composition/Canvas.cpp` `drawText` | Replace the `TextRect → drawRun → toBitmap → drawGETexture` chain with: shape the run, then for each glyph emit a positioned quad against the *resolved* font's atlas. The visual command carries the per-sub-run glyph quads + the atlas texture handle for each. Shaping is per-platform — see the per-engine notes below; today's `GlyphRun` is a placeholder on macOS / Linux (it stores the string + font but doesn't produce glyph IDs), so each engine grows a `GlyphRun::shape()` that populates `(resolvedFont, glyphId, x, y, advance)` tuples before quad emission. |
| `wtk/src/Composition/backend/dx/DWriteFontEngine.cpp` `DWriteGlyphRun::shape` | Use DWrite shaping with fallback: `IDWriteTextAnalyzer::GetGlyphs` against the requested font; for clusters that map to `.notdef` (glyph 0), invoke `IDWriteFontFallback::MapCharacters` to resolve a substitute face, then re-shape that range against the substitute. Emit one sub-run per resolved face. Each sub-run's glyph IDs are valid only against its own face's atlas. |
| `wtk/src/Composition/backend/mtl/CTFontEngine.mm` `CTGlyphRun::shape` | Use `CTLine` / `CTRun`. Core Text already performs font fallback as part of line construction — `CTLineGetGlyphRuns` returns one `CTRun` per resolved face. Walk the array; per run, call `CTRunGetAttributes` → `kCTFontAttributeName` to recover the resolved `CTFontRef`, then `CTRunGetGlyphs` / `CTRunGetPositions` / `CTRunGetAdvances`. Emit one sub-run per `CTRun`. |
| `wtk/src/Composition/backend/vk/HarfbuzzFontEngine.cpp` `HarfBuzzGlyphRun::shape` | Lift shaping out of `drawRun`. Build the `PangoLayout` once (font + text), call `pango_layout_get_lines_readonly`, walk each `PangoLayoutLine::runs` → `PangoLayoutRun::glyphs` (a `PangoGlyphString`). Pango/PangoFc resolves fallback at run granularity already — `PangoLayoutRun::item->analysis.font` is the resolved `PangoFont` for that run's substring (which may differ from the layout's font description). Group runs by resolved `PangoFont` and surface one sub-run per group, each with its `(glyph, geometry.x_offset, geometry.y_offset, geometry.width)` tuples. The `geometry.width` is in Pango units (`/ PANGO_SCALE` for pixels). Existing layout-time properties (alignment, wrap, line limit) still apply — the layout produces correctly positioned glyphs, we just consume the result before Cairo would have rasterized it. |
| `wtk/src/Composition/backend/shaders/sdf.omegasl` | Add a fragment function `msdfTextFragment` that samples the MSDF, computes the median of the three channels, applies a screen-space derivative for AA width (`fwidth(median)`) and `smoothstep`s. Outputs the text color × coverage × `currentOpacity`. |
| `wtk/src/Composition/backend/Pipeline.{h,cpp}` | Add `text_` render pipeline state. The vertex layout is `(position, atlasUV)` per glyph quad; uniform / per-draw constants carry the text color. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `VisualCommand::TextRun` case | Iterate sub-runs; for each, bind the resolved font's atlas and emit one 6-vertex quad per glyph with the atlas UV rect from `subRun.resolvedFont->atlas().lookup(glyphId)`. **One draw call per sub-run, not per text run** — a string that mixes Latin and CJK against a Latin-primary font produces (typically) two draws. |

##### Font fallback and multi-atlas runs

The native layout engines remain authoritative for layout — line
breaking, BiDi, kerning, complex-script shaping, *and* font fallback.
A glyph that is missing in the requested face (e.g. CJK text against a
Latin-primary `Font`) is satisfied by the platform's fallback face,
not by any new logic in WTK. The MSDF path is downstream of that
substitution: each resolved face owns its own `GlyphAtlas`, keyed by
the platform's font handle (DWrite `IDWriteFontFace*`, Core Text
`CTFontRef`, Pango `PangoFont*`).

Implications:

- The `Font` factory must be able to materialize a WTK `Font` from a
  *platform-resolved* face that the layout engine produced
  internally, not just from an explicit `FontDescriptor`. Add a
  `FontEngine::adoptResolvedFace(nativeHandle)` returning a cached
  `SharedPtr<Font>`. The cache is keyed by native handle so repeated
  fallback to the same substitute face shares one `Font` and one
  atlas across the process.
- A `TextRun` visual command holds **one atlas-texture binding per
  sub-run**. The render path issues one draw per sub-run; sub-runs
  are not coalesced across faces because their atlas textures differ.
- Atlas-population is per resolved face, not per requested face.
  Cold-start cost of a Latin+CJK string against a Latin font is
  paid against the CJK fallback face's atlas the first time those
  glyphs appear, independent of whether anything else in the app has
  touched that fallback.
- The fallback-vs-MSDF detection in §6.7.4 runs on each resolved face
  at adoption time, not just on user-constructed fonts. A Latin-MSDF
  primary face plus a color-emoji fallback face cleanly produces a
  `TextRun` with one MSDF sub-run and one bitmap-fallback sub-run in
  the same string; the render path dispatches them to the
  appropriate pipelines.

#### 6.7.3 Bonus: free outlines, glow, drop-shadow

The MSDF shader's `dist > 0` is the band outside the glyph silhouette.
That gives outline / stroke for free (`smoothstep` around `dist =
outlineWidth`) and per-glyph drop-shadow at multiple offsets without
re-rasterization. Out of scope as a *required* deliverable but the
shader should be authored with the relevant uniforms in place so the
API surface can light it up.

#### 6.7.4 Fallback

For fonts whose outlines can't be extracted (rare on modern systems —
mostly bitmap-only fonts and color emoji fonts), the existing
`TextRect` → `drawGETexture` path stays as a fallback. The
`FontEngine::CreateFont(...)` factory checks at construction time
whether the platform face has extractable outlines and stamps the
choice on the `Font` for its lifetime. Per-platform detection:

- **DWrite.** `IDWriteFontFace1::IsMonochromatic` is *not* the right
  check (it reports glyph color, not vector availability). Probe with
  `IDWriteFontFace::GetGlyphRunOutline` against a representative glyph
  (e.g. 'A' or `.notdef`) and an empty geometry sink; if the call
  succeeds and the sink received any segments, outlines are
  extractable. Color fonts (COLR/CPAL, sbix) typically still expose
  vector outlines and thus stay on the MSDF path; bitmap-only fonts
  (`EBDT`/`EBLC` only) flunk the probe.
- **Core Text.** `CTFontCopyTraits` returns the symbolic traits dict;
  bitmap-only faces are vanishingly rare on macOS, but the safe check
  is `CTFontCreatePathForGlyph(...) != NULL` for a probe glyph. Apple
  Color Emoji fails this and routes to the fallback.
- **PangoFc / FreeType.** After locking the `FT_Face`, check
  `FT_HAS_COLOR(face)` and `(face->face_flags & FT_FACE_FLAG_SCALABLE)`.
  A face that has color tables but is also scalable (e.g. some COLRv1
  fonts) stays on MSDF; a face that lacks `FT_FACE_FLAG_SCALABLE`
  (bitmap-only — e.g. legacy CJK bitmap fonts, some emoji fonts on
  older systems) routes to the fallback. Unlock the face after the
  probe.

The two paths are mutually exclusive per font; the choice is logged
once at font construction (`MSDF` vs `BitmapFallback`).

### 6.8 Tessellation engine context lifecycle [DONE]

Once Phase 6 lands, only `VisualCommand::VectorPath` calls
`textures_.tessellationContext()` (or whatever its post-Phase-4 home is).
`Rect` / `RoundedRect` / `Ellipse` / `Shadow` / `Bitmap` / `TextRun` no
longer need the tessellation context. Audit:

| File | Change |
|------|--------|
| `wtk/src/Composition/backend/Texture.cpp` `BackingTextureSet::rebuild` (or wherever it landed after Phase 4 collapse) | The tessellation context is now created lazily on first `VectorPath` draw rather than at rebuild time. For frames that contain no vector paths (the common case), no tessellation context exists. |
| `wtk/src/Composition/backend/RenderTarget.cpp` `renderToTarget` | The early-out `if(... textures_.tessellationContext() == nullptr) return;` becomes path-specific rather than blanket. |

### 6.9 Verification

#### Simple primitives + paths

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

#### Bitmaps

- Visual: `VideoViewPlaybackTest` (or any bitmap-heavy test) at DPR=1
  with a 256×256 bitmap shrunk to 64×64 is *less shimmery* with mipmaps
  than without. Compare with `OMEGAWTK_TRACE_RENDER=1` to confirm
  trilinear sampling is engaged.
- Visual: bitmap drawn under a 3D rotation no longer aliases at the
  far edge — anisotropic filtering kicks in.
- Memory: a frame that draws the same bitmap to 4 different positions
  should hold *one* `GETexture`, not four. Confirm via
  `ResourceTrace::emit("Create", "Texture", ...)` event count.
- API: a `drawImage` with `tintColor = red` against a white sprite
  produces a red sprite — exactly the same pixels as the white sprite
  with the red channel masked.
- API: a `drawImage` with `sourceRect = { 64, 0, 32, 32 }` against a
  256×256 atlas samples that 32×32 region, scaled to the destination
  rect.

#### Text

- Visual: a `drawText` call rendered at DPR=1, then the window moved to
  a DPR=2 display, produces *crisp* text both times — no
  re-rasterization, no soft pixels mid-transition. The atlas is
  rasterized once at MSDF resolution and sampled at the display DPR.
- Visual: 100 unique labels render with one atlas texture — confirm
  via `ResourceTrace` that there is exactly one `Create / Texture`
  for the font's atlas, not 100.
- Visual: `TextCompositorTest` produces text whose pixel density is
  appropriate for the surrounding shape AA (post-Phase-6.3) — text
  should not stand out as "softer than everything else."
- Performance: cold-start glyph rasterization (first time a glyph is
  seen) is a one-time CPU cost. Steady-state rendering of repeated
  text uses the cached atlas entries.
- Fallback: a font with no extractable outlines (test with a bitmap-only
  emoji font on macOS) falls back to the existing `TextRect` →
  `drawGETexture` path. The fallback is logged once per font.

---

## Out of Scope

- **Color emoji and bitmap-only fonts.** MSDF works on vector outlines.
  Bitmap-only fonts (some legacy Asian fonts) and emoji fonts that
  embed pre-rendered color bitmaps (Apple Color Emoji, Google Noto
  Color Emoji) keep the existing CPU-bitmap text path. Detected per
  font at `Font` construction time; the choice is sticky for the
  font's lifetime.
- **Subpixel-RGB text rendering** (ClearType-style horizontal RGB
  triplets). MSDF text at DPR=1 on a non-HiDPI display *will* look
  slightly different from ClearType. Most modern UI compositors have
  abandoned subpixel-RGB rendering in favor of grayscale AA at higher
  DPR; we follow suit. If a downstream consumer needs ClearType
  semantics, that's a follow-up that re-introduces a third per-glyph
  rasterization mode.
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

  ALEX: This will be taken care of by SwiftShader fallback plan in OmegaGTE
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

  ALEX: See @UIView-Render-Redesign-Plan.md. This should be resolved in that plan.
- **SDF ellipse at extreme aspect ratios.** Newton iteration for the
  exact ellipse SDF can be unstable for aspect ratios > ~50:1. Cap the
  iteration count and fall back to a bounding-box approximation
  (`length(p / r) - 1`, scaled) when iteration diverges.
- **`compositor.omegasl` source path resolution.** `Pipeline.cpp` already
  resolves the shader source path from the executable's location on each
  platform. The new `sdf.omegasl` (if separate from `compositor.omegasl`)
  needs the same resolution; mirror the existing logic. Or merge into
  the single existing file to avoid the path-resolution duplication.

  ALEX: For now dup logic. We will want to use our Asset Compiler/Loader eventually.

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

- **Bitmap mipmap memory pressure.** Generating a mipmap chain for
  every uploaded bitmap costs ~+33% texture memory per bitmap. For
  apps with many large textures (thumbnail grids, video frames) this
  is significant. Make mipmap generation opt-out per upload, with a
  size threshold default (e.g., textures below 64×64 don't get mips —
  they fit in one cache line and minification doesn't matter).
- **sRGB pixel format selection.** When the swap chain reports an
  sRGB-encoded format, the texture render pipeline must be created
  with the matching sRGB `colorPixelFormats` so the GPU's
  encode-on-write does the right thing. Mismatched configurations
  produce washed-out colors (linear pixels written to an sRGB swap
  chain that re-applies sRGB encoding). Validate per-platform.
- **Premultiplied alpha for bitmaps with transparency.** Bitmap
  decoders may produce straight-alpha or premultiplied-alpha pixels
  depending on the codec and source format. Today's path doesn't
  document a convention. Pick one (premultiplied is the modern
  default), enforce it at upload, and document. Tinting math in
  6.6.2 assumes premultiplied input.
- **MSDF atlas thread safety.** Glyph rasterization happens on the
  thread that calls `drawText` (potentially the compositor thread),
  but the atlas texture's sub-region update has to coordinate with
  GPU sampling on previous frames. Atlas updates run through
  `BackendResourceFactory::runOnMainThread` (existing utility) so
  the GPU isn't sampling from a region under modification. Validate
  no race between "atlas needs new glyph" and "compositor already
  recorded a draw that samples it."
- **PangoFc `FT_Face` lock contention.** On Linux, `pango_fc_font_lock_face`
  serializes access to the underlying `FT_Face` against Pango's own
  rendering. Holding the lock for the full msdfgen pass (potentially
  milliseconds for complex glyphs) blocks any concurrent Pango
  rasterization on the same face. Mitigation: lock only across the
  `FT_Load_Glyph` + `FT_Outline_Decompose` window, copy the
  decomposed contours into a heap-owned msdfgen `Shape`, unlock, then
  run msdfgen on the copy. The lock window is then proportional to
  outline complexity, not distance-field rasterization cost.
- **HarfbuzzFontEngine.cpp file name is misleading.** The file
  implements Pango + Cairo + FontConfig; raw HarfBuzz is reached only
  transitively through Pango. Once MSDF lands, the actual HarfBuzz /
  FreeType usage is still indirect (PangoFc → `FT_Face`). Renaming to
  `PangoFontEngine.cpp` would reflect reality but is not part of this
  plan; flag it as a follow-up cleanup.
- **MSDF cell size vs visual quality trade-off.** Too small (16×16) and
  fine details disappear in the median-of-three encoding; too large
  (128×128) and the atlas runs out of room or the rasterization is
  expensive. Empirical sweet spot is 32–48 pixels per em. Tune per
  font on first use; allow override.
- **Bitmap font fallback path drift.** The fallback path (existing
  CPU-bitmap rasterization) shares almost no code with the MSDF path.
  Bug fixes to one don't propagate. Either (a) keep the fallback
  small and stable, ideally just for color emoji, or (b) consider
  dropping it once MSDF coverage is wide enough and color emoji has
  a dedicated bitmap-glyph path.

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
