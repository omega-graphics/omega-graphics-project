# Direct GPU I/O (DirectStorage / MTLIO Equivalent) Implementation Plan

## Goal

Add a unified, asynchronous file→GPU streaming subsystem to OmegaGTE that abstracts over **Microsoft DirectStorage** (D3D12) and **MTLIO** (Metal). This lets engine clients submit "load this byte range from this file directly into this GEBuffer / GETexture region" requests that bypass the traditional `fread → staging buffer → upload` path. On supported hardware, decompression happens on the GPU.

The Vulkan backend will report the feature as unsupported initially. A fallback emulation (CPU thread pool + transfer queue) and a survey of hardware-accelerated paths is included so the API can light up later without breaking ABI.

## Why this belongs in OmegaGTE now

- **GEMesh / texture streaming.** The mesh and texture asset plans (`GEMesh-TextureAssets-Implementation-Plan.md`) describe streaming large vertex/index/texel buffers. Doing those uploads through the existing `BufferDescriptor::Upload` path serializes everything through the CPU and a staging copy.
- **Triangulation engine outputs.** Large triangulation results currently round-trip through host memory before becoming a `GEBuffer`. Direct I/O removes that hop for cached results stored on disk.
- **Cross-platform parity.** Both Apple (`MTLIOCommandQueue`, iOS 16 / macOS 13) and Microsoft (`IDStorageQueue`, Win10 1909+ with the DirectStorage runtime) ship first-class APIs. Without an OmegaGTE abstraction, every consumer would have to write platform-specific upload paths.

## Non-Goals

- Replacing the existing `GEBuffer` / `GETexture` upload paths. Direct I/O is an additional path, not a replacement. Small one-shot uploads stay on the synchronous `copyBytes` route.
- A general-purpose VFS. `GEIOFileHandle` wraps a single file (or MTLIO file/D3D12 file handle). Pak files, virtual mounts, and asset bundles are layered on top by the consumer.
- GPU compression *encoding*. Only decompression is in scope. Asset baking happens offline.
- Custom decompression formats. We expose what the platform implements natively (GDeflate on D3D12, LZ4/ZLib/LZBitmap/LZFSE on Metal). No CPU shimming of formats one platform supports and the other doesn't — that defeats the point.

---

## Current State

OmegaGTE has no streaming I/O concept. Resource population happens exclusively through:

- `GEBuffer`-bound `BufferDescriptor::Upload` followed by `memcpy` into a mapped pointer.
- `GETexture::copyBytes(void *bytes, size_t bytesPerRow)` (`GETexture.h:46`), which expects the caller to have the bytes in CPU memory already.
- `GEHeap::makeBuffer` / `makeTexture` for placed allocations, again with CPU-side initialization.

Every byte that ends up in VRAM passes through the CPU and through a staging buffer. There is no fence/queue for "the file is now in the buffer," so the consumer has to block on a CPU read first.

`GTEDeviceFeatures` (`OmegaGTE.h:20`) currently has three booleans (`raytracing`, `msaa4x`, `msaa8x`) and no place to advertise direct-I/O capability.

---

## Design Principles

1. **One API, three implementations, one feature flag.** The public surface lives in `omegaGTE/GEIO.h` and is identical on all backends. `GTEDeviceFeatures::directIO` says whether the device has a real backend; if it doesn't, calls to `makeIOCommandQueue` return `nullptr` and the consumer is expected to fall back to the existing upload path.
2. **Mirror MTLIO's command-queue/command-buffer/handle shape.** It's the higher-level of the two APIs. DirectStorage's `IDStorageQueue::EnqueueRequest` maps cleanly down onto it; the reverse is harder.
3. **Compression is a per-request enum, never auto-detected.** The consumer baked the asset and knows the format. We never sniff headers.
4. **Asynchronous by default.** Every load returns through a `GEFence` signal. Synchronous waiting is opt-in via `waitUntilCompleted()`. No hidden CPU blocking.
5. **No new resource types.** Loads target existing `GEBuffer` / `GETexture` handles. The I/O subsystem is purely a *transport*; allocation stays where it is.
6. **Graceful runtime gating.** Code that wants direct I/O queries `device->features.directIO` at runtime. No compile-time `#ifdef OMEGAGTE_DIRECT_IO_SUPPORTED` walls — that's the mistake the raytracing path is currently locked into.

---

## Public API

