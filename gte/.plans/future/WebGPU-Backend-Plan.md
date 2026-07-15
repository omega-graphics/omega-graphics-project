# WebGPU Backend Plan

> **Lifecycle:** `future/` — proposed and exploratory, not yet greenlit. This
> plan exists to capture the design before the work is scheduled; move it up to
> the top level of `.plans/` if and when implementation begins.

## Goal

Give OmegaGTE a fourth graphics backend that targets **WebGPU**, so an
application built on OmegaGTE — and by extension OmegaWTK and kREATE — can run
in a **web browser** (compiled to WebAssembly) and, on the desktop, against a
native WebGPU implementation (Dawn or wgpu-native). The backend must look
identical to the three hardware backends from the caller's point of view:
`enumerateDevices()` returns a `GTEDevice`, `OmegaGraphicsEngine::Create()`
accepts it, and the rest of the engine — `GTEShaderLibrary`, `GECommandQueue`,
`GERenderPipelineState`, `GEComputePipelineState`, blits, fences, render
targets, textures, sampler state — works on top of it with no backend-specific
branching at the call site.

This document follows the multi-phase rule from `AGENTS.md`: research, proposal,
refinement, plan, then incremental implementation.

---

## 0. Why This Is Not a Contradiction of `docs/About.rst`

`About.rst` argues, under *Why Not an Existing Abstraction Layer?*, that WebGPU
is the wrong **primary abstraction** for a native application: its resource
barriers, bind-group layouts, and pipeline-compilation model reflect the
browser's threat model, and for a native app that never runs in a browser those
constraints add overhead without benefit. That argument still holds and this
plan does not dispute it.

Adding WebGPU as a **backend** is a different proposition. The three native
backends (D3D12, Metal, Vulkan) cannot reach the one target that matters here:
**the browser.** A WebGPU backend is justified by exactly that — reaching a
platform the native abstraction physically cannot — not by replacing the native
path on Windows/macOS/Linux, where D3D12/Metal/Vulkan remain the primary
backends and are strictly preferable. The mental model is the same one the
SwiftShader CPU-fallback plan uses: OmegaGTE stays the abstraction; WebGPU is
one more thing it can drive, selected where it is the *only* option, not where
it is merely *an* option.

Concretely:

- **Browser / WebAssembly** — WebGPU is the only GPU API available. This is the
  reason the backend exists.
- **Desktop native** — WebGPU (via Dawn / wgpu-native) is available but is
  **not** the default; it is a portability/testing convenience and a way to
  exercise the same code path the browser build takes. The native backend for
  that platform still wins by default.

---

## 1. Research — WebGPU as a Native API

WebGPU is a modern, explicit GPU API in the same family as D3D12/Metal/Vulkan,
but it deliberately sits one notch higher: it was designed to be *safe* and
*portable* rather than *maximal*. That higher altitude is, conveniently, very
close to the altitude OmegaGTE already targets.

| Concern | D3D12 / Vulkan | WebGPU | Consequence for OmegaGTE |
|---|---|---|---|
| Resource barriers | Manual (or backend-tracked in OmegaGTE today) | **Automatic** — the implementation inserts them | OmegaGTE's "no manual barrier tracking" contract is *native* here; the barrier-tracking layer is a no-op passthrough |
| Synchronisation | Semaphores / timeline values / fences | Queue submit + `onSubmittedWorkDone` / buffer `mapAsync` | Maps onto OmegaGTE's fence contract (`submitCommandBuffer`/`notifyCommandBuffer`/`commitToGPUAndWait`) with the async caveat in §4 |
| Shader input | DXIL / SPIR-V / MSL | **WGSL** (browser) or **SPIR-V** (native Dawn/wgpu) | See §2 — reuse the existing SPIR-V path natively; WGSL only needed for the browser |
| Pipeline state | PSO / VkPipeline | `WGPURenderPipeline` / `WGPUComputePipeline` | Direct 1:1 with `GERenderPipelineState` / `GEComputePipelineState` |
| Binding model | Descriptor heaps / sets | `WGPUBindGroup` + `WGPUBindGroupLayout` | Closest to Vulkan descriptor sets; the existing Vulkan binding logic is the best template |
| Device acquisition | Synchronous enumerate | **Asynchronous** `RequestAdapter` / `RequestDevice` | Friction with the synchronous `enumerateDevices()` — see §4 |

**WebGPU implementations to target:**

| Implementation | Language | Where | Shader ingestion |
|---|---|---|---|
| Browser (native) | via Emscripten `<webgpu/webgpu.h>` | Chrome/Edge/Firefox/Safari | **WGSL only** |
| **Dawn** (Google) | C/C++, `webgpu.h` | Desktop native, also the browser impl in Chrome | WGSL **and** SPIR-V |
| **wgpu-native** (gfx-rs) | Rust core, C `webgpu.h` | Desktop native | WGSL **and** SPIR-V |

