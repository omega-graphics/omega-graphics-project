# Composition Backend Modularization Plan

## Problem

`wtk/src/Composition/backend/RenderTarget.{h,cpp}` has grown into the de facto
home for the entire compositor backend. The .cpp file is ~1700 lines and mixes
concerns that have no business living together:

1. **Engine lifecycle** — `InitializeEngine`, `CleanupEngine`, shader-library
   load, fullscreen-quad bootstrap, resource pool construction.
2. **Pipeline cache** — static color/texture/copy render pipeline states, per-
   format copy pipeline cache, blur/gradient compute pipelines.
3. **Resource pools / heaps** — texture heap, buffer heap, `TexturePool`,
   `BufferPool`, `FencePool` globals, plus deferred-release plumbing.
4. **Backing surface lifecycle** — `targetTexture`/`effectTexture`,
   `preEffectTarget`/`effectTarget`, `rebuildBackingTarget`, the present-blit
   in `commit()`.
5. **Render pass control** — `beginFrame`, `endFrame`, `clear`, viewport /
   scissor, pipeline-kind tracking, `frameCB_` / `frameActive_` /
   `renderingToNative_` invariant, mid-pass restart for texture fences.
6. **Draw dispatch** — `renderToTarget` (~530 lines): tessellation, vertex
   buffer authoring, transform/opacity, vector path / shadow / ellipse /
   bitmap branches, draw recording.
7. **Effect processor** — `GPUCanvasEffectProcessor` (gaussian + directional
   blur) lives at the bottom of the same .cpp.
8. **Lookup store** — `RenderTargetStore` cleanup helpers.
9. **Helpers** — `getCompositorShaderSourcePath`, sanitizers, gradient texture
   helper, finalCopyPipelineForFormat.

Concrete pain points:
- One .cpp owns five unrelated invariants. A change to draw dispatch forces a
  recompile of pipeline init, effect processor, and store.
- Shared static globals (`shaderLibrary`, `renderPipelineState`,
  `texturePool`, …) are file-scoped, so anything that needs them has to live
  in this file or grow extern-style accessors. The effect processor reaches
  into the same file's globals (`gaussianBlurHPipelineState`, `bufferWriter`)
  for that reason.
- `BackendResourceFactory` already exists but only covers visual-tree and
  texture-target creation that must run on the main thread. GPU resource
  allocation that does **not** require the main thread (pipelines, heaps,
  pools, fence acquire/release, per-format pipeline cache, gradient texture
  creation, vertex buffer acquisition) is scattered across `RenderTarget.cpp`
  with no single owner.
- `BackendCanvasEffectProcessor` is declared in `RenderTarget.h` but the only
  concrete subclass (`GPUCanvasEffectProcessor`) is defined inline at the
  bottom of `RenderTarget.cpp`. Adding a new effect requires editing the same
  file as the render pass.

## Guiding Principles

- **One concern per translation unit.** `RenderTarget.cpp` becomes a thin
  coordinator that owns the `BackendRenderTargetContext` lifecycle and
  delegates render-pass, texture, pipeline, and effect concerns to dedicated
  TUs.
- **`BackendResourceFactory` owns every GPU allocation.** Pipelines,
  heaps, pools, fence acquire/release, vertex buffers, gradient textures,
  per-format copy pipelines — all flow through the factory. Callers get
  handles back; they don't reach for file-static globals. Main-thread
  marshalling stays in the factory for the resources that need it.
- **No behavior changes in this plan.** This is mechanical decomposition.
  The render path, fence ordering, present sequence, and effect math stay
  byte-identical. Verification at each phase is "all existing tests still
  pass and the visual output of `EllipsePathCompositorTest`,
  `TextCompositorTest`, `SVGViewRenderTest`, `VideoViewPlaybackTest` are
  unchanged."
- **Header surface stays stable as long as possible.** External callers
  (`Execution.cpp`, `Compositor.cpp`, `CompositorClient.cpp`,
  `CALayerTree.mm`, `DCVisualTree.cpp`, `VKLayerTree.cpp`) keep including
  `RenderTarget.h` until Phase 6 collapses the umbrella.

## Target File Layout

