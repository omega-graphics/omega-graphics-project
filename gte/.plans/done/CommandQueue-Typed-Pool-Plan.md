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

### Phase 1 — `GECommandQueueDesc` lands (no behavior change) — **DONE**

1. Add `GECommandQueueDesc` to `gte/include/omegaGTE/GECommandQueue.h`.
2. Add the new `makeCommandQueue(const GECommandQueueDesc&)` virtual
   alongside the existing `makeCommandQueue(unsigned)`. The new virtual
   has a default body that calls the old one and ignores type/priority.
3. Each backend implements the new overload to forward to the old one
   for now (no functional change). All existing call sites compile.

**Implementation notes (2026-06-06):**

- `GECommandQueueDesc` now lives in
  [`gte/include/omegaGTE/GECommandQueue.h`](../include/omegaGTE/GECommandQueue.h)
  with the full `Type` / `Priority` enums, `maxBufferCount`,
  `requireDedicated`, and `label` fields described above. The enums are
  `enum class` with explicit `std::uint8_t` backing.
- The new virtual on `OmegaGraphicsEngine` is declared in
  [`gte/include/omegaGTE/GE.h`](../include/omegaGTE/GE.h) (with a
  forward-decl of `GECommandQueueDesc` next to the existing
  `class GECommandQueue;` forward-decl) and its default body lives
  out-of-line in [`gte/src/GE.cpp`](../src/GE.cpp). The body forwards to
  the legacy `makeCommandQueue(unsigned)` virtual, which keeps every
  existing call site (WTK, tests, kreate) compiling against the old
  overload.
- All three backends override the new overload as a forward to the
  legacy path:
  [`GED3D12Engine::makeCommandQueue`](../src/d3d12/GED3D12.cpp),
  [`GEVulkanEngine::makeCommandQueue`](../src/vulkan/GEVulkan.cpp),
  and the inline `makeCommandQueue` on the Metal engine
  ([`gte/src/metal/GEMetal.mm`](../src/metal/GEMetal.mm)). This gives
  Phase 2 a dedicated entry point on every backend without needing
  another header touch.
- **Verified:** Linux Vulkan host — `ninja` builds `libOmegaGTE.so`,
  `libOmegaWTK.so`, and every test/sample target without errors. **Not
  verified on this commit:** D3D12 (Windows), Metal (macOS). Both
  backends are additive forwards that mirror the Vulkan change line for
  line, but they have not been compiled here — see
  `feedback_mark_unverified_backends_in_plan.md`.

### Phase 2 — Backends honor Type/Priority — **DONE**

| File | Change |
|---|---|
| [`gte/src/d3d12/GED3D12.cpp`](../src/d3d12/GED3D12.cpp) | `makeCommandQueue` builds a real `D3D12_COMMAND_QUEUE_DESC` from the desc. `GED3D12CommandQueue` records type/priority. |
| [`gte/src/vulkan/GEVulkan*.cpp`](../src/vulkan/) | Add `VulkanQueueFamilies` helper. `GEVulkanCommandQueue` consults it at construction to pick family + queue index. `VK_KHR_global_priority` opt-in if available. |
| [`gte/src/metal/GEMetal.mm`](../src/metal/GEMetal.mm) | Record desc on the queue. Set debug label. No behavioral split — documented. |
| New: [`gte/tests/common/CommandQueueDescTest.cpp`](../tests/common/) | Cross-backend unit test: create one of each type, assert `isDedicated()` and `type()` match expectations on a known driver. |

Phase 2 also tightens the present-queue validation already in
`makeNativeRenderTarget`: reject `Type::Transfer` outright; warn but
allow `Type::Compute` only on platforms where it is presentable.

**Implementation notes (2026-06-06):**

- The base class [`GECommandQueue`](../include/omegaGTE/GECommandQueue.h)
  now exposes `type()`, `requestedType()`, `priority()`, `label()`, and
  `isDedicated()` accessors backed by a new protected
  `GECommandQueueDesc desc_` / `Type requestedType_` pair. A second
  constructor `GECommandQueue(const GECommandQueueDesc &, Type achievedType)`
  takes the user's original descriptor and the post-fallback achieved
  type; the legacy `GECommandQueue(unsigned)` ctor stays as a forwarder
  that records `Universal / Normal` (so isDedicated() never lies for the
  legacy path).
