# Pipeline State Caching Plan

## Goal

Avoid rebuilding GPU pipeline state objects (PSOs) that have already been
created — both *within a run* (return the existing object instead of
recompiling) and *across runs* (persist the driver-compiled blob to disk and
reload it, skipping the expensive shader-compilation step at startup). The user
ask: **"load pre-existing pipelines that already have been created."**

Pipeline creation is one of the most expensive operations in a graphics engine:
`makeRenderPipelineState` / `makeComputePipelineState` hand the shader to the
driver, which compiles backend bytecode (DXIL / `.metallib` / SPIR-V) into
GPU-specific machine code and links it against the fixed-function state. Doing
this every launch — and worse, redundantly within a launch — is pure waste.

## Current State

| Aspect | Today |
|---|---|
| Creation entry points | `OmegaGraphicsEngine::makeRenderPipelineState(RenderPipelineDescriptor&)`, `makeComputePipelineState(ComputePipelineDescriptor&)`, `makeBlitPipelineState(BlitPipelineDescriptor&)` (all `virtual`, per-backend) — `gte/include/omegaGTE/GE.h`. |
| Pipeline handle | Opaque `GERenderPipelineState` / `GEComputePipelineState` / `GEBlitPipelineState`; each backend's concrete struct holds the native PSO + the source `GTEShader`s. |
| Shaders | Loaded from a `.omegasllib` archive via `loadShaderLibrary` / `loadShaderLibraryRuntime`; each `GTEShader` carries the backend bytecode + the `omegasl_shader` layout. |
| Caching | **None.** Every `make*PipelineState` call recreates the native PSO from scratch. Two calls with identical descriptors build two independent PSOs. Nothing is persisted across runs. |
| Native cache primitives | **Available but unused.** `d3dx12.h` already exposes `CD3DX12_PIPELINE_STATE_STREAM_CACHED_PSO`; Metal has `MTLBinaryArchive`; Vulkan has `VkPipelineCache`. None are wired in. |

### What a cache must key on

A cached pipeline is only valid if *everything that fed its creation* is
unchanged. The cache key is the tuple:

1. **The descriptor**, in full — every field of `RenderPipelineDescriptor`
   (shaders, `colorPixelFormats`, blend descriptors, vertex input layout,
   depth/stencil, cull mode, sample count, topology category) or
   `ComputePipelineDescriptor`.
2. **The shader identity** — the `.omegasllib` content (a shader edit must miss
   the cache). Use a content hash of the library archive plus the entry name.
3. **The device/driver** (on-disk only) — native blobs are *not* portable
   across GPUs or driver versions.

## Non-Goals

- Caching shader *source* compilation (OmegaSL → backend source). That is the
  `omegaslc` / `.omegasllib` build step, already an offline artifact.
- A cross-vendor portable pipeline blob. Native caches are device/driver-local
  by construction; we persist per-machine and rebuild on miss.
- Hot pipeline *eviction* / LRU sizing. The working set of pipelines in this
  engine is small and long-lived; a plain map is sufficient until profiling
  says otherwise.

---

## Design Overview

Two independent layers, each useful on its own:

```
            make*PipelineState(desc)
                     │
        ┌────────────▼─────────────┐
        │ Layer 1: in-memory dedup │   key = hash(descriptor + shader id)
        │  hit → return existing    │   (backend-agnostic)
        └────────────┬─────────────┘
                     │ miss
        ┌────────────▼─────────────┐
        │ Layer 2: native on-disk   │   MTLBinaryArchive / VkPipelineCache /
        │  warm → driver skips       │   D3D12 cached-PSO / PipelineLibrary
        │  compile; cold → compile + │   (device/driver-local blob on disk)
        │  add to archive            │
        └────────────┬─────────────┘
                     │
              native PSO created
```

- **Layer 1 (in-memory dedup)** turns a redundant `make*` into a map lookup. It
  is fully backend-agnostic and the cheapest win. It does *not* survive a
  process restart.
- **Layer 2 (native on-disk archive)** is what makes a *restart* fast: the
  driver-compiled machine code is persisted and reloaded, so a cold pipeline
  still has to be *created* (the API object) but the driver skips the
  multi-millisecond compile. This is each backend's native mechanism, wrapped in
  one GTE object.

The two compose: Layer 1 avoids creating the API object at all on a repeat
request; Layer 2 makes the first creation of each pipeline cheap across runs.

---

## Proposed API

A new opaque cache object owns both layers. It is created from the engine,
optionally backed by a file, and passed to (or defaulted into) pipeline
creation.

**`gte/include/omegaGTE/GEPipeline.h`:**

```cpp
using GEPipelineCache = struct __GEPipelineCache;
```

**`OmegaGraphicsEngine` (`GE.h`):**

