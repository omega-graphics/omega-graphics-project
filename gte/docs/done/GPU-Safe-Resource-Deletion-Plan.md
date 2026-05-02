# GPU-Safe Resource Deletion Plan

## Goal

Introduce an engine-owned **retention queue** in OmegaGTE that defers resource releases until the GPU has provably finished reading the underlying memory. This fixes a latent bug in the Vulkan backend (VMA buffers/images are released immediately in their destructors, ignoring the existing render-pass deferred-destroy queue) and pre-empts the same bug in the D3D12 backend before the D3D12MA migration makes it acutely dangerous.

Metal needs nothing — `MTLCommandBuffer`'s implicit retain-until-completion plus ARC handle resource lifetime correctly.

## Current State

### D3D12 (`gte/src/d3d12/GED3D12CommandQueue.cpp`)

- `retainedCommandBuffers` and `retainedDescriptorHeaps` are populated during encoding and submission.
- Both are **cleared immediately after `ExecuteCommandLists` returns** (lines 1470–1471 and 1523–1524) — *not* fence-gated. The shared_ptrs/ComPtrs decrement, destructors run, GPU may still be reading.
- The queue carries `ComPtr<ID3D12Fence> fence`, but it is only used by `commitToGPUAndWait` (lines 1530–1533) to perform a full-blocking wait. There is no rolling fence value tracking submitted work.

### Vulkan (`gte/src/vulkan/GEVulkanCommandQueue.cpp`)

- `deferredRenderPassDestroys` and `deferredFramebufferDestroys` accumulate render-pass / framebuffer handles during encoding.
- `flushDeferredDestroys()` is called from `commitToGPUPresent` (line 2082) and `commitToGPUAndWait` (line 2159) **after** `vkWaitForFences` — these two paths are GPU-safe.
- `commitToGPU` (line 1974, the fire-and-forget path) submits with `VK_NULL_HANDLE` fence and never calls `flushDeferredDestroys`. Render-pass destruction is deferred until the *next* fenced submit happens, which is fine for long-lived render targets but breaks for any per-frame VMA-backed resource.
- VMA-allocated buffers and images call `vmaDestroyBuffer`/`vmaDestroyImage` directly in their destructors (`gte/src/vulkan/GEVulkan.h:188` and `:203`), bypassing the deferred queue entirely. **This is a latent bug today**, independent of D3D12MA work.

### Metal (`gte/src/metal/`)

No mechanism — and none needed. ARC plus `MTLCommandBuffer` retain-until-completion semantics handle this correctly.

## Design

### Engine-level ownership

The retention queue lives on the engine, not on individual command queues. Rationale: resources are created and destroyed at the engine level, and users may legitimately use the same resource across multiple command queues. Per-queue retention would either duplicate state (one `shared_ptr` retain per queue) or fail to handle cross-queue use safely.

### Fence-gate abstraction

A `FenceGate` is a `std::function<bool()>` returning true once a specific GPU signal has fired. Backends supply concrete callables:

- **D3D12**: `[fence, value]() { return fence->GetCompletedValue() >= value; }` against the queue's `ID3D12Fence`.
- **Vulkan**: `[device, sem, value]() { uint64_t v; vkGetSemaphoreCounterValue(device, sem, &v); return v >= value; }` against a per-queue **timeline semaphore** (`VK_KHR_timeline_semaphore`, core in 1.2). The Vulkan engine's device-creation path will need to enable this feature; that change is part of Slice 3.

The retention queue holds N gates per entry. An entry is releasable only when *every* gate reports signaled, which makes cross-queue retention work naturally: a resource bound to queues A and B gets two gates, one per queue.

### Retention entry

```cpp
struct Entry {
    std::vector<FenceGate> gates;
    std::function<void()>  release;
};
```

The release is opaque. The most common form captures a `SharedHandle<T>` by value, so dropping the lambda runs the resource's destructor at the GPU-safe moment. For backend-specific handles (`D3D12MA::Allocation*`, `VkRenderPass`, etc.), a release captures the handle plus a destroy call — this is what lets us subsume the existing Vulkan render-pass / framebuffer queue into the new infrastructure.

### Threading

`enqueue()` and `drainCompleted()` are safe to call concurrently. An internal mutex protects the deque. The mutex is dropped *before* user releases run, so a release that itself calls `enqueue()` (e.g., a destructor cascade) cannot deadlock.

### Lifecycle

