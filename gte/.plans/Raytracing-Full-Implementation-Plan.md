# Full Raytracing Implementation Plan

## Goal

Bring raytracing from its current partial state to a fully functional, cross-platform feature across all three backends (D3D12, Vulkan, Metal) and the OmegaSL shader language. Additionally, remove the compile-time `OMEGAGTE_RAYTRACING_SUPPORTED` macro and replace it with runtime device feature checks via `GTEDeviceFeatures::hasFeature(GTEDEVICE_FEATURE_RAYTRACING)`. **(Phase 0 — done, 2026-05-23.)**

## Current State

Raytracing exists in a half-built state across the codebase. What works and what doesn't:

### What's Built

| Component | D3D12 | Vulkan | Metal |
|---|---|---|---|
| Accel struct creation | Yes | Yes | Yes |
| Accel struct building (cmd) | Yes | Yes | Yes |
| Accel struct copy (cmd) | Yes | Partial | Stub |
| Accel struct refit (cmd) | No | Yes | Yes |
| `dispatchRays()` | Stub (empty SBT) | Stub (empty SBT) | Stub (redirects to compute) |
| Ray tracing pipeline | **No** | **No** | **No** |
| Shader binding table | **No** | **No** | **No** |
| RT shader types in OmegaSL | **No** | **No** | **No** |

### What's Missing Everywhere

1. **Ray tracing pipeline state objects** — No `ID3D12StateObject`, no `VkPipeline` with `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR`, no Metal intersection function tables.
2. **Shader binding tables (SBT)** — `dispatchRays()` on D3D12 and Vulkan creates empty `D3D12_DISPATCH_RAYS_DESC` / `VkStridedDeviceAddressRegionKHR` with all shader table pointers zeroed.
3. **OmegaSL ray tracing shader types** — The shader language only knows `vertex`, `fragment`, `compute`, `hull`, `domain`. No ray generation, closest hit, any hit, miss, or intersection shaders.
4. **Public API for RT pipelines** — `GEPipeline.h` has `RenderPipelineDescriptor` and `ComputePipelineDescriptor` but no `RayTracingPipelineDescriptor`.

### The Macro Problem — RESOLVED (Phase 0, 2026-05-23)

> Resolved: the macro is gone and replaced by runtime
> `features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)` checks. The description
> below is the original problem statement, kept for context.

`OMEGAGTE_RAYTRACING_SUPPORTED` was a compile-time gate defined in `GE.h` based on SDK version/platform:
- **D3D12**: `NTDDI_VERSION >= NTDDI_WIN10_RS5` (Windows 10 1809+)
- **Metal**: `__MAC_11_0` / `__IPHONE_14_0`
- **Vulkan**: Always defined

This gates the **public API** (`GEAccelerationStruct`, `GEAccelerationStructDescriptor`, `dispatchRays`, accel struct commands), meaning downstream code must also `#ifdef` around raytracing usage. This is fragile — a binary compiled on Windows 10 RS5+ includes the symbols, but the GPU may not support DXR at all. The correct approach is always-present API with runtime `features.raytracing` checks.

The macro appears in **17 files** across public headers, all three backend headers and implementations, and downstream docs.

## Approach: inline-first (decided 2026-05-23)

OmegaSL models ray tracing on the **inline ray-query** model — the common
denominator across DXR 1.1 `RayQuery`, Vulkan `GL_EXT_ray_query`, and Metal's
`intersector`. Ray tracing is a **capability of compute shaders**, not a new
family of shader stages: a compute shader instantiates a ray query, runs
traversal, and shades the hit inline. There is no ray-tracing pipeline and no
shader binding table in this model.