- **D3D12** ([`GED3D12CommandQueue.cpp`](../src/d3d12/GED3D12CommandQueue.cpp))
  — the queue ctor now builds a real `D3D12_COMMAND_QUEUE_DESC` from the
  descriptor: Type → `D3D12_COMMAND_LIST_TYPE_{DIRECT,COMPUTE,COPY}`,
  Priority → `D3D12_COMMAND_QUEUE_PRIORITY_{NORMAL,HIGH,GLOBAL_REALTIME}`.
  Realtime silently downgrades to HIGH on `CreateCommandQueue` failure
  (no entitlement); the user's requested priority remains visible via
  `priority()`. Labels flow through `ID3D12Object::SetName`. The legacy
  `(engine, unsigned)` ctor is a delegating forwarder. `makeCommandQueue`
  on the engine routes through the desc ctor.
- **Vulkan** ([`VulkanQueueFamilies.{h,cpp}`](../src/vulkan/VulkanQueueFamilies.h),
  [`GEVulkanCommandQueue.cpp`](../src/vulkan/GEVulkanCommandQueue.cpp),
  [`GEVulkan.cpp`](../src/vulkan/GEVulkan.cpp)) — new
  `VulkanQueueFamilies::Pick(props, count, type, requireDedicated)`
  helper implements the per-Type fallback ladder from the table above.
  Pure logic, no engine dependency; covered by
  `tests/vulkan/VulkanQueueFamiliesTest`. The engine's family
  enumeration loop was rewritten to (a) iterate by index so the `id`
  variable can't desync with the loop iterator, and (b) open every
  family that exposes any of GRAPHICS/COMPUTE/TRANSFER (previously only
  GRAPHICS/COMPUTE), which is what gives the picker a dedicated transfer
  family to pick from on hardware that exposes one. `GEVulkanCommandQueue`
  now stores its own `(boundFamilyIndex, nativeQueue)` pair resolved at
  construction; the three submit sites that used to grab
  `engine->deviceQueuefamilies.front().front()` now use the stored
  `nativeQueue` so multi-queue requests actually hit different VkQueues.
  Labels flow through `vkSetDebugUtilsObjectNameEXT`.
- **Metal** ([`GEMetalCommandQueue.mm`](../src/metal/GEMetalCommandQueue.mm),
  [`GEMetal.mm`](../src/metal/GEMetal.mm)) — the queue ctor records the
  descriptor and applies `[MTLCommandQueue setLabel:]` either from
  `desc.label` or — when empty — from an auto-generated type-derived
  suffix ("GECommandQueue[gfx]", "[compute]", "[xfer]", "[any]") so the
  Xcode GPU capture / Instruments traces remain useful even without a
  user label. Metal has no native family split, so `isDedicated()` is
  always true.
- **Present-queue validation** — each backend's `makeNativeRenderTarget`
  rejects a Transfer-typed `presentQueue` up front and logs.
- **Tests**:
  - [`tests/vulkan/VulkanQueueFamiliesTest`](../tests/vulkan/VulkanQueueFamiliesTest/main.cpp) — pure-CPU unit test for
    `VulkanQueueFamilies::Pick`, covers discrete GPU, integrated GPU,
    compute-only device, empty family list, and `requireDedicated`
    enforcement. Deterministic.
  - [`tests/command_queue_desc_test.cpp`](../tests/command_queue_desc_test.cpp) — backend-independent GPU
    integration test: creates one queue of each Type, round-trips
    `priority()` / `label()` / `getSize()` / `requestedType()`, verifies
    `Transfer` presentQueue rejection. Wired into the Vulkan backend's
    CMakeLists; the source is shared so D3D12 / Metal can add the same
    `add_test` entry without copying the body.
- **Verified:** Linux Vulkan host, NVIDIA RTX 2080 Ti — every queue
  test (`omegagte_vulkan_queue_families`, `omegagte_command_queue_desc`,
  `omegagte_sampler_bind`, `omegagte_sampler_bind_negative`,
  `omegagte_matrix_ops`, `omegagte_mesh_shader` against the bundled
  `gte/deps/vulkan_sdk/1.3.283.0` validation layer) passes; integration
  test reports all four Types as `dedicated=1` on this discrete GPU. The
  ctest run from a bare shell hits the system VVL which is older than
  the bundled one and perturbs unrelated calls — point `VK_LAYER_PATH`
  at the bundled SDK before believing any VVL error. **Not verified on
  this commit:** D3D12 (Windows), Metal (macOS).
