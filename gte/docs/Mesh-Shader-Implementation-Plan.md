# Mesh Shader Implementation Plan

## Goal

Add mesh shader support to OmegaGTE as a new pipeline type, with matching `mesh` and `amplification` stages in OmegaSL. The feature will be runtime-gated behind `GTEDEVICE_FEATURE_MESH_SHADER`, following the pattern established by the raytracing and pipeline-completion plans.

Cross-platform parity is the acceptance bar: code written against the public API compiles and runs on D3D12, Metal, and Vulkan, or fails cleanly at descriptor validation when the device lacks support.

---

## Implementation Status

> Updated 2026-05-30. Tracks against **Phasing** below.

### Phase 2a follow-up — `setMeshOutputs(nv, np)` builtin: **code-complete, build-verified, functional smoke-test + spirv-val passed**

The runtime-count builtin Phase 2a's trade-off note flagged is in. `setMeshOutputs(uint nv, uint np)` is now a valid OmegaSL builtin (mesh-stage-only, void-returning, at-most-once per shader). When present, the GLSL backend lowers it to `SetMeshOutputsEXT(nv, np)` in place and SUPPRESSES the locked-to-maxima auto-emit at body start (GL_EXT_mesh_shader permits only one such call). When absent, the auto-emit fires exactly as before — every previously-valid mesh shader still compiles bit-identically. HLSL/MSL lowerings are wired ahead of Phase 2b/2c (per [[feedback_clean_uniform_fixes]] / [[feedback_frontend_backend_uniformity]]) but **compile/run-unverified on those backends** ([[feedback_mark_unverified_backends_in_plan]] — HLSL needs DXC off-platform; MSL has no Metal toolchain on this Linux host AND its `[[mesh]]` handle name is sentinel-placeholdered for 2c to rewrite).

Verified in-session 2026-05-30:

- `gte/omegasl/tests/mesh_basic.omegasl` extended with a fourth entry `mesh_triangle_runtime` that calls `setMeshOutputs(3u, 1u)`. GLSL output contains exactly one `SetMeshOutputsEXT(3, 1);` (the user's call at the source position; the auto-emit is gone). The other three pre-existing entries still emit the locked-to-maxima auto-call — no behavioral regression. All four `.mesh` outputs compile via `glslc --target-env=vulkan1.2 -fshader-stage=mesh` and validate clean via `spirv-val --target-env vulkan1.2`.
- HLSL / MSL still halt cleanly at `supportsStage(Mesh)` for every mesh entry incl. the new runtime one — Sema runs first and passes for all four entries, then per-backend codegen aborts with the precise "not yet implemented" diagnostic and writes no partial source. Confirms the front-end is uniform across backends even though codegen is GLSL-only.
- Negatives (each fires the intended Sema diagnostic with a nonzero `error:` line; exit-status convention matches the existing `invalid_barrier_outside_compute.omegasl` pattern):
  - `invalid_set_mesh_outputs_outside_mesh.omegasl` — called from compute → `` `setMeshOutputs` is only valid inside a mesh shader``.
  - `invalid_set_mesh_outputs_wrong_types.omegasl` — `int` literal arg → `` `setMeshOutputs` requires two scalar `uint` arguments``.
  - `invalid_set_mesh_outputs_duplicate.omegasl` — two calls in one body → `` `setMeshOutputs` may be called at most once per mesh shader``.
  - `invalid_set_mesh_outputs_exceeds_max.omegasl` — `setMeshOutputs(64u, 1u)` against `max_vertices=3` → `numVertices (64) exceeds the shader's declared max_vertices (3)`.

Files touched (~120 lines):

| Area | File | Change |
|---|---|---|
| Macro | `gte/omegasl/src/AST.def` | `BUILTIN_SET_MESH_OUTPUTS "setMeshOutputs"` + cross-backend lowering note. |
| AST flag | `gte/omegasl/src/AST.h` | `ShaderDecl::meshHasUserSetMeshOutputsCall` — set by Sema when a user call is seen; consulted by each backend's body-emit to suppress the auto-emit. |
| Reservation | `gte/omegasl/src/AST.cpp::isReservedBuiltinName` | `BUILTIN_SET_MESH_OUTPUTS` added so user code can't define a `func setMeshOutputs(...)`. |
| Sema (stage + arg-count) | `gte/omegasl/src/Sema.cpp::performSemForExpr` | `isSetMeshOutputs` branch alongside `isBarrier`: stage check (`Mesh` only), `expectedArgs = 2`. |
| Sema (type + duplicate + bounds) | `gte/omegasl/src/Sema.cpp::performSemForExpr` | After arg sema: both args must resolve to `uint`; at-most-once per shader (stamps `meshHasUserSetMeshOutputsCall`); when both args are integer literals, `nv > max_vertices` / `np > max_primitives` is rejected; returns `void_type`. |
| GLSL lowering | `gte/omegasl/src/GLSLTarget.cpp::renameBuiltin` | `setMeshOutputs` → `SetMeshOutputsEXT`. Shared `(args)` print does the rest. |
| GLSL suppress auto-emit | `gte/omegasl/src/GLSLTarget.cpp::emitShaderEntryBody` | Skip the locked-to-maxima `SetMeshOutputsEXT(<max>, <max>)` when `_decl->meshHasUserSetMeshOutputsCall` is true. |
| HLSL lowering (dormant) | `gte/omegasl/src/HLSLTarget.cpp::renameBuiltin` | `setMeshOutputs` → `SetMeshOutputCounts`. Lights up when Phase 2b flips `supportsStage(Mesh)`. |
| MSL lowering (dormant) | `gte/omegasl/src/MSLTarget.cpp::tryEmitBuiltinCall` | `setMeshOutputs(nv, np)` → `__omegasl_mesh_output_handle.set_primitive_count(np)` — `nv` is dropped by design (MSL infers vertex count from the highest `set_vertex` slot; see Cross-Backend Differences). Sentinel handle name for 2c to rewrite. |
| Tests | `gte/omegasl/tests/mesh_basic.omegasl` | +`mesh_triangle_runtime` exercising the runtime form on GLSL. |
| Negative tests | `gte/omegasl/tests/invalid_set_mesh_outputs_*.omegasl` | Four new fixtures: outside-mesh, wrong types, duplicate, exceeds-max. |

Trade-offs / deferred:

- The HLSL body-emission stub does NOT yet auto-emit a locked-to-maxima `SetMeshOutputCounts(...)` — HLSL has no mesh body emission at all (still halts at `supportsStage(Mesh)`). When Phase 2b lands, the body emitter MUST consult `meshHasUserSetMeshOutputsCall` for the same suppression rule, mirroring GLSL.
- Same for MSL Phase 2c: when the `[[mesh]]` parameter is materialized, the `__omegasl_mesh_output_handle` sentinel in `MSLTarget.cpp::tryEmitBuiltinCall` is what gets rewritten to the real handle name. Sema's "at-most-once" guarantee carries over unchanged.
- Sema's literal-bounds check intentionally doesn't const-fold non-literal expressions. A call like `setMeshOutputs(maxV + 1u, 0u)` where `maxV` is a const-folded constant is currently accepted; the device catches it at runtime. Adding a constant-evaluator pass for §2a alone would be over-fitted — it can land alongside the broader §5.1 constant-folding work if it's ever worth the spend.

### Phase 2a — GLSL mesh-stage codegen: **code-complete, build-verified, functional smoke-test passed**

GLSL is now the first backend with live mesh codegen. `omegaslc --glsl` emits `GL_EXT_mesh_shader` source for `mesh` entries, glslc lowers it to SPIR-V 1.4 under the `vulkan1.2` target env, the per-shader SPIR-V validates clean with `spirv-val --target-env vulkan1.2`, and the resulting `.omegasllib` carries each mesh entry's `omegasl_mesh_shader_desc` through the writer (`CodeGen.h`) into the runtime reader (`GE.cpp`) — the serialization format defined-but-dormant in Phase 1 is now exercised live. HLSL and Metal still bail with the original "mesh codegen not yet implemented" diagnostic, exactly as before.

Verified in-session 2026-05-30 with the new `gte/omegasl/tests/mesh_basic.omegasl` fixture (one `triangle`, one `line`, one `point` mesh entry; per-vertex output struct `MeshletVertex internal { float4 pos : Position; float4 color : Color(0); }`):

- `mesh_triangle` emits `layout(max_vertices=3, max_primitives=1, triangles) out;`, routes `verts[i].pos` to `gl_MeshVerticesEXT[i].gl_Position`, `verts[i].color` to `MeshletVertex_color[i]` at `location=1`, and `tris[0]` to `gl_PrimitiveTriangleIndicesEXT[0]`. SPIR-V output validates against Vulkan 1.2.
- `mesh_line` / `mesh_point` exercise the per-topology index builtins (`gl_PrimitiveLineIndicesEXT` / `gl_PrimitivePointIndicesEXT`), the matching `lines` / `points` layout, and `uint2` / `uint` index element widths. Both compile to SPIR-V and validate clean.
- `SetMeshOutputsEXT(<max_vertices>, <max_primitives>)` is emitted as the first statement of every `void main()` — required by the extension before any output array write. MVP locks the active count to the declared maxima; a future `setMeshOutputs(nv, np)` builtin can replace it when the front-end grows the call.
- The `out vertices` / `out indices` parameters are suppressed from `void main()`'s signature (they have no GLSL representation); thread-ID params bridge from `gl_GlobalInvocationID` / `gl_LocalInvocationID` / `gl_WorkGroupID` the same way compute does.
- Regression: `discard.omegasl` (vertex+fragment) and `compute_barriers.omegasl` (compute) still compile + validate; HLSL / MSL still refuse mesh with the precise "not yet implemented" diagnostic.

Files touched (~180 lines):

| Area | File | Change |
|---|---|---|
| Stage gate | `gte/omegasl/src/Target.h`, `GLSLTarget.cpp` | `GLSLTarget::supportsStage(Mesh) -> true`; mesh routing state on `GLSLTarget` (`meshVertsParamName`, `meshIndicesParamName`, `meshVertsStructDecl`, `meshTopology`, `meshMaxVertices`, `meshMaxPrimitives`); `emitIndexExpr` override declared. |
| File ext | `GLSLTarget.cpp` | `.mesh` extension added to `shaderFileExt(Mesh)` (so glslc gets `-fshader-stage=mesh` from the shared derivation). |
| Header emission | `GLSLTarget.cpp::emitShaderEntryHeader` | `#extension GL_EXT_mesh_shader : require`; `layout(local_size_*) in;` + `layout(max_vertices=.., max_primitives=.., triangles\|lines\|points) out;`; per-non-`Position` field arrayed `out` varyings; mesh-output params suppressed from the param loop; thread-ID attribute-bridge identical to compute; `shader_entry.{type, threadgroupDesc, meshDesc.{max_vertices,max_primitives,topology}}` stamped from the AST `MeshDesc`. |
| Body emission | `GLSLTarget.cpp::emitShaderEntryBody` | `SetMeshOutputsEXT(<mv>, <mp>);` emitted before the user body; mesh routing state cleared at body close. |
| Member-expr routing | `GLSLTarget.cpp::emitMemberExpr` | `verts[i].field` writes route to `gl_MeshVerticesEXT[i].gl_Position` (for `Position`) or `<struct>_<field>[i]` (every other field) — the same pattern that handles fragment-output structs, extended to recognize an `INDEX_EXPR` base whose identifier matches the verts param. |
| Index-expr routing | `GLSLTarget.cpp::emitIndexExpr` | New override: `tris[i]` writes route to `gl_Primitive{Triangle,Line,Point}IndicesEXT[i]`; everything else falls through to the default `lhs[idx]`. |
| Toolchain gates | `GLSLTarget.cpp::compileShader`, `compileShaderRuntime` | glslc gets `--target-env=vulkan1.2` for `Mesh` stage; shaderc gets `shaderc_target_env_vulkan_1_2` + `shaderc_spirv_version_1_4` for the same reason (`GL_EXT_mesh_shader` needs SPIR-V 1.4 / `SPV_EXT_mesh_shader`). `shaderc_glsl_mesh_shader` kind mapped. |
| Test fixture | `gte/omegasl/tests/mesh_basic.omegasl` | New: three mesh entries (`triangle` / `line` / `point` topologies) over a shared `internal` per-vertex output struct. Drives the smoke-test described above. |

Trade-offs / deferred:

- ~~`SetMeshOutputsEXT` is locked to the declared maxima because the front-end has no syntax for a runtime active count yet.~~ **Resolved by the Phase 2a follow-up above (`setMeshOutputs(nv, np)` builtin).** When the user provides the call, GLSL lowers it in place and suppresses the auto-emit; when they don't, the auto-emit still fires at maxima for safety. Cross-backend lowering is NOT 1:1 — see **Cross-Backend Differences → Active-count "set outputs" call**: GLSL/HLSL take both counts, MSL takes only the primitive count and infers vertex count.
- Per-primitive attributes (`perprimitiveEXT`) remain Phase-6 (per Open Decision 3). The vertex-output side of the meshlet is covered; the primitive-output side is not. Per-backend native forms are catalogued in **Cross-Backend Differences → Per-primitive output**.
- The mesh vertex output struct is required to be declared `internal` (so its fields can carry semantics like `Position` / `Color(N)`). Sema already enforces "semantic required on internal-struct fields"; that rule carries directly into the mesh routing.
- `topology=point` is accepted by Sema today but HLSL SM 6.5 has no equivalent — see **Cross-Backend Differences → Topology spelling and supported set**. To be revisited (likely rejected in Sema for portability) when Phase 2b lands.

### Phase 1 — OmegaSL mesh-stage front-end (mesh-only): **code-complete, build-verified, functional smoke-test passed**

The language front-end for the `mesh` stage is in (amplification/task stage deferred to Phase 5, per Open Decision 1). The whole `omegaslc` target rebuilds clean with clang — 32 TUs, 0 errors. The functional smoke-test passed in-session 2026-05-30: the canonical meshlet shader parses, passes full Sema (incl. body), and halts cleanly at the codegen stub with a nonzero exit on all three backends (GLSL/HLSL via the shared `supportsStage` stub, Metal via its own), writing no partial source; `line`/`point` topologies are accepted alongside `triangle`; and every negative Sema branch fires its intended diagnostic (extent ≠ maxima, missing `out indices`, wrong index width for topology, non-`void` return, zero maxima, duplicate `out vertices`, non-struct vertex element). Front-end behavior is now verified, not just build-verified. (Codegen output and the serialization round-trip remain unexercised — codegen halts at the stub before any mesh entry serializes — exactly as designed for this checkpoint; first live exercise lands in Phase 2.)

Files touched (~300 lines):

| Area | File | Change |
|---|---|---|
| Lexer | `gte/omegasl/src/Lexer.cpp` | `mesh` is now a hard keyword (`isKeyword`). `vertices`/`indices`/`max_vertices`/`max_primitives`/`topology` stay contextual. |
| AST | `gte/omegasl/src/AST.h` | `AttributedFieldDecl::MeshOutputKind {NotMeshOutput, Vertices, Indices}`. (`ShaderDecl::Mesh` + `MeshDesc` already existed from the enum-scaffolding commit.) |
| Parser | `gte/omegasl/src/Parser.cpp` | `mesh(max_vertices=N, max_primitives=M, topology=T [, x,y,z])` descriptor; `out vertices` / `out indices` contextual qualifiers; parameter array dimensions `T name[N]`. |
| Sema | `gte/omegasl/src/Sema.cpp`, `Sema.h` | `MeshShaderArgument` attribute context (thread-IDs, like compute); mesh must return `void`; structural validation — exactly one `out vertices` (user-struct element, extent == `max_vertices`) and one `out indices` (`uintN` by topology, extent == `max_primitives`), positive maxima. |
| Serialization | `gte/include/omegasl.h`, `gte/omegasl/src/CodeGen.{h,cpp}`, `gte/src/GE.cpp` | `omegasl_mesh_shader_desc` + `omegasl_shader::meshDesc`; `OMEGASL_SHADER_MESH` type mapping; library writer (CodeGen.h) + runtime reader (GE.cpp). Format is defined but **not yet exercised** — mesh stubs at codegen, so nothing serializes a mesh entry until Phase 2. |
| Stage gate | `gte/omegasl/src/Target.h`, `MSLTarget.cpp` | `Target::supportsStage(Mesh)` returns `false` on every backend with a precise "not implemented yet" diagnostic (the clean checkpoint boundary). Runtime feature gating reuses the existing `#requires(MESH_SHADERS)` path — uniform with tessellation; no stage auto-sets the bit. |

Decisions locked (see Open Decisions): mesh-only first; gate via `#requires(MESH_SHADERS)` (no separate compiler flag, no golden tests yet); front-end landed as a reviewable checkpoint ahead of codegen.

---

## Feasibility

All three backends support mesh shaders, and feature detection is already wired up.

| Backend | Extension / API | Detection site | Feature flag set |
|---|---|---|---|
| Metal | `MTLMeshRenderPipelineDescriptor` (Metal 3) | `gte/src/metal/GEMetal.mm:112` | `GTEDEVICE_FEATURE_MESH_SHADER` |
| D3D12 | `D3D12_FEATURE_D3D12_OPTIONS7.MeshShaderTier` (SM 6.5) | `gte/src/d3d12/GED3D12.cpp:133` | `GTEDEVICE_FEATURE_MESH_SHADER` |
| Vulkan | `VK_EXT_mesh_shader` + `meshShader` feature | `gte/src/vulkan/GEVulkan.cpp:300` | `GTEDEVICE_FEATURE_MESH_SHADER` |

Flag: `constexpr uint64_t GTEDEVICE_FEATURE_MESH_SHADER = 1ULL << 1;` at `gte/include/OmegaGTE.h:27`.

### Hardware floor

| Backend | Minimum hardware | Notes |
|---|---|---|
| Metal | Apple7 GPU family (M3, A17+) | Rules out every Intel Mac, M1, M2. macOS 13+, iOS 17+. |
| D3D12 | Mesh Shader Tier 1 — NVIDIA RTX 20-series+, AMD RDNA2+, Intel Arc | SM 6.5 required. |
| Vulkan | Device-advertised `VK_EXT_mesh_shader` | Mature on NVIDIA discrete; driver-dependent on AMD. |

This is a high floor. The runtime gate is mandatory — the API must be present on all targets and throw a descriptive error when the device does not support it.

---

## Current State

### Shader stages in OmegaSL

Supported today (see `gte/docs/OmegaSL-Reference.md:525–529`):

- `vertex`
- `fragment`
- `compute(x, y, z)`
- `hull` (tessellation control)
- `domain` (tessellation evaluation)

Status update: the `mesh` stage **front-end is implemented** (parse + Sema + serialization model; see Implementation Status). `task`/`amplification` remains not implemented (Phase 5). Mesh **codegen** is **live on GLSL** (Phase 2a, see Implementation Status); HLSL (2b) and Metal (2c) still halt cleanly at `supportsStage`.

### Pipeline types in OmegaGTE

Defined in `gte/include/omegaGTE/GEPipeline.h`:

- `RenderPipelineDescriptor` — vertex + fragment
- `ComputePipelineDescriptor` — compute only
- No blit / raytracing / mesh pipeline types yet (blit and raytracing are covered in their own plans)

### Command encoding

Render draws today go through `drawPolygons(...)`-style calls on `GERenderPass` / render encoder. There is no `dispatchMesh`-equivalent.

---

## Proposed Design

### Public API

New descriptor in `gte/include/omegaGTE/GEPipeline.h`:

```cpp
struct MeshPipelineDescriptor {
    SharedHandle<GTEShader> amplificationFunc; // optional — may be null
    SharedHandle<GTEShader> meshFunc;          // required
    SharedHandle<GTEShader> fragmentFunc;      // required

    // Reuse render-pipeline state:
    OmegaCommon::Vector<ColorAttachmentDescriptor> colorAttachments;
    DepthStencilDescriptor                 depthStencil;
    RasterizationState                     rasterState;
    uint32_t                               sampleCount = 1;
};
```

Creation API on `GTEDevice`:

```cpp
SharedHandle<GERenderPipelineState> makeMeshPipelineState(MeshPipelineDescriptor desc);
```

Returns the same handle type as the graphics render pipeline (all three backends model mesh PSOs as a render pipeline variant, not as a separate object type on the public side).

Dispatch on `GECommandBuffer` / render-pass encoder:

```cpp
void drawMeshTasks(uint32_t groupCountX,
                   uint32_t groupCountY,
                   uint32_t groupCountZ);
```

Indirect variant deferred to a follow-up (matches how `drawPolygons` indirect is also deferred in `Pipeline-Completion-Extension-Plan.md`).

### Runtime gating

Both `makeMeshPipelineState` and `drawMeshTasks` throw (or return an error handle — match existing convention) when `!(features.flags & GTEDEVICE_FEATURE_MESH_SHADER)`. Follow the raytracing pattern: no `#ifdef`s in public headers, feature check in method bodies.

---

## OmegaSL Language Extensions

The language layer is the critical path. Backend PSO plumbing is ~1000 lines; OmegaSL is ~2000.

### Syntax

Mirror the existing `compute(x, y, z)` threadgroup-descriptor pattern at `gte/omegasl/src/Parser.cpp:335–427`.

```omegasl
// Amplification (a.k.a. task / object) stage — optional
[in scene]
amplification(x=32, y=1, z=1)
void cullMeshlets(uint3 tid : GlobalThreadID,
                  out payload MeshletPayload p) {
    // decide which meshlets survive, emit p, dispatch child mesh groups
}

// Mesh stage
[in scene]
mesh(max_vertices=64, max_primitives=126, topology=triangle)
void emitMeshlet(uint3 tid : GlobalThreadID,
                 in payload MeshletPayload p,
                 out vertices VertexOut verts[64],
                 out indices  uint3     tris[126]) {
    // compute verts + indices for this meshlet
}
```

### Terminology

Use `amplification` in OmegaSL rather than `task`. Rationale: matches D3D12's name directly; Metal calls it "object" and Vulkan calls it "task," so either pick collides with one backend anyway. D3D12's naming has the clearest published semantics, and the per-backend codegen can translate the keyword.

### Compiler passes

| Pass | File | Change |
|---|---|---|
| Lexer | `gte/omegasl/src/Lexer.cpp`, `Toks.def` | Add `mesh`, `amplification`, `payload`, `vertices`, `indices`, `primitives`, `topology`, `max_vertices`, `max_primitives` keywords. |
| Parser | `gte/omegasl/src/Parser.cpp` | Parse `mesh(max_vertices=N, max_primitives=M, topology=T)` and `amplification(x,y,z)` descriptors. Parse `in payload` / `out payload`, `out vertices`, `out indices`. |
| Sema | `gte/omegasl/src/Sema.cpp` | Validate: payload struct type matches between amp and mesh; `max_vertices`/`max_primitives` within backend limits; topology enum; output array sizes match declared maxima. |
| HLSL codegen | `gte/omegasl/src/HLSLTarget.cpp` | Emit SM 6.5 mesh/amplification shaders with `[numthreads(...)]`, `[outputtopology("triangle")]`, `vertices out`, `indices out`, `payload` attributes. |
| MSL codegen | `gte/omegasl/src/MetalTarget.cpp` | Emit `[[mesh]]` / `[[object]]` functions using the `mesh<V, I, N_v, N_p, topology>` template. |
| GLSL codegen | `gte/omegasl/src/GLSLTarget.cpp` | Emit `#extension GL_EXT_mesh_shader : require`, `layout(local_size_x=..) in;`, `layout(max_vertices=.., max_primitives=.., triangles) out;`, `taskPayloadSharedEXT` / `EmitMeshTasksEXT`. |

### Deferred to v2

- Per-primitive attributes (HLSL `primitives`, Vulkan `perprimitiveEXT`). Require extra OmegaSL syntax and extra validation. Not needed for a meaningful mesh-shader MVP.
- Derivatives in mesh shaders.
- View-instancing through mesh shaders.

---

## Backend Implementation

### Metal — `gte/src/metal/`

- Pipeline creation: use `MTLMeshRenderPipelineDescriptor`. Set `.objectFunction` from `amplificationFunc` (if present), `.meshFunction`, `.fragmentFunction`. Color attachments map 1:1.
- Dispatch: `[renderEncoder drawMeshThreadgroups:MTLSizeMake(x,y,z) threadsPerObjectThreadgroup:... threadsPerMeshThreadgroup:...]`. Threadgroup sizes come from the shader's declared dimensions (stored on the pipeline at creation time).
- Estimate: ~250 lines.

### D3D12 — `gte/src/d3d12/`

- Pipeline creation: use `CD3DX12_PIPELINE_MESH_STATE_STREAM` (already present at `gte/src/d3d12/d3dx12.h:2643`). Populate `AS` (amplification) and `MS` (mesh) bytecode. Fragment shader fills the `PS` slot as usual.
- Dispatch: `commandList->DispatchMesh(x, y, z)`. Command list must be a graphics command list; state is set via `SetPipelineState`.
- Root signature: reuse existing graphics root-sig builder; mesh shaders bind through the same mechanism.
- Estimate: ~400 lines.

### Vulkan — `gte/src/vulkan/`

- Pipeline creation: standard `VkGraphicsPipeline` with shader stages `VK_SHADER_STAGE_MESH_BIT_EXT` and optionally `VK_SHADER_STAGE_TASK_BIT_EXT`, replacing the vertex stage. `pVertexInputState` / `pInputAssemblyState` are ignored by the spec when mesh stages are present, but pass well-formed empty structs.
- Dispatch: `vkCmdDrawMeshTasksEXT(cmdBuf, x, y, z)`. Load the function pointer from `VK_EXT_mesh_shader` at device init.
- Device creation: enable `VK_EXT_mesh_shader` and chain `VkPhysicalDeviceMeshShaderFeaturesEXT` into `pNext` when the feature is detected (the detection at `GEVulkan.cpp:300` already queries this).
- Estimate: ~300 lines.

---

## Cross-Backend Differences (impacts 2a follow-ups + 2b/2c design)

The three target languages diverge on more than spelling. Calling these out early so the Phase 2a follow-up (`setMeshOutputs(nv, np)` builtin) and Phase 2b/2c codegen don't have to rediscover them.

### Active-count "set outputs" call

The MVP locks the count to the declared maxima (Phase 2a Trade-off note). The natural follow-up is an OmegaSL builtin like `setMeshOutputs(nv, np)`. It does NOT lower 1:1 on every backend:

| Backend | Native call | Shape | Notes |
|---|---|---|---|
| GLSL (`GL_EXT_mesh_shader`) | `SetMeshOutputsEXT(uint nv, uint np)` | both counts together | mandatory before any output-array write |
| HLSL (SM 6.5) | `SetMeshOutputCounts(uint nv, uint np)` | both counts together | mandatory before any output-array write |
| MSL (Metal 3) | `mesh.set_primitive_count(uint np)` | **primitive count only** | vertex count is implicit — derived from the highest `set_vertex(i, …)` slot touched |

**Consequence for the builtin (landed in Phase 2a follow-up — see Implementation Status):** `setMeshOutputs(nv, np)` lowers to `SetMeshOutputsEXT(nv, np)` on GLSL (live), to `SetMeshOutputCounts(nv, np)` on HLSL (wired, dormant until 2b), and to `<mesh_handle>.set_primitive_count(np)` on Metal (wired, dormant until 2c) — the `nv` argument is dropped on MSL by design because vertex count there is implicit. The MSL handle name is currently a sentinel (`__omegasl_mesh_output_handle`) that 2c rewrites when it materializes the `[[mesh]]` parameter.

### Output access pattern

| Backend | Vertex write | Index write |
|---|---|---|
| GLSL | `gl_MeshVerticesEXT[i].gl_Position = …;` + per-field arrayed `out` varying `<struct>_<field>[i] = …;` | `gl_Primitive{Triangle,Line,Point}IndicesEXT[i] = uvec{3,2,1}(…);` |
| HLSL | direct array index on the `out vertices` param: `verts[i].field = …;` (Position via `SV_Position`) | direct array index on the `out indices` param: `tris[i] = uint{3,2}(…);` |
| MSL | accessor calls on the `mesh<…>` handle: `mesh.set_vertex(i, vertStruct);` (whole struct at once) | accessor calls: `mesh.set_index(slot, vertexIdx)` — *exact signature TBD at Phase 2c; Metal documents both "one index per slot" and per-primitive forms across spec revs; pin this against the targeted MSL version before emitting* |

Practical impact on the front-end: today's OmegaSL syntax `verts[i].field = expr` reads naturally on HLSL (literal lowering) and GLSL (the per-field arrayed varying we already emit). MSL is the outlier — it wants a whole-struct `set_vertex(i, S)` call. Phase 2c codegen will need to either (a) accumulate the user's per-field writes into a temp and flush once at end-of-iteration, or (b) require the user to write through a temp struct themselves. Worth deciding before 2c emission lands; the front-end will not change shape regardless.

### Topology spelling and supported set

| Backend | `triangle` | `line` | `point` |
|---|---|---|---|
| GLSL | `triangles` in `layout(...) out;` | `lines` | `points` |
| HLSL | `[outputtopology("triangle")]` | `[outputtopology("line")]` | **not supported at SM 6.5** — point mesh output is a Vulkan/Metal capability, not a D3D12 one |
| MSL | `metal::topology::triangle` template arg | `metal::topology::line` | `metal::topology::point` |

**Action item ([[feedback_frontend_backend_uniformity]]):** OmegaSL currently accepts `topology=point` (and Sema does not reject it). On HLSL there is no lowering. When Phase 2b lands, the cleanest fix is to reject `topology=point` in Sema for portability — matching how we already require front-end-uniform constructs — unless we discover SM 6.6+ added point output that we can target instead. Flag this when 2b emission goes in; do not silently emit a non-`point` topology on HLSL.

### Threadgroup-size limits

| Backend | Per-spec max threads/group on mesh stage |
|---|---|
| GLSL | implementation-defined; Vulkan exposes via `maxMeshWorkGroupInvocations` (commonly 128 on NV, 256 on AMD/Intel) |
| HLSL (SM 6.5) | 128 (hard cap in the spec) |
| MSL | implementation-defined; check `MTLDevice.maxThreadsPerThreadgroup` |

Implication: portable mesh shaders should keep `local_size_x * y * z ≤ 128`. Worth a Sema-level advisory warning (not an error) when the declared threadgroup exceeds 128 on a mesh stage — a future polish item.

### Per-primitive output

All three backends have a separate per-primitive output channel (HLSL `out primitives`, Metal `set_primitive`, GLSL `perprimitiveEXT`). OmegaSL has no syntax for it yet (deferred to Phase 6 per Open Decision 3). Mentioning it here so the Phase 6 plan can lift these per-backend forms straight in.

---

## Phasing

Revised into reviewable increments. Phase 1's front-end is in; the rest follows. The original "stages + 3 codegens + tests" Phase 1 is split so the front-end could land and be reviewed as a checkpoint (front-end = Phase 1, per-backend codegen = Phase 2). Amplification is pulled out into its own additive Phase 5 (Open Decision 1).

### Phase 1 — OmegaSL mesh-stage front-end (mesh-only) — *code-complete, build-verified, smoke-test passed*

Lexer, parser, Sema, AST, serialization data-model + writer/reader, `supportsStage` stubs, `#requires` gating. See **Implementation Status** for the file-by-file breakdown.

- [x] **Functional smoke-test** — *passed 2026-05-30*. Ran `omegaslc -S -t <tmpdir> <file.omegasl>` (note: `-t`/temp-dir is mandatory; `-S` = emit-source-only so no glslc/dxc/metal is invoked):
  - canonical meshlet shader → parses, passes Sema, halts with the `supportsStage` "mesh codegen not yet implemented" message + nonzero exit — confirmed on GLSL, HLSL, and Metal source-only, with no partial source written;
  - negatives each fire the intended Sema diagnostic: `out vertices` extent ≠ `max_vertices`; missing `out indices`; index element width wrong for topology (e.g. `uint2` under `topology=triangle`); non-`void` return. Also confirmed beyond the original list: zero maxima, duplicate `out vertices`, non-struct vertex element, and `line`/`point` topology acceptance.
  - Note (pre-existing, non-mesh): each rejected global decl also prints a generic `unexpected token: Failed to evaluate statement` cascade line (`Parser.cpp` "stop at first failed global decl" sentinel) after the precise diagnostic. Not introduced by the mesh work; reproduces on existing `invalid_*` tests.

### Phase 2 — OmegaSL mesh-stage codegen (per backend)

The bulk of the remaining language work (~1000+ lines). Each backend emits mesh source from the same AST/`MeshDesc` and flips its own `supportsStage(Mesh)` to `true`. Land in verifiability order:

- **2a — GLSL** *(verified on this Linux host via `glslc` + `spirv-val`)* — **DONE 2026-05-30**: `#extension GL_EXT_mesh_shader : require`, `layout(local_size_x=..) in;`, `layout(max_vertices=.., max_primitives=.., triangles|lines|points) out;`, `SetMeshOutputsEXT(...)`, `gl_MeshVerticesEXT[]` + `gl_PrimitiveTriangleIndicesEXT[]` (and line/point variants), per-non-`Position` field arrayed `out` varyings, mesh-output params suppressed from the entry signature. Serialization writer + runtime reader exercised live for the first time via the `mesh_basic.omegasl` test fixture. glslc / shaderc are pinned to Vulkan 1.2 / SPIR-V 1.4 on mesh stage so `SPV_EXT_mesh_shader` lights up. See **Implementation Status → Phase 2a** above.
- **2b — HLSL** *(text emission verifiable; DXC→DXIL compile is **off-platform / unverified on Linux**)*: SM 6.5, `[numthreads(...)]`, `[outputtopology("triangle")]`, `out vertices`/`out indices` params, `SetMeshOutputCounts(...)`.
- **2c — MSL** *(**off-platform / unverified** — no Metal toolchain here)*: `[[mesh]]` function using the `mesh<V, I, N_v, N_p, topology>` handle.
- [ ] Golden-file round-trip tests: compile the canonical meshlet shader to HLSL/MSL/GLSL and diff against checked-in goldens (the original Phase-1 test intent).
- [ ] **Per project convention** ([[feedback_mark_unverified_backends_in_plan]]): when 2b/2c land, record HLSL/DXC and MSL/Metal as compile/run-unverified off-platform as a callout here, not just in chat.

### Phase 3 — OmegaGTE public API + runtime surface (~150 lines)

`MeshPipelineDescriptor`, `GTEDevice::makeMeshPipelineState(...)`, `GECommandBuffer::drawMeshTasks(x,y,z)`. Feature-gated behind `GTEDEVICE_FEATURE_MESH_SHADER` (no `#ifdef`s in public headers; check in method bodies, per the raytracing pattern). All three backends throw a descriptive "not implemented" first to lock the surface. *(= original plan's Phase 2.)*

### Phase 4 — Backend pipeline + dispatch implementations

Per-backend PSO creation + dispatch, each gated on its own feature check, verified with a meshlet demo:

- **4a — Vulkan** *(verifiable here)*: `VkGraphicsPipeline` with `VK_SHADER_STAGE_MESH_BIT_EXT`; `vkCmdDrawMeshTasksEXT`; enable `VK_EXT_mesh_shader` + chain `VkPhysicalDeviceMeshShaderFeaturesEXT` at device init.
- **4b — D3D12** *(off-platform)*: `CD3DX12_PIPELINE_MESH_STATE_STREAM` (populate `AS`/`MS`/`PS`), `commandList->DispatchMesh(x,y,z)`.
- **4c — Metal** *(off-platform)*: `MTLMeshRenderPipelineDescriptor`, `drawMeshThreadgroups:...`.

### Phase 5 — Amplification / task stage (additive)

The deferred payload machinery: `amplification(x,y,z)` descriptor, `in payload` / `out payload` params, payload-type matching between amp & mesh in Sema, dispatch-children (`EmitMeshTasksEXT` GLSL / `DispatchMesh` HLSL / object stage MSL), and the pipeline's optional amplification slot. A new `OMEGASL_SHADER_AMPLIFICATION` enum appends at the tail (the enum/serialization were laid out to allow this). Touches every layer again but is purely additive on top of Phases 1–4.

### Phase 6 — Follow-ups (separate plan)

Indirect mesh dispatch (`drawMeshTasksIndirect`), per-primitive attributes (HLSL `primitives` / Vulkan `perprimitiveEXT`), derivatives/view-instancing in mesh shaders, and GEMesh integration (automatic meshlet generation feeding the pipeline).

---

## Open Decisions (STICK to Recommendation for all)

1. **Land `amplification` with `mesh` in phase 1, or mesh-only first?** **Resolved → mesh-only.** Phase 1's front-end landed mesh-only; amplification is now its own additive Phase 5. (Recommendation rationale: all three backends allow a mesh shader without an amplification stage, and the payload-type machinery is the most error-prone piece of the language work.)

   **Also resolved (this pass):** runtime gating uses the existing `#requires(MESH_SHADERS)` mechanism rather than a separate compiler feature flag, and golden-file tests are deferred to Phase 2 — keeping mesh gating uniform with tessellation.


2. **Metal support policy.** The Apple7 floor excludes every Mac currently in common use. Options: (a) implement Metal anyway and let users opt in on M3+; (b) report `GTEDEVICE_FEATURE_MESH_SHADER` as false on Metal and ship D3D12/Vulkan only in v1. Recommendation: (a). The detection is already there, the hardware exists, and we keep the cross-backend surface symmetric.

3. **Per-primitive attributes** — confirm deferral to v2.

4. **`GEMesh` integration** — should the existing mesh representation feed mesh shaders directly (automatic meshlet generation), or is that a separate effort on top of this plan? Recommendation: separate effort. This plan delivers the raw pipeline; the higher-level "draw a GEMesh via mesh shaders" wrapper can come later.


---

## Rough Effort

| Component | Estimate |
|---|---|
| OmegaSL mesh/amplification stages (lexer, parser, sema, 3 codegens, tests) | ~2000 lines |
| `MeshPipelineDescriptor` + `drawMeshTasks` public API | ~150 lines |
| Vulkan backend | ~300 lines |
| D3D12 backend | ~400 lines |
| Metal backend | ~250 lines |
| Tests, demo, docs | ~500 lines |
| **Total** | **~3600 lines** |

Solo: 4–6 weeks. Two-developer split (OmegaSL ‖ backends): 2–3 weeks.
