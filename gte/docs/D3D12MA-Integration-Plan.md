# D3D12MemoryAllocator (D3D12MA) Integration Plan

## Goal

Replace the raw `CreateCommittedResource` / `CreatePlacedResource` / `CreateHeap` calls in the D3D12 backend with **D3D12MemoryAllocator** (D3D12MA), AMD's open-source suballocator for Direct3D 12. This mirrors the Vulkan backend's use of **VulkanMemoryAllocator** (VMA) and gives us block-based pooling, automatic suballocation, defragmentation support, and memory budget awareness — all things the current D3D12 backend handles manually or not at all.

## Current State

The D3D12 backend allocates GPU memory in two ways:

1. **Engine-level (`GED3D12Engine::makeBuffer` / `makeTexture`)** — Every resource is a committed resource via `d3d12_device->CreateCommittedResource()`. Each allocation creates an implicit heap sized to exactly one resource. This is simple but wasteful: the driver cannot share heaps across small allocations, leading to heap fragmentation and higher memory overhead.

2. **Heap-level (`GED3D12Heap::makeBuffer` / `makeTexture`)** — A monolithic `ID3D12Heap` is created via `CreateHeap()`, then resources are placed with `CreatePlacedResource()` using a linear bump allocator (`currentOffset += allocInfo.SizeInBytes`). This has no free-list, no fragmentation tracking, no overflow validation, and offsets never reclaim freed space.

Additionally:
- Each buffer and texture gets its own **per-resource descriptor heap** (1-element `ID3D12DescriptorHeap`), which is a separate issue but compounds the overhead.
- Resources are released immediately when their `shared_ptr` ref count hits zero — there is **no deferred deletion** to protect against GPU-in-flight reads.
- The Vulkan backend already uses VMA throughout (`vmaCreateBuffer`, `vmaCreateImage`, `vmaCreatePool`), so we have a proven pattern to follow.

## Non-Goals

- Changing the public `OmegaGraphicsEngine` / `GEHeap` API shape. D3D12MA is an internal implementation detail of the D3D12 backend.
- Consolidating per-resource descriptor heaps into shared descriptor tables (valuable but separate work).
- Adding deferred deletion / frame-based cleanup (orthogonal improvement; the plan notes where it would attach).
- Modifying the Vulkan or Metal backends.

---

## Phase 0: Dependency Setup

### 0.1 Vendor D3D12MA

D3D12MA is a header + source pair (similar to VMA being header-only, except D3D12MA has one `.cpp` file).

- Add the D3D12MA through AUTOMDEPS under `gte/deps/D3D12MemoryAllocator/`.
- The library lives at:
  - `gte/deps/D3D12MemoryAllocator/include/D3D12MemAlloc.h`
  - `gte/deps/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp`
- Minimum version: **2.0.1** (supports `ID3D12Device8`, which we already use).

### 0.2 CMake integration

In `gte/CMakeLists.txt`, inside the `if(WIN32)` block:

```cmake
set(D3D12MA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/D3D12MemoryAllocator")
target_include_directories("OmegaGTE" PRIVATE "${D3D12MA_DIR}/include")
target_sources("OmegaGTE" PRIVATE "${D3D12MA_DIR}/src/D3D12MemAlloc.cpp")
```

No additional link libraries — D3D12MA uses the same `d3d12.lib` and `dxgi.lib` already linked via `#pragma comment(lib,...)`.

**Files**: `gte/CMakeLists.txt`

---

## Phase 1: Allocator Lifetime — Create and Destroy

### 1.1 Add `D3D12MA::Allocator*` to `GED3D12Engine`

In `GED3D12.h`, add the include and member:

```cpp
#include "D3D12MemAlloc.h"
// ...
class GED3D12Engine : public OmegaGraphicsEngine {
    // ... existing members ...
    D3D12MA::Allocator* memAllocator = nullptr;
    // ...
};
```

### 1.2 Create the allocator during engine construction

In the `GED3D12Engine` constructor (`GED3D12.cpp`), after `d3d12_device` and `dxgi_factory` are set up:

```cpp
D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
allocatorDesc.pDevice = d3d12_device.Get();
allocatorDesc.pAdapter = /* the IDXGIAdapter the device was created from */;
allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED; // perf: skip zeroing
HRESULT hr = D3D12MA::CreateAllocator(&allocatorDesc, &memAllocator);
```