- **Deferred to Phase 2 follow-ups** (not blockers for Phase 3):
  1. **`VK_KHR_global_priority` opt-in.** ✅ **DONE (2026-06-06).** The
     Vulkan engine probes the extension at device-create
     ([`GEVulkan.cpp:1133`](../src/vulkan/GEVulkan.cpp)) and, when
     present, chains `VkDeviceQueueGlobalPriorityCreateInfoKHR { MEDIUM }`
     per opened family. Per-queue priority differentiation is delivered
     via `pQueuePriorities = [1.0, 0.5, 0.0]` (the standard Vulkan
     relative-float array), because Vulkan's VUID-VkDeviceCreateInfo
     -queueFamilyIndex-02802 forbids multiple `VkDeviceQueueCreateInfo`
     for the same family — so global priority is necessarily a per-family
     setting in standard Vulkan, while per-queue priority lives in the
     float array. Each opened family asks for `min(3, family.queueCount)`
     queues; `lookupQueueOnFamily` maps each `GECommandQueueDesc::Priority`
     to the matching VkQueue (with HIGH→MEDIUM→LOW fallback on families
     that exposed fewer queues). REALTIME requests resolve to HIGH at
     runtime because REALTIME global priority requires CAP_SYS_NICE /
     the Windows GPU-priority entitlement; the device-create retry block
     strips the chain entirely on `VK_ERROR_NOT_PERMITTED_KHR`.
     Upload-queue / TE-context / `debugReadbackPixelRGBA8` sites that
     previously grabbed `deviceQueuefamilies[0][0]` were rewritten to
     ask `lookupQueueOnFamily` for the MEDIUM queue explicitly, since
     `[0][0]` is now the HIGH-float bucket.
     **Verified on Linux Vulkan / NVIDIA RTX 2080 Ti** — the
     `omegagte_command_queue_desc` integration test gained a
     `checkDistinctNativeQueuesPerPriority` case that creates Universal
     queues at LOW/NORMAL/HIGH and asserts the returned `native()`
     handles are distinct (`distinct=3` observed). Unverified on D3D12 /
     Metal (no global-priority equivalent on those backends; the
     priority field stays informational there per the original plan).
  2. **Multi-queue (`queueCount > 1`) within a family.** ✅ Implicitly
     delivered by the global-priority opt-in above — every opened
     family now allocates up to 3 VkQueues, indexed by priority. A
     dedicated "round-robin same-priority requests across multiple
     equal-priority queues" pool is still a separate concern (would help
     workloads that hammer one priority bucket with many independent
     submissions), but the typed-pool plan's primary motivation
     (priority differentiation) is now satisfied.

### Phase 3 — Growable pool — **DONE**

1. Rework `getAvailableBuffer()` in each backend
   (`GED3D12CommandQueue.cpp`, `GEVulkanCommandQueue.cpp`,
   `GEMetalCommandQueue.mm`) to use the submission-index recycler
   described above.
2. Remove the "out of buffers" hard fail. Replace with a soft warning
   when the pool grows past 4× the initial hint.
3. Document the new contract in [`API.rst`](API.rst).

**Implementation notes (2026-06-06):**

- **Vulkan** ([`GEVulkanCommandQueue.{h,cpp}`](../src/vulkan/GEVulkanCommandQueue.h)) —
  the pre-existing pool of `VkCommandBuffer` slots gained a parallel
  `commandBufferSubmissionIndex[]` array tracking the retention-timeline
  value each slot was last submitted at. `getAvailableBuffer()` reads
  the current completed value via `vkGetSemaphoreCounterValue` and
  recycles the first slot whose `submissionIndex <= completed`. On miss
  it grows the pool by `vkAllocateCommandBuffers(commandBufferCount=1)`
  up to a hard `kPoolCeiling = 256`, soft-warning exactly once when the
  pool first exceeds 4× `desc.maxBufferCount`. `GEVulkanCommandBuffer`
  now carries a `poolSlot` field set at checkout; both
  `submitCommandBuffer` overloads stamp the slot busy with
  `UINT64_MAX` at submit time (so an issued-but-not-yet-committed
  buffer is correctly busy when a sibling checkout races the commit),
  and `stampPendingSlots()` re-stamps with the real signal value after
  every successful `vkQueueSubmit + ++nextSubmitValue` pair in
  `commitToGPU`, `commitToGPUPresent`, and `commitToGPUAndWait`. Early
  return paths clear `pendingSlots` so flagged-but-not-submitted slots
  don't leak between commits.