```
wtk/src/Composition/backend/
├── RenderTarget.{h,cpp}    # BackendRenderTargetContext, RenderTargetStore,
│                           #   ViewPresentTarget, BackendCompRenderTarget,
│                           #   BackendSubmissionTelemetry. Thin coordinator.
├── RenderPass.{h,cpp}      # FrameRenderPass: beginFrame/endFrame/clear,
│                           #   viewport/scissor, pipeline-kind tracking,
│                           #   mid-pass restart for texture fences.
├── Texture.{h,cpp}         # BackingTextureSet: targetTexture/effectTexture,
│                           #   preEffectTarget/effectTarget, rebuild,
│                           #   present-blit helper, gradient texture upload.
├── Pipeline.{h,cpp}        # PipelineRegistry: shader library load,
│                           #   color/texture/copy render pipelines,
│                           #   per-format copy cache, blur/gradient
│                           #   compute pipelines, fullscreen quad buffer.
├── Effect.{h,cpp}          # BackendCanvasEffectProcessor interface and
│                           #   GPUCanvasEffectProcessor (gaussian +
│                           #   directional blur).
├── ResourceFactory.{h,cpp} # All GPU allocation. Owns heaps, pools, fence
│                           #   acquire/release, pipeline registry handle,
│                           #   vertex buffer acquire, per-format pipeline
│                           #   lookup, gradient texture creation, plus the
│                           #   existing main-thread visual-tree helpers.
├── BufferPool.h            # (unchanged)
├── TexturePool.h           # (unchanged)
├── FencePool.h             # (unchanged)
├── VisualTree.h            # (unchanged)
├── GeometryConvert.h       # (unchanged)
├── ResourceTrace.h         # (unchanged)
├── MainThreadDispatch.h    # (unchanged)
└── shaders/, mtl/, dx/, vk/  # (unchanged)
```

`RenderTarget.h` keeps the same public surface (the types listed above),
but its implementation file shrinks to a few hundred lines that wire
`BackendRenderTargetContext` to `RenderPass`, `BackingTextureSet`, the
factory, and the effect processor.

---

## Phase 1 — Pipeline registry extraction

**Goal:** All pipeline state objects, shader library, fullscreen quad buffer,
and `getFinalCopyPipelineForFormat` move into a `PipelineRegistry` owned by
`BackendResourceFactory`. No file-static globals remain in `RenderTarget.cpp`
for pipelines.

### 1.1 Create `Pipeline.{h,cpp}`

| File | Change |
|------|--------|
| `Pipeline.h` (new) | Declare `class PipelineRegistry`. Public surface: `bool initialize();` `void shutdown();` `SharedHandle<OmegaGTE::GERenderPipelineState> color() const;` `SharedHandle<OmegaGTE::GERenderPipelineState> texture() const;` `SharedHandle<OmegaGTE::GERenderPipelineState> finalCopy() const;` `SharedHandle<OmegaGTE::GERenderPipelineState> finalCopyForFormat(OmegaGTE::PixelFormat);` `SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurH() const;` (V, directional, linearGradient analogous). `SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter() const;` `SharedHandle<OmegaGTE::GEBuffer> fullscreenQuadBuffer() const;` `SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary() const;`. |
| `Pipeline.cpp` (new) | Move `getCompositorShaderSourcePath`, `loadGlobalRenderAssets`, `destroyGlobalRenderAssets`, the static pipeline globals, `finalCopyPipelinesByFormat`, and `getFinalCopyPipelineForFormat` here as `PipelineRegistry` members. Storage becomes member fields, not file-statics. |

### 1.2 Hold the registry on the factory

| File | Change |
|------|--------|
| `ResourceFactory.h` | Add `PipelineRegistry pipelines_;` (or `std::unique_ptr<PipelineRegistry>` to keep the header from including `Pipeline.h`). Add `PipelineRegistry & pipelines() { return *pipelines_; }`. |
| `ResourceFactory.cpp` | Construct/destroy the registry in factory ctor/dtor. |
| `RenderTarget.cpp` `InitializeEngine` | Replace the `loadGlobalRenderAssets()` call with `factory().pipelines().initialize()`. |
| `RenderTarget.cpp` `CleanupEngine` | Replace `destroyGlobalRenderAssets()` with `factory().pipelines().shutdown()`. |

`factory()` returns the process-wide singleton factory introduced in 1.3.

### 1.3 Process-wide factory accessor