The `pAdapter` is available from the `GTED3D12Device` that is already passed into the constructor. Store or forward it.

### 1.3 Destroy the allocator in the engine destructor

```cpp
GED3D12Engine::~GED3D12Engine() {
    if (memAllocator) {
        memAllocator->Release();
        memAllocator = nullptr;
    }
}
```

**Note**: If the engine does not currently have a destructor, add one. All D3D12MA allocations must be freed before the allocator is released (COM ref counts on the device keep it alive, but leaking allocations triggers D3D12MA validation errors).

**Files**: `gte/src/d3d12/GED3D12.h`, `gte/src/d3d12/GED3D12.cpp`

---

## Phase 2: Buffer Allocation via D3D12MA

### 2.1 Add `D3D12MA::Allocation*` to `GED3D12Buffer`

```cpp
class GED3D12Buffer : public GEBuffer {
public:
    ComPtr<ID3D12Resource> buffer;
    D3D12MA::Allocation* d3d12maAllocation = nullptr;  // NEW
    // ... rest unchanged ...
    ~GED3D12Buffer() override {
        // emit tracking event (existing)
        if (d3d12maAllocation) {
            d3d12maAllocation->Release();
        }
    }
};
```

When D3D12MA creates a resource, it returns both the `ID3D12Resource*` and a `D3D12MA::Allocation*`. The allocation must be released to free the underlying memory.

### 2.2 Replace `CreateCommittedResource` in `GED3D12Engine::makeBuffer`

Current code (line 1337):
```cpp
hr = d3d12_device->CreateCommittedResource(&heap_prop, D3D12_HEAP_FLAG_NONE, &d3d12_desc, state, nullptr, IID_PPV_ARGS(&buffer));
```

Replace with:
```cpp
D3D12MA::ALLOCATION_DESC allocDesc = {};
allocDesc.HeapType = heap_type;  // already computed: UPLOAD / READBACK / DEFAULT

D3D12MA::Allocation* allocation = nullptr;
hr = memAllocator->CreateResource(
    &allocDesc,
    &d3d12_desc,      // D3D12_RESOURCE_DESC (already built)
    state,             // initial resource state (already computed)
    nullptr,           // optimized clear value (buffers: nullptr)
    &allocation,
    IID_PPV_ARGS(&buffer)
);
```

Then pass `allocation` into the `GED3D12Buffer` constructor. The resource pointer comes back the same way — D3D12MA wraps `CreateCommittedResource` or `CreatePlacedResource` internally depending on its block strategy.

### 2.3 Map/Unmap considerations

D3D12MA does not change how `Map`/`Unmap` works on the resource. Existing code that maps upload/readback buffers (`buffer->Map(0, &r, &dataPtr)`) continues to work unchanged, since the `ID3D12Resource*` is still the same COM object.

**Files**: `gte/src/d3d12/GED3D12.h`, `gte/src/d3d12/GED3D12.cpp`

---

## Phase 3: Texture Allocation via D3D12MA

### 3.1 Add `D3D12MA::Allocation*` to `GED3D12Texture`

```cpp
class GED3D12Texture : public GETexture {
public:
    // ... existing members ...
    D3D12MA::Allocation* d3d12maAllocation = nullptr;       // for the GPU texture
    D3D12MA::Allocation* d3d12maCpuSideAllocation = nullptr; // for the CPU-side staging buffer
    // ...
    ~GED3D12Texture() override {
        // emit tracking event (existing)
        if (d3d12maAllocation) d3d12maAllocation->Release();
        if (d3d12maCpuSideAllocation) d3d12maCpuSideAllocation->Release();
    }
};
```

### 3.2 Replace `CreateCommittedResource` in `GED3D12Engine::makeTexture`

There are **two** committed resource calls in `makeTexture`:

1. **GPU-side texture** (line 1219):
   ```cpp
   // BEFORE:
   hr = d3d12_device->CreateCommittedResource(&textureHeapProps, D3D12_HEAP_FLAG_SHARED, &d3d12_desc, states, nullptr, IID_PPV_ARGS(&texture));
   
   // AFTER:
   D3D12MA::ALLOCATION_DESC allocDesc = {};
   allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
   allocDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_SHARED; // preserve existing shared flag
   
   D3D12MA::Allocation* texAlloc = nullptr;
   hr = memAllocator->CreateResource(&allocDesc, &d3d12_desc, states, nullptr, &texAlloc, IID_PPV_ARGS(&texture));
   ```