The original plan was written around the DXR/Vulkan **pipeline + SBT** model
(separate raygen/closesthit/miss/… stages, a ray-tracing PSO, an SBT the GPU
walks during traversal). That model maps 1:1 to DXR and Vulkan but has no Metal
equivalent (it's faked with visible/intersection function tables), so it does
not transpile universally. It is preserved here as the **Advanced Track**
(former Phases 3–5) for a future path-tracing use case.

## Non-Goals

- **Ray-tracing pipeline objects + shader binding tables.** Deferred to the
  Advanced Track. The inline model covers shadows, AO, reflections, and
  single-bounce GI without them.
- **Hardware ray recursion, callable shaders, and GPU-driven per-geometry
  shader dispatch.** These exist only in the pipeline/SBT model (Advanced Track).
  With inline, you branch on instance/primitive index and select materials in
  shader code.
- GPU work graphs or shader execution reordering (SER).
- Changes to the triangulation engine (TE).

---

## Phase 0: Remove `OMEGAGTE_RAYTRACING_SUPPORTED` Compile-Time Gate

> **Status: COMPLETE (2026-05-23).** Reality differed from this plan's original
> assumptions — recorded here so the rest of the plan can be read accurately:
> - **0.1 (remove `#ifdef` gates):** was already done in commit `ef3b7bd
>   "Remove Raytracing Macro"`. The RT API is unconditionally present.
> - **0.2 (delete macro `#define`s):** the defines were left commented-out in
>   `GE.h`; those dead comment blocks are now deleted.
> - **"Thread `GTEDeviceFeatures` into the engine" (0.3):** unnecessary. All
>   three engines already store `SharedHandle<GTEDevice> gteDevice`, and
>   `GTEDevice::features` is a public `const` member — features are already
>   reachable from every engine method.
> - **There is no `bool raytracing` field.** `GTEDeviceFeatures` exposes a
>   `uint64_t flags` bitmask; the runtime guard is
>   `gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)`.
> - **Runtime guards added (factory-only scope):** `createBoundingBoxesBuffer`
>   and `allocateAccelerationStructure` in all three backends return `nullptr` +
>   `DEBUG_STREAM` when RT is unsupported. Command-buffer methods are not
>   guarded — they cannot receive a valid accel-struct handle without the
>   factory succeeding first.
> - **Docs updated:** `gte/docs/API.rst`, `wtk/docs/OmegaGTEView-Proposal.md`.
>
> The subsections below are the *original* plan, preserved for reference.

### 0.1 Make all raytracing types and methods unconditionally present

Remove every `#ifdef OMEGAGTE_RAYTRACING_SUPPORTED` / `#endif` pair from:

**Public headers:**
- `gte/include/omegaGTE/GE.h` — `GERaytracingBoundingBox`, `GEAccelerationStructDescriptor`, `GEAccelerationStruct`, engine methods `createBoundingBoxesBuffer()`, `allocateAccelerationStructure()`
- `gte/include/omegaGTE/GECommandQueue.h` — `beginAccelStructPass()`, `buildAccelerationStructure()`, `copyAccelerationStructure()`, `refitAccelerationStructure()`, `finishAccelStructPass()`, `bindResourceAtComputeShader(GEAccelerationStruct)`, `dispatchRays()`

**D3D12 backend:**
- `gte/src/d3d12/GED3D12.h` — `GED3D12AccelerationStruct`, engine method declarations
- `gte/src/d3d12/GED3D12.cpp` — method implementations
- `gte/src/d3d12/GED3D12CommandQueue.h` — command buffer declarations
- `gte/src/d3d12/GED3D12CommandQueue.cpp` — command buffer implementations

**Vulkan backend:**
- `gte/src/vulkan/GEVulkan.h`, `GEVulkan.cpp`
- `gte/src/vulkan/GEVulkanCommandQueue.h`, `GEVulkanCommandQueue.cpp`

**Metal backend:**
- `gte/src/metal/GEMetal.h`, `GEMetal.mm`
- `gte/src/metal/GEMetalCommandQueue.h`, `GEMetalCommandQueue.mm`

### 0.2 Remove the macro definitions

Delete the `#define OMEGAGTE_RAYTRACING_SUPPORTED` lines from `GE.h` (lines 18-20, 35-41, 62).

### 0.3 Add runtime guards in method bodies

Each backend method that touches hardware RT APIs must check at runtime:

```cpp
// D3D12 example:
SharedHandle<GEAccelerationStruct> GED3D12Engine::allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) {
    if (!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)) {
        DEBUG_STREAM("Raytracing not supported on this device");
        return nullptr;
    }
    // ... existing implementation ...
}
```

`GTEDeviceFeatures` exposes a `uint64_t flags` bitmask (queried with `hasFeature(GTEDEVICE_FEATURE_RAYTRACING)`), populated on all three backends during `enumerateDevices()`. **Correction:** there is no `bool raytracing` field. The engine already has access to its device's features — all three engines store `SharedHandle<GTEDevice> gteDevice` and `GTEDevice::features` is a public `const` member, so no additional threading is required.

### 0.4 Update downstream references

- `gte/docs/API.rst` — Remove `#ifdef` examples, document runtime feature check pattern
- `wtk/docs/OmegaGTEView-Proposal.md` — Replace macro references with `features.raytracing`

**Files**: All 17 files listed above, plus docs.

---

## Phase 1: OmegaSL Inline Ray Tracing (Ray Query)

> **Status: IMPLEMENTED + VERIFIED on all three backends (2026-07-13).**
> What landed vs. what this section originally scoped:
> - **Types:** `Ray`, `RayHit`, `AccelerationStructure` are added as builtin
>   types (`Toks.def`, `AST.h/.def/.cpp`, `Lexer.cpp` `isKeywordType`, Sema
>   `builtinsTypeMap`). `Ray`/`RayHit` are the first builtin *struct* types —
>   declared `builtin = false` with a populated `fields` map so `ray.origin` /
>   `hit.t` take Sema's MEMBER_EXPR struct-field path (the `builtin` flag hard-
>   forks member access to vector-swizzle otherwise) and so each backend emits a
>   real `struct` (pre-seeded into `generatedStructs` in `emitDefaultHeaders`,
>   RT-bit-gated). `RayHit` fields: `committed`, `t`, `primitiveIndex`,
>   `instanceIndex`, `barycentrics`.
> - **Intrinsic:** `intersect(as, ray)` and `intersect(as, ray, mask)` — one
>   `intersect` `FuncType`, overloads distinguished by arg count in the Sema
>   branch (mirrors `sample`). Compute-stage-only; returns `RayHit`.
> - **Resource binding:** `AccelerationStructure name : N` declared + listed in
>   a compute resource map (read-only `In` access, enforced in Sema). Required a
>   new `OMEGASL_SHADER_ACCELERATION_STRUCTURE_DESC` layout-descriptor enum in
>   `omegasl.h` (appended at the tail) — implied by §1.4/§1.5 but not in the
>   original file summary. The runtime side that consumes it to bind a
>   `GEAccelerationStruct` is Phase 6.
> - **Feature gate:** follows the codebase convention — the author writes
>   `#requires(RAYTRACING)` (serialized to `requiredFeatures`), and a
>   `FeatureScanner::inspectCall` trigger trips `OMEGASL_FEATURE_BIT_RAYTRACING`
>   so an undeclared use warns (same shape as FLOAT16/INT64).
> - **Deferred to sub-phase 1.5 (NOT built):** the `RayQuery` opaque type +
>   `ray_query_*` intrinsics; the 4-arg `intersect(as, ray, mask, rayFlags)`
>   overload and the `RAY_FLAG_*` constants (need a named-constant surface and a
>   non-trivial Metal flag mapping — HLSL/GLSL flag *values* align, Metal's
>   intersector does not take the same bitmask).
> - **Test:** `gte/omegasl/tests/inline_raytracing.omegasl` (+ compile test in
>   the tests `CMakeLists.txt`).

This is the prerequisite for everything else. Ray tracing is exposed as a
**capability of `compute` shaders** via the inline ray-query model — no new
shader stages, no pipeline, no SBT. A compute shader builds a ray, runs a
query against an acceleration structure, and shades the result inline.

### 1.1 No new shader-type keywords

**Do not** add `raygen`/`closesthit`/`anyhit`/`miss`/`intersection`/`callable`
to `Toks.def`, `Lexer.cpp`, `ShaderDecl::Type`, or `omegasl_shader_type`.
Inline ray tracing reuses the existing `Compute` shader type. (Those stage
keywords belong to the Advanced Track only.) This deletes the bulk of the
original Phase 1.

### 1.2 Built-in types

In `gte/omegasl/src/AST.h` builtins, `AST.def`, and `AST.cpp`:

```cpp
DECLARE_BUILTIN_TYPE(Ray);                   // { float3 origin; float3 direction; float tmin; float tmax; }
DECLARE_BUILTIN_TYPE(RayHit);                // intersection result (fields below)
DECLARE_BUILTIN_TYPE(AccelerationStructure); // TLAS handle, bound as a compute resource
DECLARE_BUILTIN_TYPE(RayQuery);              // opaque query object — low-level path (§1.5)
```

