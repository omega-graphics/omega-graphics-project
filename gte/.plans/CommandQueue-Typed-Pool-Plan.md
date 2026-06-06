# GECommandQueue Typed-Pool / Multi-Queue Plan

## Goal

Pick up the three explicit non-goals of the
[RenderTarget / Command-Queue Decoupling Plan](done/RenderTarget-Queue-Decoupling-Plan.md):

1. **Multi-queue scheduling** — let the user create graphics, compute, and
   copy queues as distinct objects, not just "one queue per draw."
2. **Queue-family abstraction on Vulkan** — stop hiding the underlying
   `VkQueue` index. Map `GECommandQueueType` → queue family at creation
   time and fail fast when the device cannot satisfy the request.
3. **Per-frame command-buffer pooling** — make the pool size, growth
   strategy, and priority of a queue explicit at creation, replacing the
   single `unsigned maxBufferCount` parameter on `makeCommandQueue`.

All three are unblocked by the decoupling refactor. None of them are
deliverable while the engine is still spawning queues internally per
render target.

## Why now

After decoupling, every queue is user-created. The only thing the API
lets the user say about a queue is "give me a queue that holds at most N
buffers." The user cannot say:

- "I want a copy queue, separate from my graphics queue, so my texture
  uploads don't stall the frame."
- "I want a high-priority graphics queue for the swap chain and a
  background graphics queue for shadow prepass."
- "I want this queue to run on Vulkan's dedicated transfer family."

That information is currently invisible to the engine, so every queue
collapses onto the same `DIRECT` / generic family on D3D12 and Vulkan
and onto whatever `[device newCommandQueue]` returns on Metal. The
performance ceiling is the same as the pre-decoupling design — we
removed waste but not the missed parallelism.

## API delta

### `GECommandQueueDesc`

```cpp
struct OMEGAGTE_EXPORT GECommandQueueDesc {
    enum class Type : uint8_t {
        /// Render + compute + copy. The default; corresponds to D3D12
        /// DIRECT, Vulkan graphics-capable family, MTLCommandQueue.
        Universal,
        /// Render + compute. No copy queue separation requested.
        Graphics,
        /// Async compute. D3D12 COMPUTE, Vulkan compute-capable family
        /// (prefers a family without graphics), Metal MTLCommandQueue
        /// (no native split — recorded as a hint).
        Compute,
        /// DMA / transfer. D3D12 COPY, Vulkan transfer-capable family
        /// (prefers a dedicated transfer family), Metal MTLCommandQueue
        /// (hint only).
        Transfer,
    };

    enum class Priority : uint8_t {
        Low,       // D3D12 NORMAL, Vulkan VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR
        Normal,    // D3D12 NORMAL, Vulkan VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR
        High,      // D3D12 HIGH,   Vulkan VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR
        Realtime,  // D3D12 GLOBAL_REALTIME (requires entitlement),
                   // Vulkan VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR (gated)
    };

    Type     type            = Type::Universal;
    Priority priority        = Priority::Normal;

    /// Max number of in-flight command buffers this queue tracks. Same
    /// meaning as today's `maxBufferCount`, but now a struct field.
    unsigned maxBufferCount  = 16;

    /// If true, the engine refuses to fall back to a less-specific
    /// family when the requested type has no dedicated family on the
    /// device. Defaults to false (best-effort).
    bool requireDedicated    = false;

    /// Debug label, plumbed to D3D12 `SetName` / Vulkan
    /// `VK_EXT_debug_utils` / Metal `[MTLCommandQueue setLabel:]`.
    OmegaCommon::String label;
};
```

### `OmegaGraphicsEngine` factory

Replace the single overload:

```cpp
// Today:
virtual SharedHandle<GECommandQueue> makeCommandQueue(unsigned maxBufferCount) = 0;

// After:
virtual SharedHandle<GECommandQueue> makeCommandQueue(const GECommandQueueDesc & desc) = 0;
```

A backwards-compatible inline forwarder lives in the header for one
release:

```cpp
inline SharedHandle<GECommandQueue> makeCommandQueue(unsigned maxBufferCount) {
    GECommandQueueDesc d;
    d.maxBufferCount = maxBufferCount;
    return makeCommandQueue(d);
}
```

This forwarder is removed at the end of the deprecation window.

### `GECommandQueue` — type introspection

```cpp
class OMEGAGTE_EXPORT GECommandQueue : public GTEResource {
public:
    /// Type/priority the queue was created with (post-fallback if the
    /// platform downgraded the request).
    virtual GECommandQueueDesc::Type     type() const = 0;
    virtual GECommandQueueDesc::Priority priority() const = 0;

    /// Returns true if `type()` matches what the user originally asked
    /// for. False means the platform fell back (e.g. Vulkan device has
    /// no dedicated transfer family). Useful for tests and asserts.
    bool isDedicated() const;
    // ...
};
```

The fallback contract is: if `requireDedicated == false`, the engine may
return a queue of a more general type (Transfer→Compute→Universal). If
`requireDedicated == true`, it returns `nullptr` and logs.

### Present-queue validation