2. **CPU-side staging buffer** (line 1229):
   ```cpp
   // BEFORE:
   hr = d3d12_device->CreateCommittedResource(&heap_prop, D3D12_HEAP_FLAG_NONE, &res, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cpuSideRes));
   
   // AFTER:
   D3D12MA::ALLOCATION_DESC cpuAllocDesc = {};
   cpuAllocDesc.HeapType = heap_prop.Type; // UPLOAD or READBACK depending on texture usage
   
   D3D12MA::Allocation* cpuAlloc = nullptr;
   hr = memAllocator->CreateResource(&cpuAllocDesc, &res, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &cpuAlloc, IID_PPV_ARGS(&cpuSideRes));
   ```

Pass both allocations into the `GED3D12Texture` constructor.

### 3.3 Render target and ray tracing resources

All other `CreateCommittedResource` calls in the D3D12 backend (render target back-buffers, acceleration structure buffers) follow the same pattern: build the `D3D12MA::ALLOCATION_DESC`, call `memAllocator->CreateResource()`, store the `D3D12MA::Allocation*` alongside the resource.

Swap chain back-buffers obtained via `IDXGISwapChain::GetBuffer` are **not** allocated by us and must NOT go through D3D12MA.

**Files**: `gte/src/d3d12/GED3D12Texture.h`, `gte/src/d3d12/GED3D12Texture.cpp`, `gte/src/d3d12/GED3D12.cpp`

---

## Phase 4: Heap (Pool) Allocation via D3D12MA

### 4.1 Replace `GED3D12Heap` internals with `D3D12MA::Pool`

This mirrors how the Vulkan backend uses `VmaPool` for `GEVulkanHeap`:

```cpp
class GED3D12Heap : public GEHeap {
    GED3D12Engine *engine;
    D3D12MA::Pool* pool = nullptr;  // replaces ComPtr<ID3D12Heap> + manual offset
    size_t poolSize;
public:
    GED3D12Heap(GED3D12Engine *engine, D3D12MA::Pool* pool, size_t size)
        : engine(engine), pool(pool), poolSize(size) {}
    ~GED3D12Heap() { if (pool) pool->Release(); }
    
    size_t currentSize() override { return poolSize; }
    SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override;
    SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override;
};
```

### 4.2 Replace `GED3D12Engine::makeHeap`

```cpp
SharedHandle<GEHeap> GED3D12Engine::makeHeap(const HeapDescriptor &desc) {
    D3D12MA::POOL_DESC poolDesc = {};
    poolDesc.HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    poolDesc.HeapFlags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    poolDesc.BlockSize = desc.len;    // single block of the requested size
    poolDesc.MaxBlockCount = 1;       // match current behavior: one contiguous block
    
    D3D12MA::Pool* pool = nullptr;
    HRESULT hr = memAllocator->CreatePool(&poolDesc, &pool);
    if (FAILED(hr)) {
        DEBUG_STREAM("Failed to create D3D12MA Pool");
        return nullptr;
    }
    return SharedHandle<GEHeap>(new GED3D12Heap(this, pool, desc.len));
}
```

### 4.3 Replace `GED3D12Heap::makeBuffer` and `makeTexture`

Both methods drop `CreatePlacedResource` + manual offset tracking. Instead:

```cpp
SharedHandle<GEBuffer> GED3D12Heap::makeBuffer(const BufferDescriptor &desc) {
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.CustomPool = pool;  // route allocation through this pool
    // ... build D3D12_RESOURCE_DESC and states the same way as today ...
    
    D3D12MA::Allocation* alloc = nullptr;
    ID3D12Resource* buffer = nullptr;
    HRESULT hr = engine->memAllocator->CreateResource(
        &allocDesc, &d3d12_desc, state, nullptr, &alloc, IID_PPV_ARGS(&buffer));
    // ... create descriptor views, return GED3D12Buffer with alloc ...
}
```

D3D12MA handles suballocation, alignment, and offset tracking within the pool block automatically.

**Files**: `gte/src/d3d12/GED3D12.h`, `gte/src/d3d12/GED3D12.cpp`

---

## Phase 5: Memory Budget and Statistics

### 5.1 Budget queries

D3D12MA exposes DXGI memory budget information:

```cpp
D3D12MA::Budget localBudget, nonLocalBudget;
memAllocator->GetBudget(&localBudget, &nonLocalBudget);
// localBudget.UsageBytes, localBudget.BudgetBytes, etc.
```