New header: `gte/include/omegaGTE/GEIO.h`. Included from `OmegaGTE.h`.

```cpp
_NAMESPACE_BEGIN_

    /// @brief Compression format used for GPU-side decompression of streamed data.
    /// Each backend supports a subset; query GTEDeviceFeatures::directIOCompressionMask
    /// before submitting requests with anything other than None.
    enum class GEIOCompression : uint8_t {
        None       = 0,
        GDeflate   = 1,  // DirectStorage GPU decompression; future Vulkan via VK_NV_memory_decompression
        LZ4        = 2,  // MTLIO native
        ZLib       = 3,  // MTLIO native
        LZBitmap   = 4,  // MTLIO native (Apple Silicon)
        LZFSE      = 5,  // MTLIO native (Apple)
    };

    /// @brief Bit flags packed into GTEDeviceFeatures::directIOCompressionMask.
    enum GEIOCompressionBits : uint32_t {
        GEIO_COMP_NONE     = 1u << 0,
        GEIO_COMP_GDEFLATE = 1u << 1,
        GEIO_COMP_LZ4      = 1u << 2,
        GEIO_COMP_ZLIB     = 1u << 3,
        GEIO_COMP_LZBITMAP = 1u << 4,
        GEIO_COMP_LZFSE    = 1u << 5,
    };

    struct OMEGAGTE_EXPORT GEIOFileHandleDescriptor {
        FS::Path path;
    };

    /// @brief A platform file handle opened for direct GPU I/O.
    /// Backed by IDStorageFile on D3D12, MTLIOFileHandle on Metal,
    /// and a POSIX fd / HANDLE in the Vulkan fallback path.
    class OMEGAGTE_EXPORT GEIOFileHandle : public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEIOFileHandle")
        virtual std::uint64_t fileSize() const = 0;
        virtual ~GEIOFileHandle() = default;
    };

    /// @brief A single load request enqueued onto a GEIOCommandBuffer.
    struct OMEGAGTE_EXPORT GEIOLoadRequest {
        SharedHandle<GEIOFileHandle> source;

        /// Byte offset within the file where the (possibly-compressed) payload begins.
        std::uint64_t fileOffset = 0;
        /// Number of bytes to read from disk.
        std::uint64_t compressedSize = 0;
        /// Expected size after decompression. Equal to compressedSize when compression == None.
        std::uint64_t uncompressedSize = 0;

        GEIOCompression compression = GEIOCompression::None;

        enum class Destination : uint8_t { Buffer, Texture } destinationKind = Destination::Buffer;

        // Buffer destination
        SharedHandle<GEBuffer> destBuffer;
        std::uint64_t bufferOffset = 0;

        // Texture destination
        SharedHandle<GETexture> destTexture;
        TextureRegion textureRegion;
        unsigned mipLevel = 0;
        unsigned arraySlice = 0;
    };

    /// @brief A batch of load requests submitted as a unit. Mirrors MTLIOCommandBuffer
    /// and a single IDStorageQueue::EnqueueRequest sequence terminated by Submit().
    class OMEGAGTE_EXPORT GEIOCommandBuffer : public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEIOCommandBuffer")
        virtual void enqueueLoad(const GEIOLoadRequest & req) = 0;

        /// Signal a fence on this queue's timeline once all loads in this buffer complete.
        /// Use the fence to gate dependent render/compute work on a different queue.
        virtual void signalFence(SharedHandle<GEFence> & fence, std::uint64_t value) = 0;

        /// Submit. After commit, no more loads may be enqueued on this buffer.
        virtual void commit() = 0;

        /// Block the calling thread until all loads complete. Optional; prefer fences.
        virtual void waitUntilCompleted() = 0;

        virtual ~GEIOCommandBuffer() = default;
    };

    /// @brief A queue dedicated to direct file→GPU transfers.
    class OMEGAGTE_EXPORT GEIOCommandQueue : public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEIOCommandQueue")
        virtual SharedHandle<GEIOCommandBuffer> getAvailableCommandBuffer() = 0;
        virtual ~GEIOCommandQueue() = default;
    };

_NAMESPACE_END_
```

Three new methods on `OmegaGraphicsEngine` (`GE.h`):

```cpp
/// @brief Open a file for direct GPU I/O. Returns nullptr on backends without support.
virtual SharedHandle<GEIOFileHandle> openIOFile(const GEIOFileHandleDescriptor & desc) = 0;

/// @brief Create an I/O command queue. Returns nullptr if features.directIO is false.
virtual SharedHandle<GEIOCommandQueue> makeIOCommandQueue(unsigned maxBufferCount) = 0;
```

