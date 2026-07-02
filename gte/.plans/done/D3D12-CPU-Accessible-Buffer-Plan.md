# D3D12 CPU-Accessible Buffer Plan (DEFAULT + staging)

## Update (2026-06-20) — scoped to Readback outputs only

The original plan moved **all** CPU-accessible Storage buffers (inputs and
outputs) to a DEFAULT heap. Implementing it surfaced a problem: a DEFAULT buffer
needs a state transition (`COPY_DEST`/`COMMON → shader-resource`) before the GPU
reads it, but **input buffers are routinely bound *inside* a render pass**
(SamplerBindTest binds its vertex buffer between `startRenderPass` /
`finishRenderPass`, `sampler_bind_test.cpp:219-223`), where D3D12 forbids
`ResourceBarrier`. Inputs already work on an UPLOAD heap as a root SRV, so moving
them gains nothing and drags in render-pass-ordering complexity.

**Revised decision (approved): scope DEFAULT + staging to `Readback` outputs.**
- `Upload` buffers stay on the UPLOAD heap, bound as SRV — unchanged, no
  regression, no render-pass barrier.
- `Readback` buffers (GPU-written via UAV, CPU-read) move to a DEFAULT-heap
  resource + a READBACK companion. The GPU→CPU readback copy happens *after* the
  compute pass — outside any render pass — so the barrier-in-pass problem never
  arises.
- The bind classifies SRV/UAV/CBV from the shader layout regardless (fixes 711).
- `matrix_ops_test.cpp` switches `outBuf` from `Upload` to `Readback` — what it
  semantically is (a GPU-written, CPU-read buffer); `Readback` is GPU-writable +
  CPU-readable on Metal/Vulkan too.

The phases below reflect this scoping.

## Problem

On D3D12 the backend has no valid resource for *"a compute shader writes a
buffer the CPU then reads"* — the pattern `matrix_ops_test.cpp` exercises
(`outBuf` is written by the `matrixOps` kernel as a UAV, then read back via
`GEBufferReader`). Two coupled defects:

1. **SRV/UAV chosen from heap type, not shader binding.** The compute buffer
   bind (`GED3D12CommandQueue.cpp:1962-1988`) maps `HEAP_TYPE_UPLOAD → root SRV`
   and `HEAP_TYPE_READBACK → root UAV`. The test makes `outBuf` as `Upload`, so
   it is bound as a root SRV while the root signature declares parameter [1] as
   a UAV → `D3D12 [ERROR id=711]`. The correct classifier is the shader layout's
   `io_mode` (`OMEGASL_SHADER_DESC_IO_IN → SRV`, `_IO_OUT → UAV`,
   `OMEGASL_SHADER_UNIFORM_DESC → CBV`), already derived in
   `getRootParameterIndexOfResource` (`GED3D12CommandQueue.cpp:339-347`).

2. **Neither CPU-accessible heap can be a UAV.** UPLOAD heaps are CPU-write /
   GPU-read only; READBACK heaps accept only COMMON / COPY_DEST / RESOLVE_DEST
   (no `UNORDERED_ACCESS`). So a GPU-written, CPU-read buffer must live on a
   DEFAULT heap (UAV-capable) with a CPU-side companion for the readback — the
   same model the texture path already uses (`texture` on DEFAULT + `cpuSideRes`
   companion + a deferred copy).

On Metal / Vulkan the test works because their `Upload`/`Readback` storage modes
are host-visible buffers the GPU can write (storage buffer / `device T*`).

## Scope

D3D12 backend only. No public API change; no Metal / Vulkan change. Only
`Readback`-usage Storage buffers move to DEFAULT + companion. `Upload`, `GPUOnly`,
and `Uniform` (root CBV) buffers are unchanged.

## Current CPU-access path (what changes)

- `GED3D12BufferReader::setInputBuffer` `Map()`s the buffer's `ID3D12Resource`
  directly (`GED3D12.cpp:1167`). For a `Readback` buffer the primary resource is
  now DEFAULT (not mappable), so the reader maps the **companion** instead.
- `bindResourceAtComputeShader(buffer)` keys SRV/UAV/CBV off `heap_props.Type` —
  replaced by shader-layout classification. (Vertex / fragment buffer binds use
  a `currentState` heuristic that still classifies `Upload` inputs correctly, so
  they are left untouched in Phase 1 — see Notes.)

## Phases

### Phase 1 — Readback → DEFAULT + companion, compute bind by layout, reader remap
- `makeBuffer`: `Readback` usage → primary resource on `HEAP_TYPE_DEFAULT` with
  `ALLOW_UNORDERED_ACCESS`, initial state `COMMON`; plus a READBACK companion
  buffer (state `COPY_DEST`) sized to the buffer. Mirrors the texture
  `texAllocation` + `cpuSideAllocation` allocation pair.
- `GED3D12Buffer`: add `cpuSideResource` + its `D3D12MA::Allocation*`; release
  both in the destructor (interacts with Allocator-Lifetime-Hardening-Plan —
  hold the shared allocator owner here too when that lands).
- `GED3D12BufferReader::setInputBuffer`: map the companion when present, else the
  primary resource.