- **D3D12** ([`GED3D12CommandQueue.{h,cpp}`](../src/d3d12/GED3D12CommandQueue.h)) —
  the backend previously had **no pool at all**: every
  `getAvailableBuffer()` call ran `CreateCommandAllocator +
  CreateCommandList`. Phase 3 introduces a real pool of
  `(ComPtr<ID3D12CommandAllocator>, ComPtr<ID3D12GraphicsCommandList6>)`
  pairs, pre-allocated to `desc.maxBufferCount` slots in the ctor with
  the D3D12 command-list type matching the queue's `desc.type` (DIRECT
  / COMPUTE / COPY). Per-slot `poolSubmissionIndex[]` tracks the
  existing `retentionFence` value; recycling reads
  `retentionFence->GetCompletedValue()`. `growPoolOnce()` allocates one
  more `(allocator, list)` pair on demand and applies the same 4×-hint
  one-shot warning + 256 ceiling. `GED3D12CommandBuffer::poolSlot` is
  set at checkout; both submit overloads stamp `UINT64_MAX` at submit
  time and the batch-Execute path in `commitToGPU` calls
  `stampPendingSlots(nextSubmitValue)` after the `Signal(retentionFence,
  …)`. The single-buffer immediate-Execute path
  (`submitCommandBuffer(buf, signalFence)`) stamps the buffer's own
  slot inline after its dedicated Signal.