The decoupling plan's open question #2 already added validation:
`makeNativeRenderTarget` returns `nullptr` if the queue cannot present.
With typed queues, the validation tightens — a `Type::Transfer` queue
can never be a present queue and must be rejected at descriptor parse
time, not after attempting swap-chain creation.

## Backend mapping

| Type      | D3D12                            | Vulkan                                                | Metal                                          |
|-----------|----------------------------------|-------------------------------------------------------|------------------------------------------------|
| Universal | `D3D12_COMMAND_LIST_TYPE_DIRECT` | First family with `GRAPHICS \| COMPUTE \| TRANSFER`   | `MTLCommandQueue` (no distinction)             |
| Graphics  | `DIRECT`                         | First family with `GRAPHICS` (prefers without compute when both exist) | `MTLCommandQueue`, label `gfx`     |
| Compute   | `COMPUTE`                        | Family with `COMPUTE` and **not** `GRAPHICS` (async); fallback to graphics family if absent | `MTLCommandQueue`, label `compute` |
| Transfer  | `COPY`                           | Family with `TRANSFER` and **not** `GRAPHICS \| COMPUTE`; fallback to compute, then graphics | `MTLCommandQueue`, label `xfer` |

| Priority  | D3D12                                  | Vulkan                                                                                   | Metal                                       |
|-----------|----------------------------------------|------------------------------------------------------------------------------------------|---------------------------------------------|
| Low       | `D3D12_COMMAND_QUEUE_PRIORITY_NORMAL`* | `VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR` (if `VK_KHR_global_priority` present, else float 0.0) | No API; label only                          |
| Normal    | `NORMAL`                               | `MEDIUM_KHR` / float 0.5                                                                  | No API; label only                          |
| High      | `HIGH`                                 | `HIGH_KHR` / float 1.0                                                                    | No API; label only                          |
| Realtime  | `GLOBAL_REALTIME` (silent fallback if denied)        | `REALTIME_KHR` (validation requires app to opt in; fallback to HIGH on denial)             | No API; label only                          |

`*` D3D12 only exposes NORMAL/HIGH/GLOBAL_REALTIME — Low collapses to
NORMAL on that backend. The queue still reports its requested priority
via `priority()` so tests stay readable.

### Vulkan queue-family resolution

This is the hardest piece. Today, every `GEVulkanCommandQueue` is built
on top of a single `VkCommandPool` bound to whatever family the device
opened at startup. The engine needs to:

1. At `GTEDevice` creation, enumerate `vkGetPhysicalDeviceQueueFamilyProperties`
   and remember which families exist, their queue counts, and their flag
   sets.
2. At `makeCommandQueue`, pick a family per the table above. The choice
   must be deterministic given the same physical device.
