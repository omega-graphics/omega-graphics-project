# Shared Shader-Visible Descriptor Heap Plan (D3D12)

## Goal

Stop allocating a brand-new `ID3D12DescriptorHeap` per GPU resource. Move
the D3D12 backend to a small set of long-lived, shared shader-visible
heaps with handle-based suballocation, plus a transient ring for
one-shot dispatches. The destination state:

- `GED3D12Engine::makeBuffer` creates zero descriptor heaps.
- `GED3D12Engine::makeTexture` creates zero shader-visible CBV/SRV/UAV
  heaps; it suballocates from a shared one.
- `makeSamplerState` suballocates from a shared SAMPLER heap.
- One-shot compute paths (tessellation, mipmap generation) draw from a
  per-queue transient ring instead of a fresh heap per dispatch.

## Why now

The proximate trigger is a runtime crash. During a fast WTK window
resize (`BasicAppTest`), the compositor worker thread rebuilds frames
continuously, the `BufferPool` misses constantly, and
`GED3D12Engine::makeBuffer` is called many tens of times per frame.
Each call attempts `CreateDescriptorHeap(NumDescriptors=1,
SHADER_VISIBLE, CBV_SRV_UAV)`. Under enough pressure that allocation
transiently fails, and prior to the safety patch in
`gte/src/d3d12/GED3D12.cpp:2807-2865` the empty `if(FAILED(hr)){}` block
fell straight through to `descHeap->GetCPUDescriptorHandleForHeapStart()`
on a `nullptr` — a driver-thread access violation reading address
`0x00000000`. The graceful-degradation path now in place returns
`nullptr` from `makeBuffer` and `emitTextSubRun` (WTK side) silently
skips the subrun. That is a band-aid: text drops for a few frames
mid-drag instead of crashing.

The underlying defect is that the engine is allocating a discrete OS-
backed descriptor heap per resource for a per-shader-binding model that
does not even read those heaps on the hot path. Removing the wasteful
per-buffer heap deletes the source of the failure entirely. The texture
and sampler paths follow the same wasteful pattern at lower frequency;
moving them onto a shared heap is the architectural cleanup that pulls
descriptor-heap allocation out of the hot path for good.

## Current state

Per-resource shader-visible heap creation lives in five hot spots in
`gte/src/d3d12/`:

| Site | What it allocates | Frequency |
|------|-------------------|-----------|
| `GED3D12Engine::makeBuffer` (`GED3D12.cpp:2807-2865`) | 1-descriptor SHADER_VISIBLE CBV/SRV/UAV heap, SRV or UAV view written into it | Every `BufferPool` miss. Hot during WTK resize. |
| `GED3D12Heap::makeBuffer` (`GED3D12.cpp:301-336`) | Same, D3D12MA-pool variant | Same path through the user-facing heap pool |
| `GED3D12Engine::makeTexture` / `Heap::makeTexture` (`GED3D12.cpp:418-519`, `2667-2738`) | 1-descriptor SHADER_VISIBLE SRV heap; separate non-shader-visible RTV / DSV / UAV heaps | Per texture creation |
| `GED3D12Engine::makeSamplerState` (`GED3D12.cpp:3095-3109`) | 1-descriptor SHADER_VISIBLE SAMPLER heap | Per sampler |
| `D3D12TEContext::*` (`D3D12TEContext.cpp:264-272`), `GED3D12CommandBuffer::generateMipmaps` (`GED3D12CommandQueue.cpp:576-580`) | 2-N descriptor SHADER_VISIBLE CBV/SRV/UAV heap | Per one-shot compute dispatch |

Per-target RTV / DSV heaps (`GED3D12.cpp:2109`, `2126`, `2131`) are
fewer, larger, and lifetime-tied to a render target or swap chain — not
the churn source — and stay as-is in this plan.

### The buffer-heap is unread on the hot path

The buffer-bind sites in `GED3D12CommandQueue.cpp:1189-1330` and
`1658-1730` bind by *GPU virtual address*, not by descriptor:

```cpp
commandList->SetDescriptorHeaps(1, d3d12_buffer->bufferDescHeap.GetAddressOf());     // line 1211
const auto rootParam = getRootParameterIndexOfResource(...);
if (role == Uniform)                              SetGraphicsRootConstantBufferView (rootParam, buffer->GetGPUVirtualAddress());
else if (state & NON_PIXEL_SHADER_RESOURCE)       SetGraphicsRootShaderResourceView (rootParam, buffer->GetGPUVirtualAddress());
else                                              SetGraphicsRootUnorderedAccessView(rootParam, buffer->GetGPUVirtualAddress());
```

