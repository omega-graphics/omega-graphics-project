# OmegaSL Feature Gap Survey

Cross-reference of OmegaSL's current surface against HLSL (SM 5.0 → SM 6.8), MSL
(2.x → 3.x), and GLSL / SPIR-V (Vulkan 1.0 → 1.3). Purpose: catalogue language
features that modern engines rely on and that OmegaSL does not yet expose.

Feature areas already covered by dedicated plans are linked, not re-specified:

- **Raytracing / RayQuery** — `Raytracing-Full-Implementation-Plan.md`
- **Mesh / amplification shaders** — `Mesh-Shader-Implementation-Plan.md`
- **Sparse resources / sampler feedback** — `Sparse-Resources-SamplerFeedback-Plan.md`
- **Subgroup / wave ops + texture swizzle** — `OmegaSL-Swizzle-Subgroup-Plan.md`, `texture-swizzle-proposal.md`

This document catalogues the rest.

## Priority rubric

- **P0** — blocks common render/compute workloads that the engine already tries
  to support. These are "missing basics."
- **P1** — required for modern engines (bindless, subgroups, 16-bit types,
  indirect draw). Adoption is already mainstream across the three backends.
- **P2** — niche or backend-asymmetric. Useful but easy to work around.

---

## 1. Fragment stage — missing basics (P0)

### 1.1 `discard` / `clip` [COMPLETED]

No mechanism to kill a fragment. Every backend supports it:

| Backend | Syntax |
|--------|--------|
| HLSL   | `discard;` or `clip(x)` (where x < 0 discards) |
| MSL    | `discard_fragment();` |
| GLSL   | `discard;` |

**Proposal:** statement keyword `discard;`. Optionally `clip(expr)` as sugar
for `if(expr < 0) discard;`.

### 1.2 Fragment depth output [COMPLETED]

Cannot write `gl_FragDepth` / `SV_Depth` / `[[depth(any)]]`. Required for
anything that overrides hardware depth (impostors, parallax, soft particles).

**Proposal:** new semantic `Depth` usable on `float` fragment output field:

```
struct FragOut internal {
    float4 color : Color;
    float  depth : Depth;
};
```

Backends: `SV_Depth`, `[[depth(any)]]` (Metal requires `any`/`greater`/`less`
qualifier — default to `any`), `gl_FragDepth`.

### 1.3 Front-face, sample index, coverage [COMPLETED — input only]

| OmegaSL (proposed) | HLSL | MSL | GLSL |
|--------------------|------|-----|------|
| `bool : FrontFacing` | `SV_IsFrontFace` | `[[front_facing]]` | `gl_FrontFacing` |
| `uint : SampleIndex` | `SV_SampleIndex` | `[[sample_id]]` | `gl_SampleID` |
| `uint : Coverage` (in/out) | `SV_Coverage` | `[[sample_mask]]` | `gl_SampleMaskIn` / `gl_SampleMask` |

Needed for two-sided shading, MSAA-aware shading, alpha-to-coverage override.

Implemented as **fragment-shader parameter** attributes (not struct fields) —
each backend models these as per-fragment scalar inputs, and putting them in
the rasterizer struct would conflict with vertex/fragment struct sharing.
**Coverage** is input only for now; the output direction needs separate
plumbing (`gl_SampleMask` write, MSL `[[sample_mask]]` on output struct).

Alex: Input/Output coverage should be seperate attributes: InputCoverage/OutputCoverage. (Should solve in/out confusion)

### 1.4 Multiple color attachments [COMPLETED]

The current `float4` return from a fragment shader implicitly binds to
attachment 0. There is no way to write MRT output. Needed for G-buffer /
deferred pipelines.

**Proposal:** allow fragment shaders to return an `internal` struct whose
fields carry `Color(0)`, `Color(1)`, ... semantics — matching the existing
attribute syntax, extended with an index argument.

Bare `: Color` (no index) keeps its original meaning as a vertex→fragment
varying (HLSL `COLOR`, MSL untagged). Indexed `: Color(N)` is a fragment
output target (HLSL `SV_TargetN`, MSL `[[color(N)]]`, GLSL
`layout(location=N) out vec4`). The two stay disjoint at the syntax level
so a single struct can never be ambiguously interpreted.

### 1.5 Early depth/stencil attribute

No way to express `[earlydepthstencil]` / `layout(early_fragment_tests)` /
`[[early_fragment_tests]]`. Required when a compute-like fragment shader
writes UAVs and must not execute for occluded fragments.

**Proposal:** shader-level attribute `[early_depth]` on `fragment`.

### 1.6 Interpolation modifiers

Every internal struct field is currently interpolated with the default
perspective-correct linear mode. Backends all support modifiers:

| Modifier | HLSL | MSL | GLSL |
|----------|------|-----|------|
| flat | `nointerpolation` | `[[flat]]` | `flat` |
| centroid | `centroid` | `[[centroid_perspective]]` | `centroid` |
| sample | `sample` | `[[sample_perspective]]` | `sample` |
| noperspective | `noperspective` | `[[center_no_perspective]]` | `noperspective` |

**Proposal:** field-level modifiers on `internal` struct fields:

```
struct Raster internal {
    float4 pos    : Position;
    flat uint  id : InstanceID;
    noperspective float2 screenUV : TexCoord;
};
```

### 1.7 Clip / cull distance

User-defined clipping planes. Used for portals, water clip planes,
terrain tile boundaries.

**Proposal:** semantics `ClipDistance(N)` and `CullDistance(N)` on float
fields of internal structs emitted from vertex/hull/domain shaders.

---

## 2. Resource types — missing textures and buffers (P0)

### 2.1 Cube & array texture types

Currently only `texture1d/2d/3d` + matching samplers. Missing:

| Type | HLSL | MSL | GLSL |
|------|------|-----|------|
| `texturecube`       | `TextureCube` | `texturecube<T>` | `samplerCube` |
| `texturecube_array` | `TextureCubeArray` | `texturecube_array<T>` | `samplerCubeArray` |
| `texture1d_array`   | `Texture1DArray` | `texture1d_array<T>` | `sampler1DArray` |
| `texture2d_array`   | `Texture2DArray` | `texture2d_array<T>` | `sampler2DArray` |
| `texture2d_ms`      | `Texture2DMS<T>` | `texture2d_ms<T>` | `sampler2DMS` |
| `texture2d_ms_array`| `Texture2DMSArray<T>` | `texture2d_ms_array<T>` | `sampler2DMSArray` |

Cube textures alone block environment maps, skyboxes, IBL.

### 2.2 Depth textures + comparison samplers

Shadow mapping requires a sampler that does depth comparison, and a texture
that returns a single-channel depth:

| OmegaSL (proposed) | HLSL | MSL | GLSL |
|--------------------|------|-----|------|
| `depth2d` / `depth2d_array` / `depthcube` | `Texture2D<float>` (with comparison sampler) | `depth2d<float>` | `sampler2DShadow` |
| `sampler_cmp` static type | `SamplerComparisonState` | `sampler(compare_func=...)` | `samplerShadow` |
| `sampleCmp(s, t, coord, ref)` intrinsic | `SampleCmp` / `SampleCmpLevelZero` | `.sample_compare(s, coord, ref)` | `texture(samplerShadow, vec3(coord, ref))` |

### 2.3 Texture sampling variants

Current intrinsic `sample(s,t,c)` only emits the default LOD chooser. Missing:

| Intrinsic | Purpose |
|-----------|---------|
| `sampleLOD(s, t, c, lod)` | explicit mip level |
| `sampleBias(s, t, c, bias)` | LOD bias |
| `sampleGrad(s, t, c, ddx, ddy)` | gradient-based sampling (terrain, raytraced reflections) |
| `gather(s, t, c)` / `gatherRed/Green/Blue/Alpha` | PCF, screen-space effects |
| `getDimensions(t)` | query mip level dimensions |
| `calculateLOD(s, t, c)` | query LOD chosen by hardware |

### 2.4 Uniform / constant buffers

OmegaSL only exposes `buffer<T>` which maps to `StructuredBuffer` / SSBO. A
true constant buffer is distinct:

| Concept | HLSL | MSL | GLSL |
|---------|------|-----|------|
| Structured buffer | `StructuredBuffer<T>` (have) | `constant T*` (have) | `layout(std430) buffer` (have) |
| Constant buffer   | `cbuffer { ... }` / `ConstantBuffer<T>` | `constant T& [[buffer(0)]]` | `layout(std140) uniform` |

Constant buffers are smaller, have stricter alignment (std140 vs std430), but
live in a faster memory path on every vendor. Engines use them for
per-frame/per-view/per-draw constants. `buffer<T>` cannot substitute.

**Proposal:** new resource form, e.g.

```
uniform<T> perFrame : 0;
```

With a distinct `std140`-style layout contract and no indexing operator.

### 2.5 Raw / byte-address buffers

Byte-indexed scratch memory:

| HLSL | MSL | GLSL |
|------|-----|------|
| `ByteAddressBuffer` / `RWByteAddressBuffer` | `device uint*` with manual indexing | `layout(std430) buffer { uint data[]; }` |

Used by mesh / geometry storage, BVH traversal, debug streaming. Without it,
engines must define a dummy `buffer<uint>` and do the arithmetic by hand —
workable, but the alignment model is different and the D3D12 side has
dedicated `Load2/Load4` paths that OmegaSL can't express. (This can be semantically resolved.)

Alex Answer:

How about this?
```
raw<T> data : 0;
```

---

## 3. Control flow & language features (P0 → P1)

### 3.1 `switch` / `case` / `default`

Listed as not-implemented in the reference. Practical impact: common in
material ID fan-outs, tile classification, and generated code.

### 3.2 Ternary `?:`

Listed as not-implemented. Impact: verbose code for compound expressions;
blocks inline vector selection (`a ? v1 : v2`).

### 3.3 `do { } while`

Listed as not-implemented. Lower impact than switch/ternary.

### 3.4 Loop / branch attributes

| Attribute | HLSL | MSL | GLSL / SPIR-V |
|-----------|------|-----|---------------|
| unroll    | `[unroll]` / `[unroll(N)]` | no equivalent; relies on compiler | `[[unroll]]` via `SPV_KHR_loop_control` or extension pragmas |
| loop      | `[loop]` (disable unroll) | — | `[[dont_unroll]]` |
| branch    | `[branch]` | — | — |
| flatten   | `[flatten]` | — | — |

**Proposal:** accept `[unroll]` and `[unroll(N)]` everywhere; on backends that
don't support it, emit nothing. Rare to need the others.

### 3.5 Function overloading

Listed as not-implemented. Means every vector/scalar variant of a user helper
needs a unique name. Blocks shader libraries that emulate the builtin math
surface (`myLerp(float,float,float)` vs `myLerp(float3,float3,float)`).

### 3.6 `const` / `let` qualifier

No way to mark a local as immutable. All three backends support it and the
SPIR-V spec can benefit (codegen can use `OpConstant` / `OpSpecConstant`).

Alex Answer:

Use `const`.

### 3.7 Multiple return values

HLSL supports `out` parameters; MSL and GLSL both support them. OmegaSL's
reference does not document `out`/`inout` on function parameters (only on
resource-map keywords). Needed for `sincos(x, s, c)` and similar.

Alex Answer:

Allow `out`/`inout` to be used in function params.

---

## 4. Numeric types (P1)

### 4.1 16-bit types — `half` / `float16_t` / `int16_t` / `uint16_t`

Supported everywhere modern:

| Backend | Requirement |
|---------|-------------|
| HLSL    | SM 6.2, `-enable-16bit-types` |
| MSL     | `half` native since Metal 1.0 |
| GLSL    | `GL_EXT_shader_explicit_arithmetic_types_float16` / `int16` |
| SPIR-V  | `VK_KHR_shader_float16_int8` + storage extension |

Drops memory bandwidth in half for mobile / AI / post-processing passes.
Not currently reachable from OmegaSL.

**Proposal:** add `half`, `half2/3/4`, `short`/`ushort` + vec, matching
existing naming conventions. Back it by a `DeviceFeature::SixteenBitTypes`
probe so the engine can decline on hardware that doesn't support them.

### 4.2 64-bit integer types

| HLSL | MSL | GLSL |
|------|-----|------|
| `int64_t` / `uint64_t` (SM 6.0) | `long` / `ulong` | `GL_ARB_gpu_shader_int64` |

Needed for large hashes, 64-bit atomics, pointer arithmetic in bindless
descriptor indexing.

### 4.3 `double` — already rejected

Reference states intentional omission. No action.

---

## 5. Intrinsics — common functions currently missing (P0 → P1)

Grouped by how often engines reach for them.

### 5.1 Core math not yet listed

| Function | Reason |
|----------|--------|
| `sign(x)` | ubiquitous |
| `saturate(x)` | clamp to [0,1]; see §5.1.1 below for backend mapping and the name-collision policy |
| `fma(a,b,c)` / `mad(a,b,c)` | precision-guaranteed multiply-add |
| `mod(a,b)` / `fmod` / `trunc` | common |
| `rsqrt(x)` | 1/sqrt, single instruction |
| `degrees(r)` / `radians(d)` | common |
| `sinh/cosh/tanh` | needed in BRDFs |
| `modf` / `frexp` / `ldexp` | serialization / compression |

#### 5.1.1 `saturate` — backend mapping and the name-collision risk

`saturate(x)` clamps `x` to `[0, 1]`. It is one of the most common BRDF /
post-process idioms. Backend availability is asymmetric:

| Backend | Built-in? | Lowering |
|---------|-----------|----------|
| HLSL    | yes — `saturate(x)` is a first-class intrinsic on scalars and vectors. Emits a free saturate modifier on many ops. | passthrough |
| MSL     | yes — `metal::saturate(x)` (`<metal_common>`) on scalars/vectors. Emits the hardware saturate when free, else `clamp(x, 0, 1)`. | passthrough |
| GLSL    | **no** — there is no `saturate`. Idiom is `clamp(x, 0.0, 1.0)`. | rewrite to `clamp(x, vec_zero, vec_one)` per type |

**Proposal:** add `saturate(x)` to the OmegaSL builtin set as a 1-arg
intrinsic over `float`/`floatN`. HLSL/MSL passthrough, GLSL rewrites to
`clamp(x, 0.0, 1.0)` (broadcasted for vectors).

**Name-collision policy (already required as of bug-1 follow-up).** Until
`saturate` is a builtin, users define it themselves:

```omegasl
float saturate(float x){ return clamp(x, 0.0, 1.0); }
```

On Metal this collided with `metal::saturate` and produced an "ambiguous
call" error at every call site, even though Metal's stdlib saturate has the
same signature. MetalCodeGen now prefixes every user-defined function name
with `osl_user_` at emit time so user helpers can never shadow the Metal
stdlib. HLSL has the same exposure (every HLSL intrinsic is in the global
namespace and user functions can shadow but not co-exist with same-arity
overloads), so `HLSLCodeGen` should adopt the same prefix policy. GLSL
allows user functions with the same name as a builtin (the user definition
wins), so the prefix is not strictly required there, but applying it
uniformly across all three backends keeps generated source predictable and
removes a class of platform-dependent failures.

Once `saturate` (and the other §5.1 entries) become first-class OmegaSL
builtins, they should be reserved names — Sema rejects user redefinition
the same way it would reject redefining `sin` — and the prefix becomes a
defense-in-depth measure rather than a load-bearing one.

### 5.2 Vector math not yet listed

| Function | Purpose |
|----------|---------|
| `distance(a,b)` | length of difference |
| `faceforward(n,i,ng)` | flip normal toward viewer |
| `refract(i,n,eta)` | refraction vector |
| `any(v)` / `all(v)` | boolean reduce |
| `transpose(m)` / `determinant(m)` / `inverse(m)` | matrix ops — `inverse` is the notable one; HLSL doesn't have it and must be emulated per-size, but GLSL/MSL do |

### 5.3 Integer / bitfield ops

| Function | HLSL | MSL | GLSL |
|----------|------|-----|------|
| `countbits`       | yes | `popcount` | `bitCount` |
| `firstbitlow`     | yes | `ctz` | `findLSB` |
| `firstbithigh`    | yes | `clz` | `findMSB` |
| `reversebits`     | yes | `reverse_bits` | `bitfieldReverse` |
| `bitfieldExtract` / `bitfieldInsert` | yes | yes | yes |

Needed for hashing, compression formats, occupancy masks.

### 5.4 Derivatives — already on the "not implemented" list

`ddx`, `ddy`, `fwidth`, and the `_coarse` / `_fine` variants. Required for
mip-level selection in non-sampled paths, normal-map reconstruction,
anti-aliased lines/fonts.

### 5.5 Pack / unpack

| Function | HLSL | MSL | GLSL |
|----------|------|-----|------|
| `f16tof32` / `f32tof16` | yes | `as_type<half2>(uint)` / reverse | `unpackHalf2x16` / `packHalf2x16` |
| `pack_snorm4x8` / `unpack_snorm4x8` | no direct, pattern | `pack_float_to_snorm4x8` | `packSnorm4x8` |
| `asint` / `asuint` / `asfloat` | yes | `as_type<>` | `floatBitsToInt` family |