```cpp
/// @brief Create (or open) a pipeline-state cache. If @p path names an
/// existing cache file written by a previous run on this machine, its
/// driver-compiled blobs are loaded so subsequent make*PipelineState calls
/// skip recompilation. A missing/incompatible file starts empty — never an
/// error (the cache is an optimization, not a source of truth).
/// @param path Persisted cache file (sidecar to the .omegasllib), or empty
///             for an in-memory-only (Layer 1) cache.
virtual SharedHandle<GEPipelineCache> makePipelineCache(FS::Path path = {}) = 0;

/// @brief Flush the cache's accumulated driver blobs to its backing file.
/// Call at shutdown (or after a warm-up pass) to persist newly built
/// pipelines for the next run. No-op for an in-memory-only cache.
virtual void savePipelineCache(SharedHandle<GEPipelineCache> & cache) = 0;
```

Pipeline creation takes the cache as an optional trailing argument. This keeps
every existing call site source-compatible (the parameter defaults to "no
cache", which is today's behavior):

```cpp
virtual SharedHandle<GERenderPipelineState> makeRenderPipelineState(
    RenderPipelineDescriptor & desc,
    SharedHandle<GEPipelineCache> cache = nullptr) = 0;

virtual SharedHandle<GEComputePipelineState> makeComputePipelineState(
    ComputePipelineDescriptor & desc,
    SharedHandle<GEPipelineCache> cache = nullptr) = 0;

virtual SharedHandle<GEBlitPipelineState> makeBlitPipelineState(
    BlitPipelineDescriptor & desc,
    SharedHandle<GEPipelineCache> cache = nullptr) = 0;
```

> **Design choice — cache argument vs. descriptor field vs. always-on engine
> cache.** A trailing argument is preferred over a field on the descriptor
> (which would force a hash over a member that isn't part of the pipeline's
> identity) and over an implicit always-on engine-wide cache (which would
> surprise callers and complicate lifetime). The blit path internally builds a
> `RenderPipelineDescriptor`, so it forwards the same cache to its inner
> `makeRenderPipelineState`.

### Usage

```cpp
auto cache = engine->makePipelineCache("pipelines.gtecache");   // warm from disk if present

RenderPipelineDescriptor desc{ /* … */ };
auto pso = engine->makeRenderPipelineState(desc, cache);        // disk-warm or built+cached

// … at shutdown, persist anything newly built this run …
engine->savePipelineCache(cache);
```

---

## Layer 1 — In-Memory Dedup (backend-agnostic)

Lives entirely in shared code (`GEPipelineCache` base), so it is written once
and verified on any backend.

- **Key.** A 128-bit hash over the canonicalized descriptor plus the shader
  identity. Add a `RenderPipelineDescriptor::hash()` / `ComputePipelineDescriptor::hash()`
  that folds every field in a fixed order (vectors hashed element-by-element:
  `colorPixelFormats`, `colorBlendDescriptors`, `vertexInputDescriptor`). Shader
  identity = the owning library's content hash + the entry name (both already
  derivable at load time; store the library hash on `GTEShaderLibrary`).
- **Store.** `unordered_map<Key, SharedHandle<GE*PipelineState>>` guarded by a
  mutex (pipelines may be created off the main thread).
- **Flow.** `make*PipelineState` computes the key, returns the stored handle on
  hit, otherwise proceeds to Layer 2 / native creation and inserts the result.
- **Correctness note.** The hash must include *every* field that changes the
  PSO, or two distinct pipelines collide to one. A unit test enumerates each
  descriptor field, mutates it, and asserts the hash changes — the cheap guard
  against a silently-wrong cache.

This layer alone eliminates redundant creation (e.g. a material system that
requests the same pipeline from many call sites) with zero backend work.

## Layer 2 — Native On-Disk Persistence (per backend)

One `GEPipelineCache` wraps one native cache object. On `make*` miss, the
backend creates the PSO *through* the native cache so the driver consults /
populates it; `savePipelineCache` serializes the blob to the file.

| Backend | Native mechanism | Create-through | Persist | Load |
|---|---|---|---|---|
| **Metal** | `MTLBinaryArchive` | Set `MTLRenderPipelineDescriptor.binaryArchives = @[archive]`; the driver loads matching functions instead of compiling. Add new pipelines with `addRenderPipelineFunctionsWithDescriptor:` / `addComputePipelineFunctionsWithDescriptor:`. | `serializeToURL:error:` | `newBinaryArchiveWithDescriptor:` with `url` set |
| **Vulkan** | `VkPipelineCache` (one object backs all pipelines) | Pass the cache handle to `vkCreateGraphicsPipelines` / `vkCreateComputePipelines`. | `vkGetPipelineCacheData` → write bytes | `vkCreatePipelineCache` with `pInitialData` from the file |
| **D3D12** | `D3D12_CACHED_PIPELINE_STATE` per-PSO, or `ID3D12PipelineLibrary` for the whole set | Per-PSO: feed `desc.CachedPSO` (already exposed via `CD3DX12_PIPELINE_STATE_STREAM_CACHED_PSO` in `d3dx12.h`). Library: `LoadPipeline`/`LoadComputePipeline` by name, else create + `StorePipeline`. | `ID3D12PipelineLibrary::Serialize`, or `ID3D12PipelineState::GetCachedBlob` per PSO | `CreatePipelineLibrary(blob)`; cold `E_INVALIDARG` / `D3D12_ERROR_*` → start empty |

