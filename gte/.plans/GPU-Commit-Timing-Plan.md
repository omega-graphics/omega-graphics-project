# GPU Commit-Timing Plan

## Goal

Let callers measure how long a *commit* (a batch submitted via
`GECommandQueue::commitToGPU` / `commitToGPUAndWait`) actually took on the GPU,
so pipeline work can be profiled and optimized. The first question this answers
is the coarse one — "is this commit my bottleneck / am I GPU-bound this frame?"
— not the per-pass-attribution question (that is the separate Layer 2 below).

## What already exists (do not re-build)

The engine is already primed for GPU timing; this plan finishes a deliberately
stubbed seam rather than inventing a new one:

- **Capability layer is complete.** All three backends detect timestamp support
  and report the tick period:
  - `GTEDEVICE_FEATURE_TIMESTAMP_QUERIES` (`GTEDevice.h:41`)
  - `GTEDeviceFeatures::timestampPeriod` — ns per GPU tick (`GTEDevice.h:89`)
  - Detection: `GEMetal.mm:159` (`supportsCounterSampling`), `GEVulkan.cpp:525`
    (`timestampComputeAndGraphics` + `timestampPeriod`), `GED3D12.cpp:220`
    (`GetTimestampFrequency`).
- **A per-command-buffer timing struct already exists** — `GERenderTarget.h:25`:
  ```cpp
  struct GECommandBufferCompletionInfo {
      enum class CompletionStatus : std::uint8_t { Completed, Error } status;
      double gpuStartTimeSec = 0.0;
      double gpuEndTimeSec   = 0.0;
  };
  using GECommandBufferCompletionHandler =
      std::function<void(const GECommandBufferCompletionInfo &)>;
  ```
  delivered via `GECommandBuffer::setCompletionHandler(...)`
  (`GERenderTarget.h:349`), with the full fence-gated async dispatch machinery
  already built on all three backends.
- **But the timing fields are only populated on Metal.**
  `GEMetalCommandQueue.mm:1261` fills them from the `MTLCommandBuffer`'s
  `GPUStartTime`/`GPUEndTime` (Metal gives whole-buffer timing for free).
  Vulkan (`GEVulkanCommandQueue.cpp` ~2878) and D3D12
  (`GED3D12CommandQueue.cpp` ~2115) explicitly leave them at `0.0` — the
  comments read *"not wired… ('if available' in the plan)"*. The only current
  consumer is the WTK buffer recycler, which uses the callback purely for
  retention and ignores the timing fields.

## The two layers

- **Layer 1 — whole-commit duration** (this plan). The committed batch's GPU
  span: end-of-last-buffer minus start-of-first-buffer. Answers *whether* a
  commit is the bottleneck.
- **Layer 2 — per-pass breakdown** (separate future plan). `beginTimingScope` /
  `endTimingScope` markers inside a command buffer, so you can see *which* pass
  (render / compute / blit) dominates. Needs real query-pool / query-heap
  plumbing and is a subsystem-sized effort; out of scope here.

Layer 1 itself splits into two pieces, because the commit-level number is just
an aggregation over the per-buffer completion path that is *already* built on
all three backends:

- **P2 — the aggregate (backend-neutral).** Attach an internal per-buffer
  completion handler to each buffer in the batch, accumulate
  `min(gpuStartTimeSec)` / `max(gpuEndTimeSec)` / worst status, and fire a
  single commit-level handler once the last buffer completes. **On Metal this
  produces real whole-commit times the moment it is written**, because the
  per-buffer fields are already populated there. On Vulkan / D3D12 the *status*
  and *count* are correct immediately; the *times* stay `0.0` until P1.
- **P1 — finish the stubbed per-buffer fields on Vulkan & D3D12.** Write a
  start/end GPU timestamp around each buffer's execution into a query
  pool / heap, resolve after the existing retention fence, convert with the
  already-reported `timestampPeriod`. (Separate follow-on; not in this change.)

**This change implements P2 + the Metal wiring + a verification test.** Vulkan
and D3D12 fall back to a safe, documented base default until P1.

## Public API (additions only — nothing changes shape)

In `GERenderTarget.h`, next to the existing per-buffer completion types:

