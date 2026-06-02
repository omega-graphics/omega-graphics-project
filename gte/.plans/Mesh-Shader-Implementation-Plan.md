# Mesh Shader Implementation Plan

## Goal

Add mesh shader support to OmegaGTE as a new pipeline type, with matching `mesh` and `amplification` stages in OmegaSL. The feature will be runtime-gated behind `GTEDEVICE_FEATURE_MESH_SHADER`, following the pattern established by the raytracing and pipeline-completion plans.

Cross-platform parity is the acceptance bar: code written against the public API compiles and runs on D3D12, Metal, and Vulkan, or fails cleanly at descriptor validation when the device lacks support.

---

## Implementation Status

> Updated 2026-05-30. Tracks against **Phasing** below.

### Phase 4c.5 — Metal mesh-stage resource binding: **code-complete, build-verified, end-to-end headless integration test (Phase A no-input + Phase B buffer-bound) passing on Apple M4 Max**

Metal's mesh PSO can now reach resources bound through the public `bindResourceAt*Shader` family, so the same call sites work on D3D12 (via unified root signatures) and Metal (via the new `isMesh` routing). The four bind methods that take `vertexShader`-slot resources (`bindResourceAtVertexShader` × {GEBuffer, GETexture, GESamplerState} + `setRenderConstants`) all branch on `renderPipelineState->isMesh` and dispatch to `setMeshBuffer:` / `setMeshTexture:` / `setMeshSamplerState:` / `setMeshBytes:` instead of the vertex-stage equivalents, with `MTLRenderStageMesh` fences on the buffer + texture paths so the GPU's hazard tracker tags the wait/signal at the right stage boundary. Fragment-shader bind methods are unaffected — fragment is fragment regardless of upstream geometry stage.

End-to-end pipeline (Phase B): CPU-built buffer of `{float4 pos, float4 color}` × 3 (solid green) → `makeBuffer(Upload, 96B, 32B stride)` → `bindResourceAtVertexShader(buf, 0)` → routes to `setMeshBuffer:0 atIndex:0` via the new isMesh dispatch → mesh shader reads `vertBuf[i].pos` / `vertBuf[i].color` → emits a triangle covering the 8×8 viewport → fragment passes through → texture readback center pixel reads `(0, 204, 0, 255)`, matching the bound color exactly (within tolerance) and ruling out a silent bind failure.

Verified in-session 2026-05-30:

