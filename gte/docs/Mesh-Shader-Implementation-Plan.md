# Mesh Shader Implementation Plan

## Goal

Add mesh shader support to OmegaGTE as a new pipeline type, with matching `mesh` and `amplification` stages in OmegaSL. The feature will be runtime-gated behind `GTEDEVICE_FEATURE_MESH_SHADER`, following the pattern established by the raytracing and pipeline-completion plans.

Cross-platform parity is the acceptance bar: code written against the public API compiles and runs on D3D12, Metal, and Vulkan, or fails cleanly at descriptor validation when the device lacks support.

---

## Implementation Status

> Updated 2026-05-30. Tracks against **Phasing** below.

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

Status update: the `mesh` stage **front-end is now implemented** (parse + Sema + serialization model; see Implementation Status). `task`/`amplification` remains not implemented (Phase 5). Mesh **codegen** on all three backends is still pending (Phase 2) — a `mesh` shader currently parses and type-checks, then halts cleanly at `supportsStage`.

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
| HLSL codegen | `gte/omegasl/src/HLSLCodeGen.cpp` | Emit SM 6.5 mesh/amplification shaders with `[numthreads(...)]`, `[outputtopology("triangle")]`, `vertices out`, `indices out`, `payload` attributes. |
| MSL codegen | `gte/omegasl/src/MetalCodeGen.cpp` | Emit `[[mesh]]` / `[[object]]` functions using the `mesh<V, I, N_v, N_p, topology>` template. |
| GLSL codegen | `gte/omegasl/src/GLSLCodeGen.cpp` | Emit `#extension GL_EXT_mesh_shader : require`, `layout(local_size_x=..) in;`, `layout(max_vertices=.., max_primitives=.., triangles) out;`, `taskPayloadSharedEXT` / `EmitMeshTasksEXT`. |

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

- **2a — GLSL** *(verifiable on this Linux host via `glslc`)*: `#extension GL_EXT_mesh_shader : require`, `layout(local_size_x=..) in;`, `layout(max_vertices=.., max_primitives=.., triangles|lines|points) out;`, `SetMeshOutputsEXT(...)`, `gl_MeshVerticesEXT[]` + `gl_PrimitiveTriangleIndicesEXT[]` (or line/point variants). Wire the serialization writer/reader live (currently dormant).
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