```cpp
struct OMEGAGTE_EXPORT GECommitCompletionInfo {
    GECommandBufferCompletionInfo::CompletionStatus status =
        GECommandBufferCompletionInfo::CompletionStatus::Completed;
    double   gpuStartTimeSec    = 0.0;  // GPU-start of the first buffer in the batch
    double   gpuEndTimeSec      = 0.0;  // GPU-end of the last buffer in the batch
    unsigned commandBufferCount = 0;
    // GPU wall-clock the committed batch occupied. 0.0 when the device lacks
    // GTEDEVICE_FEATURE_TIMESTAMP_QUERIES, or on a backend whose per-buffer
    // timing is not wired yet (Vulkan / D3D12 until P1) — check the feature
    // bit, do not infer "instantaneous" from a zero here.
    double gpuDurationSec() const { return gpuEndTimeSec - gpuStartTimeSec; }
};
using GECommitCompletionHandler = std::function<void(const GECommitCompletionInfo &)>;
```

Also on `GECommandBuffer` (lets the queue compose onto an existing per-buffer
handler instead of clobbering it — the WTK recycler sets one):

```cpp
virtual GECommandBufferCompletionHandler getCompletionHandler() const { return {}; }
```

On `GECommandQueue`:

```cpp
// Async: fires after the whole committed batch finishes on the GPU.
virtual void commitToGPU(const GECommitCompletionHandler & onComplete);
// Sync: blocks like commitToGPUAndWait, returns the batch's GPU span.
virtual GECommitCompletionInfo commitToGPUAndWaitTimed();
protected:
// Backend-neutral helper a backend's commitToGPU(handler) override calls
// with its own batch right before committing.
void installCommitAggregator(const std::vector<SharedHandle<GECommandBuffer>> & batch,
                             const GECommitCompletionHandler & onComplete);
```

## Design notes / contracts

- **Aggregation rides the existing per-buffer seam.** No new GPU objects on
  Metal — the commit handler is pure CPU-side bookkeeping over callbacks that
  already fire.
- **Composition, not clobbering.** `installCommitAggregator` reads each buffer's
  existing handler via `getCompletionHandler()` and wraps it, so a recycler /
  user per-buffer handler still runs.
- **Thread-safety.** Per-buffer callbacks fire on backend-internal threads, so
  the accumulator is mutex-guarded and fires the commit handler exactly once.
- **Timestamp folding skips errored / untimed buffers** so a `0.0` from a
  buffer that did not run can't poison `min(start)`.
- **`commitToGPUAndWaitTimed()` is backend-neutral**, implemented on top of
  `commitToGPU(handler)` + a condition variable, so any backend that implements
  the async form correctly gets the sync form for free.
- **Safe degraded base default.** A backend that has not overridden
  `commitToGPU(handler)` (Vulkan / D3D12 today) runs the batch via
  `commitToGPUAndWait()` and then fires the handler once with `status =
  Completed` and zero timing. `gpuDurationSec() == 0` + the feature bit signal
  "timing unavailable on this backend yet". P1 replaces this with the real
  async, timestamp-backed path.

## Backend mapping

| Backend | P2 (this change) | P1 (follow-on) |
| --- | --- | --- |
| Metal | Override `commitToGPU(handler)` → `installCommitAggregator` + commit. Real times now (free from `GPUStartTime`/`GPUEndTime`). | n/a (whole-buffer timing already free) |
| Vulkan | Safe base default (status + count, zero timing). | Write `vkCmdWriteTimestamp` into a `VkQueryPool(TIMESTAMP)`; resolve after the retention timeline semaphore; mask to `timestampValidBits`; reset pool per reuse. |
| D3D12 | Safe base default (status + count, zero timing). | `EndQuery(TIMESTAMP)` into a `D3D12_QUERY_HEAP_TYPE_TIMESTAMP`; `ResolveQueryData` → readback; read after the retention fence. |

## P1 — D3D12 per-buffer timestamps (implementation)

Scope: populate the per-buffer `GECommandBufferCompletionInfo` GPU-time fields
that `GED3D12CommandQueue::pollCompletions` currently leaves at `0.0`, and route
them up to commit-level timing via the existing P2 aggregator. Self-contained to
the D3D12 backend; no public-API change. ~250–300 lines across one header + one
`.cpp`.

The work rides the *existing* G.5.1 completion seam (submit → stage → gate →
`pollCompletions` fires), which already gates each command list's handler to a
`retentionFence` value. P1 adds the GPU timestamps that seam reports, plus two
structural fixes the current seam needs before it can carry real per-buffer
times.

