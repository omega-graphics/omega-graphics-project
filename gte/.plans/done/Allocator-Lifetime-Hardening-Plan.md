# Allocator-Lifetime Hardening Plan

## Problem

The GPU memory allocator can be destroyed while resources it allocated are
still alive, which trips the allocator's leak validator and can free memory out
from under live `ID3D12Resource` / `VkBuffer` handles.

Concretely on D3D12 (the case that surfaced this):

- A `GED3D12Buffer` / `GED3D12Texture` owns a `D3D12MA::Allocation *`
  (`GED3D12.h:46`) and `Release()`s it in its destructor (`GED3D12.h:85-88`).
- The engine owns the `D3D12MA::Allocator *memAllocator` and releases it in
  `~GED3D12Engine` (`GED3D12.cpp:710-712`), after draining the retention queue
  and the descriptor allocators.
- `OmegaGTE::Close(gte)` resets `graphicsEngine` (`OmegaGTE.cpp` ~179-181),
  running `~GED3D12Engine` — and thus `memAllocator->Release()` — **while any
  caller-held `GEBuffer` / `GETexture` is still alive**. D3D12MA's destructor
  then sees live committed allocations and asserts *"Unfreed committed
  allocations found!"*.

The current implicit contract is "release every engine-allocated resource
before calling `Close()`". `SamplerBindTest` happens to satisfy it (its
resources live in helper-function scopes that return before `Close`);
`MatrixOpsTest` did not (buffers were outer-scope locals), which is how the
assert was found. A targeted test-side fix (release the handles before `Close`)
is already in `matrix_ops_test.cpp` to unblock the suite, but relying on every
caller to hand-order teardown is a footgun — especially for resources held as
plain locals.

## Goal

Make the allocator outlive every allocation it produced, so `Close()` (and any
engine teardown) is safe regardless of whether the caller still holds resource
handles. The last surviving party — engine or resource — tears the allocator
down. No public API change; this is an internal ownership change.

## Design — shared allocator ownership

Both touched backends use a manually-released allocator object
(`D3D12MA::Allocator` is `IUnknown`-derived; VMA's `VmaAllocator` is an opaque
handle destroyed via `vmaDestroyAllocator`). A uniform, backend-neutral shape:

- Introduce a small **allocator-owner** holder, one per backend, that owns the
  raw allocator and destroys it exactly once in its own destructor
  (`memAllocator->Release()` on D3D12; `vmaDestroyAllocator` on Vulkan).
- The engine holds a `std::shared_ptr<AllocatorOwner>`; **every resource also
  holds a copy** of that `shared_ptr` alongside its existing allocation handle.
- Resource destructor order stays as-is (drop the resource COM/VK handle, then
  release its own allocation), but the *allocator* is now only destroyed when
  the last `shared_ptr` — engine or resource — drops. Teardown becomes
  order-independent.

This is preferable to making the resource hold a raw `ComPtr<D3D12MA::Allocator>`
on D3D12 alone, because the `shared_ptr<AllocatorOwner>` pattern also covers
Vulkan's non-refcounted `VmaAllocator` with one mechanism.

## Phases

### Phase 1 — D3D12
- Add an `AllocatorOwner` (owns `D3D12MA::Allocator *`, `Release()`s once in its
  dtor) and store it on `GED3D12Engine` as `std::shared_ptr<AllocatorOwner>`.
- Thread the `shared_ptr` into every `GED3D12Buffer` / `GED3D12Texture` at
  construction (the `makeBuffer` / `makeTexture` paths that call
  `memAllocator->CreateResource`). Each resource keeps its `D3D12MA::Allocation*`
  exactly as today *and* a `shared_ptr<AllocatorOwner>`.
- Remove the explicit `memAllocator->Release()` from `~GED3D12Engine` — dropping
  the engine's `shared_ptr` is now the release path. Keep the
  drain-retention-then-descriptor-allocators ordering (those still need the
  device, not the memory allocator).
- Resources created *outside* the allocator (heap-placed / imported swap-chain
  buffers, `d3d12maAllocation == nullptr`) simply hold a null owner — no change
  to their teardown.

### Phase 2 — Vulkan
- **Outcome: no `AllocatorOwner` mirror.** Investigation found the Vulkan
  backend already meets the plan's goal — and a literal mirror of Phase 1 would
  be both unnecessary and *unsafe* here. The goal is satisfied by a different,
  pre-existing mechanism; Phase 2 verifies it and documents the parity. See the
  Phase 2 implementation notes below for the full reasoning and the on-platform
  verification.
- Original intent (superseded): mirror Phase 1 for `VmaAllocator` — an
  `AllocatorOwner` that calls `vmaDestroyAllocator` in its dtor, a `shared_ptr`
  on the engine, and a copy on each VMA-allocated `GEVulkanBuffer` /
  `GEVulkanTexture`.