`SetGraphicsRoot{Constant,ShaderResource,UnorderedAccess}View` take a
GPU virtual address directly and do not consult any descriptor heap.
The SRV / UAV view written into the buffer's heap inside `makeBuffer`
is therefore *never sampled* by any shader the runtime issues. The
`SetDescriptorHeaps(1, bufferDescHeap)` call additionally evicts
whichever shader-visible heap a previously-bound texture or sampler
required, so removing the per-buffer heap also fixes a latent draw-
state-corruption bug where a buffer-bind between a texture-bind and a
`Draw*` invalidated the texture's descriptor table.

The one genuine reader of `bufferDescHeap` anywhere in the codebase is
`GED3D12CommandBuffer::fillBuffer` (`GED3D12CommandQueue.cpp:687-698`),
which uses `ClearUnorderedAccessViewUint` — a one-off path that
genuinely needs a matched (CPU, GPU) UAV-handle pair. Phase 1 routes
that single consumer through a dedicated engine-owned helper heap and
deletes everything else.

## Phase 1 — Retire the per-buffer descriptor heap

### Goal

Zero `CreateDescriptorHeap` calls inside any code path leading from
`makeBuffer`. `GED3D12Buffer::bufferDescHeap` deleted. The `fillBuffer`
ClearUAV path serviced by a small engine-owned helper heap created once
at engine init.

### Concrete changes

- `GED3D12Engine::makeBuffer` (`gte/src/d3d12/GED3D12.cpp:2807-2865`)
  delete the descriptor heap creation block and the SRV / UAV
  `CreateShaderResourceView` / `CreateUnorderedAccessView` calls. The
  `GED3D12Buffer` constructor takes one less argument.
- `GED3D12Heap::makeBuffer` (`gte/src/d3d12/GED3D12.cpp:301-336`)
  same delete, for the D3D12MA-pool path.
- `GED3D12.h` — drop
  `ComPtr<ID3D12DescriptorHeap> bufferDescHeap` from `GED3D12Buffer`
  (line 35), drop it from the constructor signature (line 58).
- `GED3D12CommandQueue.cpp:1211, 1319, 1662` — delete the three
  `commandList->SetDescriptorHeaps(1, d3d12_buffer->bufferDescHeap.GetAddressOf())`
  lines. They were vestigial *and* clobbering the prior texture /
  sampler heap.
- `GED3D12CommandBuffer::fillBuffer` (`GED3D12CommandQueue.cpp:687-698`)
  replace `buf->bufferDescHeap.Get()` with a slot pulled from
  `engine->clearUavHelperHeap` — a single 1-descriptor shader-visible
  CBV/SRV/UAV heap created once at engine init and reused for every
  ClearUAV call. The UAV is rewritten in place for each fill (the
  previous fill is already complete by the time the next one runs;
  `ClearUnorderedAccessViewUint` reads the descriptor synchronously on
  record). If concurrent fills across command buffers are possible, the
  helper heap upgrades to a small ring keyed by command-buffer fence
  value — but the current call site is single-threaded per queue, so
  one slot suffices for v1.

### Risk

