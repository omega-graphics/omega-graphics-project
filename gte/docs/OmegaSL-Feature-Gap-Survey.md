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

## 1. Fragment stage — missing basics (P0) [COMPLETED]

### 1.1 `discard` / `clip`

No mechanism to kill a fragment. Every backend supports it:

| Backend | Syntax |
|--------|--------|
| HLSL   | `discard;` or `clip(x)` (where x < 0 discards) |
| MSL    | `discard_fragment();` |
| GLSL   | `discard;` |

**Proposal:** statement keyword `discard;`. Optionally `clip(expr)` as sugar
for `if(expr < 0) discard;`.

### 1.2 Fragment depth output

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

### 1.3 Front-face, sample index, coverage

| OmegaSL (proposed) | HLSL | MSL | GLSL |
|--------------------|------|-----|------|
| `bool : FrontFacing` | `SV_IsFrontFace` | `[[front_facing]]` | `gl_FrontFacing` |
| `uint : SampleIndex` | `SV_SampleIndex` | `[[sample_id]]` | `gl_SampleID` |
| `uint : Coverage` (in/out) | `SV_Coverage` | `[[sample_mask]]` | `gl_SampleMaskIn` / `gl_SampleMask` |

Needed for two-sided shading, MSAA-aware shading, alpha-to-coverage override.

### 1.4 Multiple color attachments

The current `float4` return from a fragment shader implicitly binds to
attachment 0. There is no way to write MRT output. Needed for G-buffer /
deferred pipelines.

**Proposal:** allow fragment shaders to return an `internal` struct whose
fields carry `Color(0)`, `Color(1)`, ... semantics — matching the existing
attribute syntax, extended with an index argument.

Research exactly how each backend API expects this.

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
(`push<T>` or similar) with strict size limits. Very performance-relevant
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

## 12. Summary — what to build first

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