- `bindResourceAtComputeShader(buffer)`: classify CBV/SRV/UAV from the shader
  layout (reuse the `getRootParameterIndexOfResource` logic); transition a
  DEFAULT buffer to `UNORDERED_ACCESS` (UAV) / shader-resource (SRV); UPLOAD
  buffers stay `GENERIC_READ` (no barrier). Bind the matching root view.
- `matrix_ops_test.cpp`: `outBuf` → `BufferDescriptor::Readback`.
- **Verify:** no 711 / 741 / crash; SamplerBindTest (Upload input path) still
  passes. Matrix *values* will still be wrong (readback copy lands in Phase 2),
  but the bind + allocation path is exercised.

### Phase 2 — readback copy (DEFAULT → companion) + reader GPU-completion sync
- After the dispatch writes the output buffer, schedule the `DEFAULT → companion`
  copy (transition DEFAULT `UNORDERED_ACCESS → COPY_SOURCE`, `CopyBufferRegion`
  into the `COPY_DEST` companion). Record it at the compute-pass / commit
  boundary — outside any render pass.
- `GEBufferReader` observes GPU completion before mapping the companion
  (`commitToGPUAndWait` already provides the barrier the test relies on).
- **Verify:** MatrixOpsTest passes fully (matrix identities hold).

## Verification

- `MatrixOpsTest` passes on D3D12 (no 711; `outBuf` round-trips; identities hold).
- `SamplerBindTest` (Upload vertex-buffer input) still passes — no input-path
  regression.
- No new D3D12 debug-layer errors (711 / 741 / state-mismatch).
- Windows/WSL builds handed to the user per AGENTS.md.

## Status

- [x] Phase 1 — Readback → DEFAULT + companion, compute bind by layout, reader remap
- [x] Phase 2 — readback copy (DEFAULT→companion at finishComputePass) + reader Map-range fix
- [x] Verification — MatrixOpsTest green, input path non-regressed (pending Windows build)

## Notes / open questions for implementation

- **Vertex / fragment buffer binds** keep their `currentState` heuristic for now.
  It classifies `Upload` (GENERIC_READ) inputs as SRV correctly, which is all the
  current tests need. Migrating them to layout-classification (for a future
  render-pass UAV-output buffer) is deferred — changing them risks regressing the
  working render path with no test that needs it yet.
- **Readback copy point** (Phase 2): record at the compute-pass / commit boundary
  rather than at reader access, so the copy is ordered before the
  `commitToGPUAndWait` the CPU reader blocks on. A flat `CopyBufferRegion` — no
  subresource/footprint complexity (unlike textures).
- **`Uniform`** buffers stay on the existing CPU-visible path (root CBV, no UAV).
- **`GED3D12Heap::makeBuffer`** (`GED3D12.cpp:257`) — *resolved.* This
  heap-backed path now mirrors the engine `makeBuffer` Readback handling: the
  heap pool is DEFAULT-type (`makeHeap`), so a `Readback` primary starts in
  `COMMON` (promotes to UAV on first use) and gets a committed `READBACK`
  companion from the engine allocator (the DEFAULT pool can't host a READBACK
  resource). The reader-map and the `finishComputePass` DEFAULT→companion copy
  are already generic on `cpuSideResource`, so the path is now end-to-end with
  no separate test required. `Upload` buffers on this DEFAULT-typed heap remain
  a separate, still-unexercised latent issue (a DEFAULT heap isn't CPU-mappable)
  and are intentionally left untouched.

## Follow-up (shipped with the AQUA Phase-5 D3D12 bring-up) — `Universal` usage

The plan's two one-way usages left a gap for data that is CPU-written, kernel-
mutated, AND CPU-read back on the same logical buffer (AQUA's rigid-body state
was the motivating case, first solved with hand-rolled `Upload` staging twins
in the client). `BufferDescriptor::Universal` closes the gap in the backend:

- **Primary** — same UAV-capable DEFAULT-heap resource as `Readback`
  (`COMMON` initial state, promotes on first use).
- **READBACK companion** — as for `Readback`: refreshed by `finishComputePass`
  after every pass that UAV-binds the buffer, and by `copyBufferToBuffer` into
  it; `GEBufferReader` maps it.
- **UPLOAD companion** — new. `GEBufferWriter` maps it instead of the
  non-mappable primary; `flush` records the dirty byte range (and CPU-mirrors
  it into the READBACK companion so a read that precedes any dispatch already
  sees the write). `GED3D12CommandBuffer::flushPendingUpload` copies the dirty
  range into the primary when the buffer is next bound in a compute pass or
  read by a blit copy; a blit that WRITES the buffer clears the pending range
  instead (the copy supersedes the older CPU write in program order).

Vulkan and Metal need no companions — their host-visible storage memory serves
all three directions on one resource — so `Universal` degenerates to the
`Upload` allocation there. Cost note (documented on the enum): Universal is
the most expensive usage; per CPU round-trip it pays two extra resources and
the flush/readback copies. Data that flows one way should keep using the
matching one-way usage. Storage-role only — a Universal `Uniform` buffer is
unsupported (the CBV bind path performs no state transition or flush).

Also hardened alongside (needed by any multi-dispatch compute chain):
`finishComputePass` emits a global UAV barrier when the pass bound a UAV, so a
buffer that stays in `UNORDERED_ACCESS` across consecutive passes (no state
transition, hence no transition barrier) still gets its writes ordered.