| File | Change |
|------|--------|
| `ResourceFactory.h` | Add `static BackendResourceFactory & instance();`. |
| `ResourceFactory.cpp` | Implement as a function-local static. The factory becomes the single owner of process-global compositor resources. |

### 1.4 Migrate call sites

| File | Change |
|------|--------|
| `RenderTarget.cpp` everywhere `renderPipelineState` / `textureRenderPipelineState` / `finalCopyRenderPipelineState` / blur / gradient / `bufferWriter` / `finalTextureDrawBuffer` / `getFinalCopyPipelineForFormat` is read | Replace with `factory().pipelines().color()` etc. The locals stay non-owning copies of the SharedHandle for the duration of the function. |

### Verification
- Build all platform targets (macOS/Metal, Win32/DX, Linux/VK).
- Run `EllipsePathCompositorTest`, `TextCompositorTest`,
  `SVGViewRenderTest`, `VideoViewPlaybackTest`, `MetalLayerTest` (Mac).
- Confirm shader compile path still resolves on each platform (logged as
  `[WTK Diag]` on failure).

---

## Phase 2 — Resource pool ownership on the factory

**Goal:** `texturePool`, `bufferPool`, `fencePool`, and the underlying heaps
move from file statics into `BackendResourceFactory`. All acquire/release
calls go through the factory.

### 2.1 Move pool ownership

| File | Change |
|------|--------|
| `ResourceFactory.h` | Add private members: `SharedHandle<OmegaGTE::GEHeap> textureHeap_, bufferHeap_; std::unique_ptr<TexturePool> texturePool_; std::unique_ptr<BufferPool> bufferPool_; std::unique_ptr<FencePool> fencePool_;`. Add accessors: `TexturePool * texturePool();` `BufferPool * bufferPool();` `FencePool * fencePool();` (all may return nullptr if pools never initialized — match current behavior). Add `bool initializePools();` and `void shutdownPools();`. |
| `ResourceFactory.cpp` | Implement `initializePools` / `shutdownPools` by lifting `createResourcePools` / `destroyResourcePools` out of `RenderTarget.cpp`. Keep the same heap sizes (`kTextureHeapSize = 64 MiB`, `kBufferHeapSize = 8 MiB`) as constants on `BackendResourceFactory`. |

### 2.2 Engine lifecycle delegates to factory

| File | Change |
|------|--------|
| `RenderTarget.cpp` `InitializeEngine` | Becomes: `factory().pipelines().initialize(); factory().initializePools();`. |
| `RenderTarget.cpp` `CleanupEngine` | Becomes: `factory().shutdownPools(); factory().pipelines().shutdown();`. |
| `RenderTarget.cpp` | Delete the file-static `texturePool` / `bufferPool` / `fencePool` / `textureHeap` / `bufferHeap` declarations and `createResourcePools` / `destroyResourcePools` definitions. |

### 2.3 Migrate consumers

| Site | Change |
|------|--------|
| `BackendRenderTargetContext` ctor (`fence(fencePool ? … : …)`) | `fence(factory().fencePool() ? factory().fencePool()->acquire() : gte.graphicsEngine->makeFence())`. |
| `BackendRenderTargetContext::rebuildBackingTarget` | Use `factory().texturePool()` for acquire. |
| `BackendRenderTargetContext::~BackendRenderTargetContext` | Same for release; same for `factory().bufferPool()` and `factory().fencePool()` releases. |
| `BackendRenderTargetContext::releaseDeferredBuffers` | `factory().bufferPool()->release(...)`. |
| `BackendRenderTargetContext::renderToTarget` (vertex buffer acquisition) | `factory().bufferPool()->acquire(requiredBytes, struct_size)`. |

### Verification
- All tests pass with no leak-related asserts in `ResourceTrace` output.
- Shutdown order on app exit: pools drain → pipelines shutdown → factory
  destruction. No use-after-free in `~BackendResourceFactory`.

---

## Phase 3 — Texture lifecycle into `Texture.{h,cpp}`

**Goal:** Backing texture pair, render-target wrappers, present-blit, and
gradient-texture upload move into a focused module. `BackendRenderTargetContext`
holds a `BackingTextureSet` instead of raw fields.

### 3.1 Create `Texture.{h,cpp}`