Bit-level reinterpretation; needed to decode vertex/mesh buffer formats that
the CPU side packs into `uint` channels.

### 5.6 Atomic operations

Already on the "not implemented" list. Recap of what's needed:

```
atomic_add(buf[i].counter, 1);
atomic_exchange(buf[i].slot, newValue);
atomic_compare_exchange(buf[i].slot, expected, desired);
atomic_min/max/and/or/xor(...);
```

All three backends have native support; Vulkan additionally has
`atomic_*` on images and on 64-bit integers via extensions.

---

## 6. Compute-shader features (P0 → P1)

### 6.1 Threadgroup / shared memory

Already on the "not implemented" list. Critical for any real compute work.

**Proposal:** storage-class keyword `threadgroup` applied to a variable
declared inside a compute shader:

```
threadgroup float tile[16][16];
```

Maps to `groupshared` (HLSL), `threadgroup` (MSL), `shared` (GLSL).

### 6.2 Barriers

Already on the "not implemented" list. Needs at minimum:

- `threadgroupBarrier()` — sync all threads in the group, include memory.
- `deviceBarrier()` — memory barrier across the device.

Extended variants (`acquire`/`release`, scoped to memory type) can come
later.

### 6.3 Wave / subgroup ops

Covered by `OmegaSL-Swizzle-Subgroup-Plan.md`. Cross-reference only.

### 6.4 Compute-stage builtins beyond the three currently exposed

Reference lists `GlobalThreadID`, `LocalThreadID`, `ThreadGroupID`. Missing:

| Builtin | HLSL | MSL | GLSL |
|---------|------|-----|------|
| Flat local index | `SV_GroupIndex` | `[[thread_index_in_threadgroup]]` | `gl_LocalInvocationIndex` |
| Dispatch size | — (constant) | `[[threadgroups_per_grid]]` | `gl_NumWorkGroups` |
| Threadgroup size | — (constant) | `[[threads_per_threadgroup]]` | `gl_WorkGroupSize` |

The flat local index in particular is heavily used to index shared-memory
tiles — without it engines must compute `lid.x + lid.y * GROUP_X + ...` by
hand.

### 6.5 Indirect dispatch / draw

Not a shading-language feature per se — it's a command-recording one — but
the language must expose `DrawArgs` / `DispatchArgs` struct types that
can live in a buffer and be read by the shader (visible via `DrawID` in
HLSL SM 6.1+). Flag for the runtime side to handle.

---

## 7. Bindless / descriptor-indexed resources (P1)

The single most impactful feature gap for modern engines. All three backends
now support it:

| Backend | Mechanism |
|---------|-----------|
| HLSL    | `ResourceDescriptorHeap[i]` / `SamplerDescriptorHeap[i]` (SM 6.6); `Texture2D<float4> tex[] : register(t0, space0)` earlier. |
| MSL     | Argument buffers (Metal 2+) — struct of resources bound as a single buffer. |
| Vulkan  | `VK_EXT_descriptor_indexing` + `nonuniformEXT(...)` / `NonUniformResourceIndex`. |

Practical surface OmegaSL would need:

1. Resource arrays of dynamic length: `texture2d tiles[] : 0;`
2. Non-uniform index marker: `sample(s, tiles[NonUniform(id)], uv)` — to
   satisfy the SPIR-V / HLSL requirement that divergent indexing be flagged.
3. Argument-buffer-ish struct type (Metal): compile a struct of resources
   and bind it whole.

This is a substantial design effort. Could be a separate plan doc.

---

## 8. Rasterizer / pipeline-state inputs the shader reads (P1)

### 8.1 `SV_RenderTargetArrayIndex` / `[[render_target_array_index]]` / `gl_Layer`

Write from vertex shader to select layer of a layered framebuffer. Used for
cubemap generation in one draw, VR stereo rendering, CSM.

### 8.2 `SV_ViewportArrayIndex` / `gl_ViewportIndex`

Same, for viewports.

### 8.3 `SV_PrimitiveID` / `[[primitive_id]]` / `gl_PrimitiveID`

Readable in fragment and geometry/mesh stages.

### 8.4 `SV_PointSize` / `[[point_size]]` / `gl_PointSize`

Writable from vertex shader. Required for point-sprite pipelines.

### 8.5 `SV_VertexID` with non-zero base / `SV_InstanceID` with non-zero base

Already exposed; note the HLSL/GLSL semantic difference — HLSL's `SV_VertexID`
starts at zero regardless of first-vertex, GLSL's `gl_VertexID` includes
`firstVertex`. The OmegaSL transpiler must normalize.

---

## 9. Geometry stage (P2)

HLSL/GLSL both support geometry shaders. Metal removed them after MSL 2. For
OmegaSL the tradeoff is:

- Supporting geometry shaders fully on Metal would require emulation via
  compute + append buffer (complex).
- Declining them preserves portability but rules out point-sprite
  expansion, line-to-tube expansion, etc.

Most engines have moved to mesh shaders (already planned). Recommend
**skipping geometry shaders entirely** and documenting this as a policy
choice rather than a missing feature.

---

## 10. Vulkan-only / SPIR-V features (P1 → P2)

### 10.1 Subpass inputs

Vulkan-specific:

```glsl
layout(input_attachment_index=0, binding=0) uniform subpassInput inputColor;
vec4 c = subpassLoad(inputColor);
```

No equivalent on D3D12 / Metal. Used for efficient tile-local deferred on
mobile. OmegaSL could add `subpass_input` + `loadSubpass()` that lowers to
no-op / error on the other backends.

### 10.2 Push constants 

Vulkan + D3D12 root constants + Metal inline `constant` under 4KB.

Exposing this as a first-class concept would need a new resource keyword
(`constant<T>` or similar) with strict size limits. Very performance-relevant
for per-draw constants.

### 10.3 Specialization constants