**1. Timestamp resources (per queue, gated on timestamp support).** At
construction, after the pool is built, query `commandQueue->GetTimestampFrequency`
once. If it fails — or the queue is neither DIRECT nor COMPUTE — leave timing
disabled and keep the current zero-timing behavior (COPY-queue timestamps need
the separate copy-queue query type; out of scope). When enabled, create one
`ID3D12QueryHeap` of type `D3D12_QUERY_HEAP_TYPE_TIMESTAMP` and one READBACK
`ID3D12Resource`, both sized for the hard pool ceiling (256 slots × 2 queries =
512 timestamps, 4 KB). Sizing to the ceiling up front avoids recreating the heap
on `growPoolOnce` while in-flight resolves reference live indices. Persistently
map the readback buffer. Each pool slot owns query indices `slot*2` (start) and
`slot*2+1` (end), and readback byte offset `slot*16`.

**2. Writing the timestamps.** Start timestamp = `EndQuery(heap, TIMESTAMP,
slot*2)` written as the first command after a list is reset — in
`getAvailableBuffer` (right after `poolSlot` is stamped on the fresh wrapper) and
in `GED3D12CommandBuffer::reset` (re-used wrapper). End timestamp + resolve =
`EndQuery(heap, TIMESTAMP, slot*2+1)` then `ResolveQueryData(heap, TIMESTAMP,
slot*2, 2, readback, slot*16)`, written immediately before every `cl->Close()`.
The three close sites — the batch loop in `commitToGPU`, the batch loop in the
two-arg `submitCommandBuffer`, and that method's dedicated single-buffer close —
each already know the slot: `commandLists[i]` is parallel to `pendingSlots[i]`,
and the single buffer carries `poolSlot` directly. (Resolve targets a READBACK
resource, permanently in `COPY_DEST`; no transition needed.)