The important asymmetry: **native WebGPU implementations accept SPIR-V; the
browser does not.** Web content is restricted to WGSL for security reasons.
OmegaGTE already produces SPIR-V today (the Vulkan path runs GLSL through
`shaderc`), so the native WebGPU path can reuse that output directly, and only
the browser build strictly needs a WGSL emitter.

**What WebGPU does *not* give us** (report these bits off in
`GTEDeviceFeatures`, exactly as the SwiftShader plan does):

- No ray tracing → `GTEDEVICE_FEATURE_RAYTRACING` off.
- No mesh shaders → `GTEDEVICE_FEATURE_MESH_SHADER` off.
- No tessellation / geometry stages.
- No variable-rate shading, no 64-bit types; 16-bit types only behind the
  `shader-f16` optional feature.
- Tight, portable limits (binding counts, workgroup sizes, buffer sizes) — query
  `WGPULimits` and fold into the device's reported limits.

None of these are blockers: `GTEDeviceFeatures` exists precisely so callers gate
on capability bits. kREATE's ray-traced lighting simply won't light up on a
WebGPU device; the WTK widget path, which needs none of those features, runs
unchanged.

---

## 2. The Shader Story — WGSL vs. Reusing SPIR-V

OmegaSL's code generator already has a clean backend seam. From
`gte/omegasl/src/Target.h`:

```cpp
struct Target {
    enum Kind { HLSL, MSL, GLSL };
    Kind kind() const { return _kind; }
    ...
};
```

with one emitter per kind (`HLSLTarget.cpp`, `MSLTarget.cpp`, `GLSLTarget.cpp`)
and the compiler driver picking a target from the same `TARGET_*` compile
definitions the runtime uses. There are two ways to feed WebGPU, and the plan
uses **both, in priority order**:

**2.1 Native desktop: reuse the existing SPIR-V (no new emitter).**
On Dawn / wgpu-native, `wgpuDeviceCreateShaderModule` accepts a SPIR-V chained
descriptor (`WGPUShaderModuleSPIRVDescriptor`). OmegaSL's GLSL→`shaderc`→SPIR-V
path already produces exactly this. So the desktop WebGPU backend consumes the
**same `.omegasllib` SPIR-V the Vulkan backend loads** — zero new shader
codegen. This mirrors the SwiftShader insight ("reuse the Vulkan output, don't
duplicate it") and lets Phase 1–4 land a working desktop backend before any WGSL
work exists.

**2.2 Browser: a fourth `Target::Kind::WGSL` emitter.**
The browser accepts only WGSL, so the WebAssembly build needs a real WGSL
back-end. This is a new `WGSLTarget.cpp` implementing the same `Target`
interface. WGSL is structurally its own language and the emitter is genuine
work, not a tweak:

- Types: `vec4<f32>`, `mat4x4<f32>`, `array<T, N>`, `texture_2d<f32>`,
  `sampler`.
- Bindings: `@group(N) @binding(M) var<...>` — closest to GLSL's explicit
  `layout` decorations, so `GLSLTarget` is the reference emitter to fork from.
- Storage buffers: `var<storage, read>` / `var<storage, read_write>` — maps onto
  OmegaSL's read-only vs. read-write structured-buffer distinction.
- Entry points: `@vertex` / `@fragment` / `@compute @workgroup_size(x,y,z)`,
  I/O via `@location(N)` / `@builtin(...)`.
- No preprocessor, no pointers-to-storage in the same shape as MSL; the
  feature-gap survey (`gte/.plans/OmegaSL-Feature-Gap-Survey.md`) should gain a
  WGSL column so gaps are tracked section-by-section like the other three.

**Recommendation:** do §2.1 first (unblocks the whole desktop backend with no
codegen risk), schedule §2.2 as its own sub-project gated on an actual browser
target being greenlit. Note honestly in the plan that until §2.2 lands, "WebGPU
backend" means *desktop Dawn/wgpu only*, not the browser — the browser is the
motivation but the SPIR-V path is what ships first.

`Target::Kind` is currently numbered `HLSL=0 / MSL=1 / GLSL=2` (see the note at
`gte/omegasl/src/CodeGen.h:536`); appending `WGSL=3` keeps existing ordinal
assumptions intact.

---

## 3. Host-Side Design — A Fourth Backend Directory

The three backends each own a directory under `gte/src/` (`d3d12/`, `metal/`,
`vulkan/`) and are selected by a `TARGET_*` compile definition set in
`gte/CMakeLists.txt`. `OmegaGraphicsEngine::Create` dispatches on it
(`gte/src/GE.cpp:232`):

```cpp
SharedHandle<OmegaGraphicsEngine> OmegaGraphicsEngine::Create(SharedHandle<GTEDevice> & device){
    #ifdef TARGET_METAL
        return CreateMetalEngine(device);
    #endif
    #ifdef TARGET_DIRECTX
        return GED3D12Engine::Create(device);
    #endif
    #ifdef TARGET_VULKAN
        return GEVulkanEngine::Create(device);
    #endif
};
```

WebGPU follows the identical pattern:

- **New directory `gte/src/webgpu/`**, mirroring `gte/src/vulkan/` file-for-file:
  `GEWebGPU.{h,cpp}` (engine + device), `GEWebGPUCommandQueue.{h,cpp}`,
  `GEWebGPUPipeline.{h,cpp}`, `GEWebGPURenderTarget.{h,cpp}`,
  `GEWebGPUTexture.{h,cpp}`, `GEWebGPUMeshAsset.cpp`, `GEWebGPUTextureAsset.cpp`,
  `WebGPUTEContext.cpp`.
- **New `TARGET_WEBGPU` define** added to `gte/CMakeLists.txt` beside
  `TARGET_VULKAN`, pushed into `PUBLIC_DEFS` so `GE.h`'s target guard
  (`#if !defined(TARGET_METAL) & ... #error`) accepts it. The Emscripten
  toolchain file selects it for the browser build; a desktop CMake option
  (`OMEGAGTE_WEBGPU=ON`) selects it against Dawn/wgpu.
- **New `Create` branch** in `GE.cpp`: `#ifdef TARGET_WEBGPU return GEWebGPUEngine::Create(device); #endif` (static-factory form, matching D3D12/Vulkan rather than the Metal free function).
- **`enumerateDevices()`** (declared `gte/include/omegaGTE/GTEDevice.h:178`) gains
  a WebGPU implementation that requests an adapter and wraps it in a
  `GTEWebGPUDevice : GTEDevice`, filling `type`, `name`, and a
  `GTEDeviceFeatures` derived from the adapter's `WGPUFeatureName` set and
  `WGPULimits`.

**Type mapping.** `GTEDevice::Type` is currently only
`{ Integrated, Discrete }` (`GTEDevice.h`). WebGPU's `WGPUAdapterType`
distinguishes `DiscreteGPU` / `IntegratedGPU` / `CPU` / `Unknown`. Map
Discrete→`Discrete`, Integrated/Unknown→`Integrated`; a `CPU` adapter maps to
`Integrated` for now. (If the SwiftShader plan's proposed `Software` type is
ever added to the enum, revisit — but do not block this plan on it, and per the
"GPU tier ≠ integrated bit" guidance do not use the type to downgrade quality.)

**Object mapping** (WebGPU handle → OmegaGTE type):

| OmegaGTE | WebGPU handle |
|---|---|
| `GTEWebGPUDevice` | `WGPUAdapter` + `WGPUDevice` |
| `GECommandQueue` | `WGPUQueue` (+ per-submit `WGPUCommandEncoder`) |
| render pass encode | `WGPURenderPassEncoder` |
| compute pass encode | `WGPUComputePassEncoder` |
| blit / copy | `wgpuCommandEncoderCopy*` |
| `GEBuffer` | `WGPUBuffer` |
| `GETexture` | `WGPUTexture` / `WGPUTextureView` |
| `GERenderPipelineState` | `WGPURenderPipeline` |
| `GEComputePipelineState` | `WGPUComputePipeline` |
| sampler state | `WGPUSampler` |
| bindings | `WGPUBindGroup` + `WGPUBindGroupLayout` |
| shader module | `WGPUShaderModule` (SPIR-V native / WGSL browser) |
| render target / swapchain | `WGPUSurface` + `WGPUSurfaceTexture` |

The C header (`webgpu.h`) is identical across Dawn, wgpu-native, and Emscripten,
so the backend body is one implementation; only the instance-creation and
surface-creation entry points differ per platform and are `#ifdef`'d.

---

## 4. Known Design Risks (surface before coding)

**4.1 Asynchronous device acquisition vs. synchronous `enumerateDevices()`.**
`wgpuInstanceRequestAdapter` and `wgpuAdapterRequestDevice` are callback-based.
On desktop Dawn/wgpu they can be driven to completion synchronously (poll /
block on the callback), so `enumerateDevices()` keeps its current synchronous
signature there. In the browser you **cannot block the main thread**; options
are (a) Emscripten Asyncify (simple, some overhead), (b) the newer
future/`WGPUFuture` synchronous-wait entry points where the browser exposes
them, or (c) a longer-term async `enumerateDevicesAsync()` surface. **Decision
needed from the developer** before Phase 2 — this is the single biggest shape
question and it touches public API. Do not guess; the browser threading model is
exactly the kind of production constraint that should be pinned by someone who
has shipped against it.

**4.2 Present / frame loop.** Browser presentation is driven by
`requestAnimationFrame`, not a swap-then-present call the app owns. The
`AppWindow` frame loop in WTK assumes it drives its own timing. Reconciling
OmegaGTE's `commit`/present with a browser-owned frame callback is a WTK-level
concern and must be flagged to that module — it is **outside the GTE backend's
boundary** and should not be solved silently inside `gte/src/webgpu/`.

**4.3 SwiftShader overlap.** Dawn ships with SwiftShader as its own CPU
fallback. If the SwiftShader CPU-fallback plan also lands, be explicit about
which software path a headless run uses (Vulkan+SwiftShader vs.
WebGPU/Dawn+SwiftShader) so the two plans don't quietly fight over the same
"software device" slot in `enumerateDevices()`.

**4.4 Dependency wiring.** Dawn and wgpu-native are large. They come in through
AUTOMDEPS (`autom/tools/autom-deps`) like every other dependency — pick **one**
default native implementation (Dawn is the more conventional C++ choice and is
what Chrome ships) and treat the other as optional. The Emscripten build needs
no dependency; the browser provides WebGPU.

---

## 5. Phased Implementation

Each phase is independently reviewable and leaves the tree building. Native
desktop (Dawn/wgpu, SPIR-V shaders) is the whole of Phases 1–6; the browser and
WGSL are Phase 7+.

- **Phase 1 — Build plumbing (no engine yet).** Add `TARGET_WEBGPU` +
  `OMEGAGTE_WEBGPU` option to `gte/CMakeLists.txt`, wire Dawn through AUTOMDEPS,
  create an empty `gte/src/webgpu/` that compiles a stub `GEWebGPUEngine::Create`
  returning `nullptr`, and make `GE.h`'s target guard accept the new define.
  *Verify:* configures + links on a desktop host with `OMEGAGTE_WEBGPU=ON`.

- **Phase 2 — Device + enumeration.** `GTEWebGPUDevice`, `enumerateDevices()`
  WebGPU path, adapter→`GTEDeviceFeatures`/limits mapping, and the §4.1 async
  decision implemented for desktop. *Verify:* `enumerateDevices()` reports a
  WebGPU device with a plausible feature set.

- **Phase 3 — Queues, buffers, textures.** `GEWebGPUCommandQueue`
  (`WGPUQueue` + encoder-per-submit), `GEBuffer`/`GETexture` on `WGPUBuffer`/
  `WGPUTexture`, sampler state, and the fence contract mapped onto
  `onSubmittedWorkDone`. Barrier-tracking layer becomes a passthrough (§1).

- **Phase 4 — Pipelines + render targets (SPIR-V shaders).** `WGPURenderPipeline`
  / `WGPUComputePipeline` from the existing `.omegasllib` SPIR-V, bind-group
  layouts from the resource layout, `WGPUSurface` render target. *Verify:*
  render + compute the existing GTE test corpus (`gte/tests/`) against a WebGPU
  device on desktop; visually confirm the 2D and compute tests match the Vulkan
  output (Visual Debugging is part of "done").

- **Phase 5 — Blits + triangulation.** `wgpuCommandEncoderCopy*` blits and the
  `WebGPUTEContext` (GPU triangulation compute path).

- **Phase 6 — Feature-gate audit.** Confirm ray-tracing / mesh-shader /
  tessellation shaders are cleanly skipped at load with a precise diagnostic on a
  WebGPU device, per the `.omegasllib` masking model.

- **Phase 7 — WGSL emitter (`Target::Kind::WGSL`).** New `WGSLTarget.cpp`,
  feature-gap-survey WGSL column, omegaslc conformance corpus extended. Gated on
  a browser target being greenlit (§2.2).

- **Phase 8 — Browser / WASM.** Emscripten toolchain build, async enumeration
  path, and hand-off to WTK for the `requestAnimationFrame` frame-loop
  reconciliation (§4.2). This is where the backend finally delivers its reason
  for existing.

---

## 6. Boundary Note

This plan is scoped to the **GTE backend only**. Two things it touches are owned
elsewhere and must be handed off, not solved inside `gte/src/webgpu/`:

- The browser frame loop / present timing (§4.2) — **OmegaWTK**.
- Any public-API change to `enumerateDevices()` for async acquisition (§4.1) —
  a cross-backend signature decision for the developer to rule on before Phase 2.

Everything else — the fourth backend directory, the `TARGET_WEBGPU` wiring, the
SPIR-V reuse, the WGSL emitter — is self-contained within OmegaGTE and OmegaSL.