Already on the "not implemented" list. Worth doing for Vulkan (`OpSpecConstant`)
— lets pipelines be specialized at create time rather than through `#define`
before compile.

### 10.4 Variable rate shading

`SV_ShadingRate` / VK_KHR_fragment_shading_rate. Low priority; niche.

---

## 11. Numeric / precision modifiers (P2)

### 11.1 `precise` / `invariant`

HLSL's `precise` keyword + GLSL's `invariant` + MSL's `[[invariant]]` prevent
the compiler from reordering floating-point ops. Needed for Z-fighting-safe
position output in depth passes and for physics determinism.

### 11.2 GLSL `highp` / `mediump` / `lowp`

Irrelevant on desktop, matters on mobile / GLES. Probably skip until OmegaSL
targets mobile GLES.

---

## 12. Cross-backend semantic alignment

Cases where the OmegaSL surface is *uniform* at the syntax level but the
generated code has *different runtime behavior* across backends. These are
not feature gaps — the operation already exists in the language — but the
lowering doesn't preserve semantic equivalence. High priority because the
divergence is silent: the same source compiles and runs on every target,
just with different results.

### 12.1 Matrix indexing — column-major alignment with `OmegaCommon::Matrix`

The engine's host-side matrix type
(`gte/include/omegaGTE/GTEBase.h:598–757`,
`OmegaCommon::Matrix<Ty, column, row>`) is **column-major in source**:
`_data` is `std::array<std::array<Ty, row>, column>`, `m.at(idx)` asserts
`idx < column`, and `m[col][row]` retrieves the element at column `col`,
row `row`. OmegaSL should match this so a matrix written with
`m[col][row] = …` on the CPU side reads with the same expression on the
GPU side.

**Current state.** Matrix `INDEX_EXPR` in `MetalCodeGen` / `GLSLCodeGen` /
`HLSLCodeGen` all emit `m[idx]` literally. Per-backend `m[i]` semantics:

| Backend | `m[i]` returns | `m[i][j]` is element at | Matches GTEBase.h? |
|---------|----------------|-------------------------|--------------------|
| GLSL    | column i       | (row=j, col=i)          | yes |
| MSL     | column i       | (row=j, col=i)          | yes |
| HLSL    | **row i**      | **(row=i, col=j)**      | **no** |

So `t[0][1]` reads element (0, 1) on HLSL but element (1, 0) on GLSL/MSL.
The existing `matrix_ops.omegasl` test passes today only because it asserts
compile success, not semantic equivalence — line 42's
`float4(t[0][0], t[0][1], t[1][0], t[1][1])` produces backend-dependent
output.

**Proposal.** Lock OmegaSL matrix indexing to the column-major source
convention (`m[col]` is column `col`; `m[col][row]` is the element at column
`col`, row `row`). HLSLCodeGen rewrites at emit time; GLSL and MSL stay
unchanged.

| OmegaSL | HLSL emit |
|---------|-----------|
| `m[a][b]` (matrix, two-level read) | `m[b][a]` — swap |
| `m[a]` (matrix, single-level read) | `floatN(m[0][a], m[1][a], …, m[N-1][a])` — synthesized column |
| `m[a] = v` (matrix, single-level write) | rejected by Sema — see option A below |
| `m[a][b] = v` (matrix, two-level write) | `m[b][a] = v` — swap |
| `v[i]`, `buf[i]`, ... | `lhs[idx]` — unchanged |

#### Patch

**1. `Sema.cpp` — propagate `resolvedType` on `INDEX_EXPR` subexpressions.**

Mirrors what `BINARY_EXPR` already does at Sema.cpp:538–539. Currently
`INDEX_EXPR` Sema computes types but doesn't write them back onto the AST,
so codegen can't ask "is the lhs of this `INDEX_EXPR` itself an
`INDEX_EXPR` whose lhs resolves to a matrix?" — a question the rewrite
needs.

```cpp
else if(expr->type == INDEX_EXPR){
    auto _expr = (ast::IndexExpr *)expr;
    auto lhs_res = performSemForExpr(_expr->lhs, funcContext);
    if(!lhs_res) return nullptr;
    auto idx_expr_res = performSemForExpr(_expr->idx_expr, funcContext);
    if(!idx_expr_res) return nullptr;

    /// Set resolvedType on sub-expressions for type-aware codegen
    /// (matrix-indexing rewrite on HLSL).
    _expr->lhs->resolvedType = lhs_res;
    _expr->idx_expr->resolvedType = idx_expr_res;

    auto _t = resolveTypeWithExpr(lhs_res);
    /// ... existing element-type return logic ...
}
```

**2. `Sema.cpp` — reject single-level matrix write (option A).**

In `performSemForExpr` `BINARY_EXPR` handling, after types resolve, detect
the `=` family on a single-level matrix `INDEX_EXPR` lhs and reject:

```cpp
/// Single-level matrix lvalue is not portably representable across all
/// backends (HLSL needs per-row statement expansion); require the user
/// to write the two-level form `m[col][row] = …`.
if(_expr->op == OP_EQUAL || _expr->op == OP_PLUSEQUAL || /* etc. */){
    if(_expr->lhs->type == INDEX_EXPR){
        auto *idx = (ast::IndexExpr *)_expr->lhs;
        auto innerTy = resolveTypeWithExpr(idx->lhs->resolvedType);
        if(innerTy && isMatrixType(innerTy)){
            auto e = std::make_unique<TypeError>(
                "Cannot assign to a matrix column; use two-level "
                "indexing `m[col][row] = …`.");
            e->loc = _expr->loc.value_or(ErrorLoc{});
            diagnostics->addError(std::move(e));
            return nullptr;
        }
    }
}
```

**3. `HLSLCodeGen.cpp` — rewrite `INDEX_EXPR`.**

Replace the literal `m[idx]` emit with three branches. Add a small helper
`hlslColumnVectorTypeForMatrix(t)` that returns `"float2"`/`"float3"`/
`"float4"` based on the matrix row count, paralleling `matrixRowCount`.