---

## GTEDeviceFeatures additions

```cpp
struct GTEDeviceFeatures {
    bool raytracing;
    bool msaa4x;
    bool msaa8x;

    // ── Direct GPU I/O ──────────────────────────────────────────
    /// True when the backend has a real DirectStorage / MTLIO path.
    bool directIO;
    /// True when the I/O path can decompress on the GPU rather than CPU.
    /// Implies directIO == true.
    bool directIOGPUDecompression;
    /// Bitmask of supported compression formats. Always contains GEIO_COMP_NONE
    /// when directIO is true.
    uint32_t directIOCompressionMask;
};
```

This dovetails with the broader struct expansion in `GTEDeviceFeatures-Extension-Plan.md` — these three fields slot into the "Direct I/O" section of that plan and don't conflict with anything else there.

---

## Backend Mapping

### Metal — MTLIO

| GEIO concept | MTLIO mapping |
|---|---|
| `GEIOFileHandle` | `id<MTLIOFileHandle>` from `[device newIOFileHandleWithURL:compressionMethod:error:]`. Compression is part of the handle, not the request, so we open one handle per (path, compression) pair and cache by tuple. |
| `GEIOCommandQueue` | `id<MTLIOCommandQueue>` from `[device newIOCommandQueueWithDescriptor:error:]`. Descriptor uses `MTLIOCommandQueueTypeConcurrent`. |
| `GEIOCommandBuffer` | `id<MTLIOCommandBuffer>` from `[queue commandBuffer]`. |
| `enqueueLoad(buffer)` | `[ioCmdBuffer loadBuffer:offset:size:sourceHandle:sourceHandleOffset:]`. |
| `enqueueLoad(texture)` | `[ioCmdBuffer loadTexture:slice:level:size:sourceBytesPerRow:sourceBytesPerImage:destinationOrigin:sourceHandle:sourceHandleOffset:]`. |
| `signalFence` | `[ioCmdBuffer encodeSignalEvent:value:]` against an `id<MTLSharedEvent>` that backs the GEFence (the existing GEFence Metal impl already wraps MTLSharedEvent for cross-queue sync). |
| `commit` | `[ioCmdBuffer commit]`. |
| `waitUntilCompleted` | `[ioCmdBuffer waitUntilCompleted]`. |

**Feature gating.** `MTLIOCommandQueue` requires macOS 13 / iOS 16. Detect at runtime via `@available(macOS 13, iOS 16, *)`. When unavailable, set `features.directIO = false` and the methods return nullptr.

**Compression mask:**
```
GEIO_COMP_NONE | GEIO_COMP_LZ4 | GEIO_COMP_ZLIB | GEIO_COMP_LZBITMAP | GEIO_COMP_LZFSE
```
GDeflate is not a Metal native format and is reported as unsupported.

**Files:** new `gte/src/metal/GEMetalIO.h` / `GEMetalIO.mm`; touches `GEMetal.mm` for queries and engine method implementations.

### D3D12 — DirectStorage

| GEIO concept | DirectStorage mapping |
|---|---|
| `GEIOFileHandle` | `Microsoft::WRL::ComPtr<IDStorageFile>` from `IDStorageFactory::OpenFile`. |
| `GEIOCommandQueue` | `ComPtr<IDStorageQueue>` from `IDStorageFactory::CreateQueue` with `DSTORAGE_QUEUE_DESC{ .SourceType = DSTORAGE_REQUEST_SOURCE_FILE, .Capacity = maxBufferCount, .Priority = DSTORAGE_PRIORITY_NORMAL, .Device = engine->d3d12_device.Get() }`. |
| `GEIOCommandBuffer` | A pooled software wrapper that accumulates `DSTORAGE_REQUEST` structs. There is no native "command buffer" in DirectStorage — the wrapper batches `EnqueueRequest` calls and a single `Submit` per `commit()`. |
| `enqueueLoad(buffer)` | `DSTORAGE_REQUEST` with `Destination.Buffer = { dest12Resource, bufferOffset, uncompressedSize }`, `Source.File = { dsFile, fileOffset, compressedSize }`, `Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE` if compression == GDeflate. |
| `enqueueLoad(texture)` | `Destination.Texture = { destTextureResource, subresourceIndex, region }`. Subresource index = `D3D12CalcSubresource(mipLevel, arraySlice, 0, mipCount, arraySize)`. |
| `signalFence` | `IDStorageQueue::EnqueueSignal(ID3D12Fence*, value)`. The existing D3D12 GEFence wraps an `ID3D12Fence` so this drops in directly. |
| `commit` | `IDStorageQueue::Submit()`. The wrapper's per-buffer `Submit` flushes only the requests staged in *this* buffer; concurrent buffers on the same queue use a small mutex around the staging→Submit handoff. |
| `waitUntilCompleted` | Internally `EnqueueSignal` against a private fence + `WaitForSingleObject` on its event. |

