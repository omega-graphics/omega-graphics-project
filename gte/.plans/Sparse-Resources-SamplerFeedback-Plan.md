# Sparse / Tiled Resources + Sampler Feedback — Implementation Plan

## Status up-front

Before anything else, the headline question: **do all three backends support these features natively?**

| Feature | D3D12 | Metal | Vulkan |
|---|---|---|---|
| Sparse / tiled **textures** | Yes (Tier 1 / 2 / 3) | Yes (Apple6+ / Mac2) | Yes (core 1.0 features) |
| Sparse / tiled **buffers** | Yes (buffer tiling) | **No** (textures only) | Yes (`sparseResidencyBuffer`) |
| **Sampler feedback** (native) | Yes (Tier 0.9 / 1.0) | **No** (no API) | **No** (no ratified extension) |

So:
- **Sparse/tiled resources** are viable cross-backend if we restrict the first pass to **2D textures**. Buffers are added later gated by `GTEDEVICE_FEATURE_SPARSE_BUFFER`.
- **Sampler feedback** is **D3D12-only natively**. Making it cross-backend means either (a) gating it behind `GTEDEVICE_FEATURE_SAMPLER_FEEDBACK` and letting consumers fall back, or (b) emulating it via a compute pass that writes access markers to a UAV. This plan proposes (a) as the primary path and describes (b) as a follow-up.

The rest of this document assumes the `GTEDeviceFeatures` extension plan (the capability-bit scheme, runtime feature checks replacing compile-time `#ifdef`) has landed. New feature bits are added below.

---

## Part 1 — Sparse / Tiled Resources

### 1.1 Current state

OmegaGTE has no sparse-resource path. Every `GETexture` and `GEBuffer` is fully resident at creation. `GEHeap` exists (`gte/include/omegaGTE/GE.h:159`) but only handles the standard placed-resource case — no tile bookkeeping, no partial residency, no per-tile update commands.

### 1.2 Design principles

1. **Textures before buffers.** All three backends support sparse 2D textures; only D3D12 and Vulkan support sparse buffers. Ship textures first; add buffers under a separate feature bit.
2. **Explicit tile heap, explicit mapping.** Sparse resources require a physical tile pool separate from the virtual resource. The API exposes both — no hidden allocation inside `makeTexture()`.
3. **Tile mapping updates are a command-queue operation, not an engine-level call.** On D3D12 and Vulkan this is queue-level (`UpdateTileMappings` / `vkQueueBindSparse`). On Metal it is an encoder (`MTLResourceStateCommandEncoder`). Wrap it as a short-lived encoder owned by `GECommandBuffer` to keep the three backends aligned.
4. **One tile size, per device.** Each backend reports a platform tile size (typically 64KB on D3D12/Vulkan, device-queried on Metal). Expose this as a device property; do not let users pick.
5. **Conservative shader path for the first pass.** The first pass does not expose "sparse sample with residency feedback" in OmegaSL. Unmapped tiles read as zeros (all three backends guarantee this under the right flags). A residency-query intrinsic is tracked as a follow-up.

### 1.3 Proposed public API

New feature bits in `gte/include/OmegaGTE.h`:

```cpp
constexpr uint64_t GTEDEVICE_FEATURE_SPARSE_TEXTURE_2D = 1ULL << 25;
constexpr uint64_t GTEDEVICE_FEATURE_SPARSE_TEXTURE_3D = 1ULL << 26;
constexpr uint64_t GTEDEVICE_FEATURE_SPARSE_BUFFER     = 1ULL << 27;
constexpr uint64_t GTEDEVICE_FEATURE_SPARSE_RESIDENCY  = 1ULL << 28; // partial residency + null-tile-reads-zero
```

Extend `GTEDeviceFeatures` with tile granularity:

```cpp
struct GTEDeviceFeatures {
    // ... existing fields ...

    /// Tile size in bytes for sparse resources (typically 65536 on D3D12/Vulkan).
    /// Zero if sparse resources are unsupported on this device.
    uint32_t sparseTileSizeBytes = 0;

    /// Standard sparse tile shape for 2D textures in texels at mip 0
    /// (e.g. 128x128 for RGBA8, 64x64 for RGBA16). Backend-reported.
    uint32_t sparseTileTexelWidth  = 0;
    uint32_t sparseTileTexelHeight = 0;
    uint32_t sparseTileTexelDepth  = 0; // 1 for 2D
};
```

New `TextureDescriptor` flag and `HeapDescriptor` variant in `gte/include/omegaGTE/GETexture.h` / `GE.h`:

```cpp
struct TextureDescriptor {
    // ... existing fields ...
    /// If true, the texture is created as a virtual (sparse/reserved) resource.
    /// Tiles must be explicitly bound to a GETileHeap via GECommandBuffer::updateTileMapping()
    /// before the texture is sampled. Requires GTEDEVICE_FEATURE_SPARSE_TEXTURE_2D.
    bool sparse = false;
};

struct HeapDescriptor {
    // ... existing fields ...
    enum HeapClass : uint8_t {
        Regular,   ///< existing behavior — holds buffers/textures
        TilePool,  ///< holds fixed-size tiles backing sparse resources
    } heapClass = Regular;
};
```

New resource class — a tile-mapping handle:

```cpp
/// @brief Identifies a contiguous range of tiles inside a tile-pool heap.
/// Produced by GEHeap::allocateTiles() on a TilePool heap; consumed by
/// GECommandBuffer::updateTileMapping(). Opaque to the caller.
class OMEGAGTE_EXPORT GETileAllocation : public GTEResource {
public:
    OMEGACOMMON_CLASS("OmegaGTE.GETileAllocation")
    virtual uint32_t tileCount() const = 0;
    virtual ~GETileAllocation() = default;
};
```

New `GEHeap` methods (TilePool-class heaps only):

```cpp
class GEHeap {
public:
    // ... existing methods ...

    /// Allocate a contiguous range of tiles from a TilePool heap.
    /// Returns nullptr if heapClass != TilePool or capacity is exhausted.
    virtual SharedHandle<GETileAllocation> allocateTiles(uint32_t tileCount) = 0;
    virtual void freeTiles(SharedHandle<GETileAllocation> & tiles) = 0;
};
```

New command-buffer methods for tile mapping:

```cpp
struct GETileRegion {
    uint32_t mipLevel;
    uint32_t arraySlice;
    uint32_t xTiles, yTiles, zTiles; // origin in tile units
    uint32_t widthTiles, heightTiles, depthTiles;
};

class GECommandBuffer {
public:
    // ... existing methods ...

    /// Begin a tile-mapping pass. Must be outside any render/compute/blit pass.
    virtual void startTileMappingPass() = 0;

    /// Bind tiles from @p sourceTiles to a region of a sparse texture.
    /// Pass nullptr for @p sourceTiles to unmap the region (reads-as-zero).
    virtual void updateTileMapping(
        SharedHandle<GETexture>        & texture,
        const GETileRegion             & region,
        SharedHandle<GETileAllocation> & sourceTiles,
        uint32_t                         sourceTileOffset = 0) = 0;

    virtual void finishTileMappingPass() = 0;
};
```

### 1.4 Backend mapping

#### D3D12

| Concept | D3D12 call |
|---|---|
| Sparse texture | `ID3D12Device::CreateReservedResource(desc, initialState)` |
| Tile pool heap | `ID3D12Device::CreateHeap(desc)` with `D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS` cleared + `D3D12_HEAP_TYPE_DEFAULT` |
| Tile allocation | internal offset/count bookkeeping into the heap |
| `updateTileMapping()` | `ID3D12CommandQueue::UpdateTileMappings(resource, 1, &coord, &regionSize, heap, 1, &flags, &heapTileOffset, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE)` |
| Unmap | same call with `D3D12_TILE_RANGE_FLAG_NULL` |
| Tile size | 64KB (`D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES`) |
| Standard tile shape | `ID3D12Device::GetResourceTiling` → `D3D12_TILE_SHAPE` |
| Feature query | `CheckFeatureSupport(OPTIONS)` → `TiledResourcesTier` |
|  Tier 1 → `SPARSE_TEXTURE_2D` | |
|  Tier 2 → + `SPARSE_RESIDENCY` (null tile reads zero, shader clamp) | |
|  Tier 3 → + `SPARSE_TEXTURE_3D` | |