Low. No public API change. The buffer's private heap field disappears
and `fillBuffer` redirects to a dedicated helper. The three deleted
`SetDescriptorHeaps` calls in the bind path eliminate a latent state-
corruption bug (a buffer-bind between a texture-bind and a draw was
silently swapping out the texture's descriptor table).

### Validator

- `wtk/tests/BasicAppTest` resize drag produces zero
  `Failed to Create D3D12 Descriptor Heap for Buffer` debug lines and
  zero dropped text subruns.
- Existing GTE compute fill tests still pass.
- `ResourceTrace` shows no `CreateDescriptorHeap` events from the
  buffer-creation path.
- New micro-test: allocate 10,000 buffers in a tight loop, confirm no
  descriptor heap is created and total allocator-side bytes stay flat.

### Sequencing

Independent. Lands alone. Required prerequisite for Phase 2 only in the
sense that Phase 2's shared CBV/SRV/UAV heap should not have to serve
buffer descriptors it never needed.

## Phase 2 — Shared shader-visible CBV/SRV/UAV heap for textures and samplers

### Goal

One device-wide shader-visible CBV/SRV/UAV heap (initial size 64K
descriptors; D3D12 Tier 1 hardware caps at 1M, Tier 2/3 at 1M+) and one
shader-visible SAMPLER heap (D3D12 caps at 2048). Texture and sampler
creation suballocate slots; bind sites set the *one* engine-wide heap
once per command list and use `SetGraphicsRootDescriptorTable` with a
GPU handle inside it. Per-resource shader-visible heaps deleted.
Per-target RTV / DSV heaps stay as-is (they are non-shader-visible, a
separate heap type, and not under churn pressure).

### Design

#### New types

`gte/src/d3d12/D3D12DescriptorAllocator.{h,cpp}` (new file pair) owns:

```cpp
struct D3D12DescriptorHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    std::uint32_t index;
    std::uint32_t count;
};

class D3D12DescriptorAllocator {
public:
    D3D12DescriptorAllocator(ID3D12Device *device,
                             D3D12_DESCRIPTOR_HEAP_TYPE type,
                             UINT capacity);

    // Reserve `count` contiguous slots. count == 1 for a single SRV /
    // UAV / CBV / SAMPLER; count > 1 for the rare cases the
    // tessellation context records adjacent SRV+UAV (Phase 3 uses
    // the transient ring for that; Phase 2 only needs count == 1).
    D3D12DescriptorHandle allocate(UINT count = 1);

    // Mark slot for deferred-free against the given retention fence
    // value. Slot returns to the free list once the fence has signaled
    // at or above this value. Drained by drainCompleted(fenceValue).
    void freeDeferred(D3D12DescriptorHandle handle, std::uint64_t fenceValue);

    void drainCompleted(std::uint64_t completedFenceValue);

    ID3D12DescriptorHeap *heap() const;

private:
    // Free list. Allocation pops; deferred-free pushes onto the
    // pending list keyed by fence value. drainCompleted moves entries
    // whose fence has passed from pending to free.
    std::mutex mutex_;
    std::vector<UINT> freeIndices_;
    std::vector<std::pair<std::uint64_t, UINT>> pendingFrees_;
    UINT incrementSize_;
    UINT capacity_;
    UINT nextFreshIndex_;
    ComPtr<ID3D12DescriptorHeap> heap_;
};
```

Free-list allocator (not bump-allocator) because texture lifetimes
overlap and slots are reused as textures release. `freeDeferred` rides
the retention-fence value already tracked by `GED3D12CommandQueue`
(`GED3D12CommandQueue.cpp:1985, 2009` via
`engine->retentionQueue.drainCompleted()`); descriptor slot frees use
the same fence value as the buffer / texture they were attached to.

#### Engine wiring

`GED3D12Engine` (`GED3D12.h:148`) gains two allocators:

```cpp
std::unique_ptr<D3D12DescriptorAllocator> resourceDescriptorAllocator;  // CBV/SRV/UAV, capacity 65536
std::unique_ptr<D3D12DescriptorAllocator> samplerDescriptorAllocator;   // SAMPLER, capacity 2048
```

Created at engine init. Released at engine teardown after `WaitForGPU`.

#### Texture path

`GED3D12Texture` (`GED3D12.h`, currently holds four `ComPtr<ID3D12DescriptorHeap>`
members) replaces its shader-visible `descHeap`, `uavDescHeap`
fields with `D3D12DescriptorHandle` values into
`resourceDescriptorAllocator`. The non-shader-visible `rtvDescHeap` and
`dsvDescHeap` stay as per-texture heaps (RTV / DSV are a different heap
type, non-shader-visible, and not under churn pressure).

`GED3D12Engine::makeTexture` (`GED3D12.cpp:418-519`) replaces:

```cpp
hr = engine->d3d12_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&descHeap));
engine->d3d12_device->CreateShaderResourceView(texture, &res_view_desc, descHeap->GetCPUDescriptorHandleForHeapStart());
```

with:

```cpp
auto srvHandle = engine->resourceDescriptorAllocator->allocate();
engine->d3d12_device->CreateShaderResourceView(texture, &res_view_desc, srvHandle.cpu);
// store srvHandle on GED3D12Texture
```

`GED3D12Heap::makeTexture` (`GED3D12.cpp:2667-2738`) gets the same
treatment for its `descHeap` (SRV slot) and `uavDescHeap` (UAV slot)
fields.

The swizzle-cache path
`GED3D12Texture::getOrCreateSwizzledSrvHeap`
(`GED3D12CommandQueue.cpp:1273`) becomes
`getOrCreateSwizzledSrvHandle` — allocates a slot from the shared heap
on demand, caches the handle keyed by the swizzle pattern.

`GED3D12Texture` destructor calls
`resourceDescriptorAllocator->freeDeferred(srvHandle, currentRetentionFenceValue)`
for each owned slot.

#### Sampler path

`GED3D12SamplerState` (`GED3D12.h:108-115`) replaces its `descHeap`
field with a `D3D12DescriptorHandle` into `samplerDescriptorAllocator`.

`GED3D12Engine::makeSamplerState` (`GED3D12.cpp:3095-3109`) replaces
the per-sampler `CreateDescriptorHeap` with
`samplerDescriptorAllocator->allocate()`.

#### Bind path

`GED3D12CommandBuffer` (`GED3D12CommandQueue.h:41-51`) gains tracking
for "active shared resource heap" and "active shared sampler heap":

```cpp
ID3D12DescriptorHeap *currentResourceDescHeap = nullptr;
ID3D12DescriptorHeap *currentSamplerDescHeap = nullptr;
```

These already exist; today they track per-resource heaps. After Phase
2 they always point at the engine's shared heaps (set once at the top
of a render pass / dispatch). The existing
`GED3D12CommandBuffer::rebindDescriptorHeaps`
(`GED3D12CommandQueue.cpp:214-220`) is already the right shape — it
calls `SetDescriptorHeaps(n, heaps)` with the active resource +
sampler heap. Phase 2 changes what gets stored into those two pointers
(always the shared heaps) and what the descriptor table call uses (the
texture's / sampler's stored GPU handle inside the shared heap).

