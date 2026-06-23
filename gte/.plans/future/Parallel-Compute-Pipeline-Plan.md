# Parallel Compute Pipeline Plan

> **Status — proposal / not yet implemented.** This is the multi-phase plan
> required by `AGENTS.md` ("Code Authoring") before any code lands. No code in
> this plan has been written. Phases are sequenced so each lands as a
> reviewable increment.

## Goal

Run **one** OmegaSL compute kernel across **multiple discrete GPUs** at once.
The caller hands us a dataset (an array of `N` fixed-stride elements); the
engine splits it into one contiguous shard per device, uploads each shard to
that device, dispatches the *same* kernel on each device against its shard,
then gathers each device's output portion back into a single host buffer. The
kernel itself stays multi-GPU-agnostic — every device just runs the ordinary
single-device compute path on the slice it was given.

Only **discrete** GPUs participate (`GTEDevice::Type::Discrete`). Integrated
GPUs are excluded by design: they share system memory with the host and with
each other, so fanning a partition out to them buys no extra memory bandwidth
and only adds staging/sync overhead.

## Naming decision — `ParallelComputePipeline`

The prompt asked whether `ParallelComputePipeline` or
`MultiDispatchComputePipeline` is more accurate. **Recommendation:
`GEParallelComputePipeline`** (orchestrator object, `GE`-prefixed to match
`GEComputePipelineState` / `GERenderPipelineState`) with a
`ParallelComputePipelineDescriptor` (no prefix — matches its sibling
descriptors `ComputePipelineDescriptor` / `RenderPipelineDescriptor` /
`BlitPipelineDescriptor` / `MeshPipelineDescriptor` at `GEPipeline.h:198-293`).

Reasoning:

- **"MultiDispatch" is actively misleading here.** This codebase already uses
  "multi" / "indirect" vocabulary for *single-device* batched submission —
  `GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT` (`GTEDevice.h:31`),
  `dispatchThreadgroupsIndirect` (`GERenderTarget.h:362`). A reader seeing
  "MultiDispatch" will assume several dispatch commands on one queue, not a
  fan-out across physical devices. The number of dispatch calls is not the
  salient axis.
- **"Parallel" captures the distinguishing axis** — the same pipeline running
  concurrently across the parallel *set of devices* on disjoint data shards
  (classic SPMD / data-parallel execution).
- A strictly-most-literal alternative — `GEMultiDeviceComputePipeline` /
  `GEDistributedComputePipeline` — is noted under **Open Decisions** in case
  the team prefers naming by the "multi-device" axis directly. `Parallel` is
  the recommendation because it reads cleanly and was the lead candidate.

## What already exists (build on it, don't rebuild)

The entire single-device compute path is complete and is the unit this plan
fans out. **No backend code is needed** — the orchestrator composes only the
public, backend-neutral `OmegaGraphicsEngine` surface, exactly like `Init()`
(`OmegaGTE.cpp:131`) and `installCommitAggregator` (`GECommandQueue.h:291`).

- **Device enumeration + type tag.** `enumerateDevices()` returns every GPU
  on the system (`GTEDevice.h:178`); each carries `const Type type` with
  `Type { Integrated, Discrete }` (`GTEDevice.h:153-158`), a `name`, and a
  `GTEDeviceFeatures features` (`GTEDevice.h:160-162`). Filtering for
  `Discrete` is a one-liner.
- **Per-device engine factory.** `OmegaGraphicsEngine::Create(device)`
  (`GE.cpp:230`) builds an engine bound to one device.
  `OmegaSLCompiler::Create(device)` compiles OmegaSL for a device. `Init()`
  bundles both into a `GTE` (`GTEDevice.h:181-185`) — one bundle per device.
- **Compute pipeline + dispatch.** `makeComputePipelineState`
  (`GE.h:498`) from `ComputePipelineDescriptor { String name;
  SharedHandle<GTEShader> computeFunc; }` (`GEPipeline.h:198-202`); recorded
  via `startComputePass` → `setComputePipelineState` →
  `bindResourceAtComputeShader` → `setComputeConstants` →
  `dispatchThreads` / `dispatchThreadgroups` → `finishComputePass`
  (`GERenderTarget.h:322-371`).
- **Queues, commit, and timing.** `makeCommandQueue(GECommandQueueDesc)`
  (`GE.h:573`); `submitCommandBuffer` / `commitToGPU` /
  `commitToGPUAndWait` and the async
  `commitToGPU(const GECommitCompletionHandler&)` (`GECommandQueue.h:251-281`)
  for a non-blocking host-side join.
- **Host-visible staging buffers.** `BufferDescriptor { Upload, Readback,
  GPUOnly }` (`GE.h:192-197`); `Upload` for scatter, `Readback` for gather
  (D3D12 readback heap landed in commit `57b9577`).
- **Reference flow.** `gte/tests/vulkan/ComputeTest/main.cpp` is the exact
  single-device sequence (writer → buffer → pass → `dispatchThreads` →
  `commitToGPUAndWait` → reader) this plan replicates per lane.

## What does NOT exist (the gap this plan fills)

- **No object spans devices.** Engines, pipelines, queues, buffers, and shader
  libraries are all device-local — created from one `OmegaGraphicsEngine`. A
  `GEComputePipelineState` built on device A *cannot* be bound on device B,
  and a `GTEShaderLibrary` is loaded through one engine. The descriptor for
  the new pipeline therefore **cannot accept a `GTEShader`/pipeline handle**;
  it must accept device-independent shader input (compiled lib or source) and
  re-materialize the pipeline on each device. This is the central correctness
  constraint.
- **No cross-device memory path.** Discrete GPUs don't share memory and there
  is no peer-to-peer copy API. All cross-device movement goes **through host
  memory** via `Upload`/`Readback` staging. Accepted as the only portable
  transport; P2P / vendor-interconnect fast paths (NVLink / xGMI / Xe Link) are
  out of scope for the *base* pipeline — pursued separately in Appendix A.6.
- **No cross-device sync primitive.** `GEFence` is created by one engine
  (`makeFence`, `GE.h:456`) and is device-local. Lanes therefore **cannot**
  be joined with a GPU fence — the join is host-side (completion handlers +
  a countdown, or a thread per lane).

## Execution contract (the kernel author's view)

The feature targets **embarrassingly-parallel, element-local** kernels:
`output[i]` depends only on `input[i]` (or on a bounded local neighborhood
*within the same shard*). This is exactly "take a partition, output its
portion." Each device sees a **local** input buffer holding only its shard and
a **local** index space `[0, shardCount)`; the orchestrator owns global
placement on gather. Kernels needing cross-shard neighbors ("halo"/stencil
exchange) are **out of scope** — documented as a non-goal, with a note that
the optional per-lane base-offset push-constant (below) is the seam a future
halo extension would build on.

## Design

### Lane model

A `GEParallelComputePipeline` owns one **lane** per participating discrete
device. A lane is a backend-neutral struct:

```cpp
struct Lane {
    SharedHandle<GTEDevice>             device;        // Discrete only
    SharedHandle<OmegaGraphicsEngine>   engine;        // Create(device)
    SharedHandle<GTEShaderLibrary>      lib;           // kernel loaded on this engine
    SharedHandle<GEComputePipelineState> pipeline;     // built on this engine
    SharedHandle<GECommandQueue>        queue;         // Type::Compute
    SharedHandle<GEBuffer>              inputBuf;       // Upload,  sized to shard
    SharedHandle<GEBuffer>              outputBuf;      // Readback, sized to shard
    size_t                              elemOffset;     // first global element index
    size_t                              elemCount;      // shard length in elements
};
```