**Note on encoder model.** D3D12 `UpdateTileMappings` is a *queue* operation, not a command-list operation. The `startTileMappingPass` / `finishTileMappingPass` pair on `GECommandBuffer` will buffer tile-mapping updates and issue them against the owning `ID3D12CommandQueue` at command-buffer submit time, *before* the command list's `ExecuteCommandLists`. Document this ordering explicitly — callers who submit a command buffer that both updates mappings and samples the texture in a compute pass get correctness by construction.

#### Metal

| Concept | Metal call |
|---|---|
| Sparse texture | `MTLTextureDescriptor` with `usage |= .shaderRead`, allocated from a **sparse heap** via `heap.makeTexture(descriptor)` |
| Tile pool heap | `MTLHeapDescriptor` with `type = .sparse`, `size = tileCount * device.sparseTileSizeInBytes` |
| Tile allocation | `MTLHeap` has no explicit allocate-tiles API; model `GETileAllocation` as a `(heap, offsetInBytes, tileCount)` tuple we maintain in a free-list on top of the `MTLHeap` |
| `updateTileMapping()` | `MTLResourceStateCommandEncoder::updateTextureMapping:mode:region:mipLevel:slice:withTileMapping:` where `withTileMapping:` is a `MTLSparseTextureMappingMode` structure referring to the heap offset |
| Unmap | same call with `mode = .unmap` |
| Tile size | `device.sparseTileSizeInBytes` |
| Standard tile shape | `device.sparseTileSize(with: textureType, pixelFormat: .., sampleCount: 1)` → `MTLSize` |
| Feature query | `device.supportsFamily(.apple6)` (iPhone 12+/iPad Pro 2020+) or `.mac2` |

**Note on sparse buffers.** Metal does not support sparse buffers as of Metal 3. The `GTEDEVICE_FEATURE_SPARSE_BUFFER` bit will stay zero on Metal. Consumers that need sparse buffers fall back to regular buffers on Metal.

**Note on heap classes.** The existing `HeapDescriptor::HeapType` enum (`Shared`/`Automatic`) conflates backing mode with heap class. Add `HeapDescriptor::heapClass = TilePool` and map to `MTLHeapDescriptor.type = .sparse` at creation time; the existing `HeapType` remains for non-sparse heaps.

#### Vulkan

| Concept | Vulkan call |
|---|---|
| Sparse texture | `vkCreateImage` with `flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT` |
| Tile pool heap | `vkAllocateMemory` from a memory type with `DEVICE_LOCAL`; logical `GEHeap` is a thin wrapper |
| Tile allocation | offset/count bookkeeping within a `VkDeviceMemory` allocation |
| `updateTileMapping()` | `vkQueueBindSparse(queue, 1, &bindInfo, fence)` with `VkSparseImageMemoryBind`/`VkSparseImageOpaqueMemoryBind` |
| Unmap | same call with `memory = VK_NULL_HANDLE` |
| Tile size | `VkPhysicalDeviceSparseProperties` granularity (typically 64KB × platform) |
| Standard tile shape | `vkGetImageSparseMemoryRequirements` → `VkSparseImageMemoryRequirements.formatProperties.imageGranularity` |
| Feature query | `VkPhysicalDeviceFeatures.sparseBinding` + `sparseResidencyImage2D` + `sparseResidencyImage3D` + `sparseResidencyBuffer` |

**Note on queue-level semantics.** Like D3D12, `vkQueueBindSparse` is a queue operation that carries its own semaphore wait/signal lists — it does *not* go into a command buffer. The `startTileMappingPass` / `finishTileMappingPass` pair buffers binds and issues a single `vkQueueBindSparse` at command-buffer submit time, with a signal semaphore the command buffer's subsequent `vkQueueSubmit` waits on. This makes the three backends behave identically from the caller's perspective: "update mappings, then submit; the submit sees the new mappings."