1. **Encode/submit**: when a command-buffer encoder records GPU work that uses a resource, it calls `engine->retentionQueue.retainShared(handle, gates)` with gates representing the queue(s) the work was submitted on. The fence value used is the **next** value the queue will signal — captured by the gate closure, satisfied when that submit's fence fires.
2. **Drain**: every command-queue commit calls `engine->retentionQueue.drainCompleted()` after submit (cheap — just a fence-status query per pending entry). Drain may also be called on idle ticks if the engine has them.
3. **Shutdown**: engine destructor blocks each command queue until idle, then calls `engine->retentionQueue.drainAll()` to release everything. Only then are backend-level allocators (`D3D12MA::Allocator*`, `VmaAllocator`) released. Releasing the allocator before draining trips D3D12MA's leak validation; for VMA it leaks GPU memory.

### What does *not* change

- Resource wrapper destructors stay simple: they call `Release()` / `vmaDestroyBuffer` directly. The deferral comes from the retention queue holding a `shared_ptr` to the wrapper, so the destructor doesn't *run* until the GPU is provably done.
- Encoders' existing retain call sites (`retainedCommandBuffers.push_back(...)`, `deferredRenderPassDestroys.push_back(...)`) get rewritten to call `engine->retentionQueue.retainShared(...)` / `engine->retentionQueue.enqueue(...)`. The intent is identical; only the storage and gating change.

---

## Slices

### Slice 1 — Shared infrastructure (this slice)

Add `gte/src/common/GERetentionQueue.h` and `gte/src/common/GERetentionQueue.cpp` containing the `OmegaGTE::Retention::Queue` class. No backend wiring yet; this slice exists in isolation and compiles into `OmegaGTE` as a no-op (the file glob in `gte/CMakeLists.txt:11` picks it up automatically).

**Files**:
- `gte/src/common/GERetentionQueue.h` — new
- `gte/src/common/GERetentionQueue.cpp` — new

**Tests**: none yet — Slice 1 is data-structure only. Behavioral tests go in with Slice 2.

### Slice 2 — D3D12 wiring

Replace the immediate-clear retain pattern in `GED3D12CommandQueue` with retention-queue calls. Specifically:

1. Add a `retentionQueue` member to `GED3D12Engine` (type `OmegaGTE::Retention::Queue`).
2. Add an `ID3D12Fence` + monotonic `nextSubmitValue` to `GED3D12CommandQueue` for retention tracking. Distinct from the existing `commitToGPUAndWait` fence — that fence's binary 0/1 use is preserved.
3. At every `ExecuteCommandLists` call site (lines 1468, 1475, 1520), `Signal(retentionFence, ++nextSubmitValue)` and bind the captured value into a closure used as a gate.
4. Convert call sites that today push to `retainedCommandBuffers` / `retainedDescriptorHeaps` to call `engine->retentionQueue.retainShared(handle, {gateForThisQueue})`. Remove the immediate `.clear()` calls at lines 1470–1471 and 1523–1524.
5. `commitToGPU` and `submitCommandBuffer` call `engine->retentionQueue.drainCompleted()` once after submission.

**Files**:
- `gte/src/d3d12/GED3D12.h` — add `Retention::Queue retentionQueue` to `GED3D12Engine`
- `gte/src/d3d12/GED3D12CommandQueue.h` — add retention fence + counter; remove `retainedCommandBuffers` / `retainedDescriptorHeaps`
- `gte/src/d3d12/GED3D12CommandQueue.cpp` — rewire submit paths

**Tests**: existing tests under `gte/tests/directx/` should pass identically. Behavior is unchanged in the happy path; only the *timing* of releases shifts later, which the tests do not observe.

### Slice 3 — Vulkan wiring

Replace `deferredRenderPassDestroys` / `deferredFramebufferDestroys` and add VMA-allocation gating.

1. Enable `VK_KHR_timeline_semaphore` in the Vulkan engine's device creation. Confirm whether the Vulkan SDK already advertises Vulkan 1.2 (in which case it's core); if 1.1, request the extension and set `VkPhysicalDeviceTimelineSemaphoreFeatures`.
2. Add a `Retention::Queue retentionQueue` member to `GEVulkanEngine`.
3. Add a timeline `VkSemaphore` + monotonic `nextSubmitValue` to `GEVulkanCommandQueue`.
4. In every `vkQueueSubmit` call site (lines 2011, 2068, 2144), include a `VkTimelineSemaphoreSubmitInfo` that signals `nextSubmitValue + 1` on the timeline. Increment the local counter.
5. Replace `deferredRenderPassDestroys.push_back(rp)` / `deferredFramebufferDestroys.push_back(fb)` with `engine->retentionQueue.enqueue(gates, [device, rp]{ vkDestroyRenderPass(device, rp, nullptr); })` etc.
6. Convert `GEVulkanBuffer::~GEVulkanBuffer` and `GEVulkanTexture::~GEVulkanTexture` to enqueue `vmaDestroyBuffer`/`vmaDestroyImage` releases against the retention queue rather than calling them inline. Resources track their own gates: when the resource is bound by an encoder, the encoder records the gate on the resource; when the destructor runs, those gates are passed to the retention queue.
7. `commitToGPU`, `commitToGPUPresent`, `commitToGPUAndWait` all call `engine->retentionQueue.drainCompleted()` after submit (replacing the existing `flushDeferredDestroys()` calls).
8. Delete `flushDeferredDestroys`, `deferredRenderPassDestroys`, `deferredFramebufferDestroys` from `GEVulkanCommandQueue`.