Lanes are fully independent: no lane reads another lane's resources, so there
is no cross-device hazard and no fence between lanes.

### Public surface (header sketch, `GEPipeline.h` + a new factory)

```cpp
// Descriptor — device-independent shader input (see "What does NOT exist").
struct OMEGAGTE_EXPORT ParallelComputePipelineDescriptor {
    OmegaCommon::String name;
    // Precompiled once by the caller (any OmegaSLCompiler); loaded per-device.
    std::shared_ptr<omegasl_shader_lib> computeLib;
    OmegaCommon::String computeEntry;     // entry-point name within computeLib
    unsigned inputSlot  = 0;              // [in ...] buffer binding
    unsigned outputSlot = 1;              // [out ...] buffer binding
    // Partition strategy across lanes; see Phase 2.
    enum class Partition { Uniform, ByMemoryBudget, Explicit } partition = Partition::Uniform;
};

// One dispatch over a host dataset.
struct OMEGAGTE_EXPORT ParallelDispatchDesc {
    const void *input;     size_t elementCount; size_t inputStride;
    void       *output;    size_t outputStride;            // defaults to inputStride
    // Optional: when set, each lane receives its global base index as a
    // push-constant via setComputeConstants (offset 0), for kernels that
    // need to know where their shard sits. Off by default (local indexing).
    bool        provideBaseOffsetConstant = false;
};

using GEParallelComputePipeline = struct __GEParallelComputePipeline;
```

Factory placement — **Open Decision**, leaning toward a free function so the
multi-device object is not implied to belong to any single engine:

```cpp
// gte/include/omegaGTE/GTEDevice.h, next to enumerateDevices()
OMEGAGTE_EXPORT SharedHandle<GEParallelComputePipeline>
    makeParallelComputePipeline(ParallelComputePipelineDescriptor & desc,
                                OmegaCommon::ArrayRef<SharedHandle<GTEDevice>> devices = {});
// Empty `devices` ⇒ auto-select all discrete devices from enumerateDevices().
```

`__GEParallelComputePipeline` exposes:

```cpp
size_t laneCount() const;                       // discrete devices actually engaged
void   dispatch(const ParallelDispatchDesc &);  // scatter → run all → gather (blocking)
```

### Data flow of one `dispatch()`

1. **Partition.** Split `[0, elementCount)` into `laneCount()` contiguous
   shards (strategy per descriptor). Record `elemOffset` / `elemCount` per
   lane.
2. **Resize + scatter.** (Re)allocate each lane's `inputBuf`/`outputBuf` to its
   shard size; `memcpy` the shard's bytes into the lane's `Upload` input
   buffer (via `GEBufferWriter` / mapped pointer, mirroring
   `ComputeTest/main.cpp`).
3. **Record + commit per lane.** For each lane: `startComputePass` →
   `setComputePipelineState` → bind `inputBuf`@`inputSlot`,
   `outputBuf`@`outputSlot` → optional `setComputeConstants(&elemOffset,4)` →
   `dispatchThreads(shardCount,1,1)` → `finishComputePass` →
   `submitCommandBuffer`. Then `commitToGPU(onComplete)` — **async**, so all
   lanes run concurrently on their own devices.