3. Cache a `VkQueue` handle per (family, index). The pool grows as the
   user creates queues, up to `queueCount` per family. Beyond that,
   subsequent `makeCommandQueue` calls **share** an underlying `VkQueue`
   (this matches Vulkan's "VkQueue is a handle, not an object" model)
   and document the sharing.
4. The `GECommandQueueDesc::requireDedicated` flag, when set, refuses
   sharing. This is mainly useful for tests.

A `gte/src/vulkan/VulkanQueueFamilies.{h,cpp}` helper holds the family
table and the family-picker. The picker is unit-testable against
synthetic `VkQueueFamilyProperties` arrays.

### D3D12 / Metal

D3D12 maps cleanly — every `makeCommandQueue` creates one
`ID3D12CommandQueue` with the right `D3D12_COMMAND_QUEUE_DESC`. The
existing `GED3D12CommandQueue` constructor learns the new enum.

Metal has no native queue families. The `Type` is recorded for
introspection and used to pick a debug label and a queue-priority hint
(`MTLCommandQueueDescriptor` is private/internal; we use the
public `MTLCommandQueue` only). Functional behavior is identical to
today on Metal — that's a known trade-off and we document it.

## Per-frame command-buffer pooling

Today a `GECommandQueue` holds a fixed array of `maxBufferCount`
buffers. `getAvailableBuffer()` spins through them looking for one that
isn't in flight. Two changes:

1. **Growable pool.** `GECommandQueueDesc::maxBufferCount` becomes a
   hint, not a hard cap. If every buffer is in flight when the user
   calls `getAvailableBuffer()`, the queue allocates one more buffer up
   to a backend-defined ceiling (256 by default; configurable later if
   needed). The hint controls the initial allocation only.
2. **In-flight tracking.** Each buffer carries a monotonic submission
   index. `getAvailableBuffer()` polls completion via the existing
   fence (`ID3D12Fence::GetCompletedValue`, `vkGetFenceStatus`,
   `[MTLCommandBuffer status]`). Buffers older than the last completed
   index are recycled in O(1). The current implementation reuses by
   slot scan; the new one indexes by submission counter.

This change is invisible to the user beyond removing the "queue ran out
of buffers" assertion that fires today when WTK creates more passes
than expected. It also makes the `maxBufferCount` parameter much
cheaper to set conservatively, which is what the decoupling plan
implicitly wanted.

## Implementation phases

### Phase 1 — `GECommandQueueDesc` lands (no behavior change)

1. Add `GECommandQueueDesc` to `gte/include/omegaGTE/GECommandQueue.h`.
2. Add the new `makeCommandQueue(const GECommandQueueDesc&)` virtual
   alongside the existing `makeCommandQueue(unsigned)`. The new virtual
   has a default body that calls the old one and ignores type/priority.
3. Each backend implements the new overload to forward to the old one
   for now (no functional change). All existing call sites compile.

### Phase 2 — Backends honor Type/Priority

| File | Change |
|---|---|
| [`gte/src/d3d12/GED3D12.cpp`](../src/d3d12/GED3D12.cpp) | `makeCommandQueue` builds a real `D3D12_COMMAND_QUEUE_DESC` from the desc. `GED3D12CommandQueue` records type/priority. |
| [`gte/src/vulkan/GEVulkan*.cpp`](../src/vulkan/) | Add `VulkanQueueFamilies` helper. `GEVulkanCommandQueue` consults it at construction to pick family + queue index. `VK_KHR_global_priority` opt-in if available. |
| [`gte/src/metal/GEMetal.mm`](../src/metal/GEMetal.mm) | Record desc on the queue. Set debug label. No behavioral split — documented. |
| New: [`gte/tests/common/CommandQueueDescTest.cpp`](../tests/common/) | Cross-backend unit test: create one of each type, assert `isDedicated()` and `type()` match expectations on a known driver. |

Phase 2 also tightens the present-queue validation already in
`makeNativeRenderTarget`: reject `Type::Transfer` outright; warn but
allow `Type::Compute` only on platforms where it is presentable.

### Phase 3 — Growable pool

1. Rework `getAvailableBuffer()` in each backend
   (`GED3D12CommandQueue.cpp`, `GEVulkanCommandQueue.cpp`,
   `GEMetalCommandQueue.mm`) to use the submission-index recycler
   described above.
2. Remove the "out of buffers" hard fail. Replace with a soft warning
   when the pool grows past 4× the initial hint.
3. Document the new contract in [`API.rst`](API.rst).

### Phase 4 — Retire the legacy overload

1. Delete `makeCommandQueue(unsigned)` from the public header.
2. Update WTK (`wtk/src/Composition/backend/`) and every test
   (`gte/tests/*`) to use a `GECommandQueueDesc`.
3. Update docs:
   - [API.rst](API.rst) — queue creation section.
   - [About.rst](About.rst) — multi-queue capability blurb.

## Open design questions

1. **Should `Type::Graphics` and `Type::Universal` differ on D3D12?**
   They both map to `DIRECT`. The distinction only matters on Vulkan,
   where `Universal` is a stronger requirement (must also expose
   transfer). Recommendation: keep both; on D3D12 they alias, on Vulkan
   they pick different families when one is more constrained.

2. **Cross-queue submission ordering.** If the user submits a compute
   buffer on queue A that writes a texture, then a graphics buffer on
   queue B that samples it, the user is still on the hook for the
   fence. The decoupling plan already established this contract — we
   are not introducing automatic dependency tracking here.

3. **Should the engine cap concurrent queues?** D3D12 / Vulkan vendor
   guidance suggests ≤ 8 total. Recommendation: warn at 9+, refuse at
   16. Both numbers are configurable via a CMake option for power users
   and headless test rigs.

   Anwser: This honestly depends on the capability of a device and whether or not we need to allocate many concurrent queues.

4. **Metal priority hints.** Apple does not expose queue priority on
   public API. We could route urgent work through dedicated queues and
   rely on per-buffer `[MTLCommandBuffer setEnqueued:...]` ordering,
   but the gain is unclear. Recommendation: defer; just record the
   priority for introspection.

## Migration cost estimate

| Area | Files touched | Risk |
|---|---|---|
| Public headers | 2 | low |
| D3D12 backend | 2 | low — additive |
| Vulkan backend | 4 | medium — queue-family picker is new logic |
| Metal backend | 2 | trivial |
| WTK compositor | ~3 | low — call-site rewrites |
| Tests | 4–5 | low — one new dedicated test |
| Docs | 2 | trivial |

Roughly 2–3 focused days, dominated by the Vulkan family picker and the
growable pool. Phase 1 ships in hours; Phase 4 (the breaking change)
can wait a release after WTK is updated.

## Non-goals (of this plan)

- Automatic cross-queue dependency tracking — the user keeps using
  fences explicitly.
- Splitting `GECommandBuffer` into typed subclasses
  (`GraphicsCommandBuffer` / `ComputeCommandBuffer` / `BlitCommandBuffer`).
  The current single class with `startRenderPass` / `startComputePass`
  / `startBlitPass` already encodes the distinction at the pass level;
  the user gets the buffer from a typed *queue*. If a future refactor
  wants stricter compile-time separation, that is a separate plan.
- Reworking `GEFence` semantics. The existing
  `signalFence` / `waitForFence` / `notifyCommandBuffer` API is enough
  for cross-queue sync once typed queues exist.