`bindResourceAtVertexShader(GETexture)` and its fragment / compute
peers (`GED3D12CommandQueue.cpp:1226-1280, 1330-1390, etc.`) replace:

```cpp
ID3D12DescriptorHeap *heapToBind = d3d12_texture->srvDescHeap.Get();
if((... & NON_PIXEL_SHADER_RESOURCE) && !effective.isIdentity()){
    heapToBind = d3d12_texture->getOrCreateSwizzledSrvHeap(...);
    cpuDescHandle = heapToBind->GetGPUDescriptorHandleForHeapStart();
}
currentResourceDescHeap = heapToBind;
rebindDescriptorHeaps();
commandList->SetGraphicsRootDescriptorTable(rootParam, cpuDescHandle);
```

with:

```cpp
D3D12_GPU_DESCRIPTOR_HANDLE gpu = d3d12_texture->srvHandle.gpu;
if((... & NON_PIXEL_SHADER_RESOURCE) && !effective.isIdentity()){
    gpu = d3d12_texture->getOrCreateSwizzledSrvHandle(parentQueue->engine, effective).gpu;
}
currentResourceDescHeap = parentQueue->engine->resourceDescriptorAllocator->heap();
rebindDescriptorHeaps();
commandList->SetGraphicsRootDescriptorTable(rootParam, gpu);
```

The sampler peer
(`GED3D12CommandQueue.cpp:1282-1295, etc.`) does the analogous swap.

### Risk

Medium. Touches every texture / sampler bind site. The load-bearing
correctness invariant is: *a descriptor slot must not be reallocated
to a new owner until every command list that referenced its previous
owner has retired.* The retention-fence machinery already tracks this
for the underlying ID3D12Resource; descriptor frees ride the same fence
value.

Failure modes if the invariant is wrong:
- Slot reused too early: a queued command list samples the new owner's
  view in place of the old owner's. D3D12 debug layer will flag this
  as a descriptor / resource mismatch.
- Slot leaked: capacity drift. Allocator capacity exhaustion fires the
  Phase 1 graceful-degradation path (`makeTexture` returns nullptr).

Mitigation: debug-build invariant on the allocator —
`#ifdef _DEBUG`, every slot carries a fence value at last-free and an
assertion that the command queue's `getCompletedValue()` is at-or-above
that value at next allocation.

### Validator

- All existing GTE texture / SDF / text / sampler tests pass.
- New test: allocate and immediately drop 10,000 textures with frames
  in flight. Confirm the fence-gated free list holds slots until the
  fence has passed; no descriptor mismatch errors from the D3D12 debug
  layer.