```cpp
case INDEX_EXPR: {
    auto _expr = (ast::IndexExpr *)expr;

    /// (a) Outer of a two-level matrix index — swap to HLSL's
    /// row-first convention. OmegaSL `m[col][row]` -> HLSL `m[row][col]`.
    if(_expr->lhs->type == INDEX_EXPR){
        auto *inner = (ast::IndexExpr *)_expr->lhs;
        auto innerLhsTy = typeResolver->resolveTypeWithExpr(inner->lhs->resolvedType);
        if(innerLhsTy && isMatrixType(innerLhsTy)){
            generateExpr(inner->lhs);
            shaderOut << "[";
            generateExpr(_expr->idx_expr);
            shaderOut << "][";
            generateExpr(inner->idx_expr);
            shaderOut << "]";
            break;
        }
    }

    /// (b) Single-level matrix read — synthesize the column.
    auto lhsTy = typeResolver->resolveTypeWithExpr(_expr->lhs->resolvedType);
    if(lhsTy && isMatrixType(lhsTy)){
        unsigned rows = matrixRowCount(lhsTy);
        const char *colTy = hlslColumnVectorTypeForMatrix(lhsTy);
        shaderOut << colTy << "(";
        for(unsigned i = 0; i < rows; ++i){
            if(i > 0) shaderOut << ", ";
            generateExpr(_expr->lhs);
            shaderOut << "[" << i << "][";
            generateExpr(_expr->idx_expr);
            shaderOut << "]";
        }
        shaderOut << ")";
        break;
    }

    /// (c) Default — vector / buffer / non-matrix.
    generateExpr(_expr->lhs);
    shaderOut << "[";
    generateExpr(_expr->idx_expr);
    shaderOut << "]";
    break;
}
```

Side-effect concern in branch (b): synthesising a column reads `_expr->lhs`
N times. For OmegaSL today (no impure user functions, `_expr->lhs` is
typically an identifier or member access), this is benign. Revisit if
pure-function tracking lands.

**4. `MetalCodeGen.cpp` / `GLSLCodeGen.cpp` — no change.**

Both already match the column-major source convention.

**5. Tests.**

- `matrix_index.omegasl` — exercises `m[col][row]` reads (square + non-square),
  single-level reads (`float4 col0 = m[0];`), and two-level writes
  (`m[1][2] = 3.0;`). Compile-only across all three backends.
- `invalid_matrix_column_write.omegasl` — `m[1] = float4(...);` on a
  matrix; expected to fail Sema with the diagnostic from step 2.
- The existing `matrix_ops.omegasl` continues to compile but its
  HLSL-vs-GLSL output now agrees on which elements are read.

**6. Reference docs.** `OmegaSL-Reference.md` §2.3 (matrix types) gains:

> Matrix indexing follows the engine's host-side `OmegaGTE::Matrix<Ty,
> column, row>` convention: `m[col][row]` accesses the element at column
> `col`, row `row`. `m[col]` (single-level) returns the column at index
> `col` as a vector. Matrices cannot be assigned a column directly — write
> per-element with `m[col][row] = …` or per-row in a loop.

#### Rollout

1. Sema `resolvedType` propagation on `INDEX_EXPR` (low-risk, pure metadata).
2. HLSL codegen rewrite (branches a + b + c above) with `matrix_index.omegasl`
   as the regression test. Any HLSL golden output that touches matrix
   indexing will need to be re-baked.
3. Sema rejection of single-level matrix writes (ergonomic break — emit a
   clear diagnostic so the migration is mechanical).

#### Out of scope

- **Storage layout.** The host `Matrix<Ty, col, row>` lays out rows
  contiguously inside columns. HLSL constant-buffer / SSBO matrix layout
  obeys the `column_major` / `row_major` storage qualifier; Vulkan SPIR-V
  matrix layout obeys the `RowMajor`/`ColMajor` decoration. Source-level
  index alignment (this section) is independent of memory-layout alignment.
  Tracking separately; recommend emitting `column_major` uniformly when we
  get to it.
- **Single-level matrix writes** (option B in earlier drafts). Statement
  expansion of `m[a] = v` → `m[0][a] = v.x; m[1][a] = v.y; …` requires the
  codegen to grow a "lower expression to statement sequence" pass. Defer
  until that infrastructure exists for other reasons.
- **Mat × Vec / Vec × Mat / `transpose` / `determinant`.** These don't
  use indexing syntax and already produce identical results across
  backends; no rewrite needed.

### 12.2 Matrix methods on `GEBufferWriter` / `GEBufferReader` + D3D12 packing lock

The host-side buffer R/W API in `gte/include/omegaGTE/GTEShader.h:18–54`
exposes scalar / vec2 / vec3 / vec4 methods for `float`, `int`, and
`uint` — but **no matrix methods**. The `omegasl_data_type` enum
(`gte/include/omegasl.h:76–87`) already enumerates `OMEGASL_FLOATCxR`
for every (C, R) ∈ {2,3,4}², but the per-backend writer/reader
implementations don't handle them: Vulkan's `sizeForType` returns 0
for matrix types (`GEVulkan.cpp:425–441`), and the D3D12 / Metal writers
have no matrix branches at all. So shaders that consume a
`buffer<Struct>` with a matrix field have no upload path today.

Adding the methods is straightforward — but only safe to do once §12.1
lands and the cross-backend storage is **explicitly locked** to a
single layout. Today the HLSL output happens to default to column-major
packing because `D3DCompile` is invoked with no packing flag
(`HLSLCodeGen.cpp:823`), and `D3DCompile`'s default is column-major.
That "happens to" is load-bearing for cross-backend correctness and
should not stay implicit.

**Goal.** Standardize on **column-major in memory** across host, GLSL,
HLSL, and MSL — matching `OmegaCommon::Matrix<Ty, column, row>` storage
(`GTEBase.h:688`) so a matrix `memcpy`'d into a buffer reads correctly
from every shader stage on every backend. Add matrix methods to the
buffer R/W API, and lock the HLSL packing explicitly.

#### Memory-layout walkthrough (why "column-major" is the right pick)

For a `float4x4` (4 columns × 4 rows) holding (col, row) elements
A=(0,0), E=(0,1), …, P=(3,3):

| Layout site | Default behavior | Column-major bytes? |
|---|---|---|
| Host `Matrix<float,4,4>` (`std::array<std::array<float,4>,4>`) | column 0's 4 floats first, then column 1's, … | ✅ yes |
| GLSL std430 / std140 matrix field | column-major, `MatrixStride = 16` | ✅ yes |
| MSL `float4x4` in `device`/`constant` buffer | column-major | ✅ yes |
| HLSL `D3DCompile` no-flag default | column-major | ✅ yes (but **implicit**) |

Source-level access semantics (after §12.1) will match across all four
even when memory is column-major — that's the whole point of §12.1.
Memory-side, three of the four are already locked by spec; HLSL is
locked only by D3DCompile's current default flag, which §12.2 will
make explicit.

#### Patch

**1. `gte/include/omegaGTE/GTEShader.h` — extend `GEBufferWriter`.**

```cpp
struct GEBufferWriter {
    // … existing scalar / vec methods …