`Ray` fields: `origin` (float3), `direction` (float3), `tmin` (float), `tmax` (float).

`RayHit` fields — the universal subset all three backends expose:

| Field | Type | Meaning |
|---|---|---|
| `committed` | `bool` | ray hit committed geometry |
| `t` | `float` | hit distance along the ray |
| `primitiveIndex` | `uint` | primitive index within its geometry |
| `instanceIndex` | `uint` | instance index in the TLAS |
| `barycentrics` | `float2` | triangle hit barycentric coords |

### 1.3 Intrinsics

**High-level one-shot (Phase 1 core)** — opaque triangle geometry, the common case:

```glsl
RayHit intersect(AccelerationStructure as, Ray ray);
RayHit intersect(AccelerationStructure as, Ray ray, uint instanceMask);
RayHit intersect(AccelerationStructure as, Ray ray, uint instanceMask, uint rayFlags);
```

**Ray flags** — a small backend-neutral enum OR'd into `rayFlags`:
`RAY_FLAG_NONE`, `RAY_FLAG_OPAQUE`, `RAY_FLAG_TERMINATE_ON_FIRST_HIT` (shadow
rays), `RAY_FLAG_CULL_BACK_FACING`, `RAY_FLAG_CULL_FRONT_FACING`. Maps to
`RAY_FLAG_*` (HLSL), `gl_RayFlags*EXT` (GLSL), and intersector params (Metal).

**Low-level query (deferred sub-phase 1.5, NOT Phase 1 core)** — required for
non-opaque (any-hit equivalent) and procedural/AABB geometry, where the
traversal loop must be visible to shader code:

```glsl
RayQuery q;
ray_query_init(q, as, ray, rayFlags, mask);
while (ray_query_proceed(q)) {
    // candidate handling: alpha test, custom AABB intersection,
    // ray_query_candidate_*(q), ray_query_commit(q) / ray_query_abandon(q)
}
if (ray_query_committed(q)) { /* ray_query_t(q), ray_query_primitive(q), ... */ }
```

### 1.4 Resource binding

The acceleration structure binds through the **existing compute resource map** —
`bindResourceAtComputeShader(GEAccelerationStruct, index)` already exists in the
public API. No SBT, no hit-group plumbing.

### 1.5 Sema validation (`Sema.cpp`)

- `intersect()` / `ray_query_*` are callable only from `Compute` shaders in
  Phase 1 (fragment-stage ray query is a later option).
- An `AccelerationStructure`-typed argument must resolve to a bound resource in
  the shader's `resourceMap`.
- Validate `Ray` / `RayHit` field access against the built-in field sets.
- **No** recursion-depth, hit-group, payload, or attribute validation — none of
  those concepts exist in the inline model.

### 1.6 Feature gating

A shader that calls `intersect()` / `ray_query_*` sets
`requiredFeatures |= OMEGASL_FEATURE_BIT_RAYTRACING` (bit already exists). The
runtime loader masks this against the device's `featuresAsBitmask()` and rejects
the shader on unsupported hardware (already wired — OmegaSL-Feature-Gap-Survey §14.3).

> **Open item — feature bit granularity.** Inline `RayQuery` requires D3D12
> **Tier 1.1** / SM 6.5, a higher bar than pipeline RT (Tier 1.0). Consider a
> distinct `GTEDEVICE_FEATURE_RAY_QUERY` flag + `OMEGASL_FEATURE_BIT_RAY_QUERY`
> bit rather than overloading `GTEDEVICE_FEATURE_RAYTRACING`. Verify HW/driver
> tier boundaries before finalizing. (Medium confidence on the exact boundary.)

**Files**: `AST.h`, `AST.def`, `AST.cpp`, `Parser.cpp` (builtin recognition
only — no new statement grammar), `Sema.cpp`, `omegasl.h` (RT metadata flag).
**Not touched**: shader-stage keywords in `Toks.def` / `Lexer.cpp`, the
`omegasl_shader_type` / `ShaderDecl::Type` stage enums.

### Sub-phase 1.5 — low-level `RayQuery` traversal loop — IMPLEMENTED (traversal only)

> **Status: IMPLEMENTED + VERIFIED on all three backends (triangle traversal
> loop only) (2026-07-13).** Scope decision (2026-07-13): the *traversal loop* for
> non-opaque / alpha-tested **triangle** geometry, which maps 1:1 across HLSL
> `RayQuery`, GLSL `rayQueryEXT`, and Metal `intersection_query`. **Ray flags
> (`RAY_FLAG_*` + 4-arg `intersect`) and procedural/AABB geometry
> (`generate_intersection`) remain deferred** — the flag surface needs Metal
> `intersection_params` decomposition and AABB needs Metal's divergent
> `bounding_box` path.
>
> `RayQuery` is a builtin opaque type (`builtin = true`, no fields), declared as
> a local (`RayQuery q;`) and mutated in place by the intrinsics. Per backend:
> HLSL `RayQuery<RAY_FLAG_NONE>`, GLSL `rayQueryEXT`, Metal
> `intersection_query<triangle_data, instancing>`.
>
> Intrinsic set (12) — one Sema family branch (compute-only, arg0 is a
> `RayQuery`); codegen lives in each backend's `tryEmitBuiltinCall` (NOT new
> virtuals), since all but `ray_query_init` are one-line method rewrites.
> `ray_query_init` is statement-shaped on HLSL/Metal (build a `RayDesc`/`ray`
> first), so it uses `queuePendingStatement`.
>
> | OmegaSL | HLSL | GLSL | Metal (`q` = intersection_query) |
> |---|---|---|---|
> | `ray_query_init(q,as,ray[,mask])` | `RayDesc`+`q.TraceRayInline(as,RAY_FLAG_NONE,mask,rd)` | `rayQueryInitializeEXT(q,as,gl_RayFlagsNoneEXT,mask,o,tmin,d,tmax)` | `ray`+`q.reset(r,as,mask)` |
> | `ray_query_proceed(q)`→bool | `q.Proceed()` | `rayQueryProceedEXT(q)` | `q.next()` |
> | `ray_query_commit(q)`→void | `q.CommitNonOpaqueTriangleHit()` | `rayQueryConfirmIntersectionEXT(q)` | `q.commit_triangle_intersection()` |
> | `ray_query_committed(q)`→bool | `q.CommittedStatus()==COMMITTED_TRIANGLE_HIT` | `rayQueryGetIntersectionTypeEXT(q,true)==…TriangleEXT` | `q.get_committed_intersection_type()!=…none` |
> | `ray_query_t(q)`→float | `q.CommittedRayT()` | `…GetIntersectionTEXT(q,true)` | `q.get_committed_distance()` |
> | `ray_query_primitive(q)`→uint | `q.CommittedPrimitiveIndex()` | `uint(…PrimitiveIndexEXT(q,true))` | `q.get_committed_primitive_id()` |
> | `ray_query_instance(q)`→uint | `q.CommittedInstanceIndex()` | `uint(…InstanceIdEXT(q,true))` | `q.get_committed_instance_id()` |
> | `ray_query_barycentrics(q)`→float2 | `q.CommittedTriangleBarycentrics()` | `…BarycentricsEXT(q,true)` | `q.get_committed_triangle_barycentric_coord()` |
> | `ray_query_candidate_t(q)`→float | `q.CandidateTriangleRayT()` | `…GetIntersectionTEXT(q,false)` | `q.get_candidate_triangle_distance()` |
> | `ray_query_candidate_primitive(q)`→uint | `q.CandidatePrimitiveIndex()` | `uint(…PrimitiveIndexEXT(q,false))` | `q.get_candidate_primitive_id()` |
> | `ray_query_candidate_instance(q)`→uint | `q.CandidateInstanceIndex()` | `uint(…InstanceIdEXT(q,false))` | `q.get_candidate_instance_id()` |
> | `ray_query_candidate_barycentrics(q)`→float2 | `q.CandidateTriangleBarycentrics()` | `…BarycentricsEXT(q,false)` | `q.get_candidate_triangle_barycentric_coord()` |
>
> **Files**: `Toks.def`, `AST.h/.def/.cpp`, `Lexer.cpp`, `Sema.cpp`,
> `FeatureScanner.cpp`, `HLSLTarget.cpp`, `GLSLTarget.cpp`, `MSLTarget.cpp`;
> test `ray_query_loop.omegasl`.