- `BasicAppTest` resize drag: zero descriptor-heap creations per frame
  in `ResourceTrace`. The only `CreateDescriptorHeap` events visible
  during a run come from engine init, swap-chain creation, and per-
  target RTV / DSV creation.
- `BasicAppTest` text rendering: previously-latent draw-state-
  corruption bug (buffer-bind between a texture-bind and a draw)
  cannot recur — confirmed by setting up a stress scene that
  interleaves texture and buffer binds and counting D3D12 debug-layer
  warnings.

### Sequencing

Depends on Phase 1. Lands as a separate plan-scoped change once Phase
1 is in.

## Phase 3 — Transient descriptor ring for one-shot dispatches

### Goal

`D3D12TEContext` tessellation dispatches and `generateMipmaps` use a
per-queue transient ring instead of allocating a fresh heap per call.

### Design

Add a small ring (initial size 4096 descriptors) to
`GED3D12CommandQueue` as a third allocator instance, recycled when the
command queue's retention fence advances past a recorded value:

```cpp
class GED3D12CommandQueue {
    ...
    std::unique_ptr<D3D12DescriptorAllocator> transientRing;
    ...
};
```

Slot allocation in the ring is bump-only (no per-slot free list); the
entire ring's read cursor advances when the fence retires the
submission that recorded the slots. Equivalent to D3D12MA's
"transient" allocator pattern.

`D3D12TEContext::*` (`D3D12TEContext.cpp:264-310`) and
`generateMipmaps` (`GED3D12CommandQueue.cpp:576`) request `N`
consecutive descriptor slots from the ring, populate them inline,
bind the *ring's* heap via `SetDescriptorHeaps`, dispatch. No
`CreateDescriptorHeap`.

### Risk

Low. Both call sites are self-contained and already follow a "create
heap → write descriptors → bind → dispatch → throw away" pattern.
Descriptor lifetime ends at the dispatch's fence value, which is
exactly what the ring expects.

### Validator

- Existing GTE tessellation + mipmap tests pass.
- `ResourceTrace` shows no per-dispatch `CreateDescriptorHeap` events
  during a tessellation-heavy or mipmap-generation-heavy run.

### Sequencing

Optional polish. Can land in any order after Phase 1. Doesn't gate
Phase 2 and isn't gated by Phase 2.

## Out of scope

- Cross-backend (Vulkan, Metal) descriptor management. Metal does not
  use the descriptor-heap model at all (`MTLArgumentEncoder` /
  argument buffers serve a similar role but the API is different).
  Vulkan's `VkDescriptorPool` already pools allocations — current
  Vulkan code already shares pools per command buffer. This plan is
  D3D12-only.
- Per-target RTV and DSV heaps (`GED3D12RenderTarget`,
  `GED3D12Texture` RTV / DSV fields). Non-shader-visible, different
  heap type, lifetime tied to the render target, low churn. Not the
  bug source.
- The WTK-side buffer-pool sizing / Phase G content cache that would
  reduce buffer-allocation pressure upstream. Tracked in
  [UIView-Render-Redesign-Plan.md Phase G](../../wtk/.plans/UIView-Render-Redesign-Plan.md);
  orthogonal to and complementary with the work here.

## Open questions

1. **Initial capacities.** 65K for CBV/SRV/UAV and 2048 for SAMPLER are
   reasonable starting points for a single-window WTK app. A multi-
   window or tab-heavy app may exhaust 2048 samplers if shaders bind
   many distinct sampler variants per frame. Profile before raising
   the SAMPLER cap — D3D12's hardware cap is 2048 per heap and a second
   heap costs a `SetDescriptorHeaps` re-issue per bind.
2. **Allocator growth.** v1 is fixed-capacity. If the allocator
   exhausts, `makeTexture` returns nullptr (Phase 1 graceful-degradation
   path is the same fall-through). A future enhancement could detect
   exhaustion, allocate a second heap, and have `currentResourceDescHeap`
   track which heap a given handle belongs to. Not in scope here.
3. **Multi-queue.** [CommandQueue-Typed-Pool-Plan.md](CommandQueue-Typed-Pool-Plan.md)
   is in flight. If queues become first-class, the transient ring in
   Phase 3 should follow that ownership model (one ring per queue, not
   one global ring). Phase 3 land-order should be after the typed-pool
   plan if it lands first; otherwise the per-queue ring is the natural
   implementation regardless.
