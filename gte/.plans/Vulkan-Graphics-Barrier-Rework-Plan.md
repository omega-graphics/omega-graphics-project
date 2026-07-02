# Vulkan Graphics-Path Buffer-Barrier Rework Plan

## Goal

Re-enable automatic buffer hazard barriers for the **graphics** (vertex /
fragment) path in the Vulkan backend, so a genuine write→read dependency on a
storage buffer between draws (or between a compute write and a later graphics
read) is correctly ordered — without reintroducing the `VK_ERROR_DEVICE_LOST`
that caused the path to be switched off.

The compute path is already correct and shipping (see below); this plan closes
the remaining half so the barrier subsystem is trustworthy for all shader
stages, not just compute.

## What already exists (do not re-build)

The hazard-barrier machinery is written and working — the graphics half is just
gated off:

- **`GEVulkanCommandBuffer::insertResourceBarrierIfNeeded(GEVulkanBuffer*, ...)`**
  (`gte/src/vulkan/GEVulkanCommandQueue.cpp:140`) is the single choke point. It:
  - resolves the resource's IO mode via `getResourceIOModeForResourceID`
    (`:84`) — defaults to `INOUT` (read+write) when a location is absent, the
    safe conservative fallback;
  - computes `shaderAccess` (READ / WRITE / READ|WRITE) and `pipelineStage`
    (vertex / fragment / compute) from the bound shader;
  - emits a `vkCmdPipelineBarrier2Khr` (Sync2 path, `:176`) or
    `vkCmdPipelineBarrier` (fallback, `:221`) from the buffer's prior
    (access, stage) to the new one — but **only** when
    `priorAccess != 0 && priorPipelineAccess != 0`, so a buffer's first use
    emits no barrier and just records state;
  - then records the new `(priorAccess, priorPipelineAccess)` on the
    `GEVulkanBuffer` for next time (`:195`/`:243`).
- **Per-buffer state lives on the resource**, not the command buffer:
  `GEVulkanBuffer::priorAccess` / `priorPipelineAccess` (and the `*2` Sync2
  variants). The comment at `GEVulkan.cpp:2512` documents that
  `insertResourceBarrierIfNeeded` and `startRenderPass` keep this state
  current.
- **Compute is re-enabled and verified.** As of the barrier re-enable, the
  helper runs for `OMEGASL_SHADER_COMPUTE` and is skipped otherwise
  (`GEVulkanCommandQueue.cpp:148`-ish, the `shader.type != OMEGASL_SHADER_COMPUTE`
  guard). All 12 AQUA GPU tests pass, including the run-to-run determinism and
  CPU/GPU parity checks that the missing barrier had been breaking. **This
  proves the barrier construction itself (flags, stages, Sync2/fallback
  selection, state tracking) is correct** — the graphics failure is therefore
  about *where* the barrier is emitted, not *whether it is built right*.

## The problem — why the graphics path device-lost

The graphics buffer binds call the helper from inside descriptor binding:

- `bindResourceAtVertexShader(GEBuffer, id)` →
  `insertResourceBarrierIfNeeded(..., vertexShader->internal)`
  (`GEVulkanCommandQueue.cpp:1034`)
- `bindResourceAtFragmentShader(GEBuffer, id)` →
  `insertResourceBarrierIfNeeded(..., fragmentShader->internal)` (`:1160`)

These binds are recorded **during an active render pass** (a render pipeline is
bound and draws are being encoded between `startRenderPass` / `finishRenderPass`).
A `vkCmdPipelineBarrier` / `vkCmdPipelineBarrier2` that carries a
`VkBufferMemoryBarrier` is only legal inside a render pass when it is expressed
as a **subpass self-dependency** — i.e. a matching `VkSubpassDependency` with
`srcSubpass == dstSubpass` must exist on the render pass, and the barrier's
stage/access masks must be a subset of it (VUID-vkCmdPipelineBarrier-None-07889
and the render-pass-instance restrictions in
`VUID-vkCmdPipelineBarrier2-dependencyFlags-*`). OmegaGTE's compatibility render
passes declare no such self-dependency, so emitting the buffer barrier mid-pass
is undefined behaviour — benign on lenient drivers, `VK_ERROR_DEVICE_LOST` on
strict ones (the symptom that got the whole helper disabled).