### Cross-backend compile verification (2026-07-13)

All three backends were transpiled with `omegaslc --<backend> --emit-source-only`
and compiled to real object code with the platform toolchains (Windows host) —
for both the `intersect` one-shot and the `ray_query` loop: HLSL→DXIL `.cso`,
MSL→AIR `.air`, GLSL→SPIR-V `.spv`. The GLSL body-loop fix was proven
regression-free by diffing 22 non-RT GLSL outputs (byte-identical pre/post-fix).

- **HLSL** — verified via DXC (`cs_6_5`): the `omegasl_compile_inline_raytracing`
  / `omegasl_compile_ray_query_loop` ctest cases pass on the DirectX build.
- **MSL** — verified via `metal.exe` (Metal 3.x, `-std=metal3.1`): all four
  entries (`intersect` one-shot + `ray_query` loop) compile to `.air`.
- **GLSL** — verified via `glslc` (Vulkan SDK 1.4.350, `--target-env=vulkan1.2`)
  after fixing **two GLSL-only bugs** the Metal/GLSL verification surfaced:
  1. **Statement-injection was silently dropped in a GLSL shader body.**
     `GLSLTarget::emitShaderEntryBody` had a custom body loop that emitted
     statements inline and never flushed `CodeGen::pendingStatements` — so the
     `intersect` lowering's injected `rayQueryEXT`/`RayHit` block vanished
     (`_rh0` undeclared). Fixed by routing non-return body statements through
     the shared `cg.emitStatementLine` (byte-identical when nothing is queued),
     matching what the HLSL/MSL bodies already did. Latent until inline RT — no
     prior GLSL body builtin used injection.
  2. **`GL_EXT_ray_query` requires GLSL 4.60**, but `emitDefaultHeaders` emitted
     `#version 450` (→ `rayQueryEXT` undeclared). Fixed to emit `#version 460`
     only when the shader `#requires(RAYTRACING)` (non-RT output unchanged).

---

## Phase 2: OmegaSL Inline Ray Tracing Code Generation

> **Status: IMPLEMENTED + VERIFIED on all three backends (2026-07-13).**
> `intersect()` lowers through a new `Target::emitIntersect` virtual (dispatched
> from `CodeGen.cpp`, alongside the texture builtins — NOT `tryEmitBuiltinCall`).
> Because inline ray query is statement-shaped (declare a query/intersector, run
> traversal, read the committed hit) and cannot be a sub-expression, each backend
> queues the block via `cg.queuePendingStatement(...)` and emits the injected
> `RayHit` temp inline — the same statement-injection pattern as
> `emitTextureGetDimensions` (shared temp counter `CodeGen::rayQueryTempId`).
> - **HLSL** (`HLSLTarget.cpp`): `RayQuery<RAY_FLAG_NONE>` + `RayDesc` +
>   `TraceRayInline` + one `Proceed()`; `RaytracingAccelerationStructure` at a
>   `t` register; compute profile bumped to `cs_6_5` when RT is required (offline
>   dxc), runtime `D3DCompile` path fails loud (SM 5.1 max). No preamble needed.
> - **GLSL** (`GLSLTarget.cpp`): `rayQueryEXT` + `rayQueryInitializeEXT` +
>   drain-loop; `accelerationStructureEXT` uniform; `#extension GL_EXT_ray_query`
>   in `emitDefaultHeaders`; glslc pinned to `--target-env=vulkan1.2` for RT
>   (SPV_KHR_ray_query needs SPIR-V 1.4). primitiveIndex/instanceId cast to uint.
> - **MSL** (`MSLTarget.cpp`): `intersector<triangle_data, instancing>` +
>   `intersection_result`; `acceleration_structure<instancing>` bound at a buffer
>   index; `#include <metal_raytracing>` + `using namespace metal::raytracing` in
>   `emitDefaultHeaders`. The `ray` type is spelled fully-qualified
>   (`metal::raytracing::ray`) to avoid being shadowed by a user `Ray` local
>   named `ray`.
> - Serialization needed no change — `#requires(RAYTRACING)` already flows
>   through `fileRequiredFeatures` → `meta.requiredFeatures`.

Every backend emits a **regular compute shader** that uses the backend's inline
ray-query API. No DXIL libraries, no RT pipeline stages, no SBT — the generated
shape is the same control flow on all three. The examples below show the
high-level one-shot `intersect()` lowering for opaque triangle geometry.

### 2.1 HLSL code generation (D3D12 / DXC)

In `gte/omegasl/src/HLSLCodeGen.cpp`. `intersect(as, ray)` lowers to:

```hlsl
RayQuery<RAY_FLAG_NONE> q;
RayDesc rd; rd.Origin = ray.origin; rd.Direction = ray.direction;
rd.TMin = ray.tmin; rd.TMax = ray.tmax;
q.TraceRayInline(as, RAY_FLAG_NONE, mask, rd);
q.Proceed();                       // opaque triangles: terminates in one step
RayHit hit;
hit.committed      = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
hit.t              = q.CommittedRayT();
hit.primitiveIndex = q.CommittedPrimitiveIndex();
hit.instanceIndex  = q.CommittedInstanceIndex();
hit.barycentrics   = q.CommittedTriangleBarycentrics();
```

- `AccelerationStructure` → `RaytracingAccelerationStructure`.
- **Compile target:** the shader stays `compute` → `cs_6_5` (RayQuery requires
  SM 6.5). *Not* `lib_6_3` — there is no DXIL library or state object.
- Requires **DXC** (already used via `hlslCodeOpts.dxc_cmd`).

### 2.2 GLSL/SPIR-V code generation (Vulkan / glslc)

In `gte/omegasl/src/GLSLCodeGen.cpp`. Emit `#extension GL_EXT_ray_query : require`.
`intersect(as, ray)` lowers to:

```glsl
rayQueryEXT q;
rayQueryInitializeEXT(q, as, gl_RayFlagsOpaqueEXT, mask,
                      ray.origin, ray.tmin, ray.direction, ray.tmax);
while (rayQueryProceedEXT(q)) {}
RayHit hit;
hit.committed = (rayQueryGetIntersectionTypeEXT(q, true)
                 == gl_RayQueryCommittedIntersectionTriangleEXT);
hit.t              = rayQueryGetIntersectionTEXT(q, true);
hit.primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
hit.instanceIndex  = rayQueryGetIntersectionInstanceIdEXT(q, true);
hit.barycentrics   = rayQueryGetIntersectionBarycentricsEXT(q, true);
```

- `AccelerationStructure` → `accelerationStructureEXT`.
- **Compile:** `glslc --target-env=vulkan1.2 -fshader-stage=comp`. No
  `rgen`/`rchit`/`rmiss` stages.

### 2.3 Metal Shading Language code generation

In `gte/omegasl/src/MetalCodeGen.cpp`. Emit `#include <metal_raytracing>` /
`using namespace metal::raytracing`. `intersect(as, ray)` lowers to:

```metal
intersector<triangle_data, instancing> isect;
ray r{ ray.origin, ray.direction, ray.tmin, ray.tmax };
intersection_result<triangle_data, instancing> res = isect.intersect(r, as, mask);
RayHit hit;
hit.committed      = (res.type != intersection_type::none);
hit.t              = res.distance;
hit.primitiveIndex = res.primitive_id;
hit.instanceIndex  = res.instance_id;
hit.barycentrics   = res.triangle_barycentric_coord;
```

- `AccelerationStructure` → `instance_acceleration_structure` (or
  `acceleration_structure<instancing>`).
- Stays a compute kernel. **No** `MTLVisibleFunctionTable` /
  `MTLIntersectionFunctionTable` for the common case — those are Advanced Track.

### 2.4 Shader library serialization

In `CodeGen.h` `linkShaderObjects()`, RT metadata collapses to a single flag plus
the feature requirement — no hit groups, payload/attribute sizes, or recursion depth:

```cpp
// per-shader: bool usesRayQuery; requiredFeatures already carries
// OMEGASL_FEATURE_BIT_RAYTRACING (or _RAY_QUERY, per Phase 1.6 open item).
```

**Files**: `HLSLCodeGen.cpp`, `GLSLCodeGen.cpp`, `MetalCodeGen.cpp`, `CodeGen.h`

---

# Advanced Track: Pipeline + Shader Binding Table model

> **TODO — planned, deferred (not abandoned).** Full ray-tracing capability for
> the engine still includes this track; it is sequenced *after* the inline
> ray-query model (Phases 1–2–6) lands, not dropped. **Status: Not Started.**
>
> The phases below (3–5) implement the DXR/Vulkan *ray-tracing pipeline* model —
> separate raygen/closesthit/anyhit/miss/intersection/callable stages,
> ray-tracing PSOs, and shader binding tables. This is what inline ray query
> *cannot* provide: hardware ray recursion, callable shaders, and GPU-driven
> per-geometry shader dispatch. It is **not required** for the inline model and
> does not transpile to Metal without `MTLVisibleFunctionTable` /
> `MTLIntersectionFunctionTable` reconstruction.
>
> When this track is taken up, the original Phase 1/2 stage-keyword and
> stage-codegen work (removed from the inline plan above) is resurrected here —
> see git history of this doc for that material.
>
> **Trigger to start:** inline track (Phases 1, 2, 6, 7) complete *and* a
> concrete use case appears that needs recursion, callables, or per-geometry
> shaders (e.g. a multi-material path tracer). Per-backend order: D3D12 →
> Vulkan → Metal.

## Phase 3 (Advanced Track): Ray Tracing Pipeline State Objects

### 3.1 Public API — `RayTracingPipelineDescriptor`

Add to `gte/include/omegaGTE/GEPipeline.h`:

```cpp
struct OMEGAGTE_EXPORT RayTracingPipelineDescriptor {
    OmegaCommon::String name;
    SharedHandle<GTEShader> rayGenShader;

    struct HitGroup {
        OmegaCommon::String name;
        SharedHandle<GTEShader> closestHitShader;     // required
        SharedHandle<GTEShader> anyHitShader;          // optional (nullptr)
        SharedHandle<GTEShader> intersectionShader;    // optional (nullptr, triangles use built-in)
    };
    OmegaCommon::Vector<HitGroup> hitGroups;

    OmegaCommon::Vector<SharedHandle<GTEShader>> missShaders;
    OmegaCommon::Vector<SharedHandle<GTEShader>> callableShaders;

    unsigned maxPayloadSize = 32;      // bytes
    unsigned maxAttributeSize = 8;     // bytes (float2 barycentrics = 8)
    unsigned maxRecursionDepth = 1;
};

typedef struct __GERayTracingPipelineState GERayTracingPipelineState;
```

### 3.2 Public API — Engine method

Add to `OmegaGraphicsEngine` in `GE.h`:

```cpp
virtual SharedHandle<GERayTracingPipelineState> makeRayTracingPipelineState(
    RayTracingPipelineDescriptor &desc) = 0;
```