| File | Change |
|------|--------|
| `Texture.h` (new) | Declare `class BackingTextureSet`. Public: ctor takes `Composition::Rect`, `float renderScale`, `SharedHandle<OmegaGTE::GENativeRenderTarget>` (may be null for offscreen-only). Members: `targetTexture`, `effectTexture`, `preEffectTarget`, `effectTarget`, `backingWidth`, `backingHeight`, `renderTargetSize`, `renderScale`. Methods: `void rebuild();` `void resizeLogical(const Composition::Rect &);` `void growBackingForViewport(unsigned w, unsigned h);` `void presentBlit(SharedHandle<OmegaGTE::GENativeRenderTarget> &, SharedHandle<OmegaGTE::GEFence> &);` `SharedHandle<OmegaGTE::GETexture> resultTexture() const;` `void uploadGradientTexture(bool linear, Gradient &, OmegaGTE::GRect &, SharedHandle<OmegaGTE::GETexture> & dest);`. |
| `Texture.cpp` (new) | Move `rebuildBackingTarget` body, the helper sanitizers (`sanitizeRenderRect`, `sanitizeRenderScale`, `toBackingDimension`), the present-blit half of `commit` (the `renderTarget != nullptr` block that draws the fullscreen quad), and `createGradientTexture` here. Pool acquire/release goes through `factory().texturePool()`. |

### 3.2 Slim `BackendRenderTargetContext`

| File | Change |
|------|--------|
| `RenderTarget.h` | Replace `targetTexture` / `effectTexture` / `preEffectTarget` / `effectTarget` / `backingWidth` / `backingHeight` / `renderTargetSize` / `renderScale` fields with `BackingTextureSet textures_;`. Forward-declare `BackingTextureSet`. |
| `RenderTarget.cpp` | `setRenderTargetSize` becomes `textures_.resizeLogical(rect)`. `setViewportOverride` calls `textures_.growBackingForViewport(...)`. `getBackingWidth/Height` proxy to `textures_`. `commit()` present-blit branch becomes `textures_.presentBlit(renderTarget, fence)`. |

### 3.3 Effect processor takes the texture set

| File | Change |
|------|--------|
| `RenderTarget.cpp` `commit` (effect path) | `imageProcessor->applyEffects(textures_.effectTexture, textures_.preEffectTarget, effectQueue, textures_.backingWidth, textures_.backingHeight);` (signature of `applyEffects` is unchanged in this phase). |

### Verification
- `VideoViewPlaybackTest` (uses bitmap render path) renders identical frames.
- Resize traces (`OMEGAWTK_TRACE_RENDER=1`) show identical
  `rebuildBackingTarget` cadence.

---

## Phase 4 — Render pass into `RenderPass.{h,cpp}`

**Goal:** The frame-level render pass invariant (`frameCB_`, `frameActive_`,
`renderingToNative_`, `lastPipelineKind_`, viewport override application,
mid-pass restart for texture fences) moves into `FrameRenderPass`.
`BackendRenderTargetContext::renderToTarget` no longer touches these fields
directly.

### 4.1 Create `RenderPass.{h,cpp}`

| File | Change |
|------|--------|
| `RenderPass.h` (new) | Declare `class FrameRenderPass`. Holds a reference to a `BackingTextureSet` and to the `BackendRenderTargetContext`'s native target / fence. Public: `void begin(float r, float g, float b, float a, ViewportOverride);` `void end();` `void clearOnce(float r, float g, float b, float a);` `SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> commandBuffer();` `void restartForTextureFence(SharedHandle<OmegaGTE::GEFence> &);` `void bindColorPipeline();` `void bindTexturePipeline();` `bool active() const;` `bool renderingToNative() const;`. The `PipelineKind` enum, `lastPipelineKind_`, and the viewport/scissor recomputation move here. |
| `RenderPass.cpp` (new) | Lift `beginFrame`, `endFrame`, `clear`, the standalone-CB fallback inside `renderToTarget`, and the mid-pass `endRenderPass` / `notifyCommandBuffer` / re-`startRenderPass` block out of `RenderTarget.cpp`. |

### 4.2 Move the viewport-override struct

| File | Change |
|------|--------|
| `RenderPass.h` | Define `struct ViewportOverride { bool active; float offsetX, offsetY, width, height; };` (extracted from `BackendRenderTargetContext`). |
| `RenderTarget.h` | `BackendRenderTargetContext` exposes `setViewportOverride` / `clearViewportOverride` as before but stores the override on `frameRenderPass_`. |