4. **Host-side join.** A countdown latch (one count per lane) released by each
   lane's completion handler. `dispatch()` blocks until all lanes report.
   (Fences can't cross devices — see the gap section.)
5. **Gather.** Read each lane's `Readback` output buffer and copy its bytes to
   `output + elemOffset * outputStride`, reassembling the full result.

## Phases

> **Granularity note (AGENTS.md).** Each phase below is a coherent, reviewable
> increment over ~300 lines and warrants its own breakdown. Phases 4–5 are
> smaller; they are kept as single notes per the "small features" exception.

### Phase 0 — Public surface + discrete selection (foundation)
- Add `ParallelComputePipelineDescriptor`, `ParallelDispatchDesc`,
  `__GEParallelComputePipeline` forward decl, and `makeParallelComputePipeline`
  declaration. Pure header + stub `.cpp` returning a not-yet-implemented
  sentinel.
- Discrete-device selection helper: filter `enumerateDevices()` by
  `type == Discrete`; intersect with caller-supplied `devices` if non-empty.
- **Verify:** unit-level — assert the selector returns only `Discrete` devices
  and preserves order; header compiles into `ComputeTest`.

### Phase 1 — Lane construction (per-device materialization)
- Build one `Lane` per selected device: `OmegaGraphicsEngine::Create(device)`,
  `engine->loadShaderLibraryRuntime(desc.computeLib)`, pick
  `shaders[desc.computeEntry]` into a `ComputePipelineDescriptor`,
  `makeComputePipelineState`, and a `Type::Compute` `makeCommandQueue`.
- Reject a lane (with a `DEBUG_ERROR` on `DEBUG_DOMAIN_PIPELINE`) whose device
  lacks a feature the shader requires — `loadShaderLibraryRuntime` already
  masks per-engine via `_deviceFeatures` (`GE.h:375`), so a missing entry
  surfaces here cleanly rather than crashing at dispatch.
- **Verify:** construct the pipeline against `[defaultDevice]` (a 1-lane
  degenerate case) and confirm `laneCount() == 1` and the pipeline/queue are
  non-null.

### Phase 2 — Partitioning + scatter
- Implement `Partition::Uniform` (equal element counts, remainder to the first
  lanes) and `Partition::ByMemoryBudget` (weight shard size by
  `device->queryMemoryBudget().availableVideoMemory`, `GTEDevice.h:171`).
  `Explicit` reads caller-supplied offsets (deferred field; can land as a
  follow-up).
- Per-lane buffer (re)allocation to shard size; scatter host bytes into each
  `Upload` input buffer.
- **Verify:** with `laneCount()==1`, the whole dataset lands in lane 0; with a
  forced 2-way split (see Phase 5 simulation), shard offsets/counts tile
  `[0,N)` exactly with no gap/overlap.

### Phase 3 — Concurrent dispatch + join + gather
- Record/commit all lanes async via
  `commitToGPU(const GECommitCompletionHandler&)`; join on a countdown latch;
  gather each `Readback` buffer into the host output at `elemOffset`.
- **Verify:** end-to-end on 1 lane — bytes out == single-device
  `ComputeTest` result for the same kernel/data (equivalence check).

### Phase 4 — Capability gating + single-GPU fallback + diagnostics (note)
- `< 2` discrete devices ⇒ degrade transparently to a 1-lane pipeline (still
  correct, just no speedup); `0` discrete devices ⇒ `makeParallelComputePipeline`
  returns `nullptr` with a `DEBUG_CRITICAL(DEBUG_DOMAIN_GENERAL, …)`.
- Log lane count, per-lane shard sizes, and join wait at `DEBUG_INFO` /
  `DEBUG_TRACE` (`GE.h:118-137`), gated on the debug layer.

### Phase 5 — Test (note)
- New `gte/tests/<backend>/ParallelComputeTest/main.cpp`: the
  `doubleValues` kernel from `ComputeTest` over `N=256`, validated against
  the CPU reference (`output[i] == input[i]*2`).
- **Single-GPU host caveat (verification).** Real multi-GPU concurrency can't
  be exercised on a 1-discrete-GPU CI box. The test must (a) run correctly on
  1 lane, and (b) include a **partition-simulation** unit that drives the
  Phase 2 split math with a forced `laneCount` of 2–4 (no second device
  required) and asserts the shards tile `[0,N)` and the gather reassembles
  byte-exactly. True 2-GPU execution is left **run-unverified** until a
  multi-discrete-GPU host is available.

## Backend verification status (per repo convention)

Mark unverified backends in the plan, not just chat
(`feedback_mark_unverified_backends_in_plan`):

- **Core orchestrator: backend-neutral.** Lives entirely in `gte/src/` over the
  public `OmegaGraphicsEngine` API — one build, no `#ifdef`. Compiles and runs
  on the Linux **Vulkan** host.
- **Vulkan:** core path compile/run-verifiable here, but only with **1 discrete
  GPU** unless this host has ≥2. The 2+ -GPU path is **run-unverified** until
  such hardware exists.
- **D3D12:** **compile- and run-unverified** (off-platform / WSL hand-off). The
  per-device path it fans out is the existing, separately-tested D3D12 compute
  + readback-heap path (commit `57b9577`).
- **Metal:** **compile- and run-unverified** (off-platform). Note macOS rarely
  exposes >1 discrete GPU today, so Metal will almost always run the 1-lane
  fallback.

## Build scoping — `OMEGA_BUILD_ML`

The multi-GPU / massively-parallel surface is for **datacenter / ML-server**
deployments (the confirmed target: datacenter cards behind a PCIe switch on a
server board), not consumer/desktop app builds. Gate it behind a CMake option so
normal OmegaGTE/WTK builds don't pull in the heavy compute stack, the
external-memory plumbing, or the `gpu-databridge` dependency. This mirrors the
repo's existing feature-flag pattern (e.g. `OMEGAWTK_ENABLE_CONTENT_CACHE`).

**The dividing line: language vs. runtime.** `OMEGA_BUILD_ML` gates only the
multi-GPU *runtime orchestration*. The OmegaSL **language / compiler** features
are **never** gated — a consumer must be able to author and compile an ML kernel
(subgroup ops, tensor-core `coopmatrix`) and run it **single-device** through the
ordinary `makeComputePipelineState` + dispatch path, in any build, without
pulling in the multi-GPU stack. Gating the language behind a multi-GPU build flag
would wrongly forbid single-GPU ML kernels. `omegaslc`, the three codegens, and
the `OMEGASL_FEATURE_BIT_*` machinery therefore stay outside the macro.

- **`OMEGA_BUILD_ML`** — CMake option, **default `OFF`**. When `ON`, the build
  defines the `OMEGA_BUILD_ML` preprocessor macro and compiles the surface below.
- **Gated ON (compiled only under `OMEGA_BUILD_ML`) — runtime orchestration only:**
  - The base `GEParallelComputePipeline` orchestrator + `makeParallelComputePipeline`
    (Phases 0–5).
  - Appendix **A.4** collectives API + host-ring transport.
  - Appendix **A.6** `gpu-databridge` integration — the `exportable`
    `BufferDescriptor` flag, the Vulkan external-memory export plumbing, and the
    AUTOMDEPS pull of `gpu-databridge`. These **additionally** require Linux +
    `TARGET_VULKAN` (the P2P path is Linux-only), i.e. effectively
    `OMEGA_BUILD_ML && TARGET_VULKAN`. On a non-Linux ML build the collectives
    still work — they just run the host-ring transport with no bridge.
  - Appendix **A.9** *distributed* GEMM orchestration — the multi-GPU fan-out
    (partition + scatter `A` + broadcast `B` + gather `C`). Multi-GPU runtime, so
    gated like the rest of the orchestrator. Its single-device kernel is **not**
    gated (below).
  - Appendix **A.9** *vendor GEMM-library fast paths* — each behind its own
    per-vendor sub-option (`OMEGA_ML_CUBLAS`, `OMEGA_ML_HIPBLASLT`,
    `OMEGA_ML_ONEMKL`; all **default `OFF`**, each requiring that vendor's toolkit —
    CUDA / ROCm / oneAPI — present at build time). They reuse the A.6 exportable-
    buffer + external-semaphore seam, so they share A.6's Linux + `TARGET_VULKAN`
    envelope. With none enabled, the portable `coopmatrix` GEMM is the only path —
    and still correct.
- **NOT gated (always built) — all OmegaSL language features:**
  - Appendix **A.2** subgroup/wave ops — a general language feature useful for any
    compute/fragment shader.
  - Appendix **A.3** cooperative-matrix / tensor-core extension (+ its feature
    bit). It is a *language* feature: the `coopmatrix` type and `coop_*` builtins
    let a kernel use tensor cores on a single device through the normal compute
    pipeline, independent of any multi-GPU orchestration. Ships regardless of
    `OMEGA_BUILD_ML`.
  - Appendix **A.9**'s *single-device* tiled GEMM **kernel** — just an OmegaSL
    kernel over the A.3 `coopmatrix` primitive, dispatched through the ordinary
    single-device compute path. Like any ML kernel it ships in every build; only
    its multi-GPU fan-out (gated, above) needs `OMEGA_BUILD_ML`.
- **Public-header discipline.** Types that only exist under `OMEGA_BUILD_ML`
  (e.g. `GEParallelComputePipeline`, the `exportable` flag) must be `#ifdef`-guarded
  in the public headers so a consumer built without the macro sees a coherent,
  smaller API rather than dangling declarations — same hygiene the backend
  `TARGET_*` guards already follow in `GE.h`.

## Open Decisions (need the developer's call before Phase 0)

1. **Name.** `GEParallelComputePipeline` (recommended) vs. the literal
   `GEMultiDeviceComputePipeline` / `GEDistributedComputePipeline`. `Parallel`
   reads well and was the lead candidate; "multi-device" names the axis most
   precisely. Your call governs every symbol below.
2. **Shader input.** Precompiled `omegasl_shader_lib` + entry name
   (recommended — caller compiles once, we load per-engine) vs. also accepting
   a raw OmegaSL **source string** (convenience; we compile per device). The
   latter is the only correct path if devices differ in capability and need
   per-device codegen — worth a convenience overload.
3. **Factory location.** Free `makeParallelComputePipeline()` next to
   `enumerateDevices()` (recommended — the object owns N engines, belongs to no
   single one) vs. a method on a "primary" `OmegaGraphicsEngine`.
4. **Join mechanism.** Async `commitToGPU(handler)` + countdown latch
   (recommended — no extra threads) vs. a worker thread per lane each calling
   `commitToGPUAndWait`. Both are host-side; the former composes with the
   existing completion path.
5. **Output buffer mode.** `Readback` (recommended, proper gather path) vs.
   `Upload` reused as in-out (what `ComputeTest` does today). `Readback`
   exercises the newer D3D12 readback-heap path.

## Non-goals

- Peer-to-peer cross-GPU copies in the *base* pipeline (it always stages
  through host). Direct GPU↔GPU transfer is pursued separately as a Linux-only,
  feasibility-gated sub-track — see Appendix A.6 (`gpu-databridge`), which rides
  each vendor's high-bandwidth GPU interconnect when present — **NVLink/NVSwitch**
  on NVIDIA, **xGMI / Infinity Fabric** on AMD, **Xe Link** on Intel — and falls
  back to direct PCIe P2P otherwise.
- Halo / stencil kernels that need neighbor data across shard boundaries.
- Integrated-GPU participation, or mixing discrete + integrated in one pipeline.
- Cross-device GPU fences (no such primitive exists; join is host-side).
- Dynamic load-balancing / work-stealing across lanes within a single dispatch
  (partition is decided up front per `Partition` strategy).

---

# Appendix A — CUDA-class massively-parallel compute (portable extension track)

> **Status — committed direction, phases not yet implemented.** This appendix
> maps "CUDA-style massively-parallel compute" onto what OmegaGTE/OmegaSL
> *actually have today* and defines the **portable** extension track to close
> the gaps — staying on the Vulkan/D3D12/Metal abstraction, no CUDA dependency,
> works on every vendor. A native-CUDA *execution* backend (NVPTX), NCCL, and an
> MLIR codegen re-architecture were considered and **deferred** (see A.7 — the
> reasoning survives). Direct GPU↔GPU **P2P** — over each vendor's interconnect
> (NVLink / xGMI / Xe Link) or PCIe P2P as the floor — is *not* deferred: it is
> pursued as a Linux-only, feasibility-gated sub-track in its own repo
> (`gpu-databridge`) — see A.6.

## A.0 First, correct the record — most of CUDA's *single-device* model already exists

A scan of the compiler (not the stale `OmegaSL-Reference.md` §11, which predates
several features) shows the single-device "massively parallel" toolkit is
**already largely shipped**. We must not re-propose it:

| CUDA primitive | OmegaSL / OmegaGTE status today | Evidence |
|---|---|---|
| grid / block / thread launch | **Shipped** — `compute(x,y,z)`, `dispatchThreadgroups`/`dispatchThreads` | `GERenderTarget.h:356-359` |
| `blockIdx`/`threadIdx` | **Shipped** — `GlobalThreadID` / `LocalThreadID` | `OmegaSL-Reference.md:773` |
| `__shared__` block memory | **Shipped** — `groupshared`/`threadgroup` storage class | `AST.h:414`, `CodeGen.cpp:1044`, `Target.h:522` |
| `__syncthreads()` / memory fences | **Shipped** — `threadgroup_barrier`, `device_barrier` | `AST.cpp:749`, `MSLTarget.cpp:1000` |
| `atomicAdd/Min/Max/CAS/...` | **Shipped** — full atomic builtin set | `AST.cpp:742-745`, `GLSLTarget.cpp:1731` |
| push / root constants | **Shipped** — `constant<T>` / `setComputeConstants` | `GERenderTarget.h:353` |
| indirect launch | **Shipped** — `dispatchThreadgroupsIndirect` | `GERenderTarget.h:362` |
| streams (async submit) | **Shipped** — `GECommandQueue` + async `commitToGPU(handler)` | `GECommandQueue.h:276` |
| events | **Shipped** — `GEFence` (device-local) | `GE.h:456` |
| warp shuffle / ballot / reduce / scan (warp-level primitives, cooperative-group tiles) | **Gap → A.2** — no subgroup builtins in the reserved set | `OmegaSL-Swizzle-Subgroup-Plan.md` (Part B) |
| **tensor cores** (`wmma` / `mma.sync`) | **Gap → A.3** — explicitly out of scope of every existing plan | Subgroup plan "Out of scope" row |
| multi-GPU collective exchange (all-reduce / all-gather / broadcast / reduce-scatter) | **Gap → A.4** — base plan only scatters/gathers, never exchanges | this doc |

**Takeaway:** the question is *not* "add CUDA's programming model" — most of it is
here. The real gaps that matter for *massively-parallel data* work and that this
track closes are three: **(1) warp/subgroup ops, (2) tensor-core matrix ops,
(3) multi-GPU collectives.** Everything else is a naming/ergonomics layer over
what exists.

## A.1 Scope of this track

Three portable additions — plus one capstone *workload* that composes them —
each extending machinery that already exists and each staying true to OmegaGTE's
cross-vendor charter — no new GPU dependency:

- **A.2** subgroup / warp operations (OmegaSL surface only).
- **A.3** cooperative-matrix / tensor-core intrinsics (new OmegaSL type + builtins).
- **A.4** multi-GPU collectives on the base plan's lane model (backend-neutral).
- **A.9** distributed **GEMM** — the capstone workload built on A.3 (the
  tensor-core primitive) + A.4 (cross-lane data sharing). Not a new
  language/runtime primitive; it is the headline application that proves the three
  above pay off, and lands last (Phase A5). Its portable `coopmatrix` path keeps
  the no-new-dependency charter; A.9 **also** adds *optional, build-gated* vendor
  GEMM-library fast paths (cuBLASLt / hipBLASLt / oneMKL) — the one place this
  track takes on vendor deps, and only opt-in, with the portable path always the
  fallback.

None should start before the base parallel pipeline (Phases 0–3) is landed and
the lane model is real.

## A.2 OmegaSL extension — subgroup / warp operations

Already specified in `OmegaSL-Swizzle-Subgroup-Plan.md` Part B (ballot,
broadcast, any/all, reductions, prefix scans, quad ops) over the portable
intrinsics: D3D12 SM6 `WaveActive*`, Metal `simd_*`/`quad_*`, GLSL
`GL_KHR_shader_subgroup_*`. These are CUDA's warp-level primitives by another
name and are the prerequisite for fast reductions/scans without round-tripping
through `groupshared`. **Action:** adopt that plan as-is; gate via the existing
`OMEGASL_FEATURE_BIT_SUBGROUP_OPS` (`omegasl.h:268`) /
`GTEDEVICE_FEATURE_*` masking the runtime loader already performs. No new design
needed here — it is a scheduling decision, not an architecture one.

## A.3 OmegaSL extension — cooperative matrix / tensor cores

The single highest-value *new* OmegaSL surface for "massively parallel data" (ML
GEMM/conv, large reductions). Tensor cores are portably reachable today:

- **Vulkan** — `VK_KHR_cooperative_matrix` (SPIR-V `OpCooperativeMatrix*`).
- **D3D12** — SM 6.8 wave-matrix (`WaveMatrix*`) where available.
- **Metal** — `simdgroup_matrix<T, M, N>` load/multiply/store.

Proposed OmegaSL surface (sketch — needs the full lexer→Sema→3×codegen pass that
every OmegaSL type addition takes, plus a `cooperative_matrix` feature bit added
to `omegasl.h` and `GTEDeviceFeatures`):

```omegasl
// element type + shape + matrix-use (a/b/accumulator), like WMMA fragments
coopmatrix<float16, 16, 16, MatrixA> a;
coopmatrix<float16, 16, 16, MatrixB> b;
coopmatrix<float32, 16, 16, Accumulator> acc;
coop_load(a, inputA, /*stride*/K);
coop_load(b, inputB, /*stride*/N);
acc = coop_mma(a, b, acc);          // D = A·B + C, one tensor-core op
coop_store(outputC, acc, /*stride*/N);
```

This is a real subsystem — a new resource/value category in the type system,
like the matrix-type or mesh-shader additions were — with **three hand-written
codegens** (Vulkan/D3D12/Metal), matching how every other OmegaSL type has been
added. It is the largest item in this track. Its headline consumer — a tiled,
optionally multi-GPU **GEMM** — is specified as the capstone workload in **A.9**;
the `coop_load`/`coop_mma`/`coop_store` triple above is literally that GEMM's
inner loop.

## A.4 Multi-GPU collectives

The base plan only does *scatter → independent compute → gather*; that covers
embarrassingly-parallel kernels but not anything where lanes must **exchange**
partial results (distributed reductions, all-to-all, ML data-parallel gradient
sync). Add a portable collectives layer to `GEParallelComputePipeline`:

```cpp
enum class Collective { AllReduce, ReduceScatter, AllGather, Broadcast, Reduce };
enum class ReduceOp   { Sum, Min, Max, Prod };
// Run after per-lane compute, before/around gather.
void allReduce(SharedHandle<GEBuffer> perLaneInOut[], ReduceOp op);
```

**Implementation (portable):** a host-side ring/tree over the existing
`Upload`/`Readback` staging — correct everywhere, bandwidth-bounded by host
PCIe, no new deps. Good enough for modest lane counts and the common
data-parallel "sum the per-shard partials" case. This directly completes the
*parallel* pipeline the whole document is about, and ships without any external
commitment. The collectives API is **transport-agnostic**: when a lane pair can
do direct GPU↔GPU P2P (Linux), the same `allReduce`/etc. calls swap the host
ring for the `gpu-databridge` transport (A.6); the host ring stays the universal
fallback.

## A.5 Phasing (layered onto the base plan)

- **Phase A1 — Subgroup/wave ops.** Execute `OmegaSL-Swizzle-Subgroup-Plan.md`
  Part B. Pure OmegaSL surface; no engine change. Low risk.
- **Phase A2 — Portable collectives.** Add the `Collective`/`ReduceOp` API
  (A.4) with the host-ring implementation over the base plan's staging.
  Backend-neutral, like the rest of the orchestrator.
- **Phase A3 — Cooperative-matrix OmegaSL extension.** New `coopmatrix` type +
  `coop_load`/`coop_mma`/`coop_store` builtins + feature bit, with three
  hand-written codegens (Vulkan/D3D12/Metal). Largest item; a full
  language-feature subsystem. Mark D3D12/Metal codegens **compile/run-unverified
  off-platform** (per repo convention).
- **Phase A4 — (feasibility-gated) `gpu-databridge` transport.** Run the A.6
  spike on real target hardware *first*; only on a "go" stand up the separate repo
  + the single OmegaGTE seam (exportable buffers) and wire it under the A.4
  collectives API as the fast transport. **NVIDIA CUDA backend first** (rides
  NVLink/NVSwitch transparently); **AMD HIP backend (rides xGMI)** and **Intel
  Level Zero backend (rides Xe Link)** follow, with Vulkan-dma-buf PCIe-P2P as
  each vendor's neutral fallback. Linux
  + `OMEGA_BUILD_ML` only; host ring stays the fallback. Do not start before A2
  (it has nothing to accelerate until the collectives API exists).
- **Phase A5 — Distributed GEMM (capstone, the later phase).** The portable GEMM
  workload (A.9): a single-device tiled GEMM kernel on `coopmatrix`, then fanned
  out across lanes (scatter `A` row-panels, broadcast `B`, gather `C`). Depends on
  **A3** (the kernel's inner loop) and, for the distributed path, **A2**
  (collectives for the `B` broadcast / panel exchange); the A4 `gpu-databridge`
  transport, when present, accelerates that exchange. **Vendor GEMM-library fast
  paths** (cuBLASLt / hipBLASLt / oneMKL) layer on as opt-in, build-gated per-lane
  accelerators that reuse A4's exportable-buffer + external-semaphore seam, with
  the portable kernel as the fallback. Lands **after** A3 — the latest phase in
  this track. Mark D3D12/Metal **and the vendor fast paths**
  **compile/run-unverified off-platform** (inherits A3's coopmatrix codegen status;
  the vendor libraries need their toolkits + matching hardware).

## A.6 Cross-lane exchange via direct GPU↔GPU P2P — the `gpu-databridge` sub-track (Linux)

**The idea (developer's).** Instead of routing cross-lane exchange (A.4) through
host memory, move data **GPU→GPU directly** with a low-level, vendor-neutral
transport in its own repo, `gpu-databridge`. It rides **each vendor's
high-bandwidth GPU interconnect when the topology has one** — **NVLink / NVSwitch**
on NVIDIA, **xGMI (Infinity Fabric Link)** on AMD, **Xe Link** on Intel (each
tens-to-hundreds of GB/s — near the on-package interconnect ceiling) — and falls
back to **direct PCIe P2P** otherwise; either way it skips the host round-trip.
The pattern is identical across vendors: each vendor's *native* runtime peer-copy
(CUDA / HIP / Level Zero) transparently routes over that vendor's fabric when
present, so the bridge gets the fast interconnect "for free" through the same call
it would use for PCIe P2P. Hard constraint the developer set: **data is shared
only between GPUs of the same architecture** (two H100s, two MI300s) — never
across vendors or arches.

**Platform & build scope (whole sub-track): Linux + `OMEGA_BUILD_ML`.** Per the
scoping call, parallel-data-compute targets Unix/ML servers; this sub-track
compiles only under `OMEGA_BUILD_ML && TARGET_VULKAN` (see "Build scoping" in the
base plan). macOS (≤1 discrete GPU in practice) and Windows (D3D12 cross-adapter
copies stage through system memory unless on a same-vendor *linked* adapter node)
are not P2P targets. The base pipeline's host-staging transport remains the
portable fallback everywhere, including Linux pairs that fail the probe.

**Confirmed target hardware:** datacenter cards behind a **PCIe switch** on a
**server board**. This is the clean case — a PCIe switch routes peer traffic
device-to-device without forcing it up to the IOMMU, server boards expose proper
ACS/ReBAR, and datacenter GPUs ship validated vendor P2P. The consumer-board
ACS-override hazard below does **not** apply to this deployment; it is kept only
as a portability caveat for anyone who runs the bridge elsewhere.

### Why "same architecture" is the *right* constraint, not a limitation
PCIe P2P DMA needs the two GPUs' copy engines and drivers to map each other's
VRAM aperture. Same vendor+arch guarantees identical BAR semantics, page sizes,
and a validated vendor peer-mapping path. The fragile/unsupported part of P2P is
precisely *cross-vendor* dma-buf import of one driver's VRAM into another
(NVIDIA→AMD). The developer's restriction sidesteps exactly that — every transfer
stays inside one vendor's well-trodden P2P path. Sound instinct. It is also the
*precondition* for the fast path: NVLink, xGMI, and Xe Link are each intra-vendor
fabrics that only link same-arch peers, so "same architecture only" is exactly the
set on which a vendor interconnect can exist at all — the constraint and the fast
path share a boundary.

### What makes it feasible — the Linux substrate already exists
- **PCIe peer-to-peer DMA** is real hardware: two devices under a permitting
  topology DMA directly to each other's BAR-mapped memory, no host bounce.
- **Resizable BAR (ReBAR)** exposes the *full* VRAM aperture (not the legacy
  256 MB window) — what makes bulk P2P actually useful.
- **`dma-buf`** — the kernel's cross-driver buffer-sharing framework: one driver
  *exports* a buffer as an FD, another *imports* it.
- **`drivers/pci/p2pdma`** — in-kernel framework that decides whether a pair
  *can* P2P (`pci_p2pdma_distance`) and manages the mapping; recent kernels
  (≈6.2+) combine it with dma-buf so an importer can map an exporter's VRAM.
- **Vulkan external memory** — `VkExportMemoryAllocateInfo` + `vkGetMemoryFdKHR`
  (`VK_KHR_external_memory_fd` + `VK_EXT_external_memory_dma_buf`) export a
  `VkDeviceMemory` as a dma-buf FD; `VkImportMemoryFdInfoKHR` imports it on the
  peer. `VK_KHR_external_semaphore_fd` carries completion across devices without
  the CPU.

### Honest blockers (these gate feasibility — probe before building)
- **IOMMU / ACS.** Many consumer boards force P2P up through the IOMMU (ACS on),
  which blocks or serializes it. Server boards / PCIe switches handle it cleanly.
  Consumer HW may need `pcie_acs_override` boot params — which carries real
  security implications.
- **ReBAR required** for full-VRAM P2P.
- **NVIDIA proprietary driver gates non-CUDA dma-buf import of VRAM** — NVIDIA
  pairs likely need the **CUDA P2P** path (`cuMemcpyPeerAsync` / IPC handles),
  not the generic Vulkan dma-buf path. AMD and Intel each have *both* options:
  their native runtime peer copy (HIP rides xGMI, Level Zero rides Xe Link — the
  preferred fast path) *and* a friendly generic Vulkan-dma-buf path
  (amdgpu/RADV, ANV) as a neutral fallback. So "vendor-neutral" = *one API
  surface with a small per-vendor backend underneath*, not one identical codepath.
- **Bandwidth tiers, not a single ceiling.** Each vendor's fabric is its own
  tier above the PCIe floor, and the matching native runtime rides it
  transparently (see L1):
  - **NVIDIA NVLink / NVSwitch** — hundreds of GB/s aggregate per GPU.
  - **AMD xGMI (Infinity Fabric Link)** — ~64 GB/s raw per link (~48 GB/s usable
    after CRC/protocol overhead); an MI300X 8-GPU fully-connected mesh sustains
    ~310–330 GB/s aggregate per GPU, MI250 up to ~100 GB/s per inter-GPU link
    direction.
  - **Intel Xe Link** — ~26.5 GB/s per port, all-to-all across 2/4/8-card Data
    Center GPU Max configurations.
  - **PCIe** is the floor (a pair on PCIe P2P, or any pair lacking a fabric):
    PCIe 4.0 x16 ≈ 32 GB/s, PCIe 5.0 ≈ 64 GB/s.

  All beat the host round-trip; the probe (L0) reports which tier a given pair
  gets.

### Why it's still worth it (the "might not be so bad" instinct is right)
A host-staged exchange costs **two** PCIe traversals (GPU→host, host→GPU) plus
host-RAM bandwidth and CPU sync. P2P costs **one** traversal, copy-engine-driven,
no host RAM, no CPU. So even at the same PCIe link speed P2P roughly *halves* data
movement and removes host contention → a realistic **2×+** on bandwidth-bound
collectives, more when host RAM is contended by other lanes. A solid win over
host-staging even on the PCIe floor — and on a pair with a vendor fabric it *is*
fabric-class, because it **is** that fabric: NVLink on NVIDIA, xGMI on AMD, Xe
Link on Intel.

### Shape of `gpu-databridge` (separate repo, Linux, pulled via AUTOMDEPS)
- **L0 — Probe.** Enumerate GPUs, read vendor/arch, PCIe gen/width, ReBAR size,
  IOMMU group / ACS, `p2pdma` distance, **and each vendor's fabric topology**:
  NVLink/NVSwitch via NVML `nvmlDeviceGetNvLink*` on NVIDIA, **xGMI via ROCm/AMD
  SMI `rsmi_topo_get_link_type` → `RSMI_IOLINK_TYPE_XGMI` vs `_PCIEXPRESS`
  (`amd-smi topology --link-type`)** on AMD, **Xe Link via Level Zero Sysman
  fabric ports `zesDeviceEnumFabricPorts` / `zesFabricPortGetProperties`** on
  Intel → a capability matrix per ordered pair:
  `{link ok?, link type: nvlink | xgmi | xelink | pcie-p2p | none, est. bandwidth, path: cuda | hip | level-zero | vulkan-dmabuf}`.
- **L1 — Export/import + transfer path (NVIDIA is the primary backend).** The
  same shape recurs per vendor: enable peer access, issue the native peer copy,
  and the driver transparently routes over the vendor fabric when the topology
  has it and over PCIe P2P when it doesn't — the bridge gets the fast
  interconnect "for free" with no separate code path.
  - **NVIDIA (primary):** CUDA peer path — `cudaDeviceEnablePeerAccess` +
    `cuMemcpyPeerAsync`, riding **NVLink/NVSwitch** when present, else PCIe P2P.
  - **AMD (secondary, follow-on):** HIP/ROCm peer path —
    `hipDeviceEnablePeerAccess` + `hipMemcpyPeerAsync`, riding **xGMI (Infinity
    Fabric Link)** when present, else PCIe P2P. (This is the same mechanism RCCL
    uses to reach xGMI.) The vendor-neutral Vulkan dma-buf path
    (`VK_KHR_external_memory_fd`) over PCIe P2P is the secondary fallback when the
    ROCm runtime isn't the chosen lane backend.
  - **Intel (secondary, follow-on):** Level Zero peer path — peer access via
    `zeDeviceCanAccessPeer` + `zeCommandListAppendMemoryCopy`, riding **Xe Link**
    when present, else PCIe P2P. (Same fabric oneCCL rides.) Vulkan dma-buf over
    PCIe P2P is again the neutral fallback.
  - Same-arch only, in every case — which is also the precondition for any of
    these fabrics to exist (see "same architecture" above).
- **L2 — Transfer.** Async peer copy on a transfer queue + a cross-device
  timeline semaphore signal so the consumer GPU waits without the CPU.
- **L3 — Fallback.** No P2P for a pair ⇒ host-staged copy (base plan's
  transport). The bridge always picks the best available; callers see one API.

> Likely **userspace** orchestration of existing kernel facilities (dma-buf +
> p2pdma + the vendor runtimes), not a kernel module — which is what keeps this
> from being a from-scratch driver. Whether a thin kernel shim is ever needed is
> a question the spike answers.

### Integration with OmegaGTE — one small seam
- The bridge consumes native handles OmegaGTE already exposes:
  `OmegaGraphicsEngine::underlyingNativeDevice()` (`GE.h:414`, the `VkDevice`)
  and `GTEResource::native()` (`GTEBase.h:585`; `GEBuffer : GTEResource`,
  `GE.h:227`).
- **The one OmegaGTE-side change:** P2P lane buffers must be allocated
  *exportable* (`VkExportMemoryAllocateInfo` at alloc time — exportability cannot
  be retrofitted onto an existing allocation). Add a Linux/Vulkan-only
  `exportable` flag to `BufferDescriptor`; the A.4 orchestrator sets it on lane
  buffers when the probe says the pair can P2P. The same exported FD feeds **every**
  transport: imported into the peer runtime via that vendor's external-memory
  interop — `cudaImportExternalMemory` (NVIDIA, rides NVLink), `hipImportExternalMemory`
  (AMD, rides xGMI), or Level Zero's external-memory import (Intel, rides Xe Link)
  for the native peer-copy fast paths, or directly into the peer `VkDevice`
  (`VkImportMemoryFdInfoKHR`) for the vendor-neutral dma-buf fallback. Everything
  else lives in `gpu-databridge`.
- The **A.4 collectives API is unchanged** — the bridge is just the transport
  chosen at runtime; host-ring remains the fallback.

### Feasibility gate — the cheap spike to run *before* any repo/driver work
A ~100-line program on the **actual target hardware**: allocate an exportable
buffer on GPU 0, fill it, export the dma-buf FD (or CUDA IPC handle), import on
GPU 1, copy peer→local on GPU 1's transfer queue, verify, measure bandwidth.
**Go** if the import succeeds and bandwidth beats host-staging; **no-go**
(host-ring only) if the import is rejected or bandwidth ≤ host-staging. This
answers feasibility for one pair cheaply, before committing to a driver or a repo.

## A.7 Out of scope for this track (deferred)

Recorded so a future reader knows these were weighed, not missed. A native
fast-path could later slot a CUDA "lane" behind the *same*
`GEParallelComputePipeline` API (the lane model was shaped to allow it), but
none of the below is part of this track. (Direct PCIe P2P moved *out* of this
list and into A.6 — it is in scope, gated on feasibility. Likewise the **vendor
GEMM libraries** moved *out* and into A.9 as the per-vendor fast paths; only the
general native *execution* backend below stays deferred.)

- **Native CUDA / NVPTX *execution* backend** (`TARGET_CUDA`, CUDA Driver API +
  NVRTC) — vendor-locked, subsystem-sized, duplicates the engine surface for one
  vendor. (Note: A.6 uses CUDA's *peer-copy* API as one transport backend, and
  A.9 uses vendor GEMM *libraries* for one op — both are calls into prebuilt
  vendor code against existing memory, not an OmegaSL→native kernel-codegen
  backend. The two are unrelated to this deferred item.)
- **NCCL / RCCL / oneCCL** — the vendors' collective *libraries* (NVIDIA / AMD /
  Intel). Stay out: `gpu-databridge` (A.6) implements its own collectives over
  whatever transport it has, so it does not need any of them. The *fabrics* they
  ride are **not** out of scope — A.6's per-vendor backends ride NVLink / xGMI /
  Xe Link transparently when present; only the collective libraries are declined.
- **MLIR codegen** — a compiler-back-half re-architecture (large LLVM/MLIR build
  dep). Its real leverage is enabling a native NVPTX/ROCDL backend and
  `linalg`→MMA GEMM lowering; not needed for any A.2–A.4 or the A.9 GEMM work,
  which all ship with hand-written codegen (A.9's GEMM is authored directly on the
  A.3 `coopmatrix` primitive, not lowered through MLIR). Revisit only if a native
  backend or first-class GEMM throughput is greenlit.

## A.8 Open Decisions (developer's call)

1. **Tensor cores near-term?** Is cooperative-matrix (A.3) a near-term goal, or
   deferred until a concrete GEMM/ML workload drives it? It is the biggest item
   in this track by far; A.1–A.2 and A.4 stand on their own without it.
2. **Collectives in v1?** Ship the portable collectives layer (A.4) alongside
   the base parallel pipeline, or land the base scatter/gather pipeline first
   and add collectives once a workload needs cross-lane exchange?
3. **P2P target hardware + vendor priority — RESOLVED.** Datacenter cards behind
   a PCIe switch on a server board (the clean, feasible case); topology
   feasibility is settled. **Vendor priority: NVIDIA primary, multi-vendor over
   time.** So `gpu-databridge` lands the **NVIDIA CUDA backend first** (CUDA peer
   copy, which transparently rides NVLink/NVSwitch when present — see A.6 L1),
   then adds the **AMD (HIP peer copy, rides xGMI)** and **Intel (Level Zero peer
   copy, rides Xe Link)** backends as follow-ons behind the same API, with the
   Vulkan-dma-buf PCIe-P2P path as each vendor's neutral fallback. The A.6 spike
   now just confirms bandwidth and the path on the real NVIDIA cards. The whole
   surface is compiled behind `OMEGA_BUILD_ML` (see "Build scoping").
4. **GEMM scope (A.9), for the later phase.** Several sub-calls, recorded now so
   Phase A5 starts with them settled:
   - **Vendor-library fast paths — RESOLVED (in scope, all three vendors).**
     cuBLASLt (NVIDIA), hipBLASLt / rocBLAS (AMD), oneMKL / oneDNN (Intel) ship as
     per-vendor fast paths in A.9, each build-gated, with the portable `coopmatrix`
     GEMM as the universal fallback. (Moved out of A.7 "deferred" per the
     developer's call.)
   - **Distributed strategy v1** — 1-D row-partition of `C` with a full **`B`
     broadcast** (simple; requires `B` to fit in each lane's VRAM) vs. 2-D
     **SUMMA/Cannon** (scales to `B` that doesn't fit, more complex). *Recommend*
     1-D first, SUMMA as a follow-on.
   - **Dtype coverage v1** — fp16/bf16 inputs with fp32 accumulate (the
     tensor-core sweet spot) only, or also fp32 and int8? *Recommend* fp16/bf16→fp32
     first; the rest follow `coopmatrix`'s supported types.
   - **API shape** — a dedicated `GEDistributedGemm` orchestrator (descriptor:
     `M`/`N`/`K`, dtype, layout, `α`/`β`) vs. a documented *recipe* composing the
     public surface (base pipeline + `Broadcast` + the GEMM kernel + gather).
     *Recommend* the thin orchestrator so callers don't re-derive the data movement.
   - **Batched GEMM** — partition the batch (embarrassingly-parallel, base-plan
     scatter/gather, no broadcast) — ship alongside, or defer? It is the *easy*
     multi-GPU case and a common ML shape (attention); *recommend* shipping it with
     the single-large-GEMM path.

---

## A.9 Capstone workload — distributed GEMM (later phase)

> **Status — committed direction, lands as Phase A5 (the latest phase).** GEMM
> (`C = α·(A·B) + β·C`) is the canonical massively-parallel workload and the
> headline reason A.3 (tensor cores) and A.4 (collectives) exist. It is **not** a
> new language or runtime primitive — it is a *kernel plus an orchestration*
> assembled entirely from pieces this track already defines, which is why it lands
> after the primitives it composes. Each lane's GEMM has **two implementations
> chosen at runtime**: the portable `coopmatrix` kernel (always present, the
> universal fallback) and a **vendor-library fast path** (cuBLASLt / hipBLASLt /
> oneMKL) selected when that vendor's library is built in and the lane's device
> matches — the same portable-baseline-plus-vendor-fast-path shape A.6 uses for
> transport.

### Two layers

1. **Single-device GEMM, per lane — portable kernel *or* vendor library.** The
   portable baseline is a tiled OmegaSL kernel on A.3 `coopmatrix`: each
   threadgroup owns a tile of `C` and streams K-dimension tiles of `A` and `B`
   through `coop_load` → `coop_mma` → `coop_store` (the A.3 sketch is literally
   this inner loop), staging tiles in `groupshared` (already shipped — see the
   A.0 table). Authored once in OmegaSL, it runs single-device through the ordinary
   `makeComputePipelineState` + dispatch path — so it ships in **every** build,
   exactly like any other `coopmatrix` kernel (see "Build scoping"). When a vendor
   GEMM library is built in and the lane's device matches its vendor, that lane's
   GEMM is instead offloaded to the **vendor fast path** below; the portable kernel
   stays the universal fallback. Either way, this is the unit the multi-GPU layer
   fans out.
2. **Distributed multi-device GEMM (on the lane model + A.4 collectives).** The
   part that belongs to *this* plan's multi-GPU mission — and the honest wrinkle:
   GEMM is **not** embarrassingly-parallel the way the base scatter/gather assumes.
   Every output block of `C` needs a full **row-panel of `A` and column-panel of
   `B`**, so lanes must *share* input, not merely split it.

### Partitioning — why GEMM needs more than scatter/gather
- **1-D row-partition (recommended v1).** Split `C` into row-blocks across lanes.
  Lane *i* computes `C_i = A_i · B`: its **row-panel `A_i` is scattered** (base-plan
  `Upload` path), but **the whole of `B` is broadcast to every lane** (A.4
  `Broadcast`). Each lane then runs the single-device tiled kernel on its panel;
  `C` row-blocks are **gathered** (base-plan `Readback` path). Maps onto existing
  machinery with one new ingredient — the `B` broadcast. Constraint: `B` must fit
  in each lane's VRAM.
- **2-D SUMMA / Cannon (scale-out follow-on).** When `B` (or `A`) is too large to
  replicate, lay lanes out as a 2-D process grid and broadcast/ring `A` row-panels
  and `B` col-panels stepwise, accumulating locally. Built on A.4 `AllGather` /
  `Broadcast` along grid rows/cols, accelerated by the A.6 `gpu-databridge`
  transport when present. Heavier; deferred to a follow-on.
- **Batched GEMM (the *easy* multi-GPU case).** Many small independent GEMMs
  (attention, grouped conv) → **partition the batch**: each lane gets a disjoint
  set of whole GEMMs, no broadcast or exchange needed. This is pure base-plan
  scatter/gather — the embarrassingly-parallel path — and the cheapest distributed
  GEMM to ship.

### Vendor-library fast paths (NVIDIA / AMD / Intel)
Each lane's single-device GEMM can be offloaded to the vendor's tuned tensor-core
GEMM library — the same portable-baseline-plus-vendor-fast-path pattern A.6 uses
for transport, applied to *execution* of one op (GEMM only). This is **not** the
general OmegaSL→native execution backend A.7 still defers: it is a single library
call against the lane's existing memory, not a kernel-codegen backend.

- **NVIDIA — cuBLASLt** (`cublasLtMatmul`): the modern tensor-core, mixed-precision
  GEMM path (NVIDIA steers Ampere+ users off the older `cublasGemmEx` to it;
  unlocks fused epilogues + split-K). **CUTLASS** is the optional open-template
  alternative for shapes where its tunable tiling beats cuBLAS (large `N`).
- **AMD — hipBLASLt** (`hipblasLtMatmul`): the direct cuBLASLt analog over CDNA
  matrix cores, with epilogue fusion; plus **rocBLAS** (`rocblas_gemm_ex`) for the
  broader BLAS surface. (ROCm's TunableOp already searches both.) Composable
  Kernel is the CUTLASS-analog escape hatch.
- **Intel — oneMKL** GPU GEMM (`oneapi::mkl::blas::gemm`, drives the Xe Matrix
  Extensions / XMX by default under DPC++); plus **oneDNN** (`dnnl::matmul`) for
  the DL-matmul shapes.

**Memory seam — reuse A.6's exportable buffers.** A vendor library operates on its
own runtime's device pointers, not on a `VkBuffer`. So a fast-path lane allocates
its `inputBuf`/`outputBuf` **exportable** (the `OMEGA_BUILD_ML` `BufferDescriptor`
flag added for A.6) and imports the FD into the vendor runtime —
`cudaImportExternalMemory` + `cudaExternalMemoryGetMappedBuffer` (NVIDIA),
`hipImportExternalMemory` + `hipExternalMemoryGetMappedBuffer` (AMD), or Level
Zero's external-memory import (Intel). No new OmegaGTE seam is needed; the GEMM
fast path rides the one A.6 already adds.

**Selection + sync + fallback (mirrors A.6 L0/L2/L3).**
- *Select:* per lane, pick the vendor path iff its library is built in (per-vendor
  build flag — see "Build scoping") **and** the lane's device vendor matches; else
  the portable `coopmatrix` kernel.
- *Sync:* the library runs the GEMM on its own stream/queue against the imported
  memory; a cross-API timeline semaphore (`VK_KHR_external_semaphore_fd` ↔ the
  vendor runtime's external semaphore) joins it back to the lane's OmegaGTE
  submission with no CPU round-trip — the same primitive A.6 L2 uses.
- *Fallback:* no matching library ⇒ the portable kernel runs, so every build has a
  working GEMM regardless of which (if any) vendor libraries are present.

This composes with the distributed layer unchanged: partition / scatter `A` /
broadcast `B` / gather `C` is identical whether a lane computed its block with the
portable kernel or a vendor library.

### Why GEMM is the capstone, not a side feature
GEMM exercises all three additions at once: A.3 supplies the tensor-core inner
loop, A.4 supplies the `B`-broadcast / panel exchange, and A.6 (when present)
supplies the fast transport for that exchange. It is the concrete workload the
A.8 decisions ("tensor cores near-term?", "collectives in v1?") are really asking
about — landing GEMM is what makes those investments pay off.

### API shape (open — see A.8)
Leaning toward a thin `GEDistributedGemm` orchestrator (descriptor: `M`, `N`, `K`,
element/accumulator dtype, row/col-major layout, `α`/`β`) that internally drives
partition → scatter `A` → broadcast `B` → per-lane dispatch → gather `C`, so a
caller never re-derives the data movement. The orchestrator also picks each lane's
GEMM **implementation** (portable kernel vs. vendor library) internally, from the
build config + the lane's device vendor; callers see one descriptor either way,
with an optional flag to force the portable path (determinism / debugging /
cross-checking the vendor result). The alternative — a documented *recipe*
composing the public surface directly (the base pipeline + `Broadcast` + the GEMM
kernel) — stays available, matching how the base plan composes existing APIs.
Mixed precision follows `coopmatrix`: fp16/bf16 inputs with an fp32 accumulator
(the tensor-core sweet spot) is the v1 target; fp32 / int8 are open.

### Verification status (per repo convention)
- **Single-device kernel:** compile/run-verifiable on the Linux **Vulkan** host
  once A.3's Vulkan `coopmatrix` codegen lands. Correctness vs. a CPU reference
  GEMM, with a numerical tolerance appropriate to fp16-input / fp32-accumulate.
- **D3D12 / Metal:** **compile/run-unverified off-platform** — inherits A.3's
  coopmatrix codegen status.
- **Vendor-library fast paths:** **compile/run-unverified off-platform** — each
  needs its vendor toolkit + matching hardware (CUDA/cuBLASLt, ROCm/hipBLASLt,
  oneAPI/oneMKL), none present on the Vulkan CI host. When exercised, validate the
  fast-path result against the portable kernel (the force-portable flag above) to
  the same fp16/fp32 tolerance — the portable path is the oracle.
- **Distributed path:** the 1-D split + gather is testable on **1 lane** (whole
  problem in lane 0) and via the base plan's **partition-simulation** unit (forced
  `laneCount` 2–4, asserting the `C` row-blocks tile `[0,M)` and reassemble
  byte-exactly). True multi-GPU GEMM is **run-unverified** until ≥2 discrete GPUs
  are available — the same caveat as the base plan's Phase 5.

### Non-goals (this phase)
- **Hand-writing peak GEMM kernels to rival the vendor libraries.** Peak
  throughput on a given vendor is delivered by *calling that vendor's library*
  (the fast paths above) — not by out-tuning cuBLAS/hipBLASLt/oneMKL in OmegaSL.
  The portable `coopmatrix` kernel aims for *correct + tensor-core-accelerated*,
  not peak; the vendor fast path provides peak where its library is present.
- **Convolution / attention fusion, autotuning, epilogue fusion.** Out of scope;
  the tiled GEMM is the foundation those would later build on.
- Cross-shard dependencies beyond the GEMM panel exchange described above (the
  base plan's halo/stencil non-goal still holds for everything else).