### 3.3 Public API — Command buffer

Add to `GECommandBuffer` in `GECommandQueue.h`:

```cpp
/// @brief Sets a Ray Tracing Pipeline State for use with dispatchRays().
/// Must be called between startComputePass() and dispatchRays().
virtual void setRayTracingPipelineState(
    SharedHandle<GERayTracingPipelineState> &pipelineState) = 0;
```

Update `dispatchRays()` to use the bound RT pipeline's SBT.

### 3.4 D3D12 implementation — `ID3D12StateObject`

In `gte/src/d3d12/GED3D12Pipeline.cpp` (or a new `GED3D12RTPipeline.cpp`):

```cpp
struct GED3D12RayTracingPipelineState : public GERayTracingPipelineState {
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;

    // Shader Binding Table
    ComPtr<ID3D12Resource> sbtBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS rayGenRecord;
    D3D12_GPU_VIRTUAL_ADDRESS missTableStart;
    UINT64 missRecordStride;
    UINT missRecordCount;
    D3D12_GPU_VIRTUAL_ADDRESS hitGroupTableStart;
    UINT64 hitGroupRecordStride;
    UINT hitGroupCount;
    D3D12_GPU_VIRTUAL_ADDRESS callableTableStart;
    UINT64 callableRecordStride;
    UINT callableCount;
};
```

**Pipeline creation flow:**
1. Build `D3D12_STATE_SUBOBJECT` array:
   - `D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY` — The compiled DXIL library containing all RT shaders
   - `D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP` — One per hit group from the descriptor
   - `D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG` — Payload + attribute sizes
   - `D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG` — Max recursion depth
   - `D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE` — From shader resource layouts
2. Call `d3d12_device->CreateStateObject(&desc, IID_PPV_ARGS(&stateObject))`
3. Query `ID3D12StateObjectProperties` for shader identifiers
4. Build SBT buffer (see Phase 4)

### 3.5 Vulkan implementation — `vkCreateRayTracingPipelinesKHR`

In a new or existing Vulkan pipeline file:

```cpp
struct GEVulkanRayTracingPipelineState : public GERayTracingPipelineState {
    GEVulkanEngine *engine;
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;

    // SBT buffer
    VkBuffer sbtBuffer;
    VmaAllocation sbtAllocation;
    VkStridedDeviceAddressRegionKHR raygenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callableRegion;
};
```

**Pipeline creation flow:**
1. Build `VkRayTracingShaderGroupCreateInfoKHR` array (raygen group, hit groups, miss groups, callable groups)
2. Build `VkRayTracingPipelineCreateInfoKHR` with shader stages and groups
3. Call `vkCreateRayTracingPipelinesKHR()`
4. Query shader group handles via `vkGetRayTracingShaderGroupHandlesKHR()`
5. Build SBT buffer (see Phase 4)

Need to add function pointer:
```cpp
PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKhr = nullptr;
PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKhr = nullptr;
```

Load these in `GEVulkanEngine` constructor alongside the existing RT function pointers.

### 3.6 Metal implementation — Compute pipeline with visible function tables

Metal doesn't have a dedicated RT pipeline object. Instead:

```cpp
struct GEMetalRayTracingPipelineState : public GERayTracingPipelineState {
    NSSmartPtr computePipeline;        // MTLComputePipelineState (raygen kernel)
    NSSmartPtr intersectionFnTable;    // MTLIntersectionFunctionTable
    NSSmartPtr visibleFnTable;         // MTLVisibleFunctionTable (hit/miss/callable)
};
```

**Creation flow:**
1. Compile raygen as compute kernel
2. Create `MTLComputePipelineDescriptor` with linked functions
3. Build `MTLIntersectionFunctionTable` from intersection functions
4. Build `MTLVisibleFunctionTable` from hit/miss/callable functions
5. Create pipeline via `newComputePipelineStateWithDescriptor:options:reflection:error:`

**Files**: `GEPipeline.h`, `GE.h`, `GECommandQueue.h`, + backend pipeline files

---

## Phase 4 (Advanced Track): Shader Binding Tables

The SBT is the data structure that connects geometry instances to their shader programs during ray traversal.

### 4.1 D3D12 SBT construction

After creating the `ID3D12StateObject`, build the SBT:

1. Get shader identifiers (32-byte handles) via `stateObjectProps->GetShaderIdentifier(exportName)`
2. Allocate a GPU buffer large enough for all records (raygen + miss table + hit group table + callable table)
3. Write shader identifiers into the buffer at aligned offsets (`D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT` = 64, record stride = `D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT` = 32 aligned up to include local root arguments if any)
4. Store the GPU virtual addresses and strides in the pipeline state object

### 4.2 Vulkan SBT construction

After creating the ray tracing pipeline:

1. Get shader group handles (32 bytes each) via `vkGetRayTracingShaderGroupHandlesKHR`
2. Allocate a buffer with `VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
3. Write group handles at aligned offsets (`shaderGroupHandleAlignment` from `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`)
4. Compute `VkStridedDeviceAddressRegionKHR` for each table (raygen, miss, hit, callable)

### 4.3 Metal "SBT" (visible function tables)

Metal uses function tables instead of an explicit SBT buffer:

1. Create `MTLVisibleFunctionTable` with entries for each hit/miss function
2. Create `MTLIntersectionFunctionTable` for custom intersection functions
3. Set tables on the compute command encoder via `setVisibleFunctionTable:atBufferIndex:` and `setIntersectionFunctionTable:atBufferIndex:`

**Files**: Backend pipeline implementation files

---

## Phase 5 (Advanced Track): Complete `dispatchRays()` on All Backends

> **Inline model note:** inline ray tracing does **not** use `dispatchRays()` —
> a ray-query compute shader is launched with the existing
> `dispatchThreadgroups()` / `dispatchThreads()` in a normal compute pass.
> `dispatchRays()` remains a stub until this Advanced Track is implemented; it
> should assert/guard rather than silently misbehave.

### 5.1 D3D12

Replace the current stub in `GED3D12CommandQueue.cpp:882`:

```cpp
void GED3D12CommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "Must be in a compute pass to dispatch rays");
    assert(currentRTPipeline && "Must bind a ray tracing pipeline before dispatching rays");

    auto *rtPipeline = currentRTPipeline; // GED3D12RayTracingPipelineState*

    commandList->SetPipelineState1(rtPipeline->stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC desc = {};
    desc.Width = x;
    desc.Height = y;
    desc.Depth = z;

    desc.RayGenerationShaderRecord.StartAddress = rtPipeline->rayGenRecord;
    desc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

    desc.MissShaderTable.StartAddress = rtPipeline->missTableStart;
    desc.MissShaderTable.SizeInBytes = rtPipeline->missRecordStride * rtPipeline->missRecordCount;
    desc.MissShaderTable.StrideInBytes = rtPipeline->missRecordStride;

    desc.HitGroupTable.StartAddress = rtPipeline->hitGroupTableStart;
    desc.HitGroupTable.SizeInBytes = rtPipeline->hitGroupRecordStride * rtPipeline->hitGroupCount;
    desc.HitGroupTable.StrideInBytes = rtPipeline->hitGroupRecordStride;

    desc.CallableShaderTable.StartAddress = rtPipeline->callableTableStart;
    desc.CallableShaderTable.SizeInBytes = rtPipeline->callableRecordStride * rtPipeline->callableCount;
    desc.CallableShaderTable.StrideInBytes = rtPipeline->callableRecordStride;

    commandList->DispatchRays(&desc);
}
```

### 5.2 Vulkan

Replace the stub in `GEVulkanCommandQueue.cpp:1062`:

```cpp
void GEVulkanCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "Must be in compute pass to dispatch rays");
    auto *engine = parentQueue->engine;
    auto *rtPipeline = currentRTPipeline; // GEVulkanRayTracingPipelineState*

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline->pipeline);
    // Bind descriptor sets for RT pipeline...

    engine->vkCmdTraceRaysKhr(
        commandBuffer,
        &rtPipeline->raygenRegion,
        &rtPipeline->missRegion,
        &rtPipeline->hitRegion,
        &rtPipeline->callableRegion,
        x, y, z
    );
}
```

### 5.3 Metal

Replace the stub in `GEMetalCommandQueue.mm:508`:

```objc
void GEMetalCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    auto *rtPipeline = currentRTPipeline; // GEMetalRayTracingPipelineState*
    auto cp = /* current compute command encoder */;

    [cp setComputePipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLComputePipelineState>, rtPipeline->computePipeline.handle())];
    [cp setVisibleFunctionTable:NSOBJECT_OBJC_BRIDGE(id<MTLVisibleFunctionTable>, rtPipeline->visibleFnTable.handle()) atBufferIndex:...];
    [cp setIntersectionFunctionTable:NSOBJECT_OBJC_BRIDGE(id<MTLIntersectionFunctionTable>, rtPipeline->intersectionFnTable.handle()) atBufferIndex:...];

    MTLSize threadsPerGrid = MTLSizeMake(x, y, z);
    MTLSize threadsPerGroup = MTLSizeMake(8, 8, 1); // or from pipeline
    [cp dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerGroup];
}
```

**Files**: `GED3D12CommandQueue.cpp`, `GEVulkanCommandQueue.cpp`, `GEMetalCommandQueue.mm`

---

## Phase 6: Top-Level Acceleration Structures (TLAS) and Instance Descriptors

> **Required for the inline model — this is the real next step after Phases 1–2,
> not part of the deferred Advanced Track.** Inline `intersect()` traces against
> a TLAS; `RayHit.instanceIndex` is only meaningful when instances exist. With
> the inline-first plan, the dependency order is **Phase 1 → 2 → 6 → 7**, and
> Phases 3–5 are skipped unless the Advanced Track is taken up.

> **Status: D3D12 IMPLEMENTED (2026-07-13); Vulkan + Metal DEFERRED.** Per the
> context-budget decision, only the D3D12 backend is built out this pass.
> - **Public API (`GE.h`):** added `GEAccelerationStructInstance` (3x4 row-major
>   transform + `instanceID`/`instanceMask`/`instanceContributionToHitGroupIndex`/
>   `flags` bitfields + a `blas` handle), a backend-neutral
>   `GEAccelerationStructInstanceFlags` enum (values match
>   `D3D12_RAYTRACING_INSTANCE_FLAG_*` / Vulkan / Metal), and extended
>   `GEAccelerationStructDescriptor` with `enum Level { BottomLevel, TopLevel }`,
>   `Level level = BottomLevel`, an `instances` vector, and an `addInstance(...)`
>   helper that flips `level` to TopLevel and **defaults the mask to 0xFF** (a
>   0 mask is invisible to the 0xFF default ray mask — a silent-no-hits trap).
> - **D3D12 (`GED3D12.{h,cpp}`, `GED3D12CommandQueue.cpp`):**
>   `allocateAccelerationStructure` now sizes a TLAS by `instances.size()` off
>   the `level` field (was a fragile `geometryDescs.empty()` check that built a
>   0-instance TLAS). `buildAccelerationStructure` / `refitAccelerationStructure`
>   translate the GE instances into a `D3D12_RAYTRACING_INSTANCE_DESC` array
>   (field-by-field — GE bitfield packing need not match D3D12's), upload it to an
>   Upload-heap buffer (GENERIC_READ satisfies the build's NON_PIXEL_SHADER_RESOURCE
>   requirement) stored on the TLAS so it outlives the recorded command, and point
>   the build at it via `InstanceDescs`. Shared helper `fillTLASInstancesFromGE`.
> - **Binding already worked:** `bindResourceAtComputeShader(GEAccelerationStruct)`
>   binds the TLAS as a compute root SRV (`SetComputeRootShaderResourceView`) —
>   no change needed; the inline `intersect` / `ray_query_*` shaders can trace the
>   built TLAS on D3D12 now.
> - **Deferred:** the Vulkan (`VkAccelerationStructureInstanceKHR` +
>   `TYPE_TOP_LEVEL_KHR`) and Metal (`MTLInstanceAccelerationStructureDescriptor`)
>   equivalents — §6.2 rows below.

The current API only supports bottom-level acceleration structures (BLAS). For real-world ray tracing, we need TLAS that reference instances of BLAS with per-instance transforms.

### 6.1 Extend public API

Add to `GE.h`:

```cpp
struct OMEGAGTE_EXPORT GEAccelerationStructInstance {
    float transform[3][4];                           // 3x4 row-major affine transform
    unsigned instanceID : 24;                        // user ID
    unsigned instanceMask : 8;                       // visibility mask
    unsigned instanceContributionToHitGroupIndex : 24;
    unsigned flags : 8;                              // front/back face cull, force opaque, etc.
    SharedHandle<GEAccelerationStruct> blas;
};