**Initialization.** `DStorageGetFactory(IID_PPV_ARGS(&factory))` is called once during `GED3D12Engine` construction. Failure (DirectStorage runtime not installed) sets `features.directIO = false` and the factory pointer stays null.

**Compression mask:**
```
GEIO_COMP_NONE | GEIO_COMP_GDEFLATE
```
GPU decompression (`directIOGPUDecompression`) is true on devices with shader model 6.0+ (DirectStorage falls back to CPU GDeflate otherwise — still report `directIOGPUDecompression = false` in that case so consumers can choose offline trade-offs).

**Dependency.** Vendor `Microsoft.Direct3D.DirectStorage` (header + `dstorage.lib` + `dstorage.dll` + `dstoragecore.dll`) under `gte/deps/DirectStorage/`. Same AUTOMDEPS pattern as the planned D3D12MA vendoring. Minimum runtime: 1.2.2 (GPU GDeflate path).

**Files:** new `gte/src/d3d12/GED3D12IO.h` / `GED3D12IO.cpp`; touches `GED3D12.cpp` for factory init, feature population, and engine method implementations; `gte/CMakeLists.txt` for the dependency.

### Vulkan — Stub today, real implementation later

There is no cross-vendor Vulkan equivalent of DirectStorage / MTLIO. Three plausible implementation paths exist; none of them are available on every Vulkan device, so the initial implementation reports `directIO = false` and provides a software fallback for symmetry.

**Initial implementation (always-available fallback):**

- `GEVulkanIOFileHandle` wraps a POSIX `fd` (or `HANDLE` on Win32) opened with `O_DIRECT` where the OS supports it.
- `GEVulkanIOCommandQueue` owns a small thread pool (default: 2 workers) and a dedicated `VkQueue` from a transfer-only family.
- `enqueueLoad` records the request. `commit` hands the batch to the worker pool. Each worker:
  1. `read()` the compressed bytes into a CPU staging area.
  2. CPU-decompress when `compression != None` (use `liblz4` / `zlib` from autom-deps; refuse `GDeflate`/`LZBitmap`/`LZFSE`).
  3. Copy into a `vmaCreateBuffer` `HOST_VISIBLE` staging buffer.
  4. Record a `vkCmdCopyBuffer` / `vkCmdCopyBufferToImage` on a transfer command buffer.
  5. Submit with a `VkSemaphore` signal that the GEFence wrapper waits on.

This fallback is **off by default** — `features.directIO` is `false` so consumers don't accidentally pick the slow path thinking it's hardware-accelerated. It's available via `engine->makeIOCommandQueue()` returning a non-null queue *only* when the consumer explicitly opts in via a future `GEIOQueueDescriptor::allowSoftwareFallback` flag (out of scope for this initial plan; mentioned so the API shape is forward-compatible).

**Hardware-accelerated paths to investigate (future work):**

1. **`VK_NV_memory_decompression`** (NVIDIA, exposed since driver 535) — gives `vkCmdDecompressMemoryNV` for GDeflate on the GPU. Pair with a transfer queue submission for the disk read. This would let us flip `directIO = true` and `directIOGPUDecompression = true` on NVIDIA hardware with the matching extension. The compression mask becomes `NONE | GDEFLATE`. The disk read still has to go through a CPU thread or `io_uring`, since Vulkan has no native async file handle.

2. **NVIDIA RTX IO / AMD GPUOpen Direct Storage prototypes.** Both vendors have published sample code that pairs `VK_KHR_external_memory_fd` with a host-side reader. Worth tracking but neither is a stable extension yet.