### 1.5 Implementation phases

**Phase A — device features.** Add the four new `GTEDEVICE_FEATURE_SPARSE_*` bits and the `sparseTileSizeBytes` / `sparseTileTexel*` fields to `GTEDeviceFeatures`. Populate from each backend's `enumerateDevices()`.

**Phase B — heap extension.** Extend `HeapDescriptor` with `heapClass` and add `GEHeap::allocateTiles` / `freeTiles`. Implement tile-pool heap creation in all three backends plus a free-list allocator in `GEHeap` base class.

**Phase C — sparse texture creation.** Extend `TextureDescriptor::sparse`. Implement `CreateReservedResource` (D3D12), sparse heap texture (Metal), `SPARSE_BINDING_BIT` image (Vulkan). Reject `sparse = true` when the feature bit is clear.

**Phase D — command-buffer tile mapping.** Add `startTileMappingPass` / `updateTileMapping` / `finishTileMappingPass` to `GECommandBuffer`. In each backend, buffer the updates and issue them at submit time (D3D12 `UpdateTileMappings` / Vulkan `vkQueueBindSparse` pre-submit, Metal `MTLResourceStateCommandEncoder` inline).

**Phase E — OmegaSL shader path.** Null tiles read-as-zero by default on all three backends (Tier 2 / `sparseResidency` / Apple6). No OmegaSL changes required for the first pass; the existing `sample` intrinsic works unchanged against a sparse texture.

**Phase F — test + doc.** Cross-backend test: create a 4096×4096 sparse texture, map a single 128×128 tile, sample it, verify correct reads on that tile and zero elsewhere.

### 1.6 Open questions

1. **Free-list allocator location.** The tile allocator lives in `GEHeap` as a base-class utility (shared code), or in each backend (each backend can use native primitives). Base-class is simpler and gives identical semantics; backend-specific could use `MTLHeap`'s sub-allocator on Metal. Default proposal: base-class free-list.
2. **Residency feedback intrinsic.** Expose `sparse_sample_with_residency()` in OmegaSL in this pass, or wait until a consumer needs it? D3D12 `CheckAccessFullyMapped`, Metal `sparse_sample`, Vulkan `OpImageSparseSampleImplicitLod` all exist — but without a consumer (virtual texturing, megatextures) it's dead API. Default proposal: defer.
3. **Sparse 3D textures.** Metal supports sparse 2D only (as of Metal 3). D3D12 requires Tier 3. Vulkan requires `sparseResidencyImage3D`. The first pass is 2D-only; 3D is a future addition gated by `GTEDEVICE_FEATURE_SPARSE_TEXTURE_3D`.

---

## Part 2 — Sampler Feedback

### 2.1 Reality check

Sampler feedback is a **D3D12-exclusive feature** as a first-class API. Metal has no equivalent; Vulkan has no ratified KHR/EXT extension. Before writing any of this, that's the call the developer needs to make:

- **Option A — expose natively, gate by feature bit.** Sampler feedback is available only where `GTEDEVICE_FEATURE_SAMPLER_FEEDBACK` is set (D3D12 only). On Metal/Vulkan consumers must have a fallback path (e.g. full-resolution mip loading, no streaming).
- **Option B — emulate on Metal/Vulkan.** A compute or fragment shader writes access markers to a R8_UINT "feedback" UAV parallel to every `sample()` call. The consumer reads the marker texture back each frame. Cost: every sampled draw gets a second UAV write, plus OmegaSL must emit the feedback write automatically.
- **Option C — skip entirely.** Leave it out of OmegaGTE; consumers that want streaming textures write their own D3D12-specific path.

This plan proposes **Option A now, Option B later** — expose the native path behind a feature bit, ship D3D12 first, and revisit emulation when a consumer actually needs it. Sampler feedback is an optimization aid, not a correctness requirement; gating is honest and avoids simulating a feature that may never get used.