- **Metal** — left as-is and documented. Metal command buffers are
  inherently single-shot — `getAvailableBuffer()` constructs a fresh
  `GEMetalCommandBuffer` whose underlying `MTLCommandBuffer` comes from
  `[MTLCommandQueue commandBuffer]`. The pool size hint is already
  honored by `[MTLDevice newCommandQueueWithMaxCommandBufferCount:]`
  (which IS Metal's pool cap), so the Phase 3 growable-pool semantics
  apply implicitly — `MTLCommandQueue` blocks/waits internally when
  the buffer cap is hit. No code change required.
- **Docs** ([`gte/docs/api/GPUSubmission.rst`](../docs/api/GPUSubmission.rst)) —
  the `GECommandQueue` class doc reframes `maxBufferCount` as an
  **initial pool size hint**, names the 256 ceiling and the 4× soft
  warning, and notes `getAvailableBuffer()` returns `nullptr` only at
  the ceiling (not on every "pool full" event).
- **Tests** — [`command_queue_desc_test.cpp`](../tests/command_queue_desc_test.cpp)
  gained `checkGrowablePool`: creates a queue with `maxBufferCount=2`,
  checks out 6 buffers back-to-back via `submitCommandBuffer` without
  committing, and asserts the queue returns 6 distinct `native()`
  handles (proving the pool grew). On the RTX 2080 Ti the test reports
  `hint=2 checkouts=6 distinct-natives=6` ✅.
- **Verified:** Linux Vulkan host, NVIDIA RTX 2080 Ti — every
  queue-adjacent test (`omegagte_vulkan_queue_families`,
  `omegagte_command_queue_desc`, `omegagte_sampler_bind`,
  `omegagte_sampler_bind_negative`, `omegagte_matrix_ops`) passes; full
  `ninja` is clean. **Not verified on this commit:** D3D12 (Windows),
  Metal (macOS — unchanged but uncompiled here).

### Phase 4 — Retire the legacy overload — **DONE**

1. Delete `makeCommandQueue(unsigned)` from the public header.
2. Update WTK (`wtk/src/Composition/backend/`) and every test
   (`gte/tests/*`) to use a `GECommandQueueDesc`.
3. Update docs:
   - [API.rst](API.rst) — queue creation section.
   - [About.rst](About.rst) — multi-queue capability blurb.

**Implementation notes (2026-06-06):**

- **Public API** ([`gte/include/omegaGTE/GE.h`](../include/omegaGTE/GE.h)) —
  `virtual SharedHandle<GECommandQueue> makeCommandQueue(unsigned)` is
  gone; the desc overload is now `= 0` (pure). Its out-of-line default
  forwarder in [`gte/src/GE.cpp`](../src/GE.cpp) was deleted.
- **Backends** — every backend's `makeCommandQueue(unsigned int)` override
  is gone:
  [`GED3D12.{h,cpp}`](../src/d3d12/GED3D12.h),
  [`GEVulkan.{h,cpp}`](../src/vulkan/GEVulkan.h),
  [`GEMetal.mm`](../src/metal/GEMetal.mm). The legacy
  `(engine, unsigned size)` delegating ctors on
  [`GED3D12CommandQueue`](../src/d3d12/GED3D12CommandQueue.h),
  [`GEVulkanCommandQueue`](../src/vulkan/GEVulkanCommandQueue.h), and
  [`GEMetalCommandQueue`](../src/metal/GEMetalCommandQueue.h) are gone
  too — the desc ctor is the only one. The base
  [`GECommandQueue(unsigned)`](../include/omegaGTE/GECommandQueue.h) ctor
  was deleted; `GECommandQueue(const GECommandQueueDesc &, Type)` is the
  only base ctor.
- **Call-site migration** — all 25 in-tree consumers were updated by a
  one-shot Python migration script (committed to history via the
  diff). Each `auto x = engine->makeCommandQueue(N);` became:

      OmegaGTE::GECommandQueueDesc xDesc{};
      xDesc.maxBufferCount = N;
      auto x = engine->makeCommandQueue(xDesc);

  WTK present-queue sites
  ([`VKVisualBinder.cpp`](../../wtk/src/Composition/backend/vk/VKVisualBinder.cpp),
  [`DCVisualBinder.cpp`](../../wtk/src/Composition/backend/dx/DCVisualBinder.cpp),
  [`MTLVisualBinder.mm`](../../wtk/src/Composition/backend/mtl/MTLVisualBinder.mm))
  were upgraded to `Type::Graphics + Priority::High` with descriptive
  labels — they drive the swap chain, so they should outrank background
  work. [`BitmapTextureCache.cpp`](../../wtk/src/Composition/backend/BitmapTextureCache.cpp)
  was upgraded to `Type::Transfer` — texture uploads belong on the DMA
  family on devices that expose one.
- **Docs** —
  [`gte/docs/api/GPUSubmission.rst`](../docs/api/GPUSubmission.rst)
  gained a Queue-creation section that walks through every
  `GECommandQueueDesc` field, both enums, and the new "build a desc,
  hand it to `makeCommandQueue`" pattern. The example block at the end
  shows both a default Universal queue and a `Graphics + High` present
  queue. [`gte/docs/About.rst`](../docs/About.rst) gained a "Queues are
  first-class and typed" paragraph alongside the existing
  "Synchronisation is fences" / "Command buffer is the API surface"
  blurbs, mentioning the per-backend mapping (Vulkan family pick + KHR
  global priority, D3D12 typed list, Metal label) and the growable
  pool.
- **Verified:** full `ninja` build clean across OmegaGTE / OmegaWTK /
  every test target / Kreate. All 7 non-mesh-shader ctest cases pass
  (`omegagte_vulkan_queue_families`, `omegagte_command_queue_desc`,
  `omegagte_sampler_bind`, `omegagte_sampler_bind_negative`,
  `omegagte_matrix_ops`, `omegagte_sampler_validation`,
  `omegagte_std140_layout`). The mesh-shader test passes against the
  bundled SDK validation layer (per the
  `project_vulkan_validator_layer_version.md` memory note); ctest from
  a bare shell hits the older system VVL and reports a false-positive
  failure. **Not verified on this commit:** D3D12 (Windows), Metal
  (macOS) — every call-site migration is mechanical and additive, but
  WTK and the per-backend tests need a real compile on those hosts
  before the migration can be called platform-complete.

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