### 4.3 Slim `renderToTarget`

| File | Change |
|------|--------|
| `RenderTarget.cpp` `renderToTarget` | The function keeps tessellation, vertex buffer authoring, and draw recording. Pipeline binding becomes `frameRenderPass_.bindColorPipeline()` / `bindTexturePipeline()`. The fallback "we're not inside a frame, start one ourselves" block becomes `frameRenderPass_.beginStandalone()` and is mechanical. The texture-fence restart becomes `frameRenderPass_.restartForTextureFence(textureFence)`. |

### Verification
- `EllipsePathCompositorTest` and `SVGViewRenderTest` produce identical
  frames pixel-for-pixel against pre-Phase 4 builds.
- `OMEGAWTK_TRACE_RENDER=1` shows identical begin/end pass and
  pipeline-rebind sequences.

---

## Phase 5 — Effects into `Effect.{h,cpp}`

**Goal:** `BackendCanvasEffectProcessor` interface and
`GPUCanvasEffectProcessor` implementation move out of
`RenderTarget.{h,cpp}` into their own TU. The factory creates them.

### 5.1 Move the interface and implementation

| File | Change |
|------|--------|
| `Effect.h` (new) | Move `BackendCanvasEffectProcessor` declaration here from `RenderTarget.h`. Keep the `Create(SharedHandle<OmegaGTE::GEFence> &)` factory function for backward compatibility, but route it through the factory in 5.3. |
| `Effect.cpp` (new) | Move the entire `GPUCanvasEffectProcessor` class (gaussian blur + directional blur paths) and `BackendCanvasEffectProcessor::Create` here from the bottom of `RenderTarget.cpp`. Replace direct references to `gaussianBlurHPipelineState`, `gaussianBlurVPipelineState`, `directionalBlurPipelineState`, and `bufferWriter` with `factory().pipelines().gaussianBlurH()` etc. |

### 5.2 Header surface

| File | Change |
|------|--------|
| `RenderTarget.h` | Remove the inline `BackendCanvasEffectProcessor` declaration. Add `#include "Effect.h"`. |
| `RenderTarget.cpp` | Remove the `GPUCanvasEffectProcessor` definition and the `BackendCanvasEffectProcessor::Create` impl. |

### 5.3 Factory creates effect processors

| File | Change |
|------|--------|
| `ResourceFactory.h` | Add `SharedHandle<BackendCanvasEffectProcessor> createEffectProcessor(SharedHandle<OmegaGTE::GEFence> & fence);`. |
| `ResourceFactory.cpp` | Implement by calling `BackendCanvasEffectProcessor::Create(fence)`. The free function stays (so existing call sites work) but `BackendRenderTargetContext` now uses the factory accessor instead. |
| `RenderTarget.cpp` `BackendRenderTargetContext` ctor | `imageProcessor = factory().createEffectProcessor(fence);`. |

### Verification
- Any test that exercises blur or canvas effects renders the same output.
  Inspect the `CanvasEffect` paths in `EllipsePathCompositorTest` / any
  test that calls `applyEffectToTarget`.
- Symbol survey: no other TU referenced `GPUCanvasEffectProcessor` by name
  before Phase 5; if any does, fail loud and add a typedef in `Effect.h`.

---

## Phase 6 — Coordinator cleanup and header collapse

**Goal:** `RenderTarget.cpp` is reduced to: `BackendRenderTargetContext`
ctor/dtor, `setRenderTargetSize`, viewport override delegation, `commit`
coordinator (calls into `RenderPass`, `BackingTextureSet`, effect processor),
`renderToTarget` (which now reads more like a switch over visual command
types), `releaseDeferredBuffers`, and `RenderTargetStore`. Target size is
roughly 500 lines.

### 6.1 Final inventory