MY NOTES: Just use option A. Users will know not every feature is supported on every platform.

### 2.2 Proposed public API (Option A)

New feature bit:

```cpp
constexpr uint64_t GTEDEVICE_FEATURE_SAMPLER_FEEDBACK_MIN_MIP       = 1ULL << 29;
constexpr uint64_t GTEDEVICE_FEATURE_SAMPLER_FEEDBACK_MIP_REGION    = 1ULL << 30;
```

New resource descriptor — a feedback map:

```cpp
/// @brief A sampler feedback map paired with a GETexture. Records which
/// mip levels / tile regions of the paired texture were accessed by
/// sample() calls in shaders. Requires GTEDEVICE_FEATURE_SAMPLER_FEEDBACK_*.
struct OMEGAGTE_EXPORT SamplerFeedbackDescriptor {
    enum class Kind : uint8_t {
        MinMip,        ///< one byte per tile: minimum mip sampled
        MipRegionUsed, ///< one bit per (tile, mip): was this region sampled?
    } kind = Kind::MinMip;

    SharedHandle<GETexture> pairedTexture;  ///< the texture being monitored
    uint32_t regionWidth  = 0;              ///< mip-region width in texels (0 = match tile shape)
    uint32_t regionHeight = 0;
};

class OMEGAGTE_EXPORT GESamplerFeedbackMap : public GTEResource {
public:
    OMEGACOMMON_CLASS("OmegaGTE.GESamplerFeedbackMap")
    virtual ~GESamplerFeedbackMap() = default;
};
```

New engine + command-buffer methods:

```cpp
class OmegaGraphicsEngine {
public:
    // ... existing methods ...
    virtual SharedHandle<GESamplerFeedbackMap> makeSamplerFeedbackMap(
        const SamplerFeedbackDescriptor & desc) = 0;
};

class GECommandBuffer {
public:
    // ... existing methods ...

    /// Bind a sampler feedback map at a fragment-shader binding slot.
    /// Shader writes via the OmegaSL `write_sampler_feedback` intrinsic are
    /// accumulated into this map over the render pass.
    virtual void bindResourceAtFragmentShader(
        SharedHandle<GESamplerFeedbackMap> & map, unsigned id) = 0;

    /// Clear a sampler feedback map to "no regions accessed" state.
    virtual void clearSamplerFeedbackMap(
        SharedHandle<GESamplerFeedbackMap> & map) = 0;

    /// Resolve the opaque feedback map into a readable texture.
    /// The destination texture is a regular R8_UINT / R8G8_UINT texture
    /// that the caller can copy back to CPU memory.
    virtual void resolveSamplerFeedbackMap(
        SharedHandle<GESamplerFeedbackMap> & map,
        SharedHandle<GETexture>            & dest) = 0;
};
```

OmegaSL addition (shader side):

```glsl
// In a fragment shader:
write_sampler_feedback(feedbackMap, texture, sampler, uv);
```

On D3D12 this lowers to `HLSL WriteSamplerFeedback(fb, tex, smp, uv)`. On Metal/Vulkan the compiler emits a no-op (feature bit is clear, consumer expected to have checked).

### 2.3 Backend mapping

#### D3D12

| Concept | D3D12 call |
|---|---|
| Feedback map resource | `ID3D12Device::CreateCommittedResource` with format `DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE` or `DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE` |
| Bind at fragment stage | `CreateSamplerFeedbackUnorderedAccessView(pairedResource, feedbackResource, uavHandle)` + `SetGraphicsRootDescriptorTable` |
| Clear | `ClearUnorderedAccessViewUint` with all-zeros (Tier 0.9) or `ClearSamplerFeedbackUnorderedAccessView` (Tier 1.0) |
| Resolve | `ResolveSubresourceRegion(dest, 0, 0, 0, src, 0, nullptr, dstFormat, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK)` |
| Shader intrinsic | `WriteSamplerFeedback(fb, tex, smp, uv)` in HLSL 6.5+ |
| Feature query | `CheckFeatureSupport(OPTIONS7)` → `SamplerFeedbackTier` (0.9 → MinMip, 1.0 → + MipRegionUsed) |