- `gte/tests/mesh_shader_test.cpp` extended with Phase B alongside the existing Phase A baseline. Both phases pass on Apple M4 Max via the shared `omegagte_mesh_shader` CTest target. Phase A center pixel = `(108, 94, 54, 255)` (R/G/B interpolation, unchanged from 4c.4); Phase B center pixel = `(0, 204, 0, 255)` (solid green from bound buffer, distinct from Phase A's color profile so a silent bind failure would show up).
- Full `ctest -j4` over `gte/tests` (9 tests) and `gte/omegasl/tests` (99 tests): 100% pass. The 4c.5.6 MSL codegen fix (see Files touched) is the most invasive change here — it touches every shader's `paramIndex` accounting — and the OmegaSL test suite passing confirms it doesn't regress non-mesh shaders.

Files touched (~80 lines, scoped to Metal command-buffer + MSL codegen fix + extended test):

| Area | File | Change |
|---|---|---|
| Buffer bind routing | `gte/src/metal/GEMetalCommandQueue.mm::bindResourceAtVertexShader(GEBuffer)` | Branch on `renderPipelineState->isMesh`: `setMeshBuffer:offset:atIndex:` + `MTLRenderStageMesh` fences when mesh, existing `setVertexBuffer:` + `MTLRenderStageVertex` path otherwise. Shader-info reads (`vertexShader->internal`) unchanged — the mesh shader sits in that slot via the 4c.1 slot-doubling. NSLog label updated to reflect routed stage. |
| Texture bind routing | `gte/src/metal/GEMetalCommandQueue.mm::bindResourceAtVertexShader(GETexture, swizzle)` | Same shape: `setMeshTexture:atIndex:` + `MTLRenderStageMesh` fences. Swizzle-view resolution (`getOrCreateSwizzledView`) is stage-independent so unchanged. |
| Sampler bind routing | `gte/src/metal/GEMetalCommandQueue.mm::bindResourceAtVertexShader(GESamplerState)` | Same routing on the selector; samplers don't ride a fence so no stage variable needed. |
| Push-constant routing | `gte/src/metal/GEMetalCommandQueue.mm::setRenderConstants` | The vertex-slot branch dispatches to `setMeshBytes:length:atIndex:` when `isMesh`. Fragment branch unchanged. |
| **MSL codegen fix (4c.5.6)** | `gte/omegasl/src/MSLTarget.cpp` | **Latent bug from Phase 2c surfaced by 4c.5.5 verification.** `emitResourcesAndFillLayout` (`CodeGen.cpp:367`) calls `target->resetForNextShader()` which was setting `paramIndex = 0`, wiping the bump the mesh-handle preamble made for `__omegasl_mesh_output_handle`. Symptom: first non-static resource didn't emit its leading comma, producing `__omegasl_mesh_output_handleconstant MeshVertexIn * vertBuf...`. Fix: hoist `paramIndex = 0` out of `resetForNextShader` and into the top of `emitShaderEntryHeader` (before the mesh-handle bump), with an explanatory comment in both places so the next maintainer doesn't accidentally re-add the reset. Stage-agnostic — non-mesh shaders see paramIndex reset at the same logical moment as before. The bug was latent because no Phase 2c test had a mesh shader with `buffer<T>` / `texture*` resources; this Phase 4c.5 test was the first to exercise that combination through MSL. |
| Verification | `gte/tests/mesh_shader_test.cpp` | Restructured into a shared `runPhaseA` + `runPhaseB` + driver-style entry point (matching `sampler_bind_test.cpp`'s shape). Phase A is the unchanged 4c.4 baseline (no-input meshlet). Phase B builds a CPU-side `GEBufferWriter`-driven buffer of `{float4 pos, float4 color}` × 3 (solid green), binds it via `bindResourceAtVertexShader(buf, 0)`, and asserts the readback center pixel is approximately the bound color (G ≈ 200, R/B ≈ 0). |

Trade-offs / deferred under 4c.5:

- **Object-stage binding (`MTLRenderStageObject`, `setObjectBuffer:`/`setObjectTexture:`/`setObjectSamplerState:`/`setObjectBytes:`) deferred to Phase 5.** Today the amplification stage is hard-stopped at `makeMeshPipelineState`, so any object-stage binding API is unreachable. The pattern lands as 4c.5.1–4c.5.4 verbatim with the `Object` selectors when Phase 5 wires amplification.
- **No new `bindResourceAtMeshShader` public method.** Routing inside the existing `bindResourceAtVertexShader` keeps the user-facing call site identical to D3D12 (which gets mesh-stage binding "for free" via unified root signatures). Adding a separate method would diverge the public API on Metal only.
- **`#requires(MESH_SHADERS)` in runtime-compiled source remains broken** as flagged in 4c trade-offs. Phase B inherits the same source-string-without-`#requires` workaround. *(Resolved — runtime now runs the OmegaSL preprocessor, rejecting `#include` since runtime source has no file-system context.)*

### Phase 4c — Metal mesh PSO + dispatch: **code-complete, build-verified, end-to-end headless integration test passing on Apple M4 Max**

Metal is now the second backend with a live mesh pipeline (after Phase 4b D3D12), and the **first verifiable directly on this host** — Metal's toolchain ships in the macOS SDK, so the `metal` compile of the MSL source emitted in Phase 2c is exercised end-to-end here for the first time, closing the Phase 2c off-platform-unverified callout. The Phase-3 stubs in `GEMetalEngine::makeMeshPipelineState` and `GEMetalCommandBuffer::drawMeshTasks` were swapped for real implementations: a `MTLMeshRenderPipelineDescriptor` PSO build (via `[device newRenderPipelineStateWithMeshDescriptor:options:reflection:error:]`) and a `[rp drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:]` issue. `GEMetalRenderPipelineState` carries a new `isMesh` flag stamped at construction so the command-buffer side can assert the right pipeline kind without changing the public-API handle type.

End-to-end pipeline (OmegaSL mesh-output struct → MSL codegen via Phase 2c → `metal` toolchain → `.metallib` → `[[mesh]]` function → `MTLMeshRenderPipelineDescriptor` → `newRenderPipelineStateWithMeshDescriptor:` → `setRenderPipelineState` → `drawMeshThreadgroups:(1,1,1) threadsPerMeshThreadgroup:(1,1,1)` → rasterizer → fragment → offscreen texture → CPU readback) was verified in-session 2026-05-30 with a new shared `gte/tests/mesh_shader_test.cpp` wired into `gte/tests/metal/CMakeLists.txt`. The 8×8 RGBA8 target's center pixel reads `(108, 94, 54, 255)` — all three R/G/B vertex contributions present and interpolated, confirming the meshlet emitted a real triangle that the rasterizer covered correctly. First time every layer was exercised in sequence on Metal and the output matched expectation.

Verified in-session 2026-05-30:

- `gte/tests/mesh_shader_test.cpp` (new): `cmake --build` + `ninja MeshShaderTest` succeeds on macOS via cmake+ninja+clang. Runtime stdout confirms `GTEDEVICE_FEATURE_MESH_SHADER = YES` and `makeMeshPipelineState -> live PSO`. The Phase-3 "not yet implemented" `DEBUG_STREAM` lines no longer fire. The readback `(108, 94, 54, 255)` pixel passes the "all three vertex contributions present, alpha not zeroed" PASS criterion encoded in the test.
- Full `ctest -j4` over `gte/tests` (9 tests including the new mesh test): 100% pass on Apple M4 Max. The other 8 tests (`std140_layout`, `sampler_validation`, `sampler_bind`, `sampler_bind_negative`, `matrix_ops`, `bitfield_ops`, `int_vector_io`, `push_constant`) continue to render correctly — the graphics PSO path and existing draw paths are byte-identical (only the mesh-variant constructor and mesh-dispatch body were added; the graphics constructor / `makeRenderPipelineState` / `drawPolygons` family are untouched).
- MSL codegen from Phase 2c is exercised live: the test's mesh shader compiles via the `metal` runtime path on Apple M4 Max with no warnings, validating the `[[mesh]]` function decorator + `using __omegasl_mesh_t_<name> = mesh<...>` type alias + per-vertex `set_vertex` flush loop + `set_index` per-slot expansion shape emitted by Phase 2c.

Files touched (~340 lines, scoped to the Metal backend + new shared test fixture + plan doc):

| Area | File | Change |
|---|---|---|
| PSO variant flag | `gte/src/metal/GEMetalPipeline.{h,mm}` | Added `bool isMesh = false;` to `GEMetalRenderPipelineState` + a second constructor (mesh-variant) that stamps the flag. The mesh shader handle goes into the existing `vertexShader` base slot — both stage types are `SharedHandle<GTEShader>` and the per-shader-info reads go through `shader->internal` uniformly, so the slot doubles cleanly (matching the Phase 4b D3D12 slot-doubling pattern). |
| Engine PSO build | `gte/src/metal/GEMetal.mm::GEMetalEngine::makeMeshPipelineState` | Phase-3 stub replaced with a real `MTLMeshRenderPipelineDescriptor` build. Subobjects populated field-for-field with the graphics PSO above: `meshFunction` + `fragmentFunction`, color-attachment pixel formats + per-target blend (identical loop), `rasterSampleCount`. Depth-stencil **state object** built via `[device newDepthStencilStateWithDescriptor:]` and stored on `GEMetalRenderPipelineState` so the existing `setRenderPipelineState` applies it at encode time; raster state (cull / fill / winding / depth-bias) captured in `GEMetalRasterizerState` the same way. `[device newRenderPipelineStateWithMeshDescriptor:options:reflection:error:]` is the Metal 13+ entry point — already in scope on the cached MTLDevice. Amplification stage rejected up front with a precise "Phase 5" diagnostic. |
| Command buffer dispatch | `gte/src/metal/GEMetalCommandQueue.mm::GEMetalCommandBuffer::drawMeshTasks` | Phase-3 stub replaced. Adds three asserts (`rp != nil`, `renderPipelineState != nullptr`, `renderPipelineState->isMesh`) then calls `[rp drawMeshThreadgroups:MTLSizeMake(x,y,z) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(tg.x,tg.y,tg.z)]`. Per-meshlet threadgroup dims come from `renderPipelineState->vertexShader->internal.threadgroupDesc` — same field compute uses for `dispatchThreadgroups`, populated by the MSL codegen in Phase 2c. `threadsPerObjectThreadgroup=(1,1,1)` because no amplification stage is bound (Metal SDK docs that arg as "ignored if object shader is not present"); when Phase 5 wires amplification, this becomes the object-stage `[numthreads(...)]`. |
| Verification test | `gte/tests/mesh_shader_test.cpp` (new) | Backend-independent headless integration test — renders a no-input meshlet (single hardcoded NDC triangle, pure R/G/B vertex colors) into an 8×8 RGBA8 offscreen texture and asserts the center pixel carries all three channel contributions. Lives at the shared `gte/tests/` level so other backends pick it up when their Phase 4 lands. Today only Metal's CMakeLists wires it in (D3D12 has its own AppKit-free `MeshAndRaytracingTest`; Vulkan's Phase 4a is pending). Source string drops the `#requires(MESH_SHADERS)` directive because the runtime `OmegaSLCompiler` doesn't yet wire the preprocessor stage that offline `omegaslc` does — the C++ feature check via `enumerateDevices` covers device gating before reaching the runtime compile step. |
| Test wiring | `gte/tests/metal/CMakeLists.txt` | New `MeshShaderTest` CLI test + `omegagte_mesh_shader` CTest registration; same `add_metal_cli_test` shape as the other shared tests. |

Trade-offs / deferred:

- **Mesh-stage resource binding (`bindResourceAt*Shader` → `setMeshBuffer:` / `setMeshTexture:` / `setMeshSamplerState:`) remains a follow-up.** D3D12 gets resource binding "for free" on the mesh slot because root signatures are unified across stages and `bindResourceAtVertexShader` already reaches the right resource table. Metal has separate per-stage binding tables (`setVertexBuffer:`, `setFragmentBuffer:`, `setMeshBuffer:`, `setObjectBuffer:`) so the same routing has to dispatch on `isMesh` inside the bind methods. The Phase 4c verification uses a no-input meshlet to side-step this gap; landing the resource-binding follow-up unlocks the FBX path the D3D12 `MeshAndRaytracingTest` already exercises. Sized as its own ~150-line landing because it touches the buffer / texture / sampler bind methods symmetrically.
- **Amplification stage is hard-stopped** at `makeMeshPipelineState` for the same Phase-5 reason D3D12 uses, with the same precise diagnostic. The `objectFunction` property on `MTLMeshRenderPipelineDescriptor` is never assigned today; `threadsPerObjectThreadgroup` is fixed at `(1,1,1)` because Metal documents the argument as ignored without an object shader.
- **`#requires` in runtime-compiled source.** The runtime `OmegaSLCompiler` doesn't yet wire the preprocessor stage (offline `omegaslc` does — see `gte/omegasl/src/main.cpp:210`). A `#requires(MESH_SHADERS)` directive in a runtime-compiled source string is currently passed through to the parser as an unrecognized token and quietly fails the parse with no diagnostic emitted. Out of scope for this Phase 4c landing — the C++ side already checks the feature flag via `enumerateDevices` before reaching the runtime compile; tracked as a separate runtime-preprocessor wiring issue.
- **Per-pipeline threadgroup hints (`maxTotalThreadsPerMeshThreadgroup`, `meshThreadgroupSizeIsMultipleOfThreadExecutionWidth`) are left at defaults.** The MSL codegen already bakes `[[max_total_threads_per_threadgroup(N)]]` into the compiled mesh function, so the Metal optimizer reads N from there. Revisit if profiling shows the hint changes scheduling on Apple GPUs.
- **`depthAttachmentPixelFormat` / `stencilAttachmentPixelFormat` left at default `MTLPixelFormatInvalid`** when depth is disabled — matches the existing graphics `makeRenderPipelineState`. When depth-enabled passes need a real format, the same fix lands in both call sites at once.
- **Cross-Apple-Silicon verification.** Verified on Apple M4 Max. M3 / A17+ (the Apple7+ floor for `MTLMeshRenderPipelineDescriptor`) should work identically — Metal 3 is the spec floor and the API surface is stable across the Apple7 generation. Flag any per-generation surprises if/when they surface.

### Phase 4b — D3D12 mesh PSO + dispatch: **code-complete, build-verified, end-to-end visual test passing on RTX 2080 Ti**

D3D12 is the first backend with a live mesh pipeline. The Phase-3 stubs in `GED3D12Engine::makeMeshPipelineState` and `GED3D12CommandBuffer::drawMeshTasks` were swapped for real implementations: a `CD3DX12_PIPELINE_MESH_STATE_STREAM` PSO build (via `ID3D12Device8::CreatePipelineState`) and a `commandList->DispatchMesh(x, y, z)` issue. `GED3D12RenderPipelineState` carries a new `isMesh` flag stamped at construction so the command-buffer side can assert the right pipeline kind without changing the public-API handle type.

End-to-end pipeline (OmegaSL `mesh_basic`-shaped meshlet → HLSL codegen via Phase 2b → DXC → DXIL → `MS` subobject → `ID3D12Device8::CreatePipelineState` → `setRenderPipelineState` → `DispatchMesh(1, 1, 1)` → rasterizer → fragment → screen) was verified in-session 2026-05-30 with `gte/tests/directx/MeshAndRaytracingTest`. The window rendered the canonical colorful triangle: red bottom-left, green bottom-right, blue top, with smooth inter-stage interpolation across the face on a slate-grey clear background. First time every layer was exercised in sequence and the output matched expectation on the first PSO-build attempt.

Verified in-session 2026-05-30:

- `gte/tests/directx/MeshAndRaytracingTest`: build succeeds via the WSL→Windows hand-off (Win11 SDK on the user's host, RTX 2080 Ti). Runtime stdout confirms `GTEDEVICE_FEATURE_MESH_SHADER = YES` and `makeMeshPipelineState -> live PSO`. The Phase-3 "not yet implemented" `DEBUG_STREAM` lines no longer fire. Visual output matches the shader's per-vertex colors (R/G/B at the three meshlet vertices) and interpolation matches.
- Existing `2DTest` and `GPUTessTest` continue to render correctly — the graphics PSO path is unchanged byte-for-byte (only the mesh-variant constructor was added; the graphics constructor / `makeRenderPipelineState` body are untouched).
- DXC profile gate from Phase 2b is exercised live: `ms_6_5` for the mesh stage compiles cleanly. The "Promoting older shader model profile to 6.0 version" warning that DXC prints for the matching fragment shader (the fragment is still emitted at `ps_5_0` and DXC auto-promotes when it sees inter-stage compatibility with a mesh shader) is informational and not fatal.

Files touched (~250 lines, scoped to the D3D12 backend + the test fixture):

| Area | File | Change |
|---|---|---|
| PSO variant flag | `gte/src/d3d12/GED3D12Pipeline.{h,cpp}` | Added `bool isMesh = false;` to `GED3D12RenderPipelineState` + a second constructor (mesh-variant) that stamps the flag. The mesh shader handle goes into the existing `vertexShader` base slot — both stage types are `SharedHandle<GTEShader>` and the resource-binding paths read `shader->internal` uniformly, so the slot doubles cleanly with no resource-binding plumbing churn. |
| Engine PSO build | `gte/src/d3d12/GED3D12.cpp::GED3D12Engine::makeMeshPipelineState` | Phase-3 stub replaced with a real `CD3DX12_PIPELINE_MESH_STATE_STREAM` build. Root signature reuses `createRootSignatureFromOmegaSLShaders` over `{meshShader, fragmentShader}`. Subobjects populated field-for-field with the graphics PSO above: `BlendState`, `RasterizerState`, `DepthStencilState`, `RTVFormats`, `SampleDesc`, `SampleMask`. `pRootSignature`, `MS`, `PS` from the per-shader bytecode. `NodeMask` from `d3d12_device->GetNodeCount()`. `CreatePipelineState(&streamDesc, ...)` via `ID3D12Device8` (already the cached device type). Amplification stage rejected up front with a precise "Phase 5" diagnostic. |
| Command buffer dispatch | `gte/src/d3d12/GED3D12CommandQueue.cpp::GED3D12CommandBuffer::drawMeshTasks` | Phase-3 stub replaced. Feature gate kept (defensive). Adds three asserts (`inRenderPass`, `currentRenderPipeline != nullptr`, `currentRenderPipeline->isMesh`) then calls `commandList->DispatchMesh(groupCountX, groupCountY, groupCountZ)`. `commandList` is already `ID3D12GraphicsCommandList6`, so `DispatchMesh` is in scope without a header bump. |
| Test fixture | `gte/tests/directx/MeshAndRaytracingTest/meshAndRaytracing.omegasl` | Inter-stage color attribute changed from `: Color(0)` to `: TexCoord`. `Color(N)` is OmegaSL's *fragment-output* semantic (→ HLSL `SV_Target<N>`, MSL `[[color(N)]]`) which DXC rejects on a struct field consumed by a fragment shader as input. `TexCoord` is the conventional "interpolated arbitrary data" slot and lowers correctly on every backend. |
| Test fixture | `gte/tests/directx/MeshAndRaytracingTest/main.cpp` | Shader-library load path resolved against `OmegaCommon::FS::getExecutableDir().append(...)` rather than `"./..."`. CMake stages the `.omegasllib` next to the test executable; the previous `"./..."` form resolved against the shell cwd (commonly the repo root when launched from a terminal), so the file lookup missed. |
| Path normalization | `common/src/win/fs-win.cpp::Path::absPath`, `common/src/unix/fs-unixother.cpp::Path::absPath` | Strip leading `./` / `.\` segments before prepending cwd, so `absPath("./file")` returns `<cwd>/file` instead of `<cwd>/./file`. Loop handles repeated forms (`././file`); lone `.` collapses to a bare-cwd path. Independent cleanup that surfaced while diagnosing the load-path issue above — not load-critical (POSIX collapses at lookup, Win32 mostly tolerates the redundant segment) but the unnormalized form was surfacing in error messages. |

Trade-offs / deferred:

- **Amplification stage is hard-stopped.** A non-null `amplificationFunc` on the descriptor produces a precise "Phase 5" diagnostic + `nullptr` return; the AS subobject in the state stream is never populated today. Re-enable when Phase 5 lands the payload-type matching and the dispatch-children plumbing.
- **Per-meshlet threadgroup size is honored implicitly.** The mesh shader's declared `[numthreads(x, y, z)]` is baked into the compiled DXIL by DXC at codegen time; the runtime doesn't need to pass it through the state stream, and the `DispatchMesh(x, y, z)` group counts are independent. The plan's mention of "threadgroup sizes come from the shader's declared dimensions (stored on the pipeline at creation time)" is the right model on Metal but not needed on D3D12 — left as a follow-up note if a future API wants to expose it.
- **Cross-vendor verification.** Verified on NVIDIA RTX 2080 Ti (Turing). AMD RDNA 2+ and Intel Arc should work identically (Mesh Shader Tier 1 is the spec floor); flag any per-vendor surprises if/when they surface.
- **Fragment shader profile promotion.** Fragment is still emitted at `ps_5_0` and DXC auto-promotes to `ps_6_0` when chained with a mesh shader (a printed informational warning). This works but if we ever need explicit `ps_6_5` (e.g. to use a mesh-shader-only fragment intrinsic), bump the compile profile to match.

### Phase 3 — OmegaGTE public API + runtime surface: **code-complete, build-verified, full test suite passing**

The public API surface for mesh shaders is now locked across all three backends. `MeshPipelineDescriptor` is the new descriptor type in `gte/include/omegaGTE/GEPipeline.h`; `OmegaGraphicsEngine::makeMeshPipelineState(MeshPipelineDescriptor &)` is a new pure-virtual in `gte/include/omegaGTE/GE.h` next to the other pipeline makers; `GECommandBuffer::drawMeshTasks(uint32_t, uint32_t, uint32_t)` is a new pure-virtual in `gte/include/omegaGTE/GERenderTarget.h` slotted into the render-pass draw cluster. Every backend overrides both methods with a deliberate Phase-3 stub that feature-gates, validates shaders, logs a precise "not yet implemented (Phase 3 stub — Phase 4X will land …)" diagnostic, and returns `nullptr` / no-ops. No `#ifdef`s in the public headers — the gate lives in each backend's body, matching the raytracing precedent (`createBoundingBoxesBuffer` / `allocateAccelerationStructure` / `dispatchRays` at `gte/src/d3d12/GED3D12.cpp:1328`, `gte/src/metal/GEMetal.mm:1046`, `gte/src/vulkan/GEVulkan.cpp:3679`).

The stub layer is *deliberate*: it lets callers build pipelines + record `drawMeshTasks` calls today (no link error, no missing symbols), and Phase 4 swaps the bodies in place without touching the public surface or every call site downstream.

`makeMeshPipelineState` returns `SharedHandle<GERenderPipelineState>` — the same handle type as the graphics render pipeline (every backend models mesh PSOs as a render-pipeline variant on the public side). Callers bind the result via the existing `setRenderPipelineState`; no new `setMeshPipelineState` is needed. The descriptor reuses RenderPipelineDescriptor's `DepthStencilDesc` so the same depth-stencil struct shape works for both kinds. Intentionally *absent* from the mesh descriptor: `vertexInputDescriptor` and `primitiveTopologyCategory` — the mesh stage emits its own primitives, and there is no vertex-buffer pull.

Verified in-session 2026-05-30: cmake+ninja+clang build succeeds on the WSL→Windows cross-toolchain, and the full test suite passes. No source-side mesh shader tests changed (Phase 3 is API plumbing, not compiler-side); existing GLSL+HLSL+MSL emission paths are byte-identical for the previously-verified Phase 2a/2b/2c fixtures.

Files touched (~280 lines, scoped to public headers + 6 backend files):

| Area | File | Change |
|---|---|---|
| Public descriptor | `gte/include/omegaGTE/GEPipeline.h` | New `MeshPipelineDescriptor` struct: optional `amplificationFunc` (Phase 5 wires the payload machinery), required `meshFunc` + `fragmentFunc`, and the same render state RenderPipelineDescriptor uses (color formats, blend, depth/stencil via `RenderPipelineDescriptor::DepthStencilDesc`, raster, sample count). |
| Public engine API | `gte/include/omegaGTE/GE.h` | `struct MeshPipelineDescriptor;` forward-decl added alongside the other pipeline-descriptor forward-decls. New pure virtual `virtual SharedHandle<GERenderPipelineState> makeMeshPipelineState(MeshPipelineDescriptor & desc) = 0;` next to the other pipeline makers. |
| Public command-buffer API | `gte/include/omegaGTE/GERenderTarget.h` | New pure virtual `virtual void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;` slotted after `drawIndexedPolygonsIndirect`. |
| D3D12 engine | `gte/src/d3d12/GED3D12.{h,cpp}` | `GED3D12Engine::makeMeshPipelineState` override — feature-gate via `gteDevice->features.hasFeature`, validate mesh/fragment/(optional amplification) shaders via the inherited `_checkPipelineShader`, log + return `nullptr`. |
| D3D12 command buffer | `gte/src/d3d12/GED3D12CommandQueue.{h,cpp}` | `GED3D12CommandBuffer::drawMeshTasks` override — feature-gate via `parentQueue->engine->gteDevice->features` (friend access on `GED3D12CommandQueue`), log + return. Added `#include "omegaGTE/GTEDevice.h"` so the complete `GTEDevice` type and `GTEDEVICE_FEATURE_MESH_SHADER` constant are visible (the rest of this TU only forward-declared GTEDevice). |
| Metal engine | `gte/src/metal/GEMetal.mm` | `GEMetalEngine::makeMeshPipelineState` override — same shape as D3D12. |
| Metal command buffer | `gte/src/metal/GEMetalCommandQueue.{h,mm}` | `GEMetalCommandBuffer::drawMeshTasks` override. Skips the per-call feature gate — the Metal command buffer has no reachable engine handle today (confirmed by an existing comment in `GEMetalCommandQueue.mm`), so the gate at `makeMeshPipelineState` is what actually contracts the device. If we ever need a per-call gate, plumb a `GEMetalEngine *` onto `GEMetalCommandQueue` first. |
| Vulkan engine | `gte/src/vulkan/GEVulkan.{h,cpp}` | `GEVulkanEngine::makeMeshPipelineState` override — same shape as D3D12. |
| Vulkan command buffer | `gte/src/vulkan/GEVulkanCommandQueue.{h,cpp}` | `GEVulkanCommandBuffer::drawMeshTasks` override — feature-gate via `parentQueue->engine->gteDevice->features`. Added `#include "omegaGTE/GTEDevice.h"` for the same complete-type reason as D3D12. |

Trade-offs / deferred:

- **Mesh PSO surfaces as `GERenderPipelineState`, not a new opaque type.** Matches the plan's "Returns the same handle type as the graphics render pipeline (all three backends model mesh PSOs as a render pipeline variant)." Means `setRenderPipelineState` is the bind path for both kinds; no new setter needed. Open if Phase 4 reveals that a backend genuinely needs a separate handle type to distinguish at bind time, but none of the three currently does.
- **Metal command buffer feature gate is implicit.** The `makeMeshPipelineState` gate is the real contract: on an unsupported device, pipeline creation returns `nullptr`, so the user can't bind a mesh pipeline to dispatch against in the first place. The other two backends gate explicitly because their command-buffer subclasses can reach the engine; Metal can't today. Symmetry would mean adding a `GEMetalEngine *` plumb-through to `GEMetalCommandQueue` — out of scope for a Phase-3 stub.
- **Indirect mesh dispatch (`drawMeshTasksIndirect`) deferred to Phase 6.** Matches the original plan's deferral and how `drawPolygons` indirect is also deferred in the Pipeline-Completion plan.
- **All three Phase 4 implementations remain.** Phase 3 is just the surface. Phase 4a (Vulkan, verifiable here) → `vkCmdDrawMeshTasksEXT` + `VK_SHADER_STAGE_MESH_BIT_EXT` PSO; Phase 4b (D3D12, off-platform) → `CD3DX12_PIPELINE_MESH_STATE_STREAM` + `DispatchMesh`; Phase 4c (Metal, off-platform) → `MTLMeshRenderPipelineDescriptor` + `drawMeshThreadgroups`. The stubs already feature-gate + validate shaders, so each Phase-4 body just swaps the "not yet implemented" diagnostic for the real PSO/dispatch construction.

### Phase 2c — MSL mesh-stage codegen: **code-complete, build-verified, source-emission verified; `metal` toolchain compile off-platform / unverified on this Linux host**

MSL is now the third backend with live mesh codegen — mesh codegen is live on **all three backends**. `omegaslc --metal` emits `[[mesh]]` source for `mesh` entries: a per-shader `using __omegasl_mesh_t_<name> = mesh<VertStruct, void, MaxV, MaxP, topology::triangle|line>;` type alias at file scope, `[[mesh]] void name(__omegasl_mesh_t_<name> __omegasl_mesh_output_handle, ...resources/params...)` (mesh-output params suppressed from the signature — they route through the handle), and a body that declares a `<VertStruct> __omegasl_verts_scratch[MaxV];` scratch array, auto-emits `__omegasl_mesh_output_handle.set_primitive_count(MaxP);` (suppressed when the user calls `setMeshOutputs` — same rule as 2a/2b), walks the user statements, and flushes the scratch into the handle via a per-vertex `for` loop at body close. The dormant Phase-2a `tryEmitBuiltinCall(setMeshOutputs)` lowering — which already pointed at `__omegasl_mesh_output_handle.set_primitive_count(...)` — is now live; the sentinel name turned out to be the final name. The mesh-vertex-output struct is re-emitted in `emitShaderUsedStructs` with `[[color(N)]]` / `[[texcoord(N)]]` stripped (those are fragment-output decorations on Metal); `[[position]]` is preserved.

Per project convention ([[feedback_mark_unverified_backends_in_plan]]): **the MSL source we emit is verified source-only on this Linux host. `metal` compile is off-platform** (the toolchain ships on macOS) and was not exercised in-session; the user reported a clean build + passing tests on the cross-toolchain that touches it, but an actual `metal` compile of `mesh_basic.metal` against Metal 3 is the next-iteration loop's responsibility.

#### Why MSL diverges — lvalue translation, not just spelling

GLSL and HLSL accept `verts[i].field = expr;` / `tris[i] = uintK(...);` as direct lvalue writes (per-field arrayed varyings on GLSL, `out vertices`/`out indices` params on HLSL). MSL's mesh handle has **no** per-field accessor; the only API surface is:

- `output.set_primitive_count(uint p)` *(already wired ahead of 2c)*
- `output.set_vertex(uint i, VertexT v)` — **whole struct at once**
- `output.set_index(uint slot, uint vertexIdx)` — **one slot at a time** (`slot = i * K + k` for K-wide topology)

So MSL needs the user's per-field / per-tuple writes translated into the right MSL shape. The chosen pattern is **scratch-array hoist + flush-loop** for vertices, **per-slot expansion via the pending-statement queue** for indices. Both support **dynamic `i`** (the alternatives — accumulating per-slot temps at the rewrite site — require statically-known indices, which OmegaSL doesn't enforce). The exact mappings:

| OmegaSL source | MSL emission |
|---|---|
| `verts[i].field = expr;` | `__omegasl_verts_scratch[i].field = expr;` *(reroute by `emitMemberExpr`)* |
| `tris[i] = uint3(a,b,c);` | `uint3 __omegasl_mesh_idx_tmp = uint3(a,b,c); __omegasl_mesh_output_handle.set_index(i*3+0, tmp.x); set_index(i*3+1, tmp.y); set_index(i*3+2, tmp.z);` *(queued pre-statements + inline last call, via `tryEmitBinaryExpr`)* |
| `segs[i] = uint2(a,b);` | `uint2 tmp = uint2(a,b); set_index(i*2+0, tmp.x); set_index(i*2+1, tmp.y);` |
| `setMeshOutputs(nv, np);` | `__omegasl_mesh_output_handle.set_primitive_count(np);` *(`nv` dropped by design — MSL infers vertex count from the highest `set_vertex` slot)* |
| *(auto, if no user `setMeshOutputs`)* | `__omegasl_mesh_output_handle.set_primitive_count(<max_p>);` at body start |
| *(auto, at body close)* | `for (uint __i = 0; __i < <max_v>; ++__i) __omegasl_mesh_output_handle.set_vertex(__i, __omegasl_verts_scratch[__i]);` |

Cost: one local `VertStructT[max_v]` (thread-local stack) and one tail loop. Worth it for the dynamic-`i` story and the per-field write ergonomics users already have on GLSL/HLSL.

Verified in-session 2026-05-30:

- `gte/omegasl/tests/mesh_basic.omegasl` (three remaining entries: `mesh_triangle`, `mesh_line`, `mesh_triangle_runtime`) now emits MSL source on `--metal`. Build succeeded and the full test suite passed via cmake+ninja+clang. `mesh_triangle_runtime` correctly suppresses the auto `set_primitive_count` and the user's `setMeshOutputs(3u, 1u)` lowers to `__omegasl_mesh_output_handle.set_primitive_count(1u);` via the existing `tryEmitBuiltinCall` path.
- GLSL output unchanged for all three entries (Phase 2a regression check); HLSL output unchanged (Phase 2b regression check).
- `invalid_topology_point.omegasl` still rejected at Sema with the portability diagnostic (Phase 2b gate held).

Files touched (~250 lines, scoped to MSL + plan):

| Area | File | Change |
|---|---|---|
| Stage gate | `gte/omegasl/src/Target.h`, `MSLTarget.cpp` | `MSLTarget::supportsStage(Mesh)` flipped from "not yet implemented" to supported (Hull/Domain rejection unchanged). Mesh-routing state on `MSLTarget` (`meshVertsParamName`, `meshIndicesParamName`, `meshVertsStructDecl`, `meshTopology`, `meshMaxVertices`, `meshMaxPrimitives`); `structDeclMap` for the inter-stage struct re-emit. `emitMemberExpr` / `tryEmitBinaryExpr` overrides declared. |
| Type alias + header | `MSLTarget.cpp::emitShaderEntryHeader` | Emits `using __omegasl_mesh_t_<name> = mesh<VertStruct, void, MaxV, MaxP, topology::triangle\|line>;` ahead of the function for mesh entries. `[[mesh]]` function decorator added (function-attribute spelling, not a stage keyword). `shadermap_entry.type = OMEGASL_SHADER_MESH` + `threadgroupDesc.*` + `meshDesc.*` stamped. Param loop prepends `__omegasl_mesh_t_<name> __omegasl_mesh_output_handle` as the first parameter; suppresses `out vertices` / `out indices` params from the signature; counts visible params so the leading comma after the resource block fires correctly. |
| Body emission | `MSLTarget.cpp::emitShaderEntryBody` | Mesh case adds three new emissions inside the `{ ... }`: (a) `<VertStruct> __omegasl_verts_scratch[MaxV];` after the static-sampler flush; (b) auto-`__omegasl_mesh_output_handle.set_primitive_count(MaxP);` unless `meshHasUserSetMeshOutputsCall`; (c) per-vertex flush loop at body close. Mesh state cleared at body close. Non-mesh entries unchanged (byte-identical output). |
| Struct re-emit | `MSLTarget.cpp::emitStructDecl`, `emitShaderUsedStructs` | `emitStructDecl` now records each struct's `StructDecl*` in `structDeclMap`. `emitShaderUsedStructs` detects the mesh-vertex-output struct and re-emits it inline with `[[color(N)]]` / `[[texcoord(N)]]` stripped (those are fragment-output attributes; on a mesh vertex output Metal treats them as varying types that take no decoration). `[[position]]` is preserved. Same conceptual fix as HLSL Phase 2b. Non-mesh entries keep the cached text unchanged. |
| Member-expr reroute | `MSLTarget.cpp::emitMemberExpr` (new override) | When LHS is `INDEX_EXPR(<vertsParam>, i)`, rewrites the base to `__omegasl_verts_scratch[<i>]` and keeps the `.field` access. `<i>` can be dynamic. Falls through to the default `lhs.field` emission for every other member access. |
| Binary-expr reroute | `MSLTarget.cpp::tryEmitBinaryExpr` (new override) | When `op == "="` and LHS is `INDEX_EXPR(<indicesParam>, i)`: queues `uintK __omegasl_mesh_idx_tmp = <rhs>;` and `K-1` `set_index(...)` calls via the shared pending-statement queue (same mechanism HLSL uses for `GetDimensions`), emits the K-th `set_index` inline (no trailing `;` — `emitStatementLine` adds it). K = 3 for triangle, 2 for line. Falls through for any other `=` and for non-`=` ops. |
| Comment update | `MSLTarget.cpp::tryEmitBuiltinCall` (`BUILTIN_SET_MESH_OUTPUTS` branch) | Pre-2c the comment noted the `__omegasl_mesh_output_handle` name was a sentinel waiting for 2c to rewrite. Updated to record that 2c kept the same name when materializing the handle — no rewrite was needed. |

Trade-offs / deferred:

- **`set_vertex` flush is unconditional across all `<max_v>` slots.** A shader that touches only 2 of 3 vertex slots still calls `set_vertex` for slot 2 — with garbage data, but the index buffer won't reference slot 2 so the GPU never reads it. Equivalent to GLSL/HLSL's behavior where the scratch slots are zero-initialized. Could narrow with `meshActiveVertexCount` tracking in a follow-up but no concrete need today.
- **`tryEmitBinaryExpr` assumes RHS is a `uintK` expression with `K` lanes matching the topology.** Sema already pins the indices param's element type to the topology-correct `uintK` and rejects any other assignment shape, so this assumption is structurally guaranteed by the front-end gate.
- **Per-primitive data (`mesh<V, PrimT, ...>` second template arg).** Still `void` — Phase 6 follow-up. When the `out primitives` param is added, the alias declaration becomes `mesh<VertStruct, PrimStruct, ...>` and the body emits an analogous `__omegasl_prims_scratch[MaxP]` flush.
- **Metal compile is off-platform.** Source emission is verified on this Linux host; the actual `metal` → `.metallib` pipeline is not. First-iteration loop on macOS is the next verification surface.
- **All three backends now agree on the lvalue shape** the user writes (`verts[i].field` / `tris[i]`), even though only HLSL accepts that shape natively. The two lvalue-translation patterns (scratch-array + flush, per-slot pending-queue expansion) are reusable building blocks for Phase 6's per-primitive output story.

### Phase 2b — HLSL mesh-stage codegen: **code-complete, build-verified, source-emission verified; DXC→DXIL compile off-platform / unverified on this Linux host**

HLSL is now the second backend with live mesh codegen. `omegaslc --hlsl` emits SM 6.5 mesh source for `mesh` entries — `[outputtopology("triangle"|"line")]`, `[numthreads(x,y,z)]`, `out vertices StructT verts[N]` / `out indices uintK tris[M]` parameter qualifiers, the auto-emitted `SetMeshOutputCounts(<max_v>, <max_p>);` at body start (suppressed when the user wrote `setMeshOutputs(nv, np)` themselves — same rule as GLSL 2a), and the mesh-vertex-output struct re-emitted with inter-stage semantics (`Color(N) → COLOR<N>`, `TexCoord(N) → TEXCOORD<N>`) instead of the cached fragment-output form (`SV_Target<N>`). The DXC profile gate routes Mesh stage to `-T ms_6_5` (regardless of FLOAT16); the runtime D3DCompile path rejects Mesh up front (SM 5.1 max — same fail-loud pattern as the FLOAT16 gate). MSL still halts cleanly at `supportsStage(Mesh)` with the original "mesh codegen not yet implemented" diagnostic, exactly as before.

Per project convention ([[feedback_mark_unverified_backends_in_plan]]): **the HLSL source we emit is verified source-only on this Linux host. DXC→DXIL is off-platform** (DXC ships on Windows) and was not exercised in-session; the user reported a clean build + passing tests on the cross-toolchain that touches it, but a real DXC compile of `mesh_basic.hlsl` against `ms_6_5` is the next-iteration loop's responsibility.

Verified in-session 2026-05-30:

- `gte/omegasl/tests/mesh_basic.omegasl` now has three entries (`mesh_triangle`, `mesh_line`, `mesh_triangle_runtime`) — `mesh_point` moved to a negative test (see below). Build succeeded and the full test suite passed via cmake+ninja+clang on Linux. HLSL backend emits the SM 6.5 spellings above; the auto-`SetMeshOutputCounts` is suppressed for `mesh_triangle_runtime` (the user's `setMeshOutputs(3u, 1u)` lowers in place via `renameBuiltin → SetMeshOutputCounts`). GLSL output is unchanged for all three remaining entries (no regression).
- `mesh_basic.omegasl` re-verified clean on GLSL (Phase 2a still passes — glslc → SPIR-V 1.4 → spirv-val).
- MSL still refuses mesh with the precise "not yet implemented" diagnostic; Sema runs first and passes for all three mesh entries before the per-backend stub aborts. Front-end uniformity preserved.
- Negatives: `gte/omegasl/tests/invalid_topology_point.omegasl` — `topology=point` on what would otherwise be a valid mesh shader → `` topology=point is not portable — HLSL SM 6.5 has no `[outputtopology("point")]` (only triangle and line) ``. Fires before per-backend codegen, so the diagnostic is identical regardless of target.

Files touched (~250 lines, scoped to HLSL and Sema + test fixture moves):

| Area | File | Change |
|---|---|---|
| Stage gate | `gte/omegasl/src/Target.h`, `HLSLTarget.cpp` | `HLSLTarget::supportsStage` flipped to `true` for every stage (Mesh now lights up). Mesh-routing state on `HLSLTarget` (`meshVertsParamName`, `meshIndicesParamName`, `meshVertsStructDecl`, `meshTopology`, `meshMaxVertices`, `meshMaxPrimitives`); `structDeclMap` for the inter-stage struct re-emit; `emitShaderEntryBody` override declared. |
| Header emission | `HLSLTarget.cpp::emitShaderEntryHeader` | Mesh branch added: `[outputtopology("triangle"\|"line")]`, `[numthreads(x,y,z)]`; `shader_entry.{type, threadgroupDesc, meshDesc.{max_vertices,max_primitives,topology}}` stamped; mesh-output param resolution into the routing state. `OMEGASL_SHADER_MESH` added to the entry-type mapping (was previously falling through to `OMEGASL_SHADER_DOMAIN` because Mesh was unreachable). Per-param loop now emits `out vertices T name[N]` / `out indices T name[M]` for mesh-output params (with `writeDeclTypeSuffix` for the array suffix), and falls through to the existing path for everything else (thread-IDs / attributed params unchanged). |
| Body emission | `HLSLTarget.cpp::emitShaderEntryBody` (new) | Mesh case: opens `{`, auto-emits `SetMeshOutputCounts(<mv>, <mp>);` (skipped when `meshHasUserSetMeshOutputsCall`), walks the body via the shared `emitStatementLine` (preserves HLSL's pre-statement queue, e.g. `GetDimensions` lowerings), closes `}`. Non-mesh path delegates to `generateBlock` so the previous behavior is byte-identical. Mesh-routing state is cleared at body close. |
| Struct re-emit | `HLSLTarget.cpp::emitStructDecl`, `emitShaderUsedStructs` | `emitStructDecl` now records each struct's `StructDecl*` in `structDeclMap` alongside the cached HLSL text. `emitShaderUsedStructs` detects the mesh-vertex-output struct and re-emits it inline with the inter-stage semantic mapping (`Color(N) → COLOR<N>`, `TexCoord(N) → TEXCOORD<N>`); `Position` still rides `SV_Position`. Every other struct (including non-mesh-vertex structs *inside* a mesh entry) keeps the cached text unchanged — no behavioral change for vertex/fragment shaders. |
| Toolchain gate | `HLSLTarget.cpp::compileShader` | Mesh stage routes to `-T ms_6_5` (regardless of FLOAT16). Off-platform on Linux. |
| Runtime gate | `HLSLTarget.cpp::compileShaderRuntime` | Mesh stage fails loud with a precise "use the offline pipeline" diagnostic — D3DCompile tops out at SM 5.1, so a runtime mesh compile is structurally impossible. Mirrors the FLOAT16 fail-loud pattern. |
| Sema | `gte/omegasl/src/Sema.cpp` | New `topology=point` rejection in the mesh-stage structural-validation block: fires before per-backend codegen with a portability-focused message that names HLSL SM 6.5 as the limiting backend and points at the `triangle`/`line` alternatives. |
| Test fixture | `gte/omegasl/tests/mesh_basic.omegasl` | `mesh_point` entry removed (`topology=point` now rejected by Sema); `mesh_triangle`, `mesh_line`, and `mesh_triangle_runtime` retained. Comment block updated to point at the new negative fixture. |
| Negative test | `gte/omegasl/tests/invalid_topology_point.omegasl` | New: a `mesh(...topology=point)` entry that pins the Sema rejection in place. |

Trade-offs / deferred:

- **`topology=point` portability.** Rejected in Sema (front-end-uniform), not just in HLSL codegen. GLSL natively supports `points` and Metal natively supports `metal::topology::point`, so this gate trades a working GLSL/MSL capability for cross-backend portability. The plan's Cross-Backend Differences section already flagged this as the recommended fix when 2b lands; revisit if SM 6.6+ ever adds a `[outputtopology("point")]` spelling or if there's a real need for backend-local point mesh shaders.
- **`Color(N)`/`TexCoord(N)` on the mesh-vertex-output struct.** Re-emitted inline in `emitShaderUsedStructs` with `COLOR<N>` / `TEXCOORD<N>` instead of the cached `SV_Target<N>` / `TEXCOORD<N>` mapping. `SV_Target` is the fragment-output semantic and is not legal as a mesh-vertex-output interstage attribute. Localized to mesh entries; vertex/fragment struct emission is unchanged. Could be generalized to apply on the vertex-output side of any stage if a future audit shows the same `SV_Target<N>` issue surfaces there.
- **DXC compile is off-platform.** Source emission is verified; the DXC → DXIL pipeline is not. First-iteration loop on Windows is the next verification surface. If DXC rejects anything in the emitted source, the iteration goes through the user (Windows build hand-off, per AGENTS.md).
- **MSL is unaffected.** Phase 2c is unblocked — the front-end is already uniform, and the cross-backend struct re-emit pattern lands as a precedent if MSL needs something similar.

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

Status update: the `mesh` stage **front-end is implemented** (parse + Sema + serialization model; see Implementation Status). `task`/`amplification` remains not implemented (Phase 5). Mesh **codegen** is **live on GLSL, HLSL, and MSL** (Phase 2a + 2b + 2c) and now exercised end-to-end on **two backends**: HLSL through D3D12 since Phase 4b, MSL through Metal since Phase 4c. **D3D12 backend is live** — Phase 4b shipped 2026-05-30; mesh PSO build via `CD3DX12_PIPELINE_MESH_STATE_STREAM` and `commandList->DispatchMesh` dispatch both pass an end-to-end visual test on an RTX 2080 Ti. **Metal backend is live** — Phase 4c shipped 2026-05-30; mesh PSO build via `MTLMeshRenderPipelineDescriptor` and `[rp drawMeshThreadgroups:...]` dispatch both pass a headless integration test on Apple M4 Max, closing the Phase 2c off-platform-unverified callout in the process. Vulkan (4a) remains on the public-API stub from Phase 3, awaiting its own Phase-4 build. Sema rejects `topology=point` as a portability gate (HLSL SM 6.5 has no `point` output topology) — see the Phase 2b status block + Cross-Backend Differences.

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

**Consequence for the builtin (landed in Phase 2a follow-up — see Implementation Status):** `setMeshOutputs(nv, np)` lowers to `SetMeshOutputsEXT(nv, np)` on GLSL (live), `SetMeshOutputCounts(nv, np)` on HLSL (live since 2b), and `__omegasl_mesh_output_handle.set_primitive_count(np)` on MSL (live since 2c) — the `nv` argument is dropped on MSL by design because vertex count there is implicit. The pre-2c sentinel name `__omegasl_mesh_output_handle` turned out to be the final name 2c materialized.

### Output access pattern

| Backend | Vertex write | Index write |
|---|---|---|
| GLSL | `gl_MeshVerticesEXT[i].gl_Position = …;` + per-field arrayed `out` varying `<struct>_<field>[i] = …;` | `gl_Primitive{Triangle,Line,Point}IndicesEXT[i] = uvec{3,2,1}(…);` |
| HLSL | direct array index on the `out vertices` param: `verts[i].field = …;` (Position via `SV_Position`) | direct array index on the `out indices` param: `tris[i] = uint{3,2}(…);` |
| MSL | accessor calls on the `mesh<…>` handle: `mesh.set_vertex(i, vertStruct);` (whole struct at once) | accessor calls: `mesh.set_index(uint slot, uint vertexIdx)` — **per-slot, not per-primitive** (`slot = i * K + k` for K-wide topology). Resolved in Phase 2c: see Implementation Status → Phase 2c lvalue-translation table. |

Resolved in Phase 2c (live since 2026-05-30): the per-field write ergonomics OmegaSL exposes (`verts[i].field = expr;`) are preserved on every backend. MSL handles the impedance via a hoisted `<VertStruct> __omegasl_verts_scratch[<max_v>];` local + a per-vertex flush loop at body close; per-tuple `tris[i] = uintK(...);` writes expand to K `set_index(...)` calls via the shared pending-statement queue (same plumbing HLSL's `GetDimensions` uses). Both patterns support **dynamic `i`** — see the lvalue-translation table in the Phase 2c status block for the exact emission.

### Topology spelling and supported set

| Backend | `triangle` | `line` | `point` |
|---|---|---|---|
| GLSL | `triangles` in `layout(...) out;` | `lines` | `points` |
| HLSL | `[outputtopology("triangle")]` | `[outputtopology("line")]` | **not supported at SM 6.5** — point mesh output is a Vulkan/Metal capability, not a D3D12 one |
| MSL | `metal::topology::triangle` template arg | `metal::topology::line` | `metal::topology::point` |

**Action item ([[feedback_frontend_backend_uniformity]]):** ~~OmegaSL currently accepts `topology=point` (and Sema does not reject it). On HLSL there is no lowering.~~ **Resolved in Phase 2b** (2026-05-30): Sema now rejects `topology=point` with a portability-focused diagnostic that names HLSL SM 6.5 as the limiting backend and points at `topology=triangle` / `topology=line` as alternatives. The negative fixture is `gte/omegasl/tests/invalid_topology_point.omegasl`. This intentionally trades a working GLSL/MSL capability for cross-backend uniformity, per the plan's "front-end-uniform constructs" stance. If SM 6.6+ ever adds `[outputtopology("point")]`, this gate can relax.

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
- **2b — HLSL** *(text emission verified on this Linux host; DXC→DXIL compile is **off-platform / unverified on Linux**)* — **DONE 2026-05-30**: SM 6.5, `[numthreads(...)]`, `[outputtopology("triangle"|"line")]`, `out vertices`/`out indices` params with array-suffix qualifiers, `SetMeshOutputCounts(<mv>,<mp>)` auto-emit (suppressed when the user calls `setMeshOutputs(nv, np)`), mesh-vertex-output struct re-emitted with inter-stage `COLOR<N>` / `TEXCOORD<N>` semantics in place of fragment-output `SV_Target<N>`. DXC profile gate routes Mesh → `ms_6_5`; runtime D3DCompile path rejects Mesh up front. `topology=point` is now Sema-rejected (front-end-uniform — see **Cross-Backend Differences → Topology** + the trade-off note in **Implementation Status → Phase 2b**). See **Implementation Status → Phase 2b** above.
- **2c — MSL** *(text emission verified on this Linux host; `metal` toolchain compile is **off-platform / unverified on Linux**)* — **DONE 2026-05-30**: `[[mesh]]` function decorator + per-shader `using __omegasl_mesh_t_<name> = mesh<VertStruct, void, MaxV, MaxP, topology::triangle|line>;` type alias, `__omegasl_mesh_output_handle` as the first param, mesh-output params suppressed from the signature, `<VertStruct> __omegasl_verts_scratch[MaxV];` scratch hoist + `__omegasl_mesh_output_handle.set_primitive_count(<max_p>);` auto-emit (suppressed when the user calls `setMeshOutputs(nv, np)`), per-vertex flush loop at body close, `set_index(...)` per-slot expansion for `tris[i] = uintK(...)` writes (via the pending-statement queue), and inter-stage attribute stripping (`[[color(N)]]` / `[[texcoord(N)]]`) on the mesh-vertex-output struct. See **Implementation Status → Phase 2c** above.
- [ ] Golden-file round-trip tests: compile the canonical meshlet shader to HLSL/MSL/GLSL and diff against checked-in goldens (the original Phase-1 test intent).
- [ ] **Per project convention** ([[feedback_mark_unverified_backends_in_plan]]): when 2b/2c land, record HLSL/DXC and MSL/Metal as compile/run-unverified off-platform as a callout here, not just in chat.

### Phase 3 — OmegaGTE public API + runtime surface (~280 lines) — **DONE 2026-05-30**

`MeshPipelineDescriptor` in `gte/include/omegaGTE/GEPipeline.h`, `OmegaGraphicsEngine::makeMeshPipelineState(MeshPipelineDescriptor &)` in `gte/include/omegaGTE/GE.h`, `GECommandBuffer::drawMeshTasks(uint32_t, uint32_t, uint32_t)` in `gte/include/omegaGTE/GERenderTarget.h`. Feature-gated behind `GTEDEVICE_FEATURE_MESH_SHADER` per backend (no `#ifdef`s in the public headers — gate lives in each backend's body, matching the raytracing pattern). All three backends return `nullptr` / no-op with a precise "Phase 3 stub — Phase 4X will land …" diagnostic, locking the surface ahead of Phase 4. `makeMeshPipelineState` returns the existing `SharedHandle<GERenderPipelineState>` so callers bind via the existing `setRenderPipelineState`. See **Implementation Status → Phase 3** above. *(= original plan's Phase 2.)*

### Phase 4 — Backend pipeline + dispatch implementations

Per-backend PSO creation + dispatch, each gated on its own feature check, verified with a meshlet demo:

- **4a — Vulkan** *(verifiable on the project's native Linux Vulkan host per AGENTS.md "Building"; the agent itself is on macOS for this landing — the Vulkan target is not configured on this host, so the build/run loop is handed to the user)* — **DONE 2026-05-31** (code-complete, **off-platform / unverified on this macOS host**): `VkGraphicsPipeline` with `VK_SHADER_STAGE_MESH_BIT_EXT` + fragment stage, empty `VkPipelineVertexInputStateCreateInfo` / `VkPipelineInputAssemblyStateCreateInfo` (spec-ignored when a mesh stage is present, but passed well-formed to keep validation layers quiet); `vkCmdDrawMeshTasksEXT` loaded from `VK_EXT_mesh_shader` at device init; `VkPhysicalDeviceMeshShaderFeaturesEXT { meshShader = VK_TRUE, taskShader = VK_FALSE }` chained into `vkCreateDevice`'s `pNext` when the extension is available; `createPipelineLayoutFromShaderDescs` now maps `OMEGASL_SHADER_MESH` → `VK_SHADER_STAGE_MESH_BIT_EXT` so descriptor-set bindings + push-constant ranges get the right stage flags (mesh-stage resource binding then "comes for free" through the existing `bindResourceAtVertexShader` path, just like D3D12). `GEVulkanRenderPipelineState` carries the same `isMesh` flag pattern Phase 4b/4c established so the command-buffer side asserts on dispatch kind. Amplification stage hard-stopped at `makeMeshPipelineState` with a precise Phase-5 diagnostic, matching the D3D12/Metal precedent. Verification handed off to the user: build the Vulkan target on the Linux host + run `MeshShaderTest` (the shared backend-independent test from Phase 4c.4 — wire it into `gte/tests/vulkan/CMakeLists.txt` if it isn't already).
- **4b — D3D12** *(verifiable on the user's Windows host via the WSL→Windows hand-off; the agent itself is in WSL — see AGENTS.md "Building")* — **DONE 2026-05-30**: `CD3DX12_PIPELINE_MESH_STATE_STREAM` populated and built via `ID3D12Device8::CreatePipelineState`; `commandList->DispatchMesh(x, y, z)` issued on the existing `ID3D12GraphicsCommandList6`. PSO surfaces as `GERenderPipelineState` with a new `isMesh` flag on the D3D12 subclass that the command-buffer side asserts. End-to-end visual test (`MeshAndRaytracingTest`) passing on RTX 2080 Ti — see **Implementation Status → Phase 4b** above.

  Implementation phasing:

  - **4b.1 — Internal data model.** Extend `GED3D12RenderPipelineState` to remember it was built as a mesh pipeline: hold the mesh-shader handle (replaces the vertex-shader slot in this variant), an `isMesh` flag the command-buffer side asserts on at `drawMeshTasks` time, and the resolved `[numthreads(...)]` threadgroup dims so the bound-PSO state is observable from one place. Constructor takes both forms; no public-API surface change.
  - **4b.2 — `makeMeshPipelineState` body.** Replace the Phase-3 stub: build via `CD3DX12_PIPELINE_MESH_STATE_STREAM` + `ID3D12Device2::CreatePipelineState` (the stream API needs Device2 — already available; the existing graphics path also uses Device2-era helpers). Root signature reuses the existing `createRootSignatureFromOmegaSLShaders` over `{meshShader, fragmentShader}` — the per-shader resource bindings the OmegaSL frontend records carry through unchanged. Subobjects populated: `MS` and `PS` bytecode (and `AS` when Phase 5 lands; for 4b, refuse a non-null `amplificationFunc` with a precise "amplification stage is Phase 5" diagnostic + return nullptr), the color-attachment pixel formats, depth-stencil state from `depthAndStencilDesc`, raster state (cull / fill / front-face rotation), and sample count. Output type stays `SharedHandle<GERenderPipelineState>` — the variant flag tells the command buffer how to handle it.
  - **4b.3 — `drawMeshTasks` body.** Replace the Phase-3 stub on `GED3D12CommandBuffer`: assert active render pass (`inRenderPass`) + bound PSO is mesh-variant (the flag from 4b.1 — fail loud if the user dispatched mesh against a non-mesh PSO), then call `commandList->DispatchMesh(groupCountX, groupCountY, groupCountZ)`. `ID3D12GraphicsCommandList6` is already the type on the command-list field, so the symbol is in scope; no header bump.
  - **4b.4 — Verification.** Build via the WSL→Windows hand-off; run `MeshAndRaytracingTest` (`gte/tests/directx/MeshAndRaytracingTest/`). Expect: the stdout `makeMeshPipelineState ->` line flips from "nullptr (Phase 3 stub — Phase 4b pending)" to "live PSO" and the slate-grey window now renders the meshlet (one colorful triangle with red/green/blue vertex colors). Both `DEBUG_STREAM` "not yet implemented" diagnostics disappear from the run output.

- **4c — Metal** *(verifiable directly on this macOS host — Metal toolchain ships with the SDK; the `metal` compile of the MSL source emitted in Phase 2c lights up live here for the first time)* — **DONE 2026-05-30**: `MTLMeshRenderPipelineDescriptor` PSO build via `[device newRenderPipelineStateWithMeshDescriptor:options:reflection:error:]`; `[rp drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:]` dispatch on the existing `id<MTLRenderCommandEncoder>`. PSO surfaces as `GERenderPipelineState` with a new `isMesh` flag on the Metal subclass that the command-buffer side asserts. Headless integration test (`gte/tests/mesh_shader_test.cpp` → `MeshShaderTest`) passing on Apple M4 Max — center pixel of an 8×8 offscreen render carries all three R/G/B vertex contributions. See **Implementation Status → Phase 4c** above.

  Implementation phasing (mirrors 4b — see Phase 4b above for the parallel D3D12 walkthrough):

  - **4c.1 — Internal data model.** Extend `GEMetalRenderPipelineState` (`gte/src/metal/GEMetalPipeline.h/.mm`) to remember it was built as a mesh pipeline: an `isMesh` flag the command-buffer side asserts on at `drawMeshTasks` time, and a second constructor that takes the mesh shader handle in the existing `vertexShader` base slot — same slot-doubling trick D3D12 uses, justified by the same observation (both stage types are `SharedHandle<GTEShader>` and the resource-binding paths read `shader->internal` uniformly). No public-API surface change.
  - **4c.2 — `makeMeshPipelineState` body.** Replace the Phase-3 stub in `GEMetalEngine::makeMeshPipelineState` (`gte/src/metal/GEMetal.mm`): build via `MTLMeshRenderPipelineDescriptor` + `[device newRenderPipelineStateWithMeshDescriptor:options:reflection:error:]` (Metal 13+ / iOS 16+; the feature gate already established by Phase 3 is what contracts the OS-level availability — Apple7+ is the hardware floor). Subobjects mirror the graphics path field-for-field where the fields apply: `label`, `meshFunction`, `fragmentFunction`, `colorAttachments[i].pixelFormat`/blend (identical loop), `rasterSampleCount`. Depth-stencil **state object** is built via the same `[device newDepthStencilStateWithDescriptor:]` path as the graphics PSO and stored on `GEMetalRenderPipelineState::depthStencilState`; raster state (cull / fill / winding / depth-bias) is captured in `GEMetalRasterizerState` the same way (Metal applies these at encoder time via `setCullMode:` / `setTriangleFillMode:` / `setFrontFacingWinding:` — handled by the existing `setRenderPipelineState`, no second code path needed). Vertex descriptor is intentionally absent (the mesh stage emits its own primitives — same omission as on `MeshPipelineDescriptor` itself). `amplificationFunc` is rejected up front with a precise "Phase 5" diagnostic + `nullptr` return, matching the D3D12 hard-stop.
  - **4c.3 — `drawMeshTasks` body.** Replace the Phase-3 stub on `GEMetalCommandBuffer::drawMeshTasks` (`gte/src/metal/GEMetalCommandQueue.mm`): assert `rp != nil` (active render pass), `renderPipelineState != nullptr` (PSO bound), `renderPipelineState->isMesh` (variant matches), then call `[rp drawMeshThreadgroups:MTLSizeMake(x,y,z) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(tg.x,tg.y,tg.z)]`. The mesh-stage per-threadgroup dimensions come from `renderPipelineState->vertexShader->internal.threadgroupDesc` — same field compute uses for `dispatchThreadgroups`, populated by the MSL codegen in Phase 2c. `threadsPerObjectThreadgroup` is `(1,1,1)` because no amplification stage is bound (the Metal SDK doc explicitly notes this arg is "ignored if object shader is not present"); when Phase 5 wires amplification, this becomes the object-stage `[numthreads(...)]`. The feature-gate is implicit per the existing Phase-3 stub comment — the gate at `makeMeshPipelineState` is the real contract.
  - **4c.4 — Verification.** Build via `cmake --build` + `ninja` against the macOS Metal toolchain. The first concrete win: **Phase 2c MSL source emission, previously verified source-only on Linux, is now end-to-end through the `metal` toolchain → `.metallib` → live PSO → rendered frame** — closes the off-platform-unverified callout in Phase 2c. Port a minimal `MeshAndRaytracingTest` to `gte/tests/metal/` that drops the FBX-buffer path (mesh-stage resource binding is a 4c follow-up — see trade-offs below) and exercises a no-input meshlet (single hardcoded triangle) to keep verification scope tight, matching the **original** Phase 4b MVP shape before FBX integration was layered on top of it.

  Phase 4c.5 — Mesh-stage resource binding (Metal-only follow-up to 4c.1–4c.4):

  - **4c.5.1 — Buffer bind routing.** Inside `GEMetalCommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> &, unsigned)`, branch on `renderPipelineState->isMesh`: when true, call `[rp setMeshBuffer:offset:atIndex:]` and use `MTLRenderStageMesh` for the fence wait/update (both ride macOS 13+/iOS 16+, same floor as `MTLMeshRenderPipelineDescriptor`, so the Phase 4c feature gate already covers availability). When false, keep the existing `setVertexBuffer:` + `MTLRenderStageVertex` path byte-for-byte. The shader-info read (`renderPipelineState->vertexShader->internal`) stays the same — the mesh shader sits in that slot by the 4c.1 slot-doubling trick, so `getResourceLocalIndexFromGlobalIndex` / `checkBufferRoleAgainstShader` / `shaderHasWriteAccessForResource` continue to address the right omegasl shader metadata. No public-API change.
  - **4c.5.2 — Texture bind routing.** Same shape inside `bindResourceAtVertexShader(SharedHandle<GETexture> &, unsigned, const TextureSwizzle &)`: `setMeshTexture:atIndex:` + `MTLRenderStageMesh` fences. Swizzle-view resolution (`getOrCreateSwizzledView`) is independent of the bind stage so the existing call survives unchanged.
  - **4c.5.3 — Sampler bind routing.** Same shape inside `bindResourceAtVertexShader(SharedHandle<GESamplerState> &, unsigned)`: `setMeshSamplerState:atIndex:` when mesh. Samplers don't ride a fence, so this is the simplest of the four.
  - **4c.5.4 — Push-constant routing.** Inside `GEMetalCommandBuffer::setRenderConstants`, the existing call enumerates which stages declared the `constant<T>` push constant via `findPushConstantBufferIndex(...)` and calls `[rp setVertexBytes:...]` / `[rp setFragmentBytes:...]` for each hit. The vertex branch needs an `isMesh` dispatch to `[rp setMeshBytes:length:atIndex:]`. Fragment branch is unaffected (fragment is fragment regardless of upstream stage).
  - **4c.5.5 — Verification.** Extend `gte/tests/mesh_shader_test.cpp` with a second phase (Phase B) that binds a CPU-built triangle-data buffer to the mesh stage (one `float4 pos` + one `float4 color` per vertex, three vertices per meshlet) and rewrites the mesh shader to read from it: `verts[i].pos = buf[i].pos; verts[i].color = buf[i].color;`. Phase A (no-input meshlet) stays as the baseline. After this lands, the test's pass criterion for Phase B reads back the center pixel and asserts it matches the bound-buffer's interpolated vertex colors specifically — a stronger contract than Phase A's "any color contribution".

  Fence-stage rationale (4c.5.1 / 4c.5.2): Metal documents `MTLRenderStageMesh` and `MTLRenderStageObject` as distinct from `MTLRenderStageVertex` so the GPU can hazard-track the mesh/object pipeline independently of the legacy vertex stage. Using `MTLRenderStageMesh` on a mesh PSO (rather than reusing `MTLRenderStageVertex`) gives the validator the right stage tag and lets the driver schedule the wait/signal at the actual stage boundary, not at vertex-fetch time which never happens in this PSO. The exact same argument transfers from the existing `MTLRenderStageVertex` / `MTLRenderStageFragment` split — we're just adding a third option for the third pipeline kind.

  Trade-offs / deferred under 4c.5:

  - **Object-stage (`MTLRenderStageObject`, `setObjectBuffer:`/`setObjectTexture:`/etc.) binding deferred to Phase 5.** Today no amplification stage is buildable (Phase 4c.2 hard-stops `amplificationFunc`), so any object-stage binding API would be unreachable. The pattern lands as 4c.5.1–4c.5.4 verbatim with `MTLRenderStageObject` / `setObject*:` when Phase 5 wires the amplification stage.
  - **No new `bindResourceAtMeshShader` public method.** Per the cross-backend uniformity goal (D3D12 reuses `bindResourceAtVertexShader` for mesh "for free" via unified root signatures), Metal routes inside the existing method so the user-facing call site is identical across backends. Adding a separate `bindResourceAtMeshShader` would diverge the public API on Metal only.
  - **`#requires(MESH_SHADERS)` in runtime-compiled source remains broken** as flagged in 4c trade-offs. The Phase B test in 4c.5.5 inherits the same source-string-without-`#requires` workaround.

  Trade-offs / deferred (carried from 4c.1–4c.4):

  - **Mesh-stage resource binding (`bindResourceAt*Shader` → `setMeshBuffer:` / `setMeshTexture:` / `setMeshSamplerState:`) is a 4c follow-up, not part of this landing.** D3D12 gets resource binding "for free" on the mesh slot because root signatures are unified across stages and the existing `bindResourceAtVertexShader` already reaches the right resource table when the mesh shader sits in the vertex slot. Metal has separate per-stage binding tables — `setVertexBuffer:`, `setFragmentBuffer:`, `setMeshBuffer:`, `setObjectBuffer:` are distinct API surfaces — so reusing `bindResourceAtVertexShader` for a bound mesh PSO would call into the vertex slot, which doesn't exist on a mesh pipeline. The clean fix is to dispatch inside `bindResourceAt*Shader` on the `isMesh` flag and route to the matching `setMesh*` API. Sized as its own ~150-line follow-up because it touches the buffer / texture / sampler bind methods symmetrically. With it absent, the Phase 4c verification has to use a no-input meshlet (single hardcoded triangle); landing it unlocks the FBX path the D3D12 test already exercises. Tracked under the same "follow-up" umbrella as indirect mesh dispatch (Phase 6).
  - **Amplification stage is hard-stopped** at `makeMeshPipelineState` for the same Phase-5 reason D3D12 uses, with the same precise diagnostic. The `objectFunction` property on `MTLMeshRenderPipelineDescriptor` is never assigned today; `threadsPerObjectThreadgroup` is fixed at `(1,1,1)` because Metal documents the argument as ignored without an object shader.
  - **Per-pipeline threadgroup hints (`maxTotalThreadsPerMeshThreadgroup`, `meshThreadgroupSizeIsMultipleOfThreadExecutionWidth`) are left at defaults.** The MSL codegen already bakes `[[max_total_threads_per_threadgroup(N)]]` into the compiled mesh function from the `mesh(...)` descriptor's threadgroup dims, so the Metal optimizer can read N from there. Setting the matching pipeline-state property would be a redundant double-statement; revisit if profiling shows the hint changes scheduling on Apple GPUs.
  - **`depthAttachmentPixelFormat` / `stencilAttachmentPixelFormat` on `MTLMeshRenderPipelineDescriptor` are left at default `MTLPixelFormatInvalid` when depth is disabled** — same behavior as the existing graphics `makeRenderPipelineState`. When the depth-stencil format does need to be set (depth-enabled passes), the same fix lands in both call sites at once; out of scope for this Phase 4c landing because the verification test runs depth-disabled.
  - **Hardware floor (Apple7 = M3 / A17+, macOS 13+).** Already covered by the `GTEDEVICE_FEATURE_MESH_SHADER` gate from Phase 3. The runtime gate at `makeMeshPipelineState` is what fails-loud on excluded devices; this Phase doesn't change that surface.

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