    /// Square matrix writes.
    virtual void writeFloat2x2(FMatrix<2,2> & m) = 0;
    virtual void writeFloat3x3(FMatrix<3,3> & m) = 0;
    virtual void writeFloat4x4(FMatrix<4,4> & m) = 0;

    /// Rectangular matrix writes.
    virtual void writeFloat2x3(FMatrix<2,3> & m) = 0;
    virtual void writeFloat2x4(FMatrix<2,4> & m) = 0;
    virtual void writeFloat3x2(FMatrix<3,2> & m) = 0;
    virtual void writeFloat3x4(FMatrix<3,4> & m) = 0;
    virtual void writeFloat4x2(FMatrix<4,2> & m) = 0;
    virtual void writeFloat4x3(FMatrix<4,3> & m) = 0;
};
```

Symmetric `getFloat<C>x<R>(FMatrix<C,R> & m)` on `GEBufferReader`. The
`FMatrix<C,R>` template aliases to `Matrix<float,C,R>` (`GTEBase.h:941`),
which already lays storage out column-major; the writer just `memcpy`'s
the underlying `_data` block after the std140/std430 column padding is
applied.

**2. `Sema.cpp` / `omegasl.h` — std430/std140 stride math for matrices.**

Add the matrix entries to `std430AlignmentForType` and `sizeForType` in
each backend (or, better, factor them into the shared
`omegaSLStructStride` helper that the docs already reference at
`GTEShader.h:16`).

```cpp
// Matrix is C columns of R-element column vectors. Each column is
// aligned and sized as if it were the matching column-vector type.
// std430:
//   colAlign = (R == 1 ? 4 : R == 2 ? 8 : 16)
//   colSize  = R * 4 (no per-column padding for R == 1, 2, 4)
//             but R == 3 columns are padded to 16 bytes (vec3 quirk).
// matrix stride = aligned column size.
size_t std430MatrixStride(unsigned R){
    if(R == 1) return 4;
    if(R == 2) return 8;
    if(R == 3) return 16;  // padded
    return 16;              // R == 4
}
size_t std430MatrixSize(unsigned C, unsigned R){
    return C * std430MatrixStride(R);
}
```

This handles the `mat3` / `float3xN` quirk where a 3-row column is
padded to 16 bytes — the host `Matrix<float,C,3>` packs three contiguous
floats per column with no padding, so the writer must insert four bytes
of padding per column when targeting std430. (This is the only case
where host bytes ≠ shader bytes; symmetry-breaking caveat documented
below.)

**3. `gte/src/vulkan/GEVulkan.cpp` — implement `writeFloat<C>x<R>`.**

```cpp
void writeFloat4x4(FMatrix<4,4> &m) override {
    // Host already column-major: 4 columns × 4 rows = 16 contiguous floats.
    // std430 stride for mat4 = 64 bytes (no padding). Direct memcpy works.
    blocks.push_back(DataBlock {OMEGASL_FLOAT4x4, new FMatrix<4,4>(m)});
}
void writeFloat3x3(FMatrix<3,3> &m) override {
    // Host: 3 columns × 3 rows = 9 contiguous floats (36 bytes).
    // std430 expects each column padded to 16 bytes → 48 bytes total.
    // Allocate 48 bytes, copy each column with padding.
    auto *padded = new float[12]{};
    for(unsigned c = 0; c < 3; ++c){
        padded[c * 4 + 0] = m[c][0];
        padded[c * 4 + 1] = m[c][1];
        padded[c * 4 + 2] = m[c][2];
        // padded[c * 4 + 3] stays 0
    }
    blocks.push_back(DataBlock {OMEGASL_FLOAT3x3, padded});
}
// ... and rectangular variants ...
```

The Vulkan-side reader does the inverse: read columns at the stride
returned by `std430MatrixStride(R)`, copy `R` floats per column into
`m._data[c]`.

**4. `gte/src/metal/GEMetal.mm` — implement `writeFloat<C>x<R>`.**

Metal's `metal::matrix` template stores column-major with the same
column-padding quirk for non-multiple-of-4 row counts. Implementation
mirrors Vulkan but uses Metal's preferred memory order (which is
identical for `float4x4`). Reuse the std430 helpers — Metal's argument
buffer / device pointer matrix layout matches.

**5. `gte/src/d3d12/GED3D12.cpp` — implement `writeFloat<C>x<R>` AND
explicitly lock packing.**

Two D3D12 changes:

a. **Implement the methods** the same way as Vulkan/Metal (column-major
   bytes with std430-equivalent column padding for `Cx3` matrices).
   `D3DCompile` with the default flag interprets this correctly.

b. **Lock packing explicitly** so the implicit "D3DCompile default = column-major"
   stops being load-bearing:

```cpp
// HLSLCodeGen::compileShaderOnRuntime, gte/omegasl/src/HLSLCodeGen.cpp:823
D3DCompile(source.data(), source.size(), name.data(), nullptr,
           D3D_COMPILE_STANDARD_FILE_INCLUDE, name.data(),
           target.data(),
           D3DCOMPILE_DEBUG | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR,
           NULL, &blob, &errorBlob);