**Files**:
- `gte/src/vulkan/GEVulkan.h` — add `Retention::Queue retentionQueue` to engine; convert `GEVulkanBuffer` / `GEVulkanTexture` destructors to enqueue
- `gte/src/vulkan/GEVulkan.cpp` — enable timeline semaphores in device creation
- `gte/src/vulkan/GEVulkanCommandQueue.h` — add timeline semaphore + counter; remove deferred-destroy vectors
- `gte/src/vulkan/GEVulkanCommandQueue.cpp` — rewire submit paths and render-pass / framebuffer cleanup
- `gte/src/vulkan/GEVulkanRenderTarget.cpp` — update render-pass/framebuffer enqueue call sites (currently lines 316 and 321)

**Tests**: existing Vulkan tests should pass. New small test: allocate a buffer, submit work using it, drop the user-side handle immediately, verify `vmaDestroyBuffer` does *not* run before the fence signals.

### Slice 4 — Engine shutdown drain

Both engine destructors must:

1. Wait until every command queue is idle (D3D12: `Signal` + `WaitForSingleObject`; Vulkan: `vkDeviceWaitIdle`).
2. Call `retentionQueue.drainAll()`.
3. Then proceed with existing destruction (`memAllocator->Release()`, `vmaDestroyAllocator`, etc.).

The `drainAll()` call ignores gates because the previous step guaranteed they're all signaled. This keeps the contract clean: gate-respecting drain in steady state, unconditional drain only at shutdown.

**Files**:
- `gte/src/d3d12/GED3D12.cpp` — extend `~GED3D12Engine` (currently empty after Phase 1) to wait + drain before `memAllocator->Release()`
- `gte/src/vulkan/GEVulkan.cpp` — same pattern for `~GEVulkanEngine`

---

## API summary

```cpp
namespace OmegaGTE::Retention {

using FenceGate = std::function<bool()>;

class Queue {
public:
    Queue();
    ~Queue();   // asserts entries_.empty(); caller must drainAll() first

    void enqueue(std::vector<FenceGate> gates,
                 std::function<void()> release);

    template <class T>
    void retainShared(SharedHandle<T> handle,
                      std::vector<FenceGate> gates);

    std::size_t drainCompleted();   // gate-respecting; called per-submit
    std::size_t drainAll();         // unconditional; only at shutdown
    std::size_t pending() const;    // diagnostic

    // non-copyable, non-movable
};

}
```

## Decisions made

- **Engine-level retention** (not per-queue). Resources can be used cross-queue; retention follows the resource, not the submission.
- **Timeline semaphores** for Vulkan — matches D3D12's monotonic-value model. The capability change is local to the Vulkan device creation path.
- **`std::function`-based releases** rather than typed unions. Slightly more allocation per entry, but encoder call sites stay simple and the surface area is small.
- **No automatic drain on `~Queue`** — by contract the engine destructor calls `drainAll()` first. A non-empty `Queue` at destruction is a programmer bug; debug builds assert, release builds silently leak (the alternative — running releases when the GPU may not be idle — is the exact bug we're trying to prevent).

## Future work

- **Idle-tick drain**: today drain happens only on submit. If a workload submits rarely but releases resources frequently, retention can grow. A periodic drain (e.g., once per frame from the application loop, or on `vkAcquireNextImageKHR`) is a small follow-up.
- **Gate coalescing**: if a resource is retained against `(queueA, value 5)` and later `(queueA, value 7)`, only the later gate matters. Today both are kept. Coalescing is straightforward but unnecessary for current workloads.
- **Cross-fiber / cross-thread submission**: today `enqueue` and `drainCompleted` are mutex-protected. If submission becomes contention-sensitive, a lock-free MPSC queue would replace the deque.