| File | Expected size after Phase 6 | Owns |
|------|----------------------------|------|
| `RenderTarget.cpp` | ~500 lines | `BackendRenderTargetContext` lifecycle, draw dispatch, `RenderTargetStore`, `InitializeEngine`/`CleanupEngine` thunks. |
| `RenderTarget.h` | unchanged public surface | Forward declarations for `BackingTextureSet`, `FrameRenderPass`. |
| `Pipeline.{h,cpp}` | ~300 lines | `PipelineRegistry`. |
| `Texture.{h,cpp}` | ~280 lines | `BackingTextureSet`. |
| `RenderPass.{h,cpp}` | ~220 lines | `FrameRenderPass`. |
| `Effect.{h,cpp}` | ~250 lines | `BackendCanvasEffectProcessor`, `GPUCanvasEffectProcessor`. |
| `ResourceFactory.{h,cpp}` | ~250 lines | All GPU allocation, factory singleton, pools, pipeline registry handle. |

### 6.2 Header dependency tightening

| File | Change |
|------|--------|
| `RenderTarget.h` | Stop including `Canvas.h` if no longer needed at the header level (move forward decls into `RenderPass.h` / `Texture.h`). |
| `Pipeline.h`, `Texture.h`, `RenderPass.h`, `Effect.h` | Forward-declare GTE types where possible to keep build times bounded. Use `OmegaGTE/GERenderTarget.h` only in the `.cpp`. |

### 6.3 CMake

`wtk/CMakeLists.txt:388` already uses `file(GLOB COMPOSITION_SRCS …)` for
`backend/*.cpp`, so the new `Pipeline.cpp`, `Texture.cpp`, `RenderPass.cpp`,
and `Effect.cpp` are picked up automatically. No CMake edit is required.
Confirm regeneration after each phase (`cmake --build` reruns glob).

### 6.4 Doc and trace pass

| File | Change |
|------|--------|
| `RenderTarget.h` | Top-of-file comment listing the modules the context delegates to. |
| `Pipeline.h`, `Texture.h`, `RenderPass.h`, `Effect.h` | One-paragraph header comment stating the single concern the file owns. |
| `ResourceTrace` events | Verify `Create` / `Destroy` events for `TextureTarget`, `BackendVisual` still fire from the new locations with the same labels — downstream telemetry tooling depends on the label strings. |

### Verification
- All compositor tests pass on Metal, DX, and Vulkan.
- Build wall-clock for a single-file edit in `RenderTarget.cpp` should drop
  measurably (this file used to drag the rest of the backend with it).
- `git log --stat` per phase should show changes scoped to the phase's
  files; phases that bleed across modules are a sign the boundary is
  wrong and must be revisited before continuing.

---

## Out of Scope

- Render correctness changes, including the direct-to-drawable / effect
  blit decision in `commit()`, fence ordering, or any tessellation-side
  behavior. These remain identical.
- Effect catalog expansion. Adding new effect types (color matrix, drop
  shadow, etc.) is a follow-up and now lands in `Effect.cpp` instead of
  `RenderTarget.cpp`.
- Removing `INTERFACE` / `INTERFACE_METHOD` macros or any other style
  cleanup unrelated to module boundaries.
- Backend-platform files under `backend/mtl`, `backend/dx`, `backend/vk`.
  They already live in their own subdirectories.

## Risks

- **Hidden ordering between globals.** `loadGlobalRenderAssets` populates
  `bufferWriter` *before* anything else; `renderToTarget` and the effect
  processor both assume it's non-null. The pipeline registry must
  preserve initialization order and the "shaderLibrary null → fail"
  short-circuit (`RenderTarget.cpp:215` and `:423`).
- **Platform-specific `_WIN32` blocks** in `BackendRenderTargetContext`
  (`waitForGPU`, `resizeSwapChain`, the dtor pre-effect target wait).
  These must move with the right module — `resizeSwapChain` and
  `waitForGPU` stay on `BackendRenderTargetContext` (they wrap
  `renderTarget`); the dtor's pre-pool-release `waitForGPU` block moves
  with the texture lifecycle into `BackingTextureSet::~BackingTextureSet`.
- **Trace label drift.** `ResourceTrace::emit("Create","TextureTarget",…)`
  and `…,"BackendVisual",…` strings are consumed by external tooling;
  they must continue to fire with the same payloads even when the
  emitting code moves files. Audit at the end of each phase.

## Sequencing Notes

Phases 1–5 are independently mergeable; each leaves the tree green. Phase 6
is the cleanup pass. Recommended order: 1 → 2 → 3 → 4 → 5 → 6. Phases 3 and
4 can technically swap, but render-pass lift is easier once the texture
set is already a clean object, so Texture-first is preferred.