**File format.** A small GTE header followed by the opaque native blob:

```
struct GTEPipelineCacheHeader {
    char     magic[4];           // "GTPC"
    uint32_t version;            // GTE cache format version
    uint32_t backend;            // Metal / D3D12 / Vulkan — refuse cross-backend
    uint64_t deviceUUID;         // adapter LUID / Metal registryID / VkPhysicalDevice pipelineCacheUUID
    uint64_t shaderLibraryHash;  // content hash of the .omegasllib this cache was built against
    uint64_t blobSize;
    // ... native blob follows ...
};
```

On load, mismatch on any header field ⇒ **discard and start empty**. This is the
first line of defense; the native layer also validates internally (Vulkan's
`pipelineCacheUUID`, D3D12's cold-blob error, Metal's archive versioning), and
on a native miss the driver simply recompiles. **The cache is never a source of
truth — a stale, corrupt, or foreign cache must degrade to a clean rebuild,
never a crash.**

> **Security note.** A pipeline-cache blob is driver input. The Vulkan spec
> explicitly warns that feeding untrusted `pInitialData` can crash or
> compromise the driver. Treat the cache file as local, trusted, per-machine
> state (same trust level as a shader binary): validate the header, never ship
> a prebuilt cache as a download, and document that it is a regenerable local
> artifact.

---

## Phasing

Sequenced so each phase is independently shippable and the verifiable backend
(Metal — only Metal compiles on the dev host) lands first. Mirrors the
Phase-A/Phase-B discipline used by `uniform<T>` / push constants.

| Phase | Scope | Verifiable here? |
|---|---|---|
| **1. In-memory dedup** | `GEPipelineCache` base + descriptor `hash()` + Layer-1 map; `make*` takes the optional cache arg and short-circuits on hit. No disk. | Yes — backend-agnostic; unit test on the hash + a Metal test that two identical `make*` calls return the same handle. |
| **2. Metal on-disk** | `MTLBinaryArchive`-backed `GEPipelineCache`; `makePipelineCache(path)` / `savePipelineCache`; header gating. | Yes — end-to-end on the macOS host: build cold, save, relaunch, assert the warm path loads without recompiling (timing or `MTLBinaryArchive` hit count). |
| **3. Vulkan + D3D12 on-disk** | `VkPipelineCache` and D3D12 `ID3D12PipelineLibrary` (or per-PSO cached blob) behind the same API. | No — written-from-source; compiled/run on the matching platform (per the backend build-verification discipline). |
| **4. (optional) Cache warm-up bundling** | A helper to pre-create a known set of pipelines at load (e.g. driven by a manifest) so the first frame is hitch-free; optionally sidecar the cache next to the `.omegasllib`. | Yes (Metal). |

A reasonable first PR is **Phase 1 + Phase 2** together: the in-memory layer is
small and the Metal disk layer is the part that delivers the "load pre-existing
pipelines" win the request names, and both are verifiable here.

---

## Open Questions / Risks

- **Descriptor hashing completeness.** The single biggest correctness risk: a
  field omitted from `hash()` collides two distinct pipelines. Mitigated by the
  field-mutation unit test, but worth a careful review of every descriptor field
  (and re-review whenever a field is added — e.g. the recent blend / vertex-input
  / MRT additions).
- **`GTEShaderLibrary` content hash.** Layer 1 needs a stable per-library hash
  and Layer 2 needs it in the header. The `.omegasllib` bytes are the natural
  source; decide whether to hash at load (`loadShaderLibrary*`) and store it on
  the library handle. Small addition, but a prerequisite for both layers.
- **Thread-safety.** If pipelines are built concurrently, Layer 1's map and the
  native archive need locking (Metal `MTLBinaryArchive` and Vulkan
  `VkPipelineCache` are documented thread-safe for concurrent create; confirm
  per backend and guard the GTE map regardless).
- **When to persist.** `savePipelineCache` at shutdown is simplest, but a crash
  loses the run's new pipelines. A periodic/explicit flush is a later refinement.
- **Cross-run device change.** A GPU swap / driver update invalidates the blob;
  the header `deviceUUID` + native validation handle it by falling back to a
  clean rebuild. Confirm each backend's UUID source (`VkPhysicalDeviceProperties::pipelineCacheUUID`,
  adapter LUID, Metal `registryID`).
- **Interaction with `loadShaderLibraryRuntime`.** Runtime-compiled shaders
  (hot reload) change the library hash every edit, so they always miss — correct,
  but means the cache only helps the stable/shipped shader set. Acceptable.
```
