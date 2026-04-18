# Mesh Shader Implementation Plan

## Goal

Add mesh shader support to OmegaGTE as a new pipeline type, with matching `mesh` and `amplification` stages in OmegaSL. The feature will be runtime-gated behind `GTEDEVICE_FEATURE_MESH_SHADER`, following the pattern established by the raytracing and pipeline-completion plans.

Cross-platform parity is the acceptance bar: code written against the public API compiles and runs on D3D12, Metal, and Vulkan, or fails cleanly at descriptor validation when the device lacks support.

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

Not implemented (explicitly noted at `OmegaSL-Reference.md:579`): `mesh`, `task`/`amplification`.

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
    std::vector<ColorAttachmentDescriptor> colorAttachments;
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

1. **OmegaSL mesh+amplification stages** — lexer, parser, sema, three codegens. Land behind a compiler feature flag so the rest of the compiler is unaffected until ready. Includes round-trip tests that compile a canonical meshlet shader to HLSL/MSL/GLSL and diff against golden files.
2. **`MeshPipelineDescriptor` + `makeMeshPipelineState` + `drawMeshTasks` public API**, with all three backends throwing "not implemented" at first. This locks the surface.
3. **Backend implementations** — in this order: Vulkan (least boilerplate), D3D12 (most mature tooling), Metal (smallest user base, validate last). Each backend gated on its own `features` flag check; verified via a simple meshlet demo.
4. **Follow-up (separate plan)**: indirect mesh dispatch, per-primitive attributes, integration with GEMesh / triangulation data for automatic meshlet generation.

---

## Open Decisions

1. **Land `amplification` with `mesh` in phase 1, or mesh-only first?** Recommendation: mesh-only. All three backends allow a mesh shader without an amplification stage, and the payload-type machinery is the most error-prone piece of the language work. Landing mesh-only first gets the pipeline plumbing exercised end-to-end, then amplification is an additive change.
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
