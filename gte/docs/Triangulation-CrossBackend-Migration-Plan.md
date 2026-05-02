# Triangulation Cross-Backend Migration Plan

## Goal

Collapse the three near-duplicate `OmegaTriangulationEngineContext` backend implementations (`gte/src/d3d12/D3D12TEContext.cpp`, `gte/src/vulkan/VulkanTEContext.cpp`, `gte/src/metal/MetalTEContext.mm` — ~1200 LOC combined) into a single cross-backend implementation built on `OmegaGraphicsEngine`, `GECommandQueue`, and OmegaSL compute shaders.

The OmegaSL kernels already exist at `gte/src/shaders/triangulate_{rect,rounded_rect,ellipsoid,rect_prism,path2d}.omegasl`. They are not currently wired into any backend. The precedent for "compile OmegaSL → cross-backend pipeline" is `GED3D12Engine::ensureMipmapGenPipeline` at `gte/src/d3d12/GED3D12.cpp:533`.

A secondary motivation: `D3D12TEContext.cpp` is the only remaining D3D12 site with raw `CreateCommittedResource` after Phase 3 of the [D3D12MA Integration Plan](D3D12MA-Integration-Plan.md). Migrating TE to the cross-backend API closes that loop without threading `memAllocator` into a subsystem we are about to delete.

## Current State

Three parallel implementations of the same algorithm:

| Backend | File | Shader form | Resource alloc |
|---|---|---|---|
| D3D12 | `D3D12TEContext.cpp` | inline HLSL strings, runtime `D3DCompile` | `dev->CreateCommittedResource` (3x per call) |
| Vulkan | `VulkanTEContext.cpp` | inline GLSL/SPIR-V | `vkCreateBuffer` |
| Metal | `MetalTEContext.mm` | inline MSL strings | `newBufferWithLength` |

Each backend re-implements: kernel compilation, root signature / descriptor layout, buffer allocation (param / output / readback), command list / encoder setup, dispatch, fence wait, and CPU-side readback into `TETriangulationResult`.

`triangulateOnGPU` has **no current callers anywhere in the project**. The four primitive kinds the backends dispatch are `Rect`, `Ellipsoid`, `RectPrism`, and `Path2D`; `RoundedRect` falls through to CPU `triangulateSync` even though its OmegaSL shader exists. This means we can be aggressive: the migration is not constrained by API compatibility with existing callers, only by the public `OmegaTriangulationEngineContext` interface declared in `gte/include/omegaGTE/TE.h`.

## Design

### One unified TE context

A single `GETriangulationContext : public OmegaTriangulationEngineContext` lives in `gte/src/common/`. It depends only on the cross-backend `OmegaGraphicsEngine` API and owns:

- A reference to the engine.
- A "viewport source" — either a render target (for `getEffectiveViewport`) or a fixed viewport.
- A dedicated `SharedHandle<GECommandQueue>` for triangulation dispatches, so `commitToGPUAndWait` does not force-flush the user's render queue.

Each per-backend file (`D3D12TEContext.cpp` etc.) collapses into a trivial factory function that constructs the unified context. The public API in `omegaGTE/TE.h` is unchanged.

### Engine-level pipeline cache

Mirrors `ensureMipmapGenPipeline`. Add to `OmegaGraphicsEngine`:

```cpp
SharedHandle<GEComputePipelineState> ensureTriangulationPipeline(TriangulationKind kind);
```

Cached on the engine, lazily compiled on first use, shared by all TE contexts. Five entries (one per OmegaSL shader). Each shader is embedded as a string constant, same generated-header pattern `kMipmapGen2DOmegaSL` already uses.

### Triangulation dispatch shape

Per call, fully through GE.h:

1. Allocate `paramBuf` (`BufferDescriptor::Upload`) and `vertexBuf` (`BufferDescriptor::Readback`) via `engine->makeBuffer`. After Phase 2/3 of the D3D12MA plan, these go through D3D12MA / VMA automatically.
2. Memcpy params via `paramBuf` mapping (existing public path; backend-agnostic).
3. `getAvailableBuffer` → `startComputePass` → `setComputePipelineState` → `bindResourceAtComputeShader(paramBuf, 0)` → `bindResourceAtComputeShader(vertexBuf, 1)` → `dispatchThreadgroups(tc, 1, 1)` → `finishComputePass`.
4. `submitCommandBuffer` → `commitToGPUAndWait`.
5. Read `vertexBuf` via the existing `GEBuffer` mapping API and build `TETriangulationResult`.

The retention queue infrastructure (slices 1–4 of [GPU-Safe Resource Deletion](GPU-Safe-Resource-Deletion-Plan.md)) keeps the buffers alive until the GPU finishes, so the local `SharedHandle`s are safe to drop immediately after submit.

### What does *not* change

- Public surface in `gte/include/omegaGTE/TE.h`: `OmegaTriangulationEngineContext`, `TETriangulationParams`, `TETriangulationResult`, the factory methods on `OmegaTriangulationEngine`.
- The CPU-side `triangulateSync` path. It is the fallback for primitive types we don't ship a GPU kernel for (and remains the implementation of `translateCoords`, `_triangulatePriv`, etc.).
- `extractGPUTriangulationParams` — already cross-backend, gets reused as-is.

---

## Slices

### Slice 1 — Embed shaders and add engine-level pipeline cache