```

And mirror the flag on the offline-compile path that uses `dxc`
(`HLSLCodeGen.cpp:771`): add `-Zpc` to the `dxc` invocation.

c. **Belt-and-suspenders: emit `column_major` qualifier** on every
   matrix-typed field in HLSL output. In `HLSLCodeGen::writeTypeExpr`,
   when the target type is a matrix, prefix `column_major ` to the
   emitted type name. This ensures the storage qualifier is correct
   even if some downstream tool ever recompiles the HLSL with `/Zpr`.

If both (b) and (c) feel redundant, prefer (c) — it's robust to
compiler-flag drift and travels with the source. (b) is cheap insurance.

**6. `gte/src/common/` — backend-shared helpers.**

`std430MatrixStride` / `std430MatrixSize` and a `matMemcpyToShaderStd430`
helper land in a single shared header so all three backends call into
the same column-padding logic. Avoids the three-copy drift that
`sizeForType` already has between Vulkan/D3D12/Metal.

#### Tests

- `matrix_buffer_roundtrip.omegasl` + a host-side test that writes a
  known `FMatrix<4,4>` via `GEBufferWriter::writeFloat4x4`, dispatches a
  compute shader that copies the matrix to an output buffer, reads it
  back via `GEBufferReader::getFloat4x4`, and asserts every element
  matches. Run on all three backends.
- `matrix_3x3_padded.omegasl` — same shape, with `float3x3`. Verifies
  the column-padding path on each backend.
- `matrix_indexing_after_upload.omegasl` (combined §12.1 + §12.2) —
  upload a non-symmetric matrix, in-shader read via `m[col][row]`,
  write the (col, row) element back, host-read it, assert it matches
  the original `m._data[col][row]`. The test that catches both source-
  level and memory-level divergence in one go.

#### Reference docs

`OmegaSL-Reference.md` §2.3 (matrix types) gains a sentence: "Matrices
are stored column-major in GPU memory across all backends. Use
`GEBufferWriter::writeFloat<C>x<R>` to upload and the matching
`GEBufferReader::getFloat<C>x<R>` to download — the API handles the
std430 column padding for `Cx3` matrices." Cross-link §12.1.

#### Rollout

1. Land §12.1 first (source-level alignment). Without it, `m[col][row]`
   means different things in different backends and the buffer test
   would fail for reasons unrelated to memory layout.
2. Land the shared `std430Matrix*` helpers and the API additions on
   `GTEShader.h`.
3. Implement matrix methods in Vulkan and Metal (already-column-major
   targets — pure additive work).
4. Implement matrix methods in D3D12, with the packing lock
   (`D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR`, `-Zpc`,
   `column_major` qualifier in HLSLCodeGen) in the same change so the
   guarantee is intact from day one.
5. Add the round-trip tests on all three backends.

#### Out of scope

- **`int` / `uint` matrix types** in the buffer API. The shader language
  only declares `float` matrices today (§2.3 of the reference). Adding
  the integer matrix path is straightforward when needed.
- **`double` matrix types.** OmegaSL has no `double` matrix type
  (intentional per §4.3) so no API surface needed.
- **Runtime transposition fallback** — the world where HLSL gets
  compiled with row-major packing and we need to transpose at upload
  time on D3D12. The patch above prevents that world from existing
  rather than handling it. If a future use case needs row-major HLSL
  (e.g. interop with a D3D9-era asset format), add a `WriteMode` enum
  parameter to the writer and a transpose path inside D3D12's
  `writeFloat<C>x<R>` that flips before the `memcpy`.
- **Push constants / root constants matrix layout.** Push constants
  follow the same column-major default but are a separate channel from
  `GEBufferWriter` (§10.2). When push-constant support lands, the same
  std430-equivalent helpers apply.

---

## 13. Summary — what to build first

Recommended ordering if the goal is to close the biggest gaps:

1. **Fragment basics** — discard, depth output, FrontFacing, interpolation
   modifiers, MRT, early-depth attribute. (§1)
2. **Texture types + samplers** — cube, arrays, MS, depth/comparison,
   `sampleLOD/Grad/Cmp/gather`, `getDimensions`. (§2.1–§2.3)
3. **Uniform/constant buffer** — distinct from structured, std140-ish layout.
   (§2.4)
4. **Switch / ternary / `out` params / loop attrs**. (§3)
5. **Threadgroup memory + barriers + atomics**. (§6.1, §6.2, §5.6)
6. **Intrinsics gap** — `saturate`, `sign`, `any`/`all`, `mod`, bit-ops,
   derivatives, pack/unpack, bit-cast. (§5)
7. **16-bit types** behind a device-feature probe. (§4.1)
8. **Bindless / descriptor indexing**. (§7) — separate design doc.
9. **Push constants + specialization constants**. (§10.2, §10.3)
10. **Compute builtins**: flat local index, dispatch-size query. (§6.4)

Steps 1–4 deliver a shading language that can express every non-compute
feature of Vulkan 1.0 / D3D12 / Metal 2 on equal footing with hand-written
shaders. Step 5 does the same for baseline compute. 6 closes a long tail
of one-line fixes. 7–10 bring the language up to mainstream modern engines.