#### Metal

No native path. For Option A, `makeSamplerFeedbackMap()` returns `nullptr` and sets an error; consumers check `GTEDEVICE_FEATURE_SAMPLER_FEEDBACK_*` before calling.

#### Vulkan

No native path. Same behavior as Metal for Option A.

### 2.4 Option B — cross-backend emulation (future work)

If we later decide to make sampler feedback cross-backend, the emulation shape is:

- **Feedback map** → a regular `R8_UINT` 2D texture at a coarser resolution than the paired texture (one texel per tile).
- **Shader write** → OmegaSL `write_sampler_feedback(fb, tex, smp, uv)` compiles on Metal/Vulkan to an explicit `imageStore(fb, floor(uv * fbSize), min(current, computed_mip))` using `textureQueryLod` (Vulkan) / `calculate_clamped_lod` (Metal) to derive the mip.
- **Resolve** → the R8_UINT texture is already CPU-readable; no decode step.
- **Cost** → one extra `imageStore` per sample call in any shader bound to a feedback map. For a typical rendering workload this is measurable; for a virtual texturing/streaming system it's cheaper than oversampling.

This is tractable but invasive: it pushes feedback awareness into OmegaSL's sample-intrinsic lowering. Defer until a consumer exists.

### 2.5 Implementation phases

**Phase A — D3D12 native implementation.** Add the two feature bits, populate from `OPTIONS7.SamplerFeedbackTier`. Implement `makeSamplerFeedbackMap`, `bindResourceAtFragmentShader(GESamplerFeedbackMap&)`, `clearSamplerFeedbackMap`, `resolveSamplerFeedbackMap`. Wire `write_sampler_feedback` in OmegaSL → HLSL codegen.

**Phase B — Metal / Vulkan stubs.** Return `nullptr` from `makeSamplerFeedbackMap`; OmegaSL → MSL/SPIR-V codegen emits no-op for `write_sampler_feedback`. Add a runtime assert if consumers call the bind method without the feature bit.

**Phase C — documentation.** Make the feature-gate contract explicit in `API.rst`: consumers must check `GTEDEVICE_FEATURE_SAMPLER_FEEDBACK_*` before creating a feedback map, and must have a non-feedback fallback path.

**Phase D (future) — emulation.** Only if a real consumer (a virtual texturing system, a mip-streaming engine) lands. Implement Option B across the OmegaSL lowering pipeline.

### 2.6 Open questions

1. **Gate or emulate from the start?** Default proposal: gate. Emulation has a real per-sample cost and no consumer yet.
2. **Tier 0.9 vs 1.0 granularity.** D3D12 `SamplerFeedbackTier 0.9` supports only `MinMip`; `Tier 1.0` adds `MipRegionUsed`. Expose as two feature bits (proposed above), or collapse into one with a `maxFeedbackTier` field? Two bits matches the existing flag-per-feature pattern.
3. **OmegaSL intrinsic naming.** `write_sampler_feedback` mirrors HLSL. Accept as-is, or use a shorter name?

---

## Summary

| Feature | Cross-backend? | Plan |
|---|---|---|
| Sparse 2D textures | Yes | Ship across all three backends, phases A–F |
| Sparse 3D textures | Partial (not Metal) | Add later, gated by `GTEDEVICE_FEATURE_SPARSE_TEXTURE_3D` |
| Sparse buffers | Partial (not Metal) | Add later, gated by `GTEDEVICE_FEATURE_SPARSE_BUFFER` |
| Sampler feedback (native) | No (D3D12 only) | Ship on D3D12, feature-gate, Metal/Vulkan stubs return null |
<!-- | Sampler feedback (emulated) | Yes but expensive | Defer until a consumer needs it | -->

Both features depend on the `GTEDeviceFeatures` runtime-query plan already in flight. Neither requires compile-time `#ifdef` gating — the feature-bit pattern established in that plan carries directly here.