**3. Per-buffer info in `pollCompletions` (structural fix #1).** The staged /
gated containers carry a bare `std::function` today, and `pollCompletions` fires
one shared zero-timing `info` for every ready handler. Replace the element type
with a small `{handler, poolSlot}` struct (gated adds the fence value). When an
entry is ready, read its slot's two ticks from the mapped readback buffer,
convert to seconds (`ticks / frequency`), and fire that handler with its own
`GECommandBufferCompletionInfo` (status as today, real start/end). `UINT32_MAX`
slot or timing-disabled → leave `0.0`, exactly the current contract.

**4. Commit-level timing override (structural fix #2).** D3D12 still uses the
base `commitToGPU(handler)` fallback, so the aggregator never runs. Override it:
call `installCommitAggregator(retainedCommandBuffers, onComplete)` to set the
fold handler on each batch buffer, then `stageCompletionHandlerFrom` each of
those buffers so the freshly-set handler is staged *with its slot* and gated at
this commit's Execute. The WTK recycler's per-buffer handler was already staged
at submit time and fires independently from its own copy, so this composes
rather than clobbers. The sync form `commitToGPUAndWaitTimed` **must** be
overridden too: the base version drives the now-async `commitToGPU(handler)` and
then blocks on a condition variable that only `pollCompletions` can signal — but
nothing pumps the poller while it blocks, so it would deadlock. The D3D12
override issues the async commit (capturing its single fire), then calls
`commitToGPUAndWait`, whose fence wait drains the batch and whose terminal
`pollCompletions` (GPU idle) fires the gated aggregator handler on this thread
before the call returns.

**5. Autonomous async completion (structural fix #3).** The seam in #4 only
*gates* the async handler to a `retentionFence` value; it is fired by
`pollCompletions`, which D3D12 calls only from frame-loop entry points
(`getAvailableBuffer` / `commitToGPU` / `commitToGPUAndWait`). That silently
assumes a frame loop keeps re-pumping the poller. A standalone
`commitToGPU(handler)` with no subsequent GPU activity is never re-polled, so the
GPU finishes but the handler never fires — the public contract
(`onComplete` "fires exactly once, after the whole committed batch finishes on
the GPU … Does not block") is violated, and a caller blocked on the result (the
async branch of `commit_timing_test`) deadlocks. The original poll-only model
named this "the waiter-thread alternative" and deliberately skipped it because
WTK always drives a frame loop; the test is the first caller to use the async
form outside one. Fix: a **lazily-started per-queue waiter thread**, armed only
by the public async `commitToGPU(handler)`. After that commit signals
`retentionFence`, the waiter `SetEventOnCompletion`-waits for the committed value
(or a stop event) and then calls `pollCompletions`, firing the gated handler with
the GPU idle — matching Metal's driver-pumped `addCompletedHandler` behavior.
Because the waiter can now poll concurrently with the frame thread, the
staged/gated containers and `pollCompletions` take a `completionMutex_` (handlers
fire *outside* the lock to keep user re-entry deadlock-free); the lock is
uncontended on the frame path. The sync `commitToGPUAndWaitTimed` keeps its
own fence-wait drive and does **not** start the waiter (no idle thread for the
synchronous one-shot path).

**Build / verify.** Windows/WSL build is handed to the user per AGENTS.md;
`commit_timing_test` already exists and exercises both the sync and async forms —
it will report real D3D12 times once this lands.

## P1 — Vulkan per-buffer timestamps (implementation)

Scope: populate the per-buffer `GECommandBufferCompletionInfo` GPU-time fields
that `GEVulkanCommandQueue::pollCompletions` currently leaves at `0.0`, and route
them up to commit-level timing via the existing P2 aggregator. Self-contained to
the Vulkan backend; no public-API change. The Vulkan analogue of the D3D12 P1
above — it rides the **same** completion seam (submit → stage → gate →
`pollCompletions` fires, each handler gated to a `retentionTimeline` value), and
needs the same three structural fixes — but on Vulkan primitives. Two Vulkan
simplifications fall out of the API: results are read host-side with
`vkGetQueryPoolResults` (no resolve-to-readback step, so no companion buffer or
persistent mapping), and a graphics/compute/transfer queue can write timestamps
whenever its family reports `timestampValidBits > 0` (no COPY-queue carve-out).

**1. Timestamp resources (per queue, gated on timestamp support).** At
construction, after the command pool is built, read
`VkPhysicalDeviceLimits::timestampPeriod` (ns per tick) via
`vkGetPhysicalDeviceProperties` and the bound family's
`VkQueueFamilyProperties::timestampValidBits`. If `validBits == 0` or the period
is non-positive, leave timing disabled and keep the current zero-timing behavior.
When enabled, create one `VkQueryPool` of type `VK_QUERY_TYPE_TIMESTAMP` sized to
the hard pool ceiling (256 slots × 2 queries = 512). Sizing to the ceiling up
front avoids recreating the pool when `getAvailableBuffer` grows the command-
buffer pool while in-flight reads reference live indices. Each pool slot owns
query indices `slot*2` (start) and `slot*2+1` (end). Cache a `timestampMask`
(`validBits >= 64 ? ~0ull : (1ull<<validBits)-1`) to mask raw ticks before use.

**2. Writing the timestamps.** A Vulkan command buffer enters recording in the
`GEVulkanCommandBuffer` ctor (`vkBeginCommandBuffer`), which only runs from
`getAvailableBuffer`. So the start write lands in `getAvailableBuffer` right
after `poolSlot` is stamped on the fresh wrapper: `vkCmdResetQueryPool(cmd,
pool, slot*2, 2)` (queries must be reset before reuse; legal here because this is
the first command, outside any render pass) then
`vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, slot*2)`. The
end write = `vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool,
slot*2+1)`, recorded immediately before each `vkEndCommandBuffer`. The three
end-of-buffer sites — the batch loop in `commitToGPU`, in `commitToGPUPresent`,
and in `commitToGPUAndWait` — each already know the slot: `commandQueue[i]` is
parallel to `pendingSlots[i]`. Unlike D3D12, Vulkan needs no resolve command —
results are pulled host-side after the buffer retires. (`GEVulkanCommandBuffer::
reset()` does **not** re-`vkBeginCommandBuffer`, so — unlike D3D12 — no start
write is added there; every reuse cycles through `getAvailableBuffer`.)

**3. Per-buffer info in `pollCompletions` (structural fix #1).** The staged /
gated containers carry a bare `std::function` today, and `pollCompletions` fires
one shared zero-timing `info` for every ready handler. Replace the element type
with a small `{handler, poolSlot}` struct (gated adds the timeline value). When
an entry is ready, read its slot's two ticks with `vkGetQueryPoolResults(...,
slot*2, 2, ..., VK_QUERY_RESULT_64_BIT)`, mask each with `timestampMask`, convert
to seconds (`ticks * timestampPeriod * 1e-9`), and fire that handler with its own
`GECommandBufferCompletionInfo`. `UINT32_MAX` slot, timing-disabled, a
`vkGetQueryPoolResults` that is not `VK_SUCCESS`, or a non-increasing pair →
leave `0.0`, exactly the current contract, so an untimed buffer can't poison the
aggregator's min/max fold.

**4. Commit-level timing overrides (structural fix #2).** Vulkan still uses the
base `commitToGPU(handler)` fallback, so the aggregator never runs. A shared
helper `stageCommitAggregator(onComplete)` calls
`installCommitAggregator(pendingRetainedBuffers, onComplete)` then
`stageCompletionHandlerFrom` each of those buffers, so the freshly-set fold
handler is staged *with its slot* and gated at this commit's submit. The WTK
recycler's per-buffer handler was already staged at submit time and fires
independently, so this composes rather than clobbers. The async override is
`stageCommitAggregator + commitToGPU() + armCompletionWaiter()`. The sync
override `commitToGPUAndWaitTimed` is **not** routed through `commitToGPU()` like
D3D12: Vulkan's `commitToGPUAndWait()` early-returns on an empty `commandQueue`
(no drain, no poll), so chaining would skip the GPU wait entirely. Instead the
sync form does `stageCommitAggregator + commitToGPUAndWait()` directly — that
path submits the batch, blocks on `submitFence` (GPU idle), gates the staged
handlers, and its terminal `pollCompletions` fires the aggregator on this thread
before returning. No waiter thread for the synchronous one-shot.

**5. Autonomous async completion (structural fix #3).** Same gap as D3D12: the
seam only *gates* the async handler; `pollCompletions` fires only from frame-loop
entry points (`getAvailableBuffer` / the commit paths), so a standalone async
`commitToGPU(handler)` with no later GPU activity never fires — violating the
"fires once after the GPU finishes, without blocking" contract and deadlocking a
caller blocked on the result (the async branch of `commit_timing_test`). Fix: a
**lazily-started per-queue waiter thread**, armed only by the public async
`commitToGPU(handler)`. After that commit signals `retentionTimeline`, the waiter
host-waits with `vkWaitSemaphores` (bounded 50 ms timeout so a teardown
`waiterStop_` is observed promptly — the timeline counter is monotonic and can't
be signalled backward to unblock an infinite wait) until the committed value
retires, then calls `pollCompletions`, firing the gated handler with the GPU
idle — matching Metal's driver-pumped behavior. Because the waiter can now poll
concurrently with the frame thread, the staged/gated containers and
`pollCompletions` take a `completionMutex_` (handlers fire *outside* the lock to
keep user re-entry deadlock-free); the lock is uncontended on the frame path. The
sync `commitToGPUAndWaitTimed` keeps its own fence-wait drive and does **not**
start the waiter. The waiter is stopped + joined at the top of `releaseNative`,
before the timeline / query pool / containers it reads are torn down.

**Build / verify.** Native Linux/Vulkan build is driven by the agent per
AGENTS.md. `commit_timing_test` is registered for Vulkan
(`gte/tests/vulkan/CMakeLists.txt`) and exercises both the sync and async forms;
it reports real Vulkan times once this lands.

## Verification

`gte/tests/commit_timing_test.cpp` (shared CLI test, registered via
`add_metal_cli_test` + `add_test`): runs a compute commit, then

- exercises the sync form `commitToGPUAndWaitTimed()` and asserts
  `status == Completed`, `commandBufferCount == 1`, `gpuDurationSec() >= 0`
  (and `> 0` when the device reports `GTEDEVICE_FEATURE_TIMESTAMP_QUERIES`);
- exercises the async form `commitToGPU(handler)` and asserts the handler fires
  exactly once with a consistent span;
- prints the measured GPU time in ms and the device's `timestampPeriod`.

GPU-verified on this macOS / Metal host and on the Linux / Vulkan host
(NVIDIA RTX 2080 Ti — sync + async both report real positive GPU spans). D3D12
(WSL/Windows, handed to the user per AGENTS.md) gets its real timing under P1.

## Status

- [x] P2 — backend-neutral commit-timing aggregate
- [x] Metal — real per-commit GPU time
- [x] `commit_timing_test` — GPU-verified on Metal, D3D12, Vulkan
- [x] P1 — Vulkan per-buffer timestamp writes
- [ ] P1 — D3D12 per-buffer timestamp writes
- [ ] Layer 2 — per-pass timing scopes (separate plan)