### Phase 3 — Metal
- Verify n/a and document: Metal has no manually-released allocator
  (`MTLBuffer` / `MTLTexture` are ARC-managed and independent of device
  teardown). No code change expected; note it in the backend mapping so the
  parity question is closed.

## Verification

- Revert the `matrix_ops_test.cpp` teardown band-aid (the explicit
  `inBuf/outBuf/mixIn/mixOut` resets before `Close`) and confirm the test passes
  with the allocator-owner change — i.e. `Close()` is now safe with outstanding
  handles. This is the regression that proves the fix.
- Re-run the full D3D12 + Vulkan GTE test suites under the debug allocator /
  validation layers; no "unfreed allocations" assert.
- Windows / Vulkan builds are handed to the user per AGENTS.md.

## Status

- [x] Phase 1 — D3D12 shared allocator ownership *(Windows build verified)*
- [x] Phase 2 — Vulkan: verified already-safe via tracked-resource teardown; no
      `AllocatorOwner` mirror (would be unsafe — see notes). Documented.
- [x] Phase 3 — Metal n/a verification + doc *(verified by code reading; macOS off-platform)*
- [x] Verification — MatrixOpsTest passes with the band-aid reverted: D3D12 on
      Windows (Phase 1), Vulkan on Linux (Phase 2, this pass)

### Phase 1 implementation notes

- Added `GED3D12AllocatorOwner` (owns `D3D12MA::Allocator *`, `Release()`s once
  in its dtor, non-copyable) in `GED3D12.h`. The engine keeps the raw
  `memAllocator` as a non-owning convenience pointer plus a
  `shared_ptr<GED3D12AllocatorOwner> allocatorOwner`; `~GED3D12Engine` now drops
  that ref (`memAllocator = nullptr; allocatorOwner.reset();`) instead of
  calling `memAllocator->Release()`, keeping the drain-retention →
  descriptor-allocators ordering ahead of it.
- `GED3D12Buffer` and `GED3D12Texture` each gained a
  `shared_ptr<GED3D12AllocatorOwner> allocatorOwner`, set at the four
  allocator-creating sites (`GED3D12Heap::makeBuffer`/`makeTexture` and the
  engine `makeBuffer`/`makeTexture`). The resource destructor bodies are
  unchanged — they release the `D3D12MA::Allocation` while the `allocatorOwner`
  member (destroyed only *after* the body) still guarantees the allocator is
  live. The DirectXTK ScratchImage upload path (`GED3D12TextureAsset.cpp`) keeps
  a null owner — it never allocated through D3D12MA.
- Surfaced this on `commit_timing_test` (buffers held as outer-scope locals
  past `Close`), the same shape as the original `MatrixOpsTest` repro.
- **Heap pool (resolved):** `GED3D12Heap` owns a `D3D12MA::Pool *` it
  `Release()`s in its destructor. It now also holds an `allocatorOwner` ref
  (threaded through its constructor from `GED3D12Engine::makeHeap`), so
  `pool->Release()` stays valid even if the engine is torn down first — same
  last-holder-frees-the-allocator guarantee as buffers / textures.

### Phase 2 implementation notes

**Finding: Vulkan already satisfies the goal, by a different mechanism than
D3D12, and a literal mirror would be unsafe.** No `GEVulkanAllocatorOwner` was
added. The reasoning, traced through the code:

- **D3D12's structure (what Phase 1 fixed):** each resource frees its own
  `D3D12MA::Allocation` *inline* in its destructor, and `D3D12MA::Allocator`
  internally COM-refs the `ID3D12Device`. So shared-owning the allocator (let it
  outlive the engine; last holder frees it) is both necessary and safe.
- **Vulkan's structure is different:** resource destructors never free inline.
  They either early-return on `nativeReleased_`, or enqueue a fence-gated
  `vmaDestroy*` lambda onto `engine->retentionQueue`. Every VMA-allocated
  resource that can escape is registered via `trackResource()` (a `weak_ptr` +
  a `releaseNative` fn-ptr) — confirmed at all `vmaCreate*` escape sites
  (`GEVulkan.cpp:1752, 1808, 1914, 2430, 4534, 4556`, `GEVulkanTexture` ctor via
  `2430`). The only untracked VMA allocations are ephemeral staging buffers
  freed inline within one function.
- **Why `Close()` is already safe with outstanding handles:** `~GEVulkanEngine`
  runs `vkDeviceWaitIdle` → `releaseAllTrackedResources()` (locks each
  `weak_ptr`; for every still-live caller-held resource calls `releaseNative()`,
  which frees its VMA allocation **inline while the allocator is still alive**
  and sets `nativeReleased_`) → `retentionQueue.drainAll()` → `vmaDestroyAllocator`
  → `vkDestroyDevice`. So the allocator is destroyed with **no live
  allocations** (no VMA "allocations not freed" assert), and when the caller
  later drops its handle the destructor early-returns on `nativeReleased_` and
  never touches the dead engine.