This is a **design/placement flaw**, not a logic bug in the barrier itself.

## Approach (research → design)

Three known-good strategies exist for graphics-stage buffer hazards; we pick per
where the hazard actually is:

1. **Hoist the barrier out of the render pass.** A buffer written by a prior
   pass (compute, or an earlier render pass) and read by this draw must be
   barriered *before* `startRenderPass`, at the point the render pass's inputs
   are known — not lazily at descriptor-bind time. This is the correct place for
   the cross-pass RAW hazard and covers the real AQUA-adjacent case
   (compute writes → graphics reads).
2. **Subpass self-dependency**, for a genuine within-pass hazard (a draw reads
   what an earlier draw in the same pass wrote). Add a `VkSubpassDependency`
   with `srcSubpass == dstSubpass` to the compatibility render pass, and keep the
   mid-pass barrier but constrained to those masks. Heavier; only needed if we
   actually support intra-pass storage-buffer feedback (we largely do not today).
3. **Split the render pass**, when neither fits — end the pass, barrier, begin a
   fresh pass. Simplest correctness, worst performance; a fallback, not the
   default.

Default to **(1)**: move graphics buffer-hazard resolution to render-pass
setup, keep the mid-pass helper a no-op for graphics. Reserve (2) for a
demonstrated intra-pass need.

## Phases

### Phase 1 — Reproduce and characterize (no code change)
Stand up the smallest failing case: a render pipeline that binds a storage
buffer written by a prior compute (or render) pass, with the graphics guard in
`insertResourceBarrierIfNeeded` removed so the mid-pass barrier fires. Confirm
it reproduces `DEVICE_LOST` (or a VVL VUID hit) on this NVIDIA driver with the
validation layer on. Record the exact VUID. This turns the hypothesis above into
a confirmed diagnosis before any rework. (If it does *not* reproduce, the real
cause is elsewhere and Phases 2–3 are re-scoped.)

### Phase 2 — Hoist graphics buffer hazards to render-pass boundary
- Collect the buffers a render pass will read/write (from the bound pipeline's
  layout + the resources bound for the upcoming draws) and emit any needed
  `vkCmdPipelineBarrier2` **before** `startRenderPass`, reusing the exact
  flag/stage/state-tracking logic already in `insertResourceBarrierIfNeeded`
  (factor the barrier-emit core out so graphics and compute share it and it is
  not duplicated).
- Keep the mid-pass vertex/fragment helper calls as state-recording only (or
  remove them) so no `VkBufferMemoryBarrier` is ever emitted inside a render
  pass instance.
- Leave the compute path exactly as it is (works today).

### Phase 3 — Validate across backends and workloads
- Full VVL-clean run of the WTK graphics tests (BasicAppTest and the GTE
  cross-backend window test) — zero validation errors, no `DEVICE_LOST`.
- Re-run all 12 AQUA GPU tests (compute path must stay green — the refactor in
  Phase 2 touches shared barrier-emit code).
- Add a targeted GTE test for the cross-pass hazard: compute writes a buffer,
  a draw samples/reads it, assert the read sees the written values
  deterministically over repeated runs.

### Phase 4 (optional) — Intra-pass self-dependency
Only if Phase 1/3 surfaces a real within-pass storage-buffer feedback case: add
the `srcSubpass == dstSubpass` `VkSubpassDependency` to the compatibility render
pass (strategy 2) and route that specific hazard through it. Skip entirely if no
such case exists.

## Verification checklist
- [ ] Phase 1 VUID captured and named in this doc.
- [ ] `VK_ERROR_DEVICE_LOST` gone with graphics barriers active.
- [ ] WTK graphics tests VVL-clean.
- [ ] All 12 AQUA GPU tests still pass.
- [ ] New cross-pass hazard test passes deterministically over N runs.

## Context — how this plan came to exist
The umbrella barrier helper was disabled wholesale with a "keep BasicAppTest
stable while Vulkan sync is reworked" note, which silently broke GPU compute
determinism (multi-pass AQUA kernels raced on unsynchronized buffers →
non-deterministic, oracle-mismatching output, and downstream out-of-bounds
reads). That was fixed by re-enabling the helper for compute only. This plan
finishes the job for the graphics stages the note deferred.