// Extend GEAccelerationStructDescriptor to support TLAS:
struct GEAccelerationStructDescriptor {
    enum Level { BottomLevel, TopLevel };
    Level level = BottomLevel;

    // For BottomLevel: existing geometry data
    OmegaCommon::Vector<Geometry> data;

    // For TopLevel: instance array
    OmegaCommon::Vector<GEAccelerationStructInstance> instances;
};
```

### 6.2 Backend implementations

**D3D12**: Use `D3D12_RAYTRACING_INSTANCE_DESC` written to a GPU buffer, build with `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL`.

**Vulkan**: Use `VkAccelerationStructureInstanceKHR` written to a device buffer, build with `VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR`.

**Metal**: Use `MTLInstanceAccelerationStructureDescriptor` with `MTLAccelerationStructureInstanceDescriptor` array.

**Files**: `GE.h`, all three backend engine files, all three backend command queue files

---

## Phase 7: Testing

### 7.1 OmegaSL compiler tests

Add test `.omegasl` files under `gte/omegasl/tests/` with an inline ray-query
compute shader:
- A compute shader that builds a `Ray`, calls `intersect(as, ray)`, and writes
  hit/miss as a solid color to an output texture
- Verify compilation to HLSL (`cs_6_5` + `RayQuery`), GLSL (`GL_EXT_ray_query`),
  and MSL (`intersector`)

### 7.2 Integration tests

Add a new test app under `gte/tests/directx/` (and equivalent for Vulkan):
- Create a triangle BLAS
- Build a TLAS with one instance
- Bind the TLAS to a compute shader via `bindResourceAtComputeShader`
- Run the inline ray-query shader with `dispatchThreadgroups` to a 2D output texture
- Read back and verify non-zero output (no RT pipeline / SBT involved)

### 7.3 Feature check test

Verify that on hardware without RT support:
- `device->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)` returns `false`
- `allocateAccelerationStructure()` returns `nullptr`
- Shaders requiring the RT feature bit are rejected by the runtime loader
- No crash, no undefined behavior

---

## File Change Summary

Inline-first scope. Rows tagged **[Adv]** belong to the deferred Advanced Track
(Phases 3–5) and are not touched by the inline plan. Phase 0 rows are **[done]**.

| File | Changes |
|---|---|
| **Phase 0 — done** | |
| `gte/include/omegaGTE/GE.h` | [done] `#ifdef` gates already gone; dead macro comments deleted |
| `gte/include/omegaGTE/GECommandQueue.h` | [done] no `#ifdef` gates remain |
| backend `*.h/.cpp/.mm` | [done] runtime feature guards added to RT factory methods |
| **OmegaSL Compiler (Phase 1–2, inline)** | |
| `gte/include/omegasl.h` | Add inline-RT metadata flag (`usesRayQuery` / feature bit). No new `omegasl_shader_type` stages. |
| `gte/omegasl/src/AST.h` | Add builtin types `Ray`, `RayHit`, `AccelerationStructure`, `RayQuery`; intrinsics `intersect`, `ray_query_*` |
| `gte/omegasl/src/AST.def` | Add inline-RT builtin macro names |
| `gte/omegasl/src/AST.cpp` | Initialize inline-RT builtins |
| `gte/omegasl/src/Parser.cpp` | Recognize new builtins (no new statement grammar, no stage keywords) |
| `gte/omegasl/src/Sema.cpp` | Validate `intersect`/`ray_query_*` in compute shaders; AS binding |
| `gte/omegasl/src/HLSLCodeGen.cpp` | `RayQuery` lowering; `cs_6_5` target (not `lib_6_3`) |
| `gte/omegasl/src/GLSLCodeGen.cpp` | `GL_EXT_ray_query` lowering; `comp` stage |
| `gte/omegasl/src/MetalCodeGen.cpp` | `intersector` lowering (compute kernel; no function tables) |
| `gte/omegasl/src/CodeGen.h` | Single inline-RT metadata flag in serialization |
| `gte/omegasl/src/Toks.def`, `Lexer.cpp` | **[Adv]** stage keywords `KW_RAYGEN`…`KW_CALLABLE` only if Advanced Track |
| **Acceleration Structures (Phase 6, inline)** | |
| `gte/include/omegaGTE/GE.h` | Add `GEAccelerationStructInstance`; extend descriptor for TLAS |
| `gte/src/{d3d12,vulkan,metal}/*` | TLAS build + instance descriptors in each backend engine/queue |
| **Advanced Track (Phases 3–5)** | |
| `gte/include/omegaGTE/GEPipeline.h` | **[Adv]** `RayTracingPipelineDescriptor`, `GERayTracingPipelineState` |
| `gte/include/omegaGTE/GE.h` / `GECommandQueue.h` | **[Adv]** `makeRayTracingPipelineState`, `setRayTracingPipelineState` |
| `gte/src/{d3d12,vulkan,metal}/*Pipeline*`, `*CommandQueue*` | **[Adv]** PSO/SBT creation; `dispatchRays` with populated tables |
| **Docs** | |
| `gte/docs/API.rst` | [done] runtime feature-check pattern documented |
| `wtk/docs/OmegaGTEView-Proposal.md` | [done] macro replaced with runtime check |

## Implementation Order

The inline-first plan has this dependency chain:

```
Phase 0 (remove macro) ─── DONE (2026-05-23)
    │
Phase 1 (OmegaSL inline ray-query types + intrinsics) ─── prerequisite
    │
Phase 2 (OmegaSL inline codegen: RayQuery / ray_query / intersector)
    │
Phase 6 (TLAS / instances) ─── inline intersect() traces a TLAS
    │
Phase 7 (testing) ─── validates the inline path end-to-end

  ── Deferred Advanced Track (only if path-tracing needs arise) ──
Phase 3 (RT pipeline objects) ──┐
Phase 4 (SBT construction) ─────┤── pipeline/SBT model; per-backend
Phase 5 (dispatchRays) ─────────┘   (D3D12 first, then Vulkan, then Metal)
```

Phases 1, 2, 6 are the inline critical path. Phases 1–2 are OmegaSL-only;
Phase 6 touches all three backend engines/queues. The Advanced Track (3–5) is a
**planned follow-on TODO** (Status: Not Started) — full RT capability includes
it — sequenced after the inline path lands and a recursion/callable/per-geometry
use case is in hand.