- **Why a shared-ownership mirror would be unsafe here:** `vmaDestroyAllocator`
  needs a live `VkDevice`, and — unlike `D3D12MA` — VMA does **not** keep the
  device alive. The engine destroys the device immediately after the allocator.
  Letting a caller-held resource defer the allocator's destruction past the
  engine (the whole point of the D3D12 pattern) would run `vmaDestroyAllocator`
  on a destroyed device. A faithful mirror would have to also transfer
  ownership of the device, instance, and retention queue into the owner and
  rewrite every resource to capture it instead of the `engine` pointer — a
  teardown-path rewrite, not a mirror, with no safety benefit over what the
  tracker already provides.
- **Code documentation added:** the lifetime contract is now spelled out at the
  `memAllocator` declaration (`GEVulkan.h`) and at the `vmaDestroyAllocator` site
  in `~GEVulkanEngine` (`GEVulkan.cpp`), each cross-referencing the D3D12 design
  so the divergence is intentional and discoverable.

**Verification (on-platform — Vulkan is the Linux backend, so this ran here, not
handed off):** reverted the `matrix_ops_test.cpp` band-aid (the four
`inBuf/outBuf/mixIn/mixOut` resets before `Close`) so the buffers are alive at
`Close()`. `MatrixOpsTest` passes — `EXIT=0`, no VMA leak assert, clean
teardown — both under the system validation layer and under the bundled SDK
`1.3.283` layer (`VK_LAYER_PATH` → bundled `libVkLayer_khronos_validation.so`).
The resource trace confirms the safe path: the four caller-held buffers are
destroyed *after* the test's `PASS`, each already at `nativeHandle=0` (freed by
`releaseAllTrackedResources` during teardown). Under the bundled layer the fake
`VUID-VkDeviceCreateInfo-pNext-pNext` (mesh-shader struct `1000328000`) error of
the stale system layer disappears, per
`gte/.plans/.../vulkan_validator_layer_version` guidance.

> Aside (not part of this change): under the bundled layer `MatrixOpsTest` still
> emits a pre-existing `VUID-vkCmdBindDescriptorSets-pDescriptorSets-06563`
> (a `VK_NULL_HANDLE` descriptor set bound during the compute dispatch). It is
> present in the passing baseline, is unrelated to allocator lifetime, and was
> left untouched.

### Phase 3 implementation notes

**Outcome: confirmed n/a — no code change to memory management.** Verified by
reading the Metal backend (macOS is off-platform from this Linux host, so this is
code-reading, not a build/run):

- **No allocator exists.** A backend-wide grep for `allocator` / `CreateResource`
  / retention-style frees turns up nothing memory-related — the only hit is
  `bufferAllocator:nil`, a Model I/O (`MDLMesh`) parameter in
  `GEMetalMeshAsset.mm`, not a memory allocator.
- **Resources are ARC-owned and device-independent.** `GEMetalBuffer::metalBuffer`
  and `GEMetalTexture::texture` are `NSSmartPtr` wrappers over `id<MTLBuffer>` /
  `id<MTLTexture>`. Their destructors (`GEMetal.mm`, `GEMetalTexture.mm`) only
  emit a resource-trace event; the native object is released by the `NSSmartPtr`
  member afterward. A live `MTLBuffer`/`MTLTexture` retains its `MTLDevice`, so a
  caller-held handle keeps the device alive until dropped.
- **`~GEMetalEngine` frees no allocator.** It only stops an optional GPU-capture
  session. `Close()` (→ `~GEMetalEngine`) thus cannot free memory out from under
  a live resource, and there is no leak validator to trip — `Close()` is
  inherently safe with outstanding handles. A short comment at `~GEMetalEngine`
  records this so the parity question is closed in-code, mirroring the Vulkan and
  D3D12 teardown comments.

### Backend parity (closes the cross-backend question)

The "`Close()` is safe with outstanding handles" guarantee now holds on all
three backends, by three mechanisms suited to each allocator's nature:

| Backend | Allocator | Mechanism |
| --- | --- | --- |
| D3D12 | `D3D12MA::Allocator` (`IUnknown`, COM-refs device) | Shared ownership — allocator outlives engine; last holder frees it (Phase 1). |
| Vulkan | `VmaAllocator` (opaque, does *not* own device) | Engine teardown proactively frees every tracked resource's allocation before `vmaDestroyAllocator`; `nativeReleased_` guards late dtors (Phase 2). |
| Metal | `MTLBuffer`/`MTLTexture` (ARC, no manual allocator) | n/a — ARC; resources are independent of device teardown (Phase 3). |

## Notes

Out of scope: this plan does not change the public teardown API or the
documented `Close()` semantics beyond making them robust; it does not introduce
per-resource allocator pools. It is purely an ownership-lifetime fix.
