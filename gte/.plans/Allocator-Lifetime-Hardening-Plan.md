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
- Mirror Phase 1 for `VmaAllocator`: an `AllocatorOwner` that calls
  `vmaDestroyAllocator` in its dtor, a `shared_ptr` on the engine, and a copy on
  each VMA-allocated `GEVulkanBuffer` / `GEVulkanTexture`. Confirm against the
  Vulkan teardown order (device/queue idle before allocator destroy).

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

- [ ] Phase 1 — D3D12 shared allocator ownership
- [ ] Phase 2 — Vulkan shared allocator ownership
- [ ] Phase 3 — Metal n/a verification + doc
- [ ] Verification — MatrixOpsTest passes with the band-aid reverted

## Notes

Out of scope: this plan does not change the public teardown API or the
documented `Close()` semantics beyond making them robust; it does not introduce
per-resource allocator pools. It is purely an ownership-lifetime fix.
