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

## Verification

`gte/tests/commit_timing_test.cpp` (shared CLI test, registered via
`add_metal_cli_test` + `add_test`): runs a compute commit, then

- exercises the sync form `commitToGPUAndWaitTimed()` and asserts
  `status == Completed`, `commandBufferCount == 1`, `gpuDurationSec() >= 0`
  (and `> 0` when the device reports `GTEDEVICE_FEATURE_TIMESTAMP_QUERIES`);
- exercises the async form `commitToGPU(handler)` and asserts the handler fires
  exactly once with a consistent span;
- prints the measured GPU time in ms and the device's `timestampPeriod`.

GPU-verified on this macOS / Metal host. Vulkan (Linux host) and D3D12
(WSL/Windows, handed to the user per AGENTS.md) get their real timing under P1.

## Status

- [x] P2 — backend-neutral commit-timing aggregate
- [x] Metal — real per-commit GPU time
- [x] `commit_timing_test` — GPU-verified on Metal
- [ ] P1 — Vulkan per-buffer timestamp writes
- [ ] P1 — D3D12 per-buffer timestamp writes
- [ ] Layer 2 — per-pass timing scopes (separate plan)