- Add a build step that generates `gte/src/shaders/triangulate_*.h` headers (string constants) for all five `.omegasl` files, mirroring how `kMipmapGen2DOmegaSL` is produced.
- Add `enum class TriangulationKind { Rect, RoundedRect, Ellipsoid, RectPrism, Path2D };` to a new common header.
- Add `SharedHandle<GEComputePipelineState> ensureTriangulationPipeline(TriangulationKind)` + cache slots to `OmegaGraphicsEngine`. Implementation per backend (~30 LOC each, mirroring `ensureMipmapGenPipeline`).

**Files**:
- `gte/src/shaders/CMakeLists.txt` (or wherever the mipmap header is generated) — add five new generated headers.
- `gte/include/omegaGTE/GE.h` — declare `TriangulationKind`, `ensureTriangulationPipeline`.
- `gte/src/d3d12/GED3D12.{h,cpp}`, `gte/src/vulkan/GEVulkan.{h,cpp}`, `gte/src/metal/GEMetal.{h,mm}` — implement.

**Tests**: none yet — pipelines are unused until Slice 2.

### Slice 2 — Unified `GETriangulationContext`

- New file `gte/src/common/GETriangulationContext.{h,cpp}`.
- `class GETriangulationContext : public OmegaTriangulationEngineContext` implementing `triangulateOnGPU` purely against GE.h.
- Two constructors: from a `GENativeRenderTarget` (calls render target's existing viewport-from-native-handle hook), and from a `GETextureRenderTarget` (uses the texture's descriptor for viewport).
- Owns its own `SharedHandle<GECommandQueue>` constructed via `engine->makeCommandQueue(2)`.
- `RoundedRect` is dispatched on GPU here (the shader exists; no backend currently uses it).

**Files**:
- `gte/src/common/GETriangulationContext.h` — new
- `gte/src/common/GETriangulationContext.cpp` — new

**Tests**: a small CPU-vs-GPU equivalence test for `Rect` and `Ellipsoid` — verify the GPU vertex output matches `triangulateSync` to within float epsilon. This is the first time these OmegaSL shaders are exercised end-to-end, so the test catches OmegaSL → backend codegen issues per kernel.

### Slice 3 — Switch factories

Each per-backend TE-context factory function reduces to:

```cpp
SharedHandle<OmegaTriangulationEngineContext>
CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget) {
    auto engine = /* upcast from renderTarget */;
    return std::make_shared<GETriangulationContext>(engine, renderTarget);
}
```

Same for `CreateTextureRenderTargetTEContext`. The platform-gated build entry points stay where they are; the heavy lifting moves out.

**Files**:
- `gte/src/d3d12/D3D12TEContext.cpp` — reduce to factory shells.
- `gte/src/vulkan/VulkanTEContext.cpp` — same.
- `gte/src/metal/MetalTEContext.mm` — same.

**Tests**: the Slice 2 equivalence test now runs on every backend the build target supports.

### Slice 4 — Delete the old code

Remove from each backend:
- Inline HLSL / GLSL / MSL string constants.
- Per-backend `TessPipelines` structs and `compileD3D12Kernel` / equivalents.
- Per-backend `dispatch` functions.
- The three raw `CreateCommittedResource` call sites in `D3D12TEContext.cpp` (the Phase 3 motivation).

Net reduction: ~1100 LOC removed across `D3D12TEContext.cpp`, `VulkanTEContext.cpp`, `MetalTEContext.mm`.

**Files**: same three as Slice 3, now down to a few dozen lines each.

---

## Decisions made

- **Aggressive migration, no compatibility shims.** No code currently calls `triangulateOnGPU`, so the migration is not constrained by existing callers. The legacy per-backend dispatch paths can be deleted outright in Slice 4 rather than kept behind a feature flag.
- **Wire up `RoundedRect` while we're here.** The shader exists; the dispatch code is ~10 lines on top of the unified path; skipping it would leave a known gap.
- **Dedicated command queue per TE context.** Sharing the user's render queue would mean `commitToGPUAndWait` flushes their pending render work, which is a subtle semantic regression. The cost of an extra queue is small.
- **Engine-virtual pipeline cache (three implementations) over a shared base helper.** Matches `ensureMipmapGenPipeline`; the alternative (lifting the cache into the base) requires also lifting `_loadShaderFromDesc` invocation patterns, which the engines deliberately keep private. Three ~30-line implementations is cheaper than the refactor.

## Risks

- **Per-backend OmegaSL codegen variance.** Each backend's runtime OmegaSL → SPIR-V / DXIL / AIR codegen path needs to handle these kernels. `ensureMipmapGenPipeline` exercises the path on D3D12; the triangulation kernels are the first cross-backend exercise. Slice 2's CPU/GPU equivalence test is the canary.
- **Inline-shader behavior drift.** The three current backends each pass viewport coords slightly differently (e.g. potential Y-flip on Metal). The OmegaSL shaders are presumably the canonical version, but it is worth diffing each inline shader against its OmegaSL counterpart before deletion in Slice 4 — any divergence is a behavior change.
- **Public down-casts.** If any external code (in this repo or downstream) down-casts `OmegaTriangulationEngineContext` to `D3D12NativeRenderTargetTEContext` or its peers, it will break in Slice 3. Grep before deletion.

## Future work

- **Async dispatch.** Today every backend's `triangulateOnGPU` does `commitToGPUAndWait` and returns a ready future, so it is not actually asynchronous. The unified context could expose true async via `submitCommandBuffer(buf, signalFence)` and a future that waits on the fence — but only if a real caller materializes that needs it.
- **Batched triangulation.** Several primitives in one dispatch via an indirect command buffer. Worth doing only if profiling shows the per-call submit cost dominates.
- **Indexed output.** Today the kernels emit duplicated vertices; an index-buffer-emitting variant would halve memory for `RectPrism` and similar topologies.