3. **Windows DirectStorage on Vulkan via interop.** On Windows, a Vulkan app can import a D3D12 resource through `VK_KHR_external_memory_win32`, hand the underlying `ID3D12Resource` to DirectStorage as the destination, and consume the loaded contents through the Vulkan view of the same memory. Complex but viable; skipped for v1.

The Vulkan section of this plan should be revisited once `VK_NV_memory_decompression` ships on more vendors or once Khronos standardizes a `VK_KHR_*` equivalent.

**Files:** new `gte/src/vulkan/GEVulkanIO.h` / `GEVulkanIO.cpp` (stub returning nullptr from `makeIOCommandQueue` and false from feature query); touches `GEVulkan.cpp` for the engine method definitions.

---

## Implementation Phases

### Phase 0 — Feature flag

Add `directIO`, `directIOGPUDecompression`, `directIOCompressionMask` to `GTEDeviceFeatures` in `OmegaGTE.h`. Default-initialize to `false` / `0`. Update all three backends' `enumerateDevices()` paths to populate them (D3D12 and Vulkan: hardcoded false; Metal: runtime `@available` check).

**Files:** `gte/include/OmegaGTE.h`, `gte/src/metal/GEMetal.mm`, `gte/src/d3d12/GED3D12.cpp`, `gte/src/vulkan/GEVulkan.cpp`.

### Phase 1 — Public header

Create `gte/include/omegaGTE/GEIO.h` with the API above. Add include to `OmegaGTE.h`. Add the two virtual methods to `OmegaGraphicsEngine` in `GE.h` with `= 0` (forces all backends to compile-implement them).

**Files:** new `gte/include/omegaGTE/GEIO.h`, edit `gte/include/OmegaGTE.h`, edit `gte/include/omegaGTE/GE.h`.

### Phase 2 — Vulkan stub (unblock build first)

The two new pure-virtuals will break the Vulkan build until implemented. Stub them in `GEVulkan.cpp`: `openIOFile` returns a POSIX-fd-backed handle (so consumers can use it for the eventual fallback path), `makeIOCommandQueue` returns `nullptr`. Update CMake test target.

**Files:** new `gte/src/vulkan/GEVulkanIO.h`, `gte/src/vulkan/GEVulkanIO.cpp`, edit `gte/src/vulkan/GEVulkan.cpp`.

### Phase 3 — Metal MTLIO backend

Implement `GEMetalIOFileHandle`, `GEMetalIOCommandBuffer`, `GEMetalIOCommandQueue`. Wire `GEMetalEngine::openIOFile` and `makeIOCommandQueue`. The fence-signal path needs the existing `GEMetalFence` to expose its underlying `id<MTLSharedEvent>`; if it doesn't already, add a friend accessor.

**Compression handle caching.** Open `MTLIOFileHandle` lazily per (URL, compressionMethod) tuple inside the engine. Mapping: `LZ4 → MTLIOCompressionMethodLZ4`, `ZLib → MTLIOCompressionMethodZlib`, `LZBitmap → MTLIOCompressionMethodLZBitmap`, `LZFSE → MTLIOCompressionMethodLZFSE`, `None → MTLIOCompressionMethodRaw`. Reject `GDeflate` with a clear error.

**Files:** new `gte/src/metal/GEMetalIO.h`, `gte/src/metal/GEMetalIO.mm`, edit `gte/src/metal/GEMetal.mm`, edit `gte/src/metal/GEMetalCommandQueue.{h,mm}` to expose the GEMetalFence event.

### Phase 4 — D3D12 DirectStorage backend

Vendor DirectStorage under `gte/deps/DirectStorage/` and wire it through CMake (link `dstorage.lib`, copy `dstorage.dll` + `dstoragecore.dll` next to the test/output binaries). Implement `GED3D12IOFileHandle`, `GED3D12IOCommandBuffer` (the software batching wrapper), `GED3D12IOCommandQueue`. Initialize the factory in `GED3D12Engine`'s constructor; on failure log and leave `features.directIO = false`.

**Subresource index calculation** for textures uses `D3D12CalcSubresource`. The `TextureRegion → D3D12_BOX` translation already exists in `GED3D12Texture.cpp` for upload paths — reuse it.

**Files:** new `gte/src/d3d12/GED3D12IO.h`, `gte/src/d3d12/GED3D12IO.cpp`, edit `gte/src/d3d12/GED3D12.cpp`, edit `gte/CMakeLists.txt`.

### Phase 5 — Tests

Add three tests under `gte/tests/`:

1. `test_io_load_buffer.cpp` — write a known byte pattern to a temp file, open it via `openIOFile`, load into a `GEBuffer`, read back via the existing `Readback` path, assert byte-equality.
2. `test_io_load_texture.cpp` — same but into a 256×256 RGBA8 `GETexture`, read back via `getBytes`.
3. `test_io_compression.cpp` — gated on `features.directIOGPUDecompression`. On Metal, write an LZ4-compressed file via Apple's `compression_encode_buffer`. On D3D12, write a GDeflate-compressed file via `IDStorageCompressionCodec`. Load and verify.

The first two tests run on every backend. The third skips on Vulkan and on Metal/D3D12 when the feature flag is off, so the suite stays green on hardware that doesn't support it.

**Files:** new files under `gte/tests/`.

### Phase 6 — Documentation

Add a `GEIO` section to `gte/docs/API.rst` with the file/queue/buffer classes and the `GEIOCompression` enum. Cross-link from `GTEDeviceFeatures`. Add a short "When to use direct I/O" note to the README.

**Files:** `gte/docs/API.rst`, `gte/README.md`.

### Phase 7 — Consumer integration (separate PR)

Update the GEMesh / texture asset loaders (per `GEMesh-TextureAssets-Implementation-Plan.md`) to query `device->features.directIO` and use the new path when available. Out of scope for this plan but called out so the API surface is validated against a real consumer before it's frozen.

---

## Failure modes the design handles

| Scenario | Behavior |
|---|---|
| DirectStorage runtime not installed on Windows | `DStorageGetFactory` fails; `features.directIO = false`; `makeIOCommandQueue` returns nullptr; consumer falls back to `BufferDescriptor::Upload`. |
| macOS 12 (no MTLIO) | `@available` check fails; same fallback path. |
| Compression format unsupported on this backend | `enqueueLoad` logs and silently skips (returns without queuing). The `GEFence` still signals so consumers don't deadlock. Alternative: return a status code from `enqueueLoad`. **Open question — see below.** |
| File too short | Backend-specific error reported through the fence's completion status. The GEFence interface doesn't currently carry a status code; it would need an addition (`GEFence::getCompletionStatus()`) or a per-load callback. **Open question.** |
| Destination buffer too small | Same as above. |
| Disk error mid-load | Same as above. |

---

## Open Questions

1. **Error reporting granularity.** Today `GEFence` only carries "signaled at value N." DirectStorage and MTLIO both report per-request errors. Do we (a) add an `errorStatus` field to `GEFence`, (b) add a `GEIOCompletionCallback` per request, or (c) keep it coarse and have the consumer re-read the destination to validate? Recommendation: **(a)** — minimal API surface, maps to both backends' "queue-level error counter" concept.

2. **Compression mismatch handling.** When a consumer submits a `GDeflate` request to a Metal queue, do we (a) silently no-op, (b) crash, (c) return a status code from `enqueueLoad`, or (d) auto-fallback to CPU decompression on the I/O queue's worker thread? Recommendation: **(c)**, change the signature to `bool enqueueLoad(...)` and make consumers handle it. CPU fallback hides perf cliffs.

3. **File handle lifetime across the queue.** MTLIO requires the file handle to outlive the command buffer; DirectStorage requires the file to outlive the submit. The `SharedHandle<GEIOFileHandle>` inside `GEIOLoadRequest` makes that automatic, but it means each in-flight buffer holds a strong ref. Acceptable, or do we want a weak-ref + explicit lifetime contract? Recommendation: keep the strong ref — it's the standard OmegaGTE pattern (see `GERenderPipelineState` holding shader handles).

4. **Vulkan software fallback in v1.** Should the initial Vulkan implementation be the always-false stub, or should it ship with the CPU thread pool fallback enabled? Recommendation: **stub only**. Shipping a slow fallback under the same `directIO = true` flag would mislead consumers about performance characteristics. Add the fallback later as an explicit opt-in.

5. **Should `GEIOCommandQueue` be a peer of `GECommandQueue` or a sub-resource of it?** MTLIO's queue is independent of `MTLCommandQueue`; DirectStorage's queue is independent of `ID3D12CommandQueue`. They're separate hardware paths. Treating them as peers (the design above) matches the underlying APIs. The alternative — making `GECommandQueue` a base class with `submitIORequests` — would couple the two and make the Vulkan fallback awkward.