This can be wired into the existing `ResourceTracking::Tracker` system or exposed via a new engine method. Not required for correctness but valuable for diagnostics.

### 5.2 Statistics for debugging

```cpp
D3D12MA::TotalStatistics stats;
memAllocator->CalculateStatistics(&stats);
// stats.Total.Stats.BlockCount, AllocationCount, UsedBytes, UnusedBytes
```

Consider logging these at engine shutdown or on demand. D3D12MA also supports JSON dump via `BuildStatsString()` for detailed heap visualization.

**Files**: `gte/src/d3d12/GED3D12.cpp`, optionally `gte/src/common/GEResourceTracker.h`

---

## Phase 6: Validation and Testing

### 6.1 Existing tests

The tests under `gte/tests/directx/` (2DTest, ComputeTest, GPUTessTest) exercise buffer creation, texture creation, compute dispatch, and rendering. After the migration, these tests should pass identically — the public API has not changed.

### 6.2 New validation

- Enable **D3D12 Debug Layer** (`ID3D12Debug1`) alongside D3D12MA's internal validation. D3D12MA respects the debug layer and will surface any invalid resource usage.
- Verify that `D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED` does not cause issues with read-before-write patterns. If it does, remove the flag.
- Test explicit heap (`makeHeap`) path: create a heap, allocate several buffers and textures into it, use them in a render pass, release them.
- Verify that engine destruction cleans up all allocations before releasing the allocator (no D3D12MA leak warnings).

### 6.3 Performance comparison (optional)

Compare before/after on a workload that creates many small buffers (e.g. per-object constant buffers) to verify that D3D12MA's block suballocation reduces heap count and allocation time.

---

## Migration Summary

| Component | Before | After |
|---|---|---|
| `GED3D12Engine` | Raw `ID3D12Device8` calls | Holds `D3D12MA::Allocator*`, delegates resource creation |
| `GED3D12Buffer` | `ComPtr<ID3D12Resource>` only | + `D3D12MA::Allocation*` (released in destructor) |
| `GED3D12Texture` | `ComPtr<ID3D12Resource>` only | + `D3D12MA::Allocation*` for GPU and CPU-side resources |
| `GED3D12Heap` | Raw `ID3D12Heap` + linear bump alloc | `D3D12MA::Pool*` with automatic suballocation |
| `GED3D12Engine::makeBuffer` | `CreateCommittedResource` | `memAllocator->CreateResource` |
| `GED3D12Engine::makeTexture` | `CreateCommittedResource` (x2) | `memAllocator->CreateResource` (x2) |
| `GED3D12Engine::makeHeap` | `CreateHeap` | `memAllocator->CreatePool` |
| `GED3D12Heap::makeBuffer/Texture` | `CreatePlacedResource` + manual offset | `memAllocator->CreateResource` with `CustomPool` |
| Swap chain buffers | `GetBuffer` (no change) | `GetBuffer` (no change) |

## File Change Summary

| File | Changes |
|---|---|
| `gte/CMakeLists.txt` | Add D3D12MA include dir and source file |
| `gte/src/d3d12/GED3D12.h` | Include D3D12MemAlloc.h; add allocator to engine; add allocation to buffer/heap |
| `gte/src/d3d12/GED3D12.cpp` | Allocator init/destroy; replace all Create*Resource in makeBuffer, makeTexture, makeHeap, heap suballocation |
| `gte/src/d3d12/GED3D12Texture.h` | Add allocation members to texture class |
| `gte/src/d3d12/GED3D12Texture.cpp` | Release allocations in destructor |
| `gte/deps/D3D12MemoryAllocator/` | New vendored dependency (header + source) |

## Future Work (out of scope)

- **Deferred deletion queue**: Track GPU fence values and defer `Allocation::Release()` until the GPU has finished reading. D3D12MA does not handle this — it's the caller's responsibility. The `GED3D12Fence` infrastructure already exists; a ring buffer of `{fenceValue, allocation}` pairs at the command queue level would be the natural integration point.
- **Shared descriptor heap pools**: Replace per-resource 1-element descriptor heaps with large shader-visible descriptor tables. This is orthogonal to D3D12MA but compounds the memory savings.
- **Defragmentation**: D3D12MA supports defragmentation passes that compact placed resources within pools. Useful for long-running applications with dynamic resource churn.
