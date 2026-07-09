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

### 1.3 Front-face, sample index, coverage [COMPLETED]

| OmegaSL | HLSL | MSL | GLSL |
|---------|------|-----|------|
| `bool : FrontFacing` (param)              | `SV_IsFrontFace` | `[[front_facing]]` | `gl_FrontFacing` |
| `uint : SampleIndex` (param)              | `SV_SampleIndex` | `[[sample_id]]`    | `gl_SampleID` |
| `uint : InputCoverage` (param)            | `SV_Coverage`    | `[[sample_mask]]`  | `gl_SampleMaskIn[0]` (uint-cast) |
| `uint : OutputCoverage` (output struct field) | `SV_Coverage` | `[[sample_mask]]` | `gl_SampleMask[0]` (int-cast write-back from synthetic uint local) |

Needed for two-sided shading, MSAA-aware shading, alpha-to-coverage override.

Per-fragment scalar inputs are modelled as **fragment-shader parameter**
attributes (not struct fields) — each backend models these as per-fragment
scalar inputs, and putting them in the rasterizer struct would conflict with
vertex/fragment struct sharing. The output direction is modelled as a field
on the fragment-output internal struct, alongside `Color(N)` and `Depth`.

Coverage is split into two distinct attribute names (per Alex's note) so
in/out direction is unambiguous from the syntax: `InputCoverage` is only
valid as a fragment parameter; `OutputCoverage` is only valid as a field of
a fragment-output internal struct.

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

### 1.5 Early depth/stencil attribute [COMPLETED]

No way to express `[earlydepthstencil]` / `layout(early_fragment_tests)` /
`[[early_fragment_tests]]`. Required when a compute-like fragment shader
writes UAVs and must not execute for occluded fragments.

**Landed (as `fragment(early_depth)`, per Alex).** The original proposal
spelled it `[early_depth]`, but `[...]` is the resource map — a bare
`[early_depth]` collides with it. The descriptor is instead a parenthesized
clause on the `fragment` keyword, parallel to `compute(x=,y=,z=)` /
`hull(domain=…)` / `domain(domain=…)`:

```
[out coverageBuf]
fragment(early_depth) float4 shade(VertexRaster raster){ ... }
```

The descriptor is optional; a plain `fragment` is byte-identical to before.

Per-backend emission:

| Backend | Emission | Placement |
|---------|----------|-----------|
| HLSL | `[earlydepthstencil]` | decorator line before the entry function |
| MSL  | `[[early_fragment_tests]]` | immediately before the `fragment` qualifier |
| GLSL | `layout(early_fragment_tests) in;` | global execution-mode directive in the `.frag`, ahead of `void main()` |

This is **purely source-level**: the attribute bakes into the generated
shader (HLSL bytecode attribute / MSL function attribute / SPIR-V execution
mode), so there is no `omegasl_shader` runtime descriptor field and **no
feature-bit gate** — every backend supports early fragment tests
universally (matching `discard` / depth-output / MRT, which also carry no
feature bit).

**Implementation**

* **AST**: `ShaderDecl::earlyDepthStencil` (`bool`, default `false`). The
  parser sets it only in the `fragment` arm, so it is true only on a
  Fragment shader — no Sema stage check is needed because the descriptor
  is structurally unreachable on any other stage.
* **Lexer**: a one-token putback (`Lexer::putBack` + a `std::optional<Tok>`
  buffer consumed at the top of `nextTok()`). `fragment(early_depth)` is
  *optional* — unlike compute's mandatory `(...)` — and `parseGlobalDecl`
  reads the raw lexer with no lookahead. The `fragment` arm peeks one
  token; if it isn't `(`, the token is pushed back so the shared advance
  re-reads it, keeping the plain-`fragment` path unchanged.
* **Parser**: the `fragment` arm parses `(early_depth)` when present and
  leaves the cursor on the closing `)` (so the shared post-stage advance
  reaches the return type, exactly like compute/hull/domain). An unknown
  token inside the parens (`fragment(early_stencil)`) or a missing `)` is
  rejected with a clear diagnostic.
* **CodeGen**: each target reads the flag in `emitShaderEntryHeader` and
  emits the attribute at the placement above.

**Tests**: `early_depth.omegasl` (a fragment that writes a storage buffer —
the UAV use case — with `[out coverage] fragment(early_depth)`, exercising
resource-map + descriptor coexistence), `invalid_early_depth.omegasl`
(`fragment(early_stencil)` — unknown descriptor token rejected).

**Out of scope (follow-ups)**

* No warning when `early_depth` co-occurs with `discard` or a `Depth`
  output. All three backends accept the combination (the depth write is
  honored but early-Z may have already run); OmegaSL's existing style
  trusts the developer here (cf. `switch` fall-through).
* `early_depth` does not participate in any stage/overload diagnostics
  beyond the parse path.

### 1.6 Interpolation modifiers [COMPLETED]

Every internal struct field is currently interpolated with the default
perspective-correct linear mode. Backends all support modifiers:

| Modifier | HLSL | MSL | GLSL |
|----------|------|-----|------|
| flat | `nointerpolation` | `[[flat]]` | `flat` |
| centroid | `centroid` | `[[centroid_perspective]]` | `centroid` |
| sample | `sample` | `[[sample_perspective]]` | `sample` |
| noperspective | `noperspective` | `[[center_no_perspective]]` | `noperspective` |

**Landed.** Field-level modifiers prefix the type on an `internal` struct
field:

```
struct Raster internal {
    float4 pos                    : Position;
    flat uint id                  : TexCoord;
    noperspective float2 screenUV : TexCoord;
};
```

* **Contextual keywords.** `flat` / `centroid` / `sample` / `noperspective`
  stay lexed as `TOK_ID` and are recognized as a modifier only when they
  prefix a field whose type is a builtin (`TOK_KW_TYPE`) — you never
  interpolate a struct, so the type after a modifier is always numeric. This
  is *required* for `sample` (it doubles as the `sample(s,t,c)` texture
  intrinsic) and keeps fields/locals named `flat`/`sample`/etc. working. The
  modifier rides on `AttributedFieldDecl::interp`
  (`Default`/`Flat`/`Centroid`/`Sample`/`NoPerspective`); `Default` emits no
  qualifier and is byte-identical to pre-§1.6 output.
* **Per backend.** HLSL prefixes the struct-field type; MSL adds the member
  attribute (`[[flat]]` / `[[centroid_perspective]]` /
  `[[sample_perspective]]` / `[[center_no_perspective]]`); GLSL puts the
  qualifier between `layout(location=N)` and the `in`/`out` storage qualifier
  on the varying. The same field drives both the vertex `out` and fragment
  `in` varying, so the qualifier matches across stages by construction.
* **Integer-varying rule (enforced, per Alex).** An integer interstage
  varying cannot be interpolated — HLSL (`nointerpolation`) and GLSL (`flat`)
  both reject a non-flat integer varying. Sema rejects an integer-typed
  (`int`/`uint`/`short`/`ushort`/`long`/`ulong`) varying field that lacks a
  modifier. System-value / output semantics (Position, Depth,
  OutputCoverage, indexed Color(N)) are exempt.
* **Internal structs only.** A modifier on a non-`internal` struct field is
  rejected at parse time (interpolation only applies to shader I/O).

Tests: `interp_modifiers.omegasl` (all four modifiers across int/float
varyings, integer `id` flat), `invalid_interp_modifier.omegasl` (integer
varying missing `flat`).

### 1.7 Clip / cull distance [COMPLETED]

User-defined clipping planes. Used for portals, water clip planes,
terrain tile boundaries.

**Landed (as a float-array field, per Alex).** The original proposal spelled
it `ClipDistance(N)` (per-plane index), but HLSL has only two
`SV_ClipDistance` registers, so per-plane indices have no clean lowering. A
single `float[N]` field carries all planes instead:

```
struct Raster internal {
    float4 pos     : Position;
    float  clip[2] : ClipDistance;
    float  cull[1] : CullDistance;   // file must #requires(CULL_DISTANCE)
};
```

(A scalar `float : ClipDistance` is also accepted, for a single plane.)

* **Field type.** A `float` scalar or `float[N]` array; at most one array
  dimension; no index — Sema enforces all three. General array-typed struct
  fields (the `[N]` suffix in a struct-field declaration) landed alongside
  this, a capability beyond clip/cull.
* **Per backend.** HLSL `SV_ClipDistance` / `SV_CullDistance`; MSL
  `[[clip_distance]]`; GLSL routes writes to the `gl_ClipDistance[]` /
  `gl_CullDistance[]` builtin arrays — a field write `r.clip[i] = x` becomes
  `gl_ClipDistance[i] = x` through the same member-reroute that handles
  `Position`→`gl_Position`, and the builtin is redeclared with an explicit
  size (`out float gl_ClipDistance[N];`) on the producing stage. Clip/cull
  are not fragment inputs, so the fragment side emits no varying for them.
* **CullDistance is Metal-gated.** Metal has `[[clip_distance]]` but no
  cull-distance equivalent, so `CullDistance` is gated by
  `OMEGASL_FEATURE_BIT_CULL_DISTANCE` (HLSL/GLSL expressible, MSL not). A
  shader using it must `#requires(CULL_DISTANCE)`; on MSL it stubs
  (header-only — the loader rejects pipelines that bind it), and the
  FeatureScanner warns on undeclared use. Because the resolved `ast::Type`
  drops field-attribute metadata (and adding a member to `Type` would shift
  the positional aggregate initializers of its `FuncType` subclass), Sema
  records which structs carry a `CullDistance` field and flags
  `usedFeatures` directly on a shader that returns one; the FeatureScanner
  only *unions* `usedFeatures`, so the bit survives the scan.
  **ClipDistance carries no bit — it is universal.**

Runtime follow-up: the `GTEDeviceFeatures` → `featuresAsBitmask` bridge
(§14.4) should advertise `CULL_DISTANCE` on D3D12/Vulkan (core there) and
withhold it on Metal, so a `#requires(CULL_DISTANCE)` shader loads on
D3D12/Vulkan. Until then the MSL stub + stub-rejection already give the
correct Metal behavior; the compile path is complete.

Tests: `clip_cull_distance.omegasl` (clip + cull arrays on a vertex output,
`#requires(CULL_DISTANCE)`), `invalid_clip_cull.omegasl` (non-float element
+ indexed form rejected).

---

## 2. Resource types — missing textures and buffers (P0)

### 2.1 Cube & array texture types [COMPLETED — compile path]

Six new texture types and one new sampler keyword (`samplercube`) land in
this pass:

| Type | HLSL | MSL | GLSL |
|------|------|-----|------|
| `texturecube`       | `TextureCube`         | `texturecube<float>`        | `samplerCube` |
| `texturecube_array` | `TextureCubeArray`    | `texturecube_array<float>`  | `samplerCubeArray` |
| `texture1d_array`   | `Texture1DArray`      | `texture1d_array<float>`    | `sampler1DArray` / `image1DArray` |
| `texture2d_array`   | `Texture2DArray`      | `texture2d_array<float>`    | `sampler2DArray` / `image2DArray` |
| `texture2d_ms`      | `Texture2DMS<float4>` | `texture2d_ms<float>`       | `sampler2DMS` |
| `texture2d_ms_array`| `Texture2DMSArray<float4>` | `texture2d_ms_array<float>` | `sampler2DMSArray` |
| `samplercube`       | `SamplerState`        | `sampler`                   | (combined into `samplerCube`) |

Cube textures alone block environment maps, skyboxes, IBL — that's why
this is the first §2 item in the implementation order.

**Cube `read`/`write` and MS `write` are deferred** — backend support is
asymmetric and the practical demand is rare:

| Op                       | HLSL                          | MSL                            | GLSL                          |
|--------------------------|-------------------------------|--------------------------------|-------------------------------|
| `texturecube` `read`     | via TextureCubeArray indexing | no `texturecube::read`         | `texelFetch` rejects samplerCube |
| `texturecube` `write`    | RWTexture2DArray aliasing     | `texturecube<...,access::write>` (limited) | no `imageCube`     |
| `texture2d_ms` `write`   | not supported                 | not supported                  | not supported                 |

Sema rejects all three with a clear diagnostic. Revisit if a real engine
use case shows up.

**Phase A — compile path (Done)**: tokens, builtin types, Sema
validation (sampler↔texture pairing, sample/read/write coord rules incl.
3-arg MS `read`), three backends emit valid HLSL/MSL/GLSL source, the
runtime layout-desc enum gains six new texture-type values plus
`OMEGASL_SHADER_SAMPLERCUBE_DESC`/`OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC`.
Tests: `texture_cube.omegasl`, `texture_array.omegasl`,
`texture_ms.omegasl`, `invalid_sample_ms.omegasl`,
`invalid_write_ms.omegasl`.

**Phase B — runtime path (Done)**: `GETexture` API extension for cube /
array-layer count / sample count, and runtime descriptor binding
(`VkImageViewType` / D3D12 view-dimension / `MTLTextureType`). Tracked in
`docs/Pipeline-Completion-Extension-Plan.md` "Texture View Type
Extension". Until Phase B lands, a shader using one of the new types
compiles but a real cube/array/MS texture cannot be bound at runtime.

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

| Intrinsic | Purpose | Status |
|-----------|---------|--------|
| `sampleLOD(s, t, c, lod)` | explicit mip level | **Phase A — completed** |
| `sampleBias(s, t, c, bias)` | LOD bias | **Phase A — completed** |
| `sampleGrad(s, t, c, ddx, ddy)` | gradient-based sampling (terrain, raytraced reflections) | **Phase A — completed** |
| `gather(s, t, c)` / `gatherRed/Green/Blue/Alpha` | PCF, screen-space effects | **Phase A — completed** |
| `getDimensions(t, lod)` | query mip level dimensions | **Phase B — completed** |
| `calculateLOD(s, t, c)` | query LOD chosen by hardware | **Phase B — completed** |

**Phase A — sampling variants (landed)**

The four composable sampling forms were straightforward to wire into the
existing `sample`-shaped builtin pipeline:

* New builtins: `sampleLOD`, `sampleBias`, `sampleGrad`, `gather`,
  `gatherRed`, `gatherGreen`, `gatherBlue`, `gatherAlpha`. Registered in
  `AST.def`, `AST.h`, `AST.cpp`, and the `builtinFunctionMap` in
  `Sema.cpp`.
* Sema factors the `(sampler, texture, coord)` pairing check into
  `Sem::validateSampleTriple`, which the existing `sample` branch leaves
  alone (to avoid touching its golden tests) but which all four new
  variants reuse. Trailing-arg validation is per-builtin: lod / bias must
  be `float`; ddx/ddy rank for `sampleGrad` follows the spatial domain
  table (1D → float, 2D / 2D[] → float2, 3D and Cube[/Array] → float3).
  `gather*` is restricted to 2D / 2D-array / cube / cube-array, matching
  every backend's gather domain.
* CodeGen dispatches via four new `Target::emit*` virtuals
  (`emitTextureSampleLOD`, `emitTextureSampleBias`, `emitTextureSampleGrad`,
  `emitTextureGather` — the gather variant takes a `channel` parameter:
  `-1` for the default form, `0..3` for `gatherRed/Green/Blue/Alpha`).
* Per-backend emission:
  * **HLSL**: `tex.SampleLevel`, `tex.SampleBias`, `tex.SampleGrad`,
    `tex.Gather` / `tex.GatherRed/Green/Blue/Alpha`.
  * **MSL**: `tex.sample(s, coord, level(lod))` /
    `tex.sample(s, coord, bias(b))` /
    `tex.sample(s, coord, gradientNd(ddx, ddy))` (where `gradientNd` is
    `gradient1d`/`gradient2d`/`gradient3d`/`gradientcube`); for
    `gather`, `tex.gather(s, coord, [int2(0,0), component::C])`. The
    array/cube layer-split that already lived inside `MSLTarget::
    emitTextureSample` was lifted into a static helper `emitMSLSampleCoord`
    so all five sample forms share one rule.
  * **GLSL**: `textureLod` / `texture(...,bias)` / `textureGrad` /
    `textureGather` against a `samplerND(t, s)` combined-sampler. The
    samplerType selection was lifted into a static helper
    `glslSamplerTypeForTextureArg` for the same reason as MSL.
* Tests: `sample_variants.omegasl` covers all four sample forms across 1D,
  2D, 2D-array, 3D, and cube textures (with the proper gradient-rank
  table for `sampleGrad`); `texture_gather.omegasl` covers default + four
  per-channel gather variants on 2D, 2D-array, and cube textures;
  `invalid_sample_variants.omegasl` is a multi-error negative test
  covering arg-count, gradient-rank, gather-on-3D, and sample-on-MS
  rejection.

**Phase A.1 — MSL 1D-texture mip-sample hole (gated by feature flag, landed)**

The `sampleLOD` / `sampleBias` / `sampleGrad` table claims uniform
support across textures, but Apple GPUs do not store a mipmap pyramid
for 1D textures. As a result MSL has no `level()` / `bias()` overload
of `texture1d::sample(...)` and no `gradient1d` function exists at all.
HLSL and GLSL both expose the operation (`Texture1D::SampleGrad`,
`textureGrad(sampler1D, …)`).

Gated by `OMEGASL_FEATURE_BIT_TEXTURE1D_MIP_SAMPLE`
(HLSL=true, MSL=false, GLSL=true). The FeatureScanner trigger fires on
`sampleLOD` / `sampleBias` / `sampleGrad` whose texture argument
resolves to `texture1d` or `texture1d_array`. A file with
`#requires(TEXTURE1D_MIP_SAMPLE)` produces a header-only stub on MSL
(loader rejects pipelines binding the shader); HLSL and GLSL emit the
call normally. A file that uses the operation without the directive
trips the Layer 2 portability scanner warning (matching the
`TEXTURECUBE_RW` / `TEXTURE2D_MS_WRITE` pattern).

Latent bug fixed alongside this: `Sema::performSemForExpr`'s `ID_EXPR`
branch was returning the resolved type without stamping it onto
`expr->resolvedType`, while every other branch routes through
`setAndReturn`. The FeatureScanner trigger table reads
`arg->resolvedType` to identify resource arguments — without the
stamp, every identifier-shaped texture argument was opaque to the
scanner. The same bug had been silently disabling the existing
`TEXTURECUBE_RW` and `TEXTURE2D_MS_WRITE` warnings; no test had
exercised those code paths, so the regression had been latent.

**Phase B — query intrinsics (landed)**

Both query intrinsics landed together, along with the statement-injection
infrastructure HLSL's `GetDimensions` needed (which `modf`/`frexp` in §5.1
can now reuse).

*Statement injection.* `CodeGen` gained a per-statement redirect:
`emitStatementLine` (shared by `generateBlock` and the `switch`-case-body
loop) renders each statement into a scratch buffer with `outputRedirect_`
pointed at it, then flushes any lines a backend queued via
`queuePendingStatement` — indented to the current level — *before* the
statement. `pendingStatements` is swapped out for each render so lines
belonging to an enclosing statement (e.g. an `if` whose condition queued
them) survive a nested block walk and flush at the right level. To make
this work, `generateDecl` now writes through `shaderOutStream()` (the
active redirect) rather than `shaderOut` directly, matching `generateExpr`;
when no redirect is set the output is byte-identical to the pre-Phase-B
walk (verified by the unchanged HLSL golden tests). `renderExprToString`
borrows the same redirect to spell a sub-expression off-stream.

* `getDimensions(texture, lod)` — `lod` is **required** (pass `0` for the
  base level), avoiding HLSL's optional-overload selection. The return
  shape is synthesized per-call in Sema (like `transpose`): `uint` for 1D,
  `uint2` for 1D-array / 2D / cube, `uint3` for 2D-array / 3D / cube-array.
  Multisample textures are rejected (the per-backend MS dimension query is
  asymmetric; deferred).

  | Backend | Emission |
  |---------|----------|
  | HLSL | Statement injection: a `uint _gd<N>_…;` temp declaration + `tex.GetDimensions(lod, …, _gd<N>_levels)` are queued before the use site (the mip-taking overload always appends an `out NumberOfLevels`, declared into a discarded temp), and the inline expression is a `uintN(…)` constructor over the dimension temps. A monotonic `getDimensionsTempId` keeps multiple queries in one shader distinct. |
  | MSL | Inline `get_width(lod)` / `get_height(lod)` / `get_depth(lod)` / `get_array_size()` accessors. 1D drops the `lod` (Apple GPUs store no 1D mip pyramid; `get_width()` takes no lod, and the width is well-defined regardless). |
  | GLSL | Inline `uvecN(textureSize(t, int(lod)))` against the bare texture object via `GL_EXT_samplerless_texture_functions` (already in the preamble) — no combined sampler needed; the `uvecN` constructor casts the `int`/`ivec2`/`ivec3` result. |

* `calculateLOD(sampler, texture, coord) → float` — every backend's query
  takes only the *spatial* coord, so the array layer / cube-array face is
  dropped (`.xy` for 2D-array, `.xyz` for cube-array). 1D textures are
  rejected (no mip pyramid; Metal has no `calculate_*_lod` for `texture1d`
  — the query is degenerate anyway). Stage restriction follows the existing
  `sample` precedent (fragment-only in practice, not enforced).

  | Backend | Form | Return |
  |---------|------|--------|
  | HLSL | `tex.CalculateLevelOfDetail(s, spatialCoord)` | `float` |
  | MSL | `tex.calculate_clamped_lod(s, spatialCoord)` | `float` |
  | GLSL | `textureQueryLod(samplerND(t, s), spatialCoord).x` | `vec2` → `.x` |

  GLSL emits `.x` to take the clamped LOD (the unclamped component is
  discarded). The semantics are advisory: an implementation may return a
  clamped or unclamped LOD, and we don't force a runtime branch to
  normalize them. `textureQueryLod` is *not* in the samplerless extension,
  so it uses the real combined sampler `samplerND(t, s)`.

*Latent swizzle bug fixed alongside.* `getDimensions`' `uint2`/`uint3`
returns are usually consumed component-wise (`.x` = width, `.y` = height),
which exposed a pre-existing bug: the `MEMBER_EXPR` swizzle resolver in
`Sema.cpp` handled only `float2/3/4`, and an always-true `else
if(ast::builtins::float4_type)` (a non-null pointer test, not
`type_res == float4_type`) routed *every other* vector type through the
float4 path — so `uint2.x` resolved to `float`. The resolver is now
generalized: `vectorComponentInfo` / `vectorTypeForScalarArity` map any
numeric vector family (float / int / uint / half / short / ushort / long /
ulong) to its scalar + arity and back, swizzle chars are validated against
the arity, and the result is the scalar (1 char) or matching N-component
vector. This also fixes integer-vector swizzle everywhere, not just for
`getDimensions`.

Tests: `texture_query.omegasl` (getDimensions on every rank + calculateLOD
on 2D/2D-array/3D/cube/cube-array + the HLSL `if`-condition injection edge
case + `.x` swizzle on the uint results), `invalid_texture_query.omegasl`
(getDimensions on MS, non-int lod; calculateLOD on 1D, wrong arg count).
The Metal build compiles the generated MSL end-to-end on the macOS host;
HLSL/GLSL source emission is verified via `-S`.

**Out of scope (follow-ups):**

* **Optional `lod` for `getDimensions`** (`getDimensions(t)` defaulting to
  mip 0). Requires HLSL out-param overload selection; marginal benefit.
* **`calculateLOD` / `getDimensions` on 1D textures.** Both deferred for
  the Metal 1D-mip hole (same rationale as Phase A.1's
  `TEXTURE1D_MIP_SAMPLE`); could be gated behind that feature bit later.
* **`getDimensions` on multisample textures**, including a sample-count
  query — asymmetric across backends.
* **Uniform stage-checking** for `calculateLOD` (fragment-only). Land as a
  shared rule across `sample` / `sampleBias` / `gather` / `calculateLOD`
  when stage enforcement is tackled generally.
* **Color-channel (`rgba`) component swizzles.** The generalized resolver
  accepts positional `xyzw` only, matching prior behavior.

**Out of scope for both phases:**

* Sampler-offset arguments (e.g. HLSL `tex.Sample(s, c, offset)`,
  GLSL `textureOffset`). These compose orthogonally with every variant
  in §2.3 and deserve their own design pass — adding `_offset` suffixes
  to eight builtins is the wrong shape; the right shape is probably
  optional trailing arguments, which OmegaSL doesn't support today
  (overloads are §3.5).
* Depth-comparison sampling (`SampleCmp` / `sample_compare` /
  `texture(samplerNDShadow, vec)`). Belongs to §2.2 (depth textures +
  comparison samplers).

### 2.4 Uniform / constant buffers [COMPLETED — Metal verified; D3D12/Vulkan written]

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

**Phase A — compile path (done)**

`uniform<T>` is a new resource form parallel to `buffer<T>`. It is a
read-only, value-access constant buffer: `T` must be a user-defined
struct, the resource is referenced as a value (`perFrame.field`), and it
cannot be indexed.

* **Token / type**: `uniform` is a `KW_TY` (`Toks.def`, `Lexer.cpp`
  `isKeywordType`); `builtins::uniform_type` is a one-type-arg builtin
  (`AST.h`, `AST.cpp`) registered in Sema's `builtinsTypeMap`. The
  generic resource-decl parser handles `uniform<T> name : N;` without
  change.
* **Sema**: `uniform_type` joins the valid-resource-type set. A uniform
  must have exactly one type arg and `T` must be a user struct (the
  arity check runs before any `args[0]` access). The same arity guard
  was added to `buffer<T>`, and the type-arg parser now also collects
  builtin type names (`TOK_KW_TYPE`) — together these fixed a
  pre-existing crash where `buffer<float4>` / a bare `buffer` produced an
  empty arg list and the INDEX_EXPR path dereferenced `args[0]` out of
  bounds. In the per-shader resource
  map, a uniform is restricted to `In` access (read-only on every
  backend) and its name binds directly to `T` (not the handle type), so
  member access resolves through the normal struct path and `u[i]` fails
  with the existing "indexing only on buffer/vector/matrix" diagnostic.
  The element struct is registered for codegen emission.
* **Runtime layout-desc**: new `OMEGASL_SHADER_UNIFORM_DESC` appended at
  the tail of `omegasl_shader_layout_desc_type` (distinct from
  `OMEGASL_SHADER_CONSTANT_DESC`, the inline single-scalar push constant,
  and `OMEGASL_SHADER_BUFFER_DESC`).
* **Per-backend emission**:
  * **HLSL**: `ConstantBuffer<T> u : register(bN, spaceM);` — a new `b`
    register class with its own `bResourceCount` (resets per shader).
  * **MSL**: `constant T& u [[buffer(N)]]` — a reference (vs the `*` a
    structured buffer uses) in the always-`constant` address space;
    shares the `[[buffer(N)]]` slot space.
  * **GLSL**: `layout(std140,set=S,binding=B) uniform u_Layout { T u; };`
    — a std140 block whose single member is named after the resource (vs
    the std430 buffer block's unsized-array member).
* **Tests**: `uniform_buffer.omegasl` (vertex shader reading a
  `uniform<PerFrame>` alongside a `buffer<VertexIn>`; fragment shader
  reading a `uniform<Light>`), `invalid_uniform.omegasl` (multi-error:
  indexing a uniform, `out` access, non-struct element type).

**Phase B — runtime binding (done)**

A `uniform<T>` shader can now be bound at runtime. The contract has two
sides: the **shader layout** (`OMEGASL_SHADER_UNIFORM_DESC`) is
authoritative for the *binding* (descriptor / root-param type, register
class), and a new **buffer-descriptor specifier** drives *creation*.

* **Buffer specifier**: `BufferDescriptor::Role { Storage, Uniform }`
  (defaults to `Storage`; declared at the struct tail so existing
  positional aggregate initializers are unaffected). Distinct from
  `BufferDescriptor::Usage` (memory residency). `makeBuffer` stamps it
  onto `GEBuffer::role`. A `Storage` buffer is the existing `buffer<T>`
  resource; a `Uniform` buffer is for `uniform<T>`.
* **Metal**: `constant T&` and `device T*` bind identically via
  `setBuffer:offset:atIndex:`, and `newBufferWithLength:` is the same for
  both, so creation/binding are unchanged. The role is recorded only for
  a bind-time assert (`checkBufferRoleAgainstShader`) that a uniform slot
  receives a `Uniform` buffer — surfacing on Metal a mismatch that would
  otherwise fail only on Vulkan/D3D12. **Built and verified.**
* **Vulkan**: `makeBuffer` adds `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT` for
  `Uniform` buffers; the descriptor-set-layout switch maps
  `OMEGASL_SHADER_UNIFORM_DESC → VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` (pool
  sizes auto-aggregate); the three descriptor-write sites now choose the
  descriptor type from the shader layout via
  `getBufferDescriptorTypeForResourceID`. *Written from the source map;
  not compiled on this macOS host.*
* **D3D12**: root signature uses `InitAsConstantBufferView` for
  `OMEGASL_SHADER_UNIFORM_DESC` (register `b`); the root-param lookup
  matches `D3D12_ROOT_PARAMETER_TYPE_CBV`; bind sites call
  `SetGraphics/ComputeRootConstantBufferView`; `makeBuffer` rounds
  `Uniform` buffers up to the 256-byte CBV placement requirement and
  skips the SRV/UAV view (which also avoids the `len / objectStride`
  divide-by-zero for a single-struct uniform). State transitions use
  `D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER`. *Written from the
  source map; not compiled on this macOS host.*

**CPU-side std140 packing (done).** `GEBufferWriter` / `GEBufferReader` and
`omegaSLStructStride` now pack a `Uniform` buffer in std140 instead of
std430, so the bytes a caller writes match what the shader reads. The
layout standard is *derived from the bound buffer's `role`* — no API
change beyond the optional `role` argument to `omegaSLStructStride`
(default `Storage`, so existing callers are byte-identical). The rule
lives in `BufferIO.h` (`BufferLayoutStd`, `matrixColumnStride/Size`,
`encode/decodeFMatrix`, `std140StructStride`): std140 shares std430's
scalar/vector alignment and diverges only by 16-strided matrix columns
and a 16-byte struct round-up.

Per backend: **Vulkan** and **D3D12** writers/readers select std140 when
`role == Uniform`; **Metal** always packs std430 (its `constant T&` reads
the natural layout — switching it to std140 would be wrong). Verified by
a pure-CPU unit test (`gte/tests/std140_layout_test.cpp`, run via ctest as
`omegagte_std140_layout`) that asserts the std140 matrix strides and
struct strides on any host — the Metal build itself never exercises the
std140 path. Vulkan/D3D12 packing is written but compiles only on those
platforms.

**Follow-up — per-member alignment in the buffer writers (LANDED).**

The per-backend `GEBufferWriter` / `GEBufferReader` used to pack members
*sequentially*, advancing the cursor by each member's raw size and only
padding the struct total. Correct for the common uniform payload (matrices
and `float4`s, all 16-aligned) but wrong for a field order that interleaved
sub-16 scalars/vectors with larger members (`float` then `float4`, or
`float2` then `float4x4`): the larger member was placed too early, so the
bytes matched neither what `std140StructStride` computes nor what the shader
reads. Worse, the allocation sizer `omegaSLStructStride` used an independent
`biggestWord` heuristic that *under*-sized such orders (e.g. `{float,
float4}` → 24 bytes on Metal), so once the writer aligns correctly it would
have overflowed the buffer.

**Landed as a unified align-then-place across both standards.** The shared
rule now lives in `BufferIO.h`:

* `memberBaseAlignment(type, std)` — the per-member base alignment (scalar 4,
  vec2 8, vec3/vec4 16, matrix = its column alignment), standard-driven and
  backend-agnostic. Alignment is shared; the per-member *size* is not, because
  Metal packs `vec3` as a 16-byte `simd_float3` while std430/std140 pack it as
  12 — each backend keeps its own size convention and only borrows this
  alignment.
* `structStride(fields, std)` — the canonical align-then-place stride (align
  each member, then round the struct to 16 for std140 / max-member-alignment
  for std430). `std140StructStride` / new `std430StructStride` delegate to it.
* `alignOffset(off, align)` — the shared round-up helper.

`omegaSLStructStride`'s std430 body was replaced with this align-then-place
walk (Vulkan/D3D12 delegate to `std430StructStride`; Metal keeps `simd_*`
sizes), so the allocation size now equals what the writer packs. The three
`GE*BufferWriter::sendToBuffer` loops align the cursor (zero-filling the gap)
before each member; the readers align before each read via a shared
`alignRead` / the Metal reader's `readBeforePaddingIfPossible` hook.

Per-backend verification (per the build-verification discipline — only Metal
compiles on the macOS host): Metal `OmegaGTE` builds clean and all four
host tests pass. The extended CPU `omegagte_std140_layout` test asserts the
std430/std140 strides and the new `memberBaseAlignment` / `alignOffset`
helpers for mixed sub-16 orders on any host. `omegagte_matrix_ops` gained a
second GPU compute kernel (`mixedLayout`) that round-trips a deliberately
mixed-order struct (`float`→`float4`→`float2`→`float4x4`→`float` — both the
scalar-before-`float4` and `float2`-before-`float4x4` cases) end-to-end
through a real dispatch, doubling each field so a misaligned read yields
garbage rather than 2x; it was confirmed to *fail* when the writer alignment
is removed, so it genuinely guards the fix. The Vulkan/D3D12 writer/reader
edits are written from the source map, not compiled here.

| # | Task | Where | Effort | Blocks | Status |
|---|------|-------|--------|--------|--------|
| §2.4-1 | Per-member align-then-place in the buffer writers/readers (fixes std140 *and* std430 inter-member offsets for mixed sub-16 field orders) | `GEVulkan.cpp`, `GED3D12.cpp`, `GEMetal.mm` `GE*BufferWriter`/`Reader`; `BufferIO.h` `memberBaseAlignment`/`structStride`/`alignOffset`; `GTEBase.cpp` `omegaSLStructStride` | medium | correct std140 packing for non-16-aligned uniform field orders | **done** |

**Out of scope (follow-up):** the D3D12 `GE*BufferReader` still lacks the
struct-alignment round in `structEnd` that Metal/Vulkan have, so reading a
contiguous *array* of structs back on D3D12 is correct only for 16-aligned
struct strides (pre-existing; single-struct reads are correct).

**Follow-up — Metal `GEBufferWriter`/`GEBufferReader` trailing-padding bug (FIXED).**
Distinct from §2.4-1 (which is about *sub-16 inter-member* under-padding). The
Metal writer/reader used a heuristic that padded to the biggest member *size*
rather than the struct *alignment*, and the reader applied that padding one
field early (its `fieldIndex` is pre-incremented in `offsetAndIncrement`). For a
struct whose tail is smaller than its biggest member — e.g. several `float4x4`s
followed by a `float4` — the writer wrote trailing padding *past* the correctly
sized buffer, and the reader read the final field from the wrong (over-padded)
offset, returning zeros. The Vulkan/D3D12 readers were already correct (plain
sequential walk + round-to-struct-alignment at `structEnd`); the Metal
`GE*BufferReader`/`Writer` now match that model. Surfaced by the §5.2 matrix-op
GPU runtime test (`gte/tests/matrix_ops_test.cpp`), which previously read
`determinant` results of 0 even though the GPU computed them correctly.

### 2.5 Raw / byte-address buffers

Byte-indexed scratch memory:

| HLSL | MSL | GLSL |
|------|-----|------|
| `ByteAddressBuffer` / `RWByteAddressBuffer` | `device uint*` with manual indexing | `layout(std430) buffer { uint data[]; }` |

Used by mesh / geometry storage, BVH traversal, debug streaming. Without it,
engines must define a dummy `buffer<uint>` and do the arithmetic by hand —
workable, but the alignment model is different and the D3D12 side has
dedicated `Load2/Load4` paths that OmegaSL can't express. (This can be semantically resolved.)


---

## 3. Control flow & language features (P0 → P1)

### 3.1 `switch` / `case` / `default` [COMPLETED]

Practical impact: common in material ID fan-outs, tile classification,
and generated code.

**Shape**

```
switch(intExpr) {
    case 0:
        ...
        break;
    case 1:
    case 2:
        // case 1 falls through into case 2's body — C-style.
        ...
        break;
    default:
        ...
        break;
}
```

**Rules**

* The switch condition must be an `int` or `uint` scalar. (HLSL/MSL/GLSL
  accept more, but every generated dispatch table I've seen uses int —
  tightening the rule catches obvious bugs like `switch(uvCoord.x)`.
  Loosen if a real use case appears.)
* Case values must be integer literals (signed or unsigned). Constant
  expressions and named constants are deferred — `ConstFold` already
  exists, hooking it in is straightforward when the need arises.
* Mandatory braces on the switch body. Inner braces around case bodies
  are optional, matching C — multiple statements per case work without
  a wrapper block.
* Fall-through is C-style. Terminate a case with `break;` to stop
  falling into the next one. The compiler does not insert implicit
  breaks. Nor does it warn on missing breaks — the existing project
  style trusts the developer here.
* `default:` is optional, may appear in any position, and must appear
  at most once.
* Empty switches (`switch(x){}`) are rejected.

**Implementation**

* Tokens: `switch` / `case` / `default` added to `Toks.def` and
  `isKeyword()` in `Lexer.cpp`.
* AST: `SWITCH_STMT` tag in `AST.def`; `SwitchStmt` (condition + vector
  of `SwitchCase`) and `SwitchCase` (value-or-null + body-stmt-vector)
  in `AST.h`.
* Parser: `parseSwitchStmtFromBuffer` does a two-pass scan of the
  buffered switch body — first pass finds every top-level `case` /
  `default` label (skipping nested `{}` and `()`), second pass slices
  the body for each label and feeds each slice through
  `parseBlockBodyFromBuffer`. `findExtentOfStatement` and
  `collectTokensUntilEndOfStatement` learned the same `(...)` + `{...}`
  shape that `for`/`while` use, and `parseStmtFromBuffer` /
  `parseStmt` route the keyword.
* Sema: `SWITCH_STMT` arm checks the condition type, validates each
  non-default case value is an integer literal, and recurses into each
  case body via `performSemForStmt`. Duplicate-`default` and
  empty-switch checks live in the parser (they're structural, not
  type-driven).
* CodeGen: `SWITCH_STMT` arm in `generateDecl` emits the C-style
  `switch(cond) { case X: stmts ... }` directly — HLSL/MSL/GLSL accept
  identical syntax, so this lives in the shared decl walker rather
  than per-target hooks. The case-body emit reuses the same
  per-statement dispatch as `generateBlock` so var decls, returns,
  nested ifs/loops, and even nested switches work inside a case body.
* GLSL backend: `emitShaderEntryBody`'s custom statement loop also
  excludes `SWITCH_STMT` from the trailing-`;` rule, matching the
  shared `generateBlock` behaviour for `if`/`for`/`while`.

**Tests**

* `switch_basic.omegasl` — int-conditioned switch with explicit `break`,
  intentional fall-through (`case 1:` empty body falls into `case 2:`),
  trailing `default:`, and a switch nested inside a `for` loop.
* `invalid_switch.omegasl` — multi-error negative test for
  non-int condition, non-literal case value, and duplicate `default:`.

**Out of scope (follow-ups)**

* Constant-expression case values (named constants, arithmetic on
  literals). Wire `ConstFold` into the case-value validation when
  needed.
* `[[fallthrough]]`-style annotation. C-style fall-through is the
  default; an explicit marker for tools / readers can come later.
* Loop-context check for `break;` — currently any `break` is accepted
  by Sema regardless of enclosing loop/switch. Matches the pre-3.1
  behaviour; a uniform stage check could land alongside §3.5 / §3.6.

### 3.2 Ternary `?:` [LANDED]

Listed as not-implemented. Impact: verbose code for compound expressions;
blocks inline vector selection (`a ? v1 : v2`).

**Landed.** OmegaSL now parses `cond ? then : else`. Right-associative —
`a ? b : c ? d : e` reads as `a ? b : (c ? d : e)`. The condition must
resolve to `bool`; both branches must produce the same type, with
asymmetric numeric-literal coercion when one side is a bare literal
and the other has a fixed scalar slot.

Per-backend mapping: identical spelling on every target — codegen
emits `(cond ? then : else)` straight through. Restricting the
condition to a scalar `bool` keeps the surface portable: GLSL doesn't
have a vector ternary at all, and HLSL's "vector select" semantics
have no MSL analogue. Callers who want lane-wise selection should use
`select(...)` (HLSL) or `mix(a, b, bvec3(...))` (GLSL) once those land
as builtin intrinsics in OmegaSL.

Pulled in along with the ternary work: relational / equality / logical
binary operators (`<`, `<=`, `>`, `>=`, `==`, `!=`, `&&`, `||`) now
correctly produce `bool` instead of inheriting the operand type. This
unblocks `bool flag = (x > 0);` and the ternary's bool-condition
constraint without breaking existing `if (x > 0)` callers (Sema never
enforced a bool constraint on `if` conditions).

Out of scope:
  - Vector-condition ternary (`bvecN ? vecN : vecN`). Backend asymmetry
    is real; defer to per-backend `select`/`mix` builtins.
  - Constant folding of ternaries with a literal `true`/`false`
    condition. Hand-written shader code rarely benefits; downstream
    backend compilers fold these too.

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
don't support it, emit nothing. Rare to need the others. (FEATURE GATE)

### 3.5 Function overloading [LANDED]

Originally: every vector/scalar variant of a user helper needed a unique
name, which blocked shader libraries that emulate the builtin math surface.

**Landed.** Two same-name functions whose parameter lists differ are now a
valid overload set. At each call site Sema collects every candidate by
name, computes the argument types, and picks the unique exact-signature
match. Codegen mangles overloaded names with a parameter-type suffix
(`osl_user_myLerp__float_float_float` vs `osl_user_myLerp__float3_float3_float`)
so each overload emits a distinct downstream symbol; single-overload
names stay clean (`spellUserFuncName` only mangles when
`userFuncNameCount(name) > 1` or the name collides with a backend
stdlib identifier). Same name + same params + different return type is
rejected — return type isn't part of resolution input, so no call-site
information could pick between them.

Per-FuncType `hasDefinition` replaces the pre-overload
`Vector<String> definedFuncNames`, which would have false-positived a
duplicate the moment a second overload was bound. Forward-decl
followed by body is detected by exact signature, not by name, so
`f(int)` forward + `f(float)` body + `f(int)` body all coexist
correctly.

Implementation note: the existing `FuncType::fields` is a `MapVec`
(`std::unordered_map`), so iteration order is implementation-defined.
The pre-overload signature check iterated it in parallel with the
positional `params` vector and worked by accident on identical
insertion sequences. Added `FuncType::paramTypes` as the canonical
ordered list; every order-sensitive consumer (overload resolution,
mangling, the prior-decl check) routes through it.

Out of scope (intentional, deferred to a follow-up pass):
  - **Implicit numeric conversion in overload resolution.** Today only
    exact match counts. Once added, `resolveOverload` will need to
    grow a "best match" tier and an ambiguity diagnostic when two
    candidates tie at the same conversion cost; the framework is in
    place but the path is unreachable until coercion lands.
  - **Overload sets across builtins.** OmegaSL still does not let user
    code shadow `sin`, `lerp`, etc. — builtins win first in the
    candidate-collection order. Revisit only if real shader libraries
    hit the wall.

### 3.6 `const` / `let` qualifier [LANDED]

Originally: no way to mark a local immutable. All three backends spell
it `const` and accept the C-style `const T x = …;` form on a local
declaration.

**Landed (as `const`, per Alex).** Recognized as a keyword (`KW_CONST`),
parsed as an optional prefix on local var-decls, stamped onto the AST
(`VarDecl::isConst`), and enforced by Sema:
  - Writes through the binding fail at the binary `=`/`+=`/etc. site,
    walking the LHS through any `INDEX_EXPR` and `MEMBER_EXPR` to the
    root identifier so `c[0] = v` and `c.f = v` are caught when `c` is
    const (matches HLSL/MSL/GLSL semantics for `const` aggregates).
  - `++`/`--` on a const operand fail in the unary-op branch.
  - Missing initializer (`const float k;`) fails at parse time — every
    backend would reject it anyway, so catch it where the diagnostic
    points at the OmegaSL source.

`SemContext::variableMap` was widened from `Map<String, TypeExpr*>` to
`Map<String, VarBinding>` where `VarBinding = { type, isConst }`. All
four insert sites (local var, function params, shader resources,
shader params) populate the new struct; only the local var-decl path
ever sets `isConst = true`.

Codegen emits `const` as a prefix in the shared `VAR_DECL` path —
single emit point covers all three backends (HLSL, MSL, GLSL accept
the spelling verbatim) so no per-target hook was needed. Verified
output across the matrix:

| Source | HLSL | MSL | GLSL |
|---|---|---|---|
| `const float k = 2.0;` | `const float k = 2.0;` | `const float k = 2.0;` | `const float k = 2.0;` |

Editor highlighting via `omegasl.yaml` adds `const` to
`declaration_keywords` so it gets the `storage.modifier` scope.

**Follow-up — `const` parameters + postfix form (LANDED).**

Both items originally deferred here have landed now that §3.7's
parameter-access work is in place:

  - **`const` on function parameters.** A parameter may carry a `const`
    qualifier (`const float v`). It rides on `AttributedFieldDecl` as a
    new `isConst` flag (parallel to `VarDecl::isConst`); only the
    function-parameter parse path sets it. The flag flows into Sema's
    variable map at both param-insert sites (FuncDecl and ShaderDecl),
    so the *existing* const-write checks (assignment, `++`/`--`, member
    / index write-through) fire on a const param with no new machinery.
    `const` is rejected in combination with `out` / `inout` — those
    write the caller's storage back through the binding, which an
    immutable binding can't do — with a clear Sema diagnostic. `const`
    may appear in either order relative to the access qualifier
    (`const in T`, `in const T`).
  - **Postfix `T const x` form.** Recognized for both locals and
    parameters (`float const k = …;`, `f(float const a)`), setting the
    same `isConst` flag the prefix form does. The two statement-level
    disambiguators (`parseStmt`, `parseStmtFromBuffer`) had to learn
    that a `const` keyword following a type token introduces a
    declaration — without that, `T const name` was misparsed as an
    expression and the type keyword read as an undeclared identifier.
    A repeated `const` (`const T const x`) and `threadgroup T const x`
    are both rejected, mirroring the prefix-side contradiction checks.

Per-backend spelling routes through the existing `Target::writeFuncParam`
hook (a `const ` prefix on HLSL / GLSL / MSL — all three accept it
verbatim, and Sema guarantees it never co-occurs with the MSL
`thread T&` reference). Local `const` continues to emit through the
shared `VAR_DECL` path. Output across the matrix:

| Source | HLSL | MSL | GLSL |
|---|---|---|---|
| `f(const float v)` | `const float v` | `const float v` | `const float v` |
| `float const k = 2.0;` | `const float k = 2.0;` | `const float k = 2.0;` | `const float k = 2.0;` |

Tests: `const_param.omegasl` (prefix + postfix const params, postfix
const local, const+explicit-`in` in both orders; Metal-compiled
end-to-end, HLSL/GLSL verified via `-S`), `invalid_const_out_param.omegasl`
(const+out rejected), `invalid_const_param_assign.omegasl` (write through
a const param rejected), `invalid_dup_const.omegasl` (duplicate const
rejected).

Out of scope (still deferred):
  - **`const` as part of the overload signature.** Const and access
    qualifiers don't distinguish overloads (inherited from §3.5 / §3.7).
    Revisit when a real shader hits it.
  - **Trailing `T name const` (const after the declarator).** Not valid
    C either; only the C-style `T const name` postfix is accepted.
**Follow-up — prefix `const` / `threadgroup` inside a nested block
(FIXED).** A pre-existing silent miscompile, surfaced while wiring the
postfix disambiguator: `parseStmtFromBuffer` (the if / for / while /
switch body parser) recognized the *postfix* `T const x` form but a
*leading* `const` / `threadgroup` keyword fell through both its TOK_KW
keyword switch and its type-token disambiguator (which only fires on
TOK_KW_TYPE / TOK_ID), reaching neither a declaration parse nor a clean
error. The symptom was severe — the *entire enclosing block body* was
silently dropped from the generated source with no diagnostic (a `for`
body containing a `const` local emitted an empty, unterminated loop).
Fixed by routing a leading `const` / `threadgroup` to `parseGenericDecl`
(which owns the prefix-qualifier var-decl form), mirroring the
`break` / `continue` / `discard` handling already in that function and
the `else if (TOK_KW) isDecl = true` branch in `parseStmt`. Test:
`nested_const.omegasl` (prefix `const` in a `for` body and an `if` body,
plus a postfix `const` in the nested block; Metal-compiled end-to-end,
HLSL/GLSL verified via `-S`).

### 3.7 Multiple return values [LANDED]

HLSL supports `out` parameters; MSL and GLSL both support them. OmegaSL's
reference does not document `out`/`inout` on function parameters (only on
resource-map keywords). Needed for `sincos(x, s, c)` and similar.

Alex Answer:

Allow `out`/`inout` to be used in function params.

**Landed.** `in` / `out` / `inout` are now accepted as a prefix qualifier
on a function parameter declaration. The keywords stay contextual (lexed
as `TOK_ID`, same treatment they got in the resource map), so plain
identifiers named `in` / `out` / `inout` continue to parse. `in` is the
implicit default and the parser accepts it as a no-op for symmetry with
the resource-map spelling. The qualifier rides on `AttributedFieldDecl`
as a new `ParamAccess` enum; only the function-parameter parse path
sets it — struct fields and shader-decl params leave it at `In`.

Per-backend spelling routes through a new `Target::writeFuncParam` hook:

| Backend | `in` (default) | `out`             | `inout`            |
|---------|----------------|-------------------|--------------------|
| HLSL    | `T name`       | `out T name`      | `inout T name`     |
| GLSL    | `T name`       | `out T name`      | `inout T name`     |
| MSL     | `T name`       | `thread T& name`  | `thread T& name`   |

MSL has no `out` keyword and no write-only reference qualifier — both
`out` and `inout` lower to a `thread T&` reference. The semantic
difference (caller's value may be undefined on entry for `out`) is
preserved only in the OmegaSL source.

The default-`In` spelling is byte-identical to the pre-3.7 output, so
no golden snapshots needed updating.

Out of scope (intentional, deferred):
  - **`out` / `inout` as part of the overload signature.** Today a
    function whose parameters differ only by access qualifier
    (`f(out int)` vs `f(int)`) is treated as a redeclaration. HLSL
    distinguishes the two; revisit once a real shader runs into the
    wall.
  - **`out` + pointer combinations.** The parser does not reject
    `out T*`, but the emitted MSL spelling (`thread T*&`) is
    untested. Add coverage when a workload needs it.
  - **`const T` parameters.** §3.6 noted this as 3.7-adjacent. **Now
    landed** — see §3.6's "`const` parameters + postfix form (LANDED)"
    follow-up. `const` rides on `AttributedFieldDecl::isConst` and is
    rejected in combination with `out` / `inout`.

---

## 4. Numeric types (P1)

### 4.1 16-bit types — `half` / `float16_t` / `int16_t` / `uint16_t` [LANDED]

Supported everywhere modern:

| Backend | Requirement |
|---------|-------------|
| HLSL    | SM 6.2, `-enable-16bit-types` |
| MSL     | `half` native since Metal 1.0 |
| GLSL    | `GL_EXT_shader_explicit_arithmetic_types_float16` / `int16` |
| SPIR-V  | `VK_KHR_shader_float16_int8` + storage extension |

Drops memory bandwidth in half for mobile / AI / post-processing passes.

**Landed.** OmegaSL exposes `half`, `half2/3/4`, `short`/`short2..4`,
`ushort`/`ushort2..4` as builtin types. Per-backend mapping:

| Backend | Scalar | Vector |
|---------|--------|--------|
| HLSL    | `float16_t` / `int16_t` / `uint16_t` (SM 6.2) | `vector<T,N>` |
| MSL     | `half` / `short` / `ushort` (native)          | `halfN` / `shortN` / `ushortN` |
| GLSL    | `float16_t` / `int16_t` / `uint16_t`          | `f16vecN` / `i16vecN` / `u16vecN` |

GLSL emission emits `#extension GL_EXT_shader_explicit_arithmetic_types_float16`
and `int16` whenever the file declares `#requires(FLOAT16)`. Use of any
of these types from a shader/function body trips
`OMEGASL_FEATURE_BIT_FLOAT16` via the FeatureScanner — the scanner walks
function param types, return types, var-decl types, cast targets, and
recurses into user struct fields so a `half` hidden in a struct still
flips the bit. The runtime feature gate (`#requires(FLOAT16)`) is what
makes the runtime decline cleanly on hardware that doesn't support
16-bit types.

HLSL invocation: when the file's required-features bitfield includes
`FLOAT16`, the offline `compileShader` path bumps the dxc target
profile from `<stage>_5_0` to `<stage>_6_2` and appends
`-enable-16bit-types`. SM 6.2 is the lowest profile that accepts the
16-bit family; older shaders without `#requires(FLOAT16)` continue to
build at SM 5.0. The runtime path uses `D3DCompile` (FXC), which tops
out at SM 5.1 — `compileShaderRuntime` refuses FLOAT16-gated shaders
loud rather than silently emitting garbage. Migrating the runtime
compile to dxc is the long-term unblock; tracked separately. The
`Target::compileShader{,Runtime}` API takes the feature bitfield as
an explicit parameter so the dispatch decision stays per-shader.

Literal forms: `1.0h` / `1.0H` parses as half-typed (stored as `f_num`
since precision is enforced at type-resolution time). Integer literals
coerce into `short`/`ushort` slots without a suffix.

`make_half2/3/4`, `make_short2/3/4`, `make_ushort2/3/4` are the
constructor builtins, mapping to `vector<T,N>` (HLSL) /
`halfN`/`shortN`/`ushortN` (MSL) / `f16vecN`/`i16vecN`/`u16vecN` (GLSL).

Out of scope for this cut: `half`-typed matrix types
(`half2x2`/`half3x3`/`half4x4` etc.) — none of the existing OmegaSL
matrices are gated on FLOAT16, and adding the half family of matrices
introduces backend-specific row/column rewrite asymmetries we'd want to
plan separately. Add them when a real workload needs them.

### 4.2 64-bit integer types [LANDED]

| HLSL | MSL | GLSL |
|------|-----|------|
| `int64_t` / `uint64_t` (SM 6.0) | `long` / `ulong` | `GL_ARB_gpu_shader_int64` |

Needed for large hashes, 64-bit atomics, pointer arithmetic in bindless
descriptor indexing.

**Landed.** OmegaSL exposes `long`/`long2..4`, `ulong`/`ulong2..4` as
builtin types, gated on `OMEGASL_FEATURE_BIT_INT64`. Per-backend mapping:

| Backend | Scalar | Vector |
|---------|--------|--------|
| HLSL    | `int64_t` / `uint64_t` (SM 6.0+)            | `vector<T,N>` |
| MSL     | `long` / `ulong` (MSL 2.0+)                 | `longN` / `ulongN` |
| GLSL    | `int64_t` / `uint64_t`                       | `i64vecN` / `u64vecN` |

GLSL emission adds
`#extension GL_EXT_shader_explicit_arithmetic_types_int64` whenever the
file declares `#requires(INT64)`. The same FeatureScanner type-walk
that handles FLOAT16 trips INT64 from any `long`/`ulong` use.

Literal forms: `123L` / `123l` for signed long, `123UL` / `123ul` /
`123Lu` / `123lU` for ulong. Integer literals (`42`) also coerce into
64-bit slots without an explicit suffix. `make_long2/3/4` and
`make_ulong2/3/4` are the constructor builtins.

Out of scope for this cut: HLSL DXC profile bumping. The existing
`compileShader` path targets SM 5.x; running 64-bit / 16-bit shaders
through DXC requires SM 6.0+ (and `-enable-16bit-types` for
`float16_t`). The runtime feature gate keeps callers on 5.x-only
hardware safe; bumping the profile when `#requires(FLOAT16)` /
`#requires(INT64)` is declared is a follow-up that touches the offline
DXC invocation rather than the type system.

### 4.3 `double` — [LANDED — compile path; HLSL/GLSL source-verified, MSL stubbed]

| HLSL | MSL | GLSL |
|------|-----|------|
| `double` / `double2..4` (SM 5.0+) | **none — Metal has no `double`** | `double` / `dvec2..4` (`GL_ARB_gpu_shader_fp64`) |

Double-precision floating point. The defining asymmetry is that **Metal/MSL
has no `double` type at all** — so this is the §1.7 `CullDistance` situation,
not the §4.2 `INT64` situation: HLSL and GLSL can express it, MSL cannot.

**Gated by `OMEGASL_FEATURE_BIT_DOUBLE` (HLSL=true, MSL=false, GLSL=true).**
The feature bit, the `{"DOUBLE", …, true, false, true}` row in the
Preprocessor feature table, the `KW_TY_DOUBLE/2/3/4` tokens, the
`LiteralExpr::d_num` / `isDouble()` storage, and the
`CodeGen::emitLiteralValue` `isDouble` branch were all pre-laid in earlier
passes; this cut wires the type itself through the rest of the pipeline.

Because MSL is not expressible, a shader that uses `double` must declare
`#requires(DOUBLE)`. On MSL the existing unsatisfied-feature path
(`fileUnsatisfiedFeatures != 0` in `CodeGen::SHADER_DECL`) stubs the whole
shader **before body emission** — the MSL backend never tries to spell
`double`, and the loader rejects pipelines that bind the stub. HLSL/GLSL
emit the shader normally. Undeclared use trips the FeatureScanner Layer-2
portability warning (same as `CULL_DISTANCE` / `TEXTURECUBE_RW`).

**Scope (matches the §4.1/§4.2 cuts):** scalar `double` + `double2/3/4`
vectors + `make_double2/3/4` constructors. **No `double` matrices** — same
rationale the half/int64 cuts used for skipping their matrix families; add
them when a real workload needs `dmat`.

**Literals.** OmegaSL source spells the full-precision double literal with
an `lf` / `LF` suffix (`3.141592653589793lf`), parsed via `std::stod` into
`d_num` and emitted at 17 significant digits. The *emitted* suffix is
**per-backend** — GLSL requires `lf`/`LF` (glslc rejects a bare `l`) and
HLSL requires `l`/`L` (dxc rejects `lf` with "invalid suffix 'lf' on
floating constant"), so the suffix comes from a `Target::doubleLiteralSuffix()`
hook (default `lf`; `HLSLTarget` overrides to `l`). Without the suffix the
downstream compiler rounds the literal through `float` first, defeating the
point. An unsuffixed float literal still coerces into a `double` slot (at
float precision), exactly like `half` (`1.0`). A `double` literal does *not*
coerce into a `float`/`half` slot (that would silently drop precision — use
an explicit cast).

**Per backend.** HLSL `double` / `double2..4` (double literal suffix `l`);
GLSL `double` / `dvec2..4` (suffix `lf`) plus a
`#extension GL_ARB_gpu_shader_fp64 : enable` line emitted on
`#requires(DOUBLE)` (mirrors the FLOAT16/INT64 extension pattern); MSL
emits nothing (stub).

**Verification.** Only Metal compiles "natively" on the macOS host, and a
`#requires(DOUBLE)` shader stubs on Metal — but Sema + the FeatureScanner
still run there, so `omegasl_compile_numeric_double` exercises the whole
front end locally. The HLSL/GLSL output was additionally **compiled through
real dxc and glslc on the dev host**: `omegaslc -S --hlsl` → dxc
(`vs_6_0`/`ps_6_0`) → DXIL, and `omegaslc -S --glsl` → glslc → SPIR-V, both
shaders, after the per-backend suffix fix. (The first cut emitted a uniform
`lf`, which dxc rejected — the `Target::doubleLiteralSuffix()` split is what
resolved it.)

Test: `numeric_double.omegasl` (scalar + vector double, `lf` literal,
int-literal + float-literal coercion, `(double)` cast, vector indexing),
mirroring `numeric_64bit.omegasl`.

**Out of scope (follow-ups):**
* `double` matrix types (`double2x2`…`double4x4` → HLSL `doubleNxM` /
  GLSL `dmat`); no MSL analogue regardless.
* HLSL DXC profile/flag handling for `double` (the SM-profile bump
  follow-up tracked for FLOAT16/INT64 covers this family too).
* Runtime `GTEDeviceFeatures → featuresAsBitmask` advertisement of
  `DOUBLE` on D3D12/Vulkan (the same bridge §1.7 notes for
  `CULL_DISTANCE`); until then the MSL stub + stub-rejection give the
  correct Metal behavior and the compile path is complete.

---

## 5. Intrinsics — common functions currently missing (P0 → P1)

Grouped by how often engines reach for them.

### 5.1 Core math not yet listed [COMPLETED]

| Function | Status |
|----------|--------|
| `sign(x)` | **landed** — passthrough on every backend |
| `saturate(x)` | **landed** — passthrough HLSL/MSL; GLSL rewrites to `clamp(x, 0.0, 1.0)`. See §5.1.1 |
| `fma(a,b,c)` | **landed** — passthrough MSL/GLSL; HLSL lowers to `mad` (precision contract looser; see notes below) |
| `mad(a,b,c)` | **landed** — alias of `fma` (§5.1.0). Canonicalized to `fma` at both dispatch and emit; HLSL keeps the native `mad` spelling, MSL/GLSL emit `fma` |
| `fmod(a,b)` | **landed** — truncation semantics, matching C/HLSL/MSL. GLSL rewrites to `(x - y*trunc(x/y))` because GLSL's `mod` has different (floor-based) semantics |
| `mod(a,b)` | **landed** — alias of `fmod` (§5.1.0). Truncation modulus, *not* GLSL's floor-based `mod`; canonicalized to `fmod` so it reuses fmod's GLSL trunc-rewrite |
| `trunc(x)` | **landed** — passthrough on every backend |
| `rsqrt(x)` | **landed** — passthrough HLSL/MSL; GLSL renames to `inversesqrt` |
| `degrees(r)` / `radians(d)` | **landed** — passthrough HLSL/GLSL; MSL inlines as a multiplication by the matching π constant (Metal stdlib has no `degrees`/`radians`). See §5.1.2 |
| `sinh` / `cosh` / `tanh` | **landed** — passthrough on every backend |
| `ldexp(x, e)` | **landed** — passthrough on every backend |
| `modf(x, out integerPart)` | **landed** (§5.1.0) — native passthrough on every backend; the out-param uses §3.7's `out` params, so no statement injection is needed |
| `frexp(x, out exp)` | **landed** (§5.1.0) — native on MSL/GLSL; HLSL writes a float exponent and the backend casts back to the int out-param via statement injection |

**Implementation**

The new builtins reuse the existing generic math-intrinsic dispatch in
Sema (1/2/3-arg buckets, return type follows first arg, scalar/vector
branching handled by the backend). New names registered:

* 1-arg same-type: `sign`, `saturate`, `trunc`, `rsqrt`, `degrees`,
  `radians`, `sinh`, `cosh`, `tanh`.
* 2-arg same-type: `fmod`, `ldexp`.
* 3-arg same-type: `fma`.

Where the name passes through unchanged on a backend, no work is needed
beyond bucket registration. For diverging spellings, `Target::renameBuiltin`
handles the simple cases:

* HLSL: `fma` → `mad`. (HLSL's `fma` exists but is double-only on SM 5+;
  `mad` is the broadly-supported fp32 multiply-add. The precision contract
  is looser than IEEE 754 fma, but it matches what every existing HLSL
  shader assumes "multiply-add" means. Strict fma can be revisited once
  §4.3 (`double`) lands.)
* GLSL: `rsqrt` → `inversesqrt`.

For builtins that need a different *call shape* (not just a different
name), a new `Target::tryEmitBuiltinCall` hook lets a backend take over
emission of a single call site. GLSL overrides it for two cases:

* `saturate(x)` → `clamp(x, 0.0, 1.0)`. GLSL's `clamp(genType, float,
  float)` overload broadcasts the scalar bounds across vector x, so the
  same emission works for scalar and vector arguments.
* `fmod(x, y)` → `(x - y * trunc(x / y))`. Centralized expression keeps
  the truncation semantics consistent across stages — emitted inline so
  the backend doesn't need a new helper function.

MSL overrides it for the `degrees` / `radians` pair (see §5.1.2):

* `degrees(x)` → `((x) * 57.29577951308232)` (180 / π).
* `radians(x)` → `((x) * 0.017453292519943295)` (π / 180).
* Vector arguments work without extra glue because `scalar * vec`
  broadcasts.

The hook follows the existing `tryEmitVarDecl` / `tryEmitReturnDecl`
pattern: returning true means the backend fully handled emission and the
shared dispatch skips its `<rename>(args)` fallback.

**Tests**

* `math_phase5_1.omegasl` — covers all 12 new builtins on scalar and
  vector arguments. Includes a composition test (`saturate(fma(...))`)
  to catch a regression in GLSL's `tryEmitBuiltinCall` returning false
  when nested inside another builtin call. The test goes through every
  positive validation path; per-arg-count argument-mismatch errors are
  already covered by the generic Sema dispatcher's tests.

#### 5.1.0 Out of scope (named follow-ups)

* **`mod` / `mad` aliases**. The codebase canonicalizes on `fmod` and
  `fma`; if alias support is desired, add a one-line rename in the
  affected backend's `renameBuiltin`. (DONE)
* **`modf` / `frexp`**. Both have an out-parameter for the integer part /
  exponent. HLSL exposes them with native out-params (`modf(in, out)`),
  MSL has separate accessors, GLSL has separate forms. Inlining the
  emission requires the same statement-injection hook proposed for
  §2.3 Phase B's `getDimensions`. Land that hook once and these
  builtins become a few lines each. (DONE)
* **Sema reservation of intrinsic names** (rejecting user `func sin(...)`,
  `func saturate(...)`, etc.). **(LANDED.)** Originally a user definition
  silently shadowed the builtin; a `func saturate` was dead code (every
  `saturate(...)` call bound to the builtin) yet compiled without
  complaint. Now `ast::isReservedBuiltinName` — one set spelled from the
  `BUILTIN_*` macros plus the string-matched `transpose`/`determinant`,
  kept in sync with the math dispatch in `Sema::performSemForExpr` and the
  `builtinFunctionMap` — is consulted at the top of the `FUNC_DECL` Sema
  arm (before param/overload checks, so the diagnostic is about the name,
  not a phantom redeclaration). A reserved name raises the new
  `ReservedName` error (`"`<name>` is a reserved builtin intrinsic name and
  cannot be used as a user function name"`). Both definitions and forward
  declarations are rejected. The reserved set is OmegaSL builtins only —
  backend-stdlib-only names OmegaSL doesn't model (e.g. GLSL `inverse`)
  stay legal user names; the unconditional `osl_user_` prefix (below) is
  what keeps those safe in generated source. Backwards-compat fallout was
  exactly the case the doc flagged: `blinn_phong.omegasl` defined its own
  `float saturate(float x){ return clamp(x, 0.0, 1.0); }` — deleted, since
  the builtin has identical semantics. Tests: `invalid_reserved_name.omegasl`
  (negative; a user `saturate`).
* **`osl_user_` user-function prefix — now unconditional on all backends.**
  **(LANDED.)** Previously each backend carried a *curated* `needsMangling`
  stdlib list and prefixed a user function only on backends whose list it
  hit. That can never be complete — Metal's namespace alone spans the math,
  geometric, and `<metal_type_traits>` surfaces (`add_const` et al.), so any
  name the list missed became a platform-dependent collision, and the *same*
  OmegaSL function could emit under different symbols across backends.
  `CodeGen::spellUserFuncName` now prefixes **every** user function with
  `osl_user_` on HLSL/MSL/GLSL unconditionally; the per-backend
  `needsMangling` virtual and its three curated overrides were deleted. The
  overload-mangling form (`osl_user_<name>__<paramtypes>`) is unchanged.
  Shader entry points (vertex/fragment/compute) are emitted through a
  different path and keep their bare source names — the runtime looks them
  up by name, so this must not change (verified: the DirectX-only HLSL
  goldens, which contain only entry points and no user helpers, are
  byte-identical). With reservation (above) in place, the prefix is now a
  defense-in-depth measure against backend-stdlib collision rather than a
  load-bearing one (per §5.1.1). Verified end-to-end on all three backends
  via `-S`: `user_functions.omegasl`'s `add` (which was in *no* curated
  list) now emits `osl_user_add` at both definition and call sites.

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
same signature. As of the §5.1.0 follow-up, **every** user-defined function
name is prefixed with `osl_user_` at emit time on **all three backends**
(not just Metal) — see the §5.1.0 "`osl_user_` user-function prefix" entry.
Each backend has the same exposure in principle (HLSL/Metal intrinsics live
in the global namespace; even where GLSL would let the user definition win,
applying the prefix uniformly keeps generated source predictable), and a
curated per-backend collision list can never enumerate Metal's full
namespace, so the prefix is unconditional rather than collision-gated.

`saturate` (and the other §5.1 entries) are now first-class OmegaSL builtins
*and* reserved names — Sema rejects a user `func saturate(...)` the same way
it rejects redefining `sin` (see the §5.1.0 "Sema reservation of intrinsic
names" entry). With reservation in place the prefix is a defense-in-depth
measure against *backend-stdlib* collision (names OmegaSL doesn't model, like
Metal `add_const`) rather than a load-bearing one.

#### 5.1.2 `degrees` / `radians` — MSL inlines because the Metal stdlib doesn't have them

`degrees(x)` and `radians(x)` are first-class HLSL and GLSL intrinsics
on scalars and vectors. They are absent from the Metal Shading Language
stdlib in every spec rev to date (verified through MSL 3.x). The MSL
backend therefore overrides `tryEmitBuiltinCall` and inlines each call
as a multiplication by the matching π constant — `(x) * 57.295…` for
`degrees`, `(x) * 0.01745…` for `radians`. Vector arguments work
without extra glue because `scalar * vec` broadcasts in MSL.

The constants are written in full double precision so the float
narrowing at use site picks the IEEE-best single-precision value. The
`MSLTarget::needsMangling` set used to list `degrees` / `radians` as
collidable Metal stdlib names — that was wrong (the names aren't
defined in Metal at all) and they have been removed so a user function
called `degrees` doesn't get spuriously mangled.

### 5.2 Vector math not yet listed [LANDED — distance / faceforward / refract / inverse / any / all + bool vectors]

**Fix (2026-07-01) — `dot()` / `cross()` as binary-expression operands.**
The two oldest vector builtins carry `"SCALAR_TYPE"` / `"VECTOR_TYPE"`
placeholder return types on their `FuncType` records, and Sema's CALL_EXPR
branch returned that placeholder verbatim. A placeholder resolves to no real
type, so any call used as a binary-expression operand (`v + cross(u, t)`,
`1.0 + dot(a, b)`) failed the binary type match — while the same call as a
full initializer (`float3 c = cross(u, t);`) slipped through because the
unresolvable placeholder skips the initializer check. Surfaced by AQUA's
Phase 5 kernel port (quaternion rotate is exactly the failing shape). Fixed
in `Sema.cpp`'s dot/cross branch: return the CONCRETE type — `float3` for
`cross`, the component scalar of the (now validated: same-typed numeric
vectors) arguments for `dot` — and stamp the args' `resolvedType` like the
other builtin branches. Test: `dot_cross_operand.omegasl`.

| Function | Purpose | Status |
|----------|---------|--------|
| `distance(a,b)` | length of difference | **landed** — 2-arg, returns scalar `float`; native on all three backends |
| `faceforward(n,i,ng)` | flip normal toward viewer | **landed** — 3-arg, returns `n`'s type; native on all three |
| `refract(i,n,eta)` | refraction vector | **landed** — 3-arg (`eta` scalar), returns `i`'s type; native on all three |
| `any(v)` / `all(v)` | boolean reduce | **landed** — 1-arg, `boolN` → scalar `bool`; native everywhere. Unblocked by bool-vector types + comparison builtins; see below |
| `lessThan` / `lessThanEqual` / `greaterThan` / `greaterThanEqual` / `equal` / `notEqual` | component-wise compare | **landed** — 2-arg `(vecN, vecN)` → `boolN`; GLSL native, HLSL/MSL lower to the operator form. See below |
| `bool2` / `bool3` / `bool4` | bool-vector types | **landed** — `bool2/3/4` (HLSL/MSL) / `bvec2/3/4` (GLSL); `make_boolN` constructors; universally supported (no feature gate) |
| `transpose(m)` / `determinant(m)` | matrix ops | already shipped (string-matched in Sema) |
| `inverse(m)` | matrix inverse | **landed** — GLSL native; HLSL **and** MSL emulated (neither has it) |

**Landed — distance / faceforward / refract**

These three are first-class intrinsics on HLSL, MSL, and GLSL with identical
spelling, so they pass straight through codegen with no rename or special
emit. They slot into the existing string-matched math dispatch in
`Sema::performSemForExpr`:

* `distance` is the 2-arg analogue of `length` — it gets the same
  `returnsScalar` treatment (return type `float`) with `expectedArgs = 2`.
* `faceforward` and `refract` join the 3-arg bucket and return the first
  argument's (vector) type. `refract`'s third argument `eta` is a scalar;
  the per-argument validation is loose (it only requires each argument to
  type-check, not to match the first), so the scalar `eta` is accepted with
  no special case — the same way the pre-existing 3-arg `clamp`/`lerp` bucket
  already tolerates mixed argument shapes.

Registered as `BUILTIN_DISTANCE` / `BUILTIN_FACEFORWARD` / `BUILTIN_REFRACT`
in `AST.def`, added to `ast::isReservedBuiltinName` (so a user `func refract`
is rejected), and matched by name in the math dispatch. No `Target` hook is
needed.

**Landed — inverse (the doc was wrong about MSL)**

The original note claimed "GLSL/MSL do [have `inverse`]." That is false for
MSL — `metal::inverse` is an undeclared identifier (verified by compiling a
probe with `xcrun metal`). HLSL has never had a matrix `inverse` intrinsic
either. So **both HLSL and MSL** must emulate it; only GLSL is native
(`inverse(mat2/3/4)`, core since GLSL 1.40).

* **GLSL**: passes through as `inverse(m)` (no hook).
* **HLSL / MSL**: `inverse(m)` is rewritten by each backend's
  `tryEmitBuiltinCall` to call the shared `CodeGen::emitInverseCall`, which
  injects an **adjugate expansion** via the existing statement-injection
  hook (`queuePendingStatement`, the same machinery HLSL's `frexp` /
  `getDimensions` use). For a call `inverse(m)` it queues three lines ahead
  of the statement:

  ```
  floatNxN _invK_m = <arg>;                      // capture once — single eval
  float    _invK_id = 1.0 / (<det expansion>);   // inverse determinant
  floatNxN _invK    = floatNxN(<adjugate entries> * _invK_id);
  ```

  and emits `_invK` at the call site. The determinant and the adjugate
  entries are generated programmatically by Laplace expansion (`entry(i,j)
  = (-1)^(i+j) * minor(remove row j, col i) / det`), so the 2×2 / 3×3 / 4×4
  cases share one code path. The formula was verified numerically on the
  host (`inverse(M) * M == I` for 60k random matrices) before transcription.

* **Why the generated text is identical for HLSL and MSL.** OmegaSL stores
  matrices column-major. HLSL emits matrix multiply as `mul(a,b)` and swaps
  index subscripts; MSL uses native `a * b` and column-major indexing. These
  two differences *cancel* for a pure scalar formula: in both backends the
  flat constructor scalar at position `N*i+j` maps to the native element
  `m[i][j]`, and the row-major-vs-column-major transpose duality cancels with
  the `mul`-vs-`*` matmul-convention difference (because `M·M⁻¹ = M⁻¹·M = I`).
  So `emitInverseCall` lives on `CodeGen` and both backends call it with no
  per-backend divergence. Confirmed by compiling the emitted HLSL (`dxc
  -T cs_6_0`), MSL (`xcrun metal`), and the native GLSL (`glslc`).

* **Square matrices only** (`float2x2` / `float3x3` / `float4x4`) — inverse
  is undefined otherwise. Sema rejects a non-square or non-matrix argument
  with a `TypeError`.

**Latent bug fixed alongside — MSL entry-body statement injection.** The
statement-injection hook drains queued lines in `CodeGen::emitStatementLine`,
which `generateBlock` (HLSL entry bodies, all user-function bodies, nested
blocks) routes through. But `MSLTarget::emitShaderEntryBody` had its own
hand-rolled statement loop that called `generateDecl` / `generateExpr`
directly and never drained `pendingStatements` — so any injected builtin used
at the *top level of a Metal entry body* (not inside a nested block or helper)
silently dropped its queued temporaries and emitted a dangling reference. This
had been latent because the only prior injectors (`frexp`, `getDimensions`)
are HLSL-only, and HLSL uses the base `generateBlock` path. `inverse` injects
on MSL too, which surfaced it. Fixed by routing the MSL entry-body loop
through `emitStatementLine` (byte-identical when nothing is queued; GLSL's own
entry-body loop has the same shape but needs no fix because its only lowerings
— `inverse` is native there, `saturate`/`fmod` emit inline — never queue).

**Landed — any / all + bool vectors + component-wise comparison.**

`any(v)` / `all(v)` reduce a *bool vector* to a scalar bool. The original
blocker was that OmegaSL had no bool-vector types and no way to produce one
(§3.2 deliberately made `<`, `==`, `&&` yield a *scalar* `bool`, and GLSL is
strict — its `any`/`all` reject anything but a `bvec`). This pass adds the
whole surface: bool-vector types, constructors, GLSL-shaped comparison
builtins, and the two reducers. The relational *operators* still yield scalar
bool (§3.2 unchanged); component-wise comparison is a named builtin so the
two never collide and the surface stays portable to GLSL (which has no vector
`<`).

* **Types.** `bool2` / `bool3` / `bool4` are builtin types
  (`KW_TY_BOOL2/3/4`, `bool{2,3,4}_type`). They map to native `bool2/3/4`
  on HLSL and MSL, and `bvec2/3/4` on GLSL. Universally supported, so —
  unlike the §4.1/§4.2 numeric families — they are **not** feature-gated.
  Registered in `vectorComponentInfo` / `vectorTypeForScalarArity`, so
  swizzles work (`bv.xy` → `bool2`).

  *Latent index-asymmetry fixed alongside.* The `INDEX_EXPR` Sema arm
  hardcoded float/int/uint vectors, so `long2.x` (swizzle, generalized by
  the §2.3 fix) worked but `long2[0]` (index) errored — and the same held
  for every half / short / ushort / long / ulong family, now bool too. The
  arm now routes through `vectorComponentInfo`, so index and swizzle stay in
  lockstep across all vector families. Codegen needed no change (the default
  `emitIndexExpr` path already emits a generic `lhs[idx]`; the matrix-column
  rewrites key off `isOmegaSLMatrixType`, which vectors never match).

  *Index-type validation made uniform alongside.* Indexing now enforces an
  `int`/`uint` index everywhere it was previously skipped — both the vector
  path (the old float/int/uint vector code never checked) and the matrix
  column path. The two-level `m[col][row]` form is covered for free: `m[col]`
  resolves to a column vector, so the outer `[row]` is validated by the vector
  rule. (Array and buffer indices were already checked.) Covered by index
  lines added to `bool_vector.omegasl`, `numeric_16bit.omegasl`, and
  `numeric_64bit.omegasl`; non-int index diagnostics in
  `invalid_bool_vector.omegasl` (vector) and `invalid_matrix_index.omegasl`
  (matrix column + two-level row).

* **Constructors.** `make_bool2/3/4` join the existing `make_*` FuncType
  family (`bool2/3/4` on HLSL/MSL, `bvec2/3/4` on GLSL via `renameBuiltin`).
  Argument validation is loose — the same generic path `make_int2` uses.

* **Comparison builtins.** `lessThan` / `lessThanEqual` / `greaterThan` /
  `greaterThanEqual` / `equal` / `notEqual` are string-matched in the
  `Sema::performSemForExpr` math dispatch (no FuncType registration, like
  `length` / `inverse`). Each takes two operands of the *same* numeric
  vector type (floatN / intN / uintN / the 16-/64-bit families) and returns
  the bool vector of matching arity. Bool-vector operands are rejected (one
  uniform rule — ordered compares are nonsensical on bools, and keeping
  `equal`/`notEqual` numeric-only avoids a second arity table). Per backend:
  * **GLSL**: native functions — pass through `renameBuiltin` unchanged.
  * **HLSL / MSL**: neither has these functions, but `a < b` on vectors
    yields a bool vector on both. The call lowers to `(arg0 OP arg1)` via
    the shared `CodeGen::emitVectorCompare` (the same shared-helper pattern
    as `emitInverseCall`), invoked from each backend's `tryEmitBuiltinCall`.
    The emitted text is identical on both backends.

* **any / all.** 1-arg, `boolN` → scalar `bool`. Identical spelling on every
  backend (HLSL/MSL/GLSL all have `any`/`all`), so they pass straight through
  `renameBuiltin` with no hook. Sema requires a `bool2/3/4` argument (a
  scalar `bool` or a numeric vector is rejected, matching GLSL's strictness).

**Tests:** `bool_vector.omegasl` (all six comparisons on float2/3/4 reduced
with any/all; int/uint comparisons; `make_bool2/3/4` incl. a nested compare
result; bool-vector swizzle `mk4.xy` / `mk3.yz`). Metal-compiled end-to-end;
HLSL verified via `--hlsl -S` (operator form + `any`/`all`), GLSL via
`--glsl -S` (`lessThan`/… + `bvec` + `any`/`all`). `invalid_bool_vector.omegasl`
(multi-error negative: any/all on a scalar bool and on a numeric vector;
comparison arg-count; mismatched operand types; non-vector operands).

**Tests:** `math_phase5_2.omegasl` (distance on float2/3/4; faceforward +
refract on float3 and float2; inverse on 2×2/3×3/4×4 and on a binary-expr
argument `inverse(a * a)` to exercise single-eval capture). Metal-compiled
end-to-end; HLSL via `dxc -T cs_6_0` and GLSL via `glslc` verified manually.
`invalid_math_phase5_2.omegasl` (multi-error negative: arg counts for
distance / faceforward / refract; inverse on a non-square `float3x2`, on a
non-matrix `float3`, and with the wrong arg count).

**GPU runtime check.** `gte/tests/matrix_ops_test.cpp` is a backend-independent
(Metal / Vulkan / D3D12) compute test that dispatches `inverse` / matrix-multiply
/ matrix×vector / `transpose` / `determinant` on the GPU and reads the results
back, asserting convention-free identities — `inverse(A)*A == I` and
`A*inverse(A) == I` on 2×2/3×3/4×4, `A*I == A`, `transpose(transpose(A)) == A`,
`transpose(A)[c][r] == A[r][c]`, `inverse(A)*(A*v) == v` — plus host-computed
determinants. Registered as `omegagte_matrix_ops` in each backend's test
`CMakeLists.txt`. Verified passing on Metal here; run on D3D12 / Vulkan to
confirm those backends. Writing this test surfaced and fixed the Metal
buffer-IO trailing-padding bug noted under §2.4.

### 5.3 Integer / bitfield ops [COMPLETED — all six ops, Phases A+B+C]

| Function | HLSL | MSL | GLSL | Status |
|----------|------|-----|------|--------|
| `countbits`       | yes (scalar-uint) | `popcount` | `bitCount` | **Phase A landed** |
| `reversebits`     | yes (scalar-uint) | `reverse_bits` | `bitfieldReverse` | **Phase A landed** |
| `firstbitlow`     | yes (scalar/vec/signed) | `ctz` | `findLSB` | **Phase B landed** |
| `firstbithigh`    | yes (scalar/vec/signed) | `clz` | `findMSB` | **Phase B landed** |
| `bitfieldExtract` | **no named intrinsic (manual shift/mask)** | `extract_bits` | `bitfieldExtract` | **Phase C landed** |
| `bitfieldInsert`  | **no named intrinsic (manual mask/or)** | `insert_bits` | `bitfieldInsert` | **Phase C landed** |

Needed for hashing, compression formats, occupancy masks. Operand domain
is `int` / `uint` and their vector forms; firstbit ops will normalize to a
GLSL-`findMSB`-style signed index (`-1` on zero) across all three backends.
Full phasing and the per-backend lowering contract: see
`OmegaSL-5.3-Integer-Bitfield-Ops-Plan.md`. (The original table's
"`bitfieldExtract`/`bitfieldInsert` = yes" for HLSL was optimistic — HLSL
has no such named intrinsic; same doc-vs-reality gap as §5.2's "MSL has
`inverse`".)

**Phase A — `countbits` / `reversebits` (landed).** Both are 1-arg,
return the operand's type, and accept integer scalar/vector operands
(`int`/`uint`/`intN`/`uintN`). They are string-matched in the
`Sema::performSemForExpr` math dispatch (a new 1-arg integer bucket,
`isIntUnary`) — no `FuncType` registration, like `distance`/`inverse` —
and added to `ast::isReservedBuiltinName`. No feature bit (universal at
SM5 / MSL native / GLSL 4.x core). Per backend:

* **MSL** — plain `renameBuiltin`: `countbits → popcount`,
  `reversebits → reverse_bits` (both native on scalars and vectors,
  `T → T`).
* **GLSL** — `reversebits → bitfieldReverse` via `renameBuiltin` (native
  `T → T`). `countbits` is *not* a plain rename: GLSL's `bitCount` returns
  a signed `iN` even for a `uint` operand, so `tryEmitBuiltinCall` wraps it
  in a cast back to the operand type (`uint(bitCount(u))` /
  `uvec4(bitCount(uv))`).
* **HLSL** — `countbits` / `reversebits` are **scalar-`uint` only**, so a
  shared `hlslEmitIntUnary` helper in `tryEmitBuiltinCall` (a) casts the
  scalar result back for a signed operand (`(int)countbits(i)`) and
  (b) expands a vector operand component-wise into the matching
  constructor, capturing the operand in a single-eval temp first via the
  statement-injection hook (`uint4 _bb0 = uv; uint4(countbits(_bb0.x),
  …)`).

Tests: `bitfield_ops.omegasl` (scalar + vector `countbits`/`reversebits`
on int/uint), `invalid_bitfield_ops.omegasl` (float operand + arg-count
negatives). Metal compiled end-to-end; the emitted HLSL compiles under
`dxc -T cs_6_0` and the emitted GLSL under `glslc -fshader-stage=comp`
(both installed on this host) — not just `-S` source inspection.

**Phase B — `firstbithigh` / `firstbitlow` (landed, fully normalized).**
Both are 1-arg, accept the same integer scalar/vector operands, but return
a signed `int` / `intN` giving the **zero-based index of the highest /
lowest set bit, or `-1` when no bit is set** — *identical on every
backend*, including the zero and sign-bit cases. The operand is treated as
its raw unsigned bit pattern so signed and unsigned operands agree. GLSL
`findMSB`/`findLSB` is the reference contract; the host-equivalence of all
three lowerings was checked numerically before coding. Per backend:

* **GLSL** — `findMSB(uint(x))` / `findLSB(uint(x))` (native; already returns
  the signed index and `-1` on zero).
* **HLSL** — `(intN)firstbithigh(uintN(x))` / `(intN)firstbitlow(uintN(x))`.
  Unlike `countbits`, HLSL's `firstbit*` accept scalar, vector, *and* signed
  operands (verified with dxc), so no component expansion is needed; the
  `0xFFFFFFFF`-on-zero result maps to `-1` for free via the `(int)` cast.
* **MSL** — `firstbithigh → (31 - intN(clz(uintN(x))))` (`clz(0)==32` gives
  `-1`); `firstbitlow → select(intN(ctz(uintN(x))), intN(-1), x==0)` (MSL's
  `ctz(0)==32`, so the zero case is fixed with a component-wise `select`).

A shared `CodeGen::intOperandShape` classifies the operand (signedness +
arity) for all three backends. The normalization is proven on **real GPU
hardware** by `gte/tests/bitfield_ops_test.cpp` (`omegagte_bitfield_ops`),
a compute dispatch that runs all four ops on edge-case operands
(`0` / `1` / `0x80000000` / `0xFFFFFFFF`) and asserts the readback matches
the host contract (`-1` on zero, index 0/31/31, popcount 0/1/1/32, and
`reversebits` against a host reference computed on-GPU to avoid float
round-trip loss).

Verification: full omegasl ctest suite (96 tests) green; the GPU
`omegagte_bitfield_ops` test passes on Metal; emitted HLSL/GLSL compile
under dxc / glslc. (D3D12/Vulkan runtime still pending CI, but the source
compiles on their real toolchains here.)

**Phase C — `bitfieldExtract` / `bitfieldInsert` (landed).**
`bitfieldExtract(value, offset, bits)` (3-arg) and `bitfieldInsert(base,
insert, offset, bits)` (4-arg). The `value`/`base`/`insert` operands are
integer scalar/vector (intN/uintN, same type for base & insert); `offset` /
`bits` are scalar int/uint. Unsigned operands zero-extend, signed operands
sign-extend; the vector forms apply the scalar offset/bits to every
component. Return type is the value/base operand type.

* **GLSL** — native `bitfieldExtract`/`bitfieldInsert`. offset/bits cast to
  `int` (GLSL signature); the value operands are cast to their own type
  spelling so a bare integer literal (which emits without uint-ness) can't
  make the overload ambiguous.
* **MSL** — native `extract_bits`/`insert_bits`. offset/bits cast to `uint`;
  value operands cast for the same overload-disambiguation reason.
* **HLSL** — **no named intrinsic** (confirmed with dxc; the original survey
  table's "yes" was wrong, like §5.2's "MSL has `inverse`"). Lowered to a
  manual shift+mask via shared `CodeGen::emitHLSLBitfield{Extract,Insert}`,
  which queue single-eval temps (statement injection): unsigned extract is
  `(v>>off)&mask`; signed extract is the arithmetic shift-left/shift-right
  sign-extend with a `bits==0` guard; insert is `(base & ~(mask<<off)) |
  ((insert & mask)<<off)`. The lowering was proven **bit-identical to the
  GLSL/MSL spec across 210 (value,offset,bits) combinations** on the host —
  including `bits==0` and every signed sign-extension case — before coding.

A real bug was caught by the GPU runtime test (not by `-S` inspection): a
bare `0xFFu` insert argument emits as `255`, which made Metal's
`insert_bits` overload set ambiguous and failed the runtime Metal compile.
Fixed by casting the value/insert operands to the operand-type spelling on
both native backends. This is exactly the class of error the
`omegagte_bitfield_ops` GPU test exists to catch — `bitfield_ops_test.cpp`
now also extracts (unsigned + signed) and inserts on a known operand and
asserts the GPU result against host references computed on-GPU.

**§5.3 complete.** All six ops land with: 96-test omegasl ctest suite green,
`omegagte_bitfield_ops` passing on real Metal hardware, and every emitted
HLSL/GLSL shader compiling under dxc / glslc. D3D12/Vulkan runtime binding
is the only thing still pending CI (the generated source compiles on their
real toolchains here).

### 5.4 Derivatives [COMPLETED]

`ddx`, `ddy`, `fwidth`, and the `_coarse` / `_fine` variants. Required for
mip-level selection in non-sampled paths, normal-map reconstruction,
anti-aliased lines/fonts.

**Landed.** Nine new 1-arg builtins, all "same float type in, same type out"
on `float` / `float2` / `float3` / `float4`, all **fragment-stage-only**:

| Name | Op |
|------|----|
| `ddx(x)`, `ddy(x)` | screen-space derivative (default precision) |
| `fwidth(x)` | `abs(ddx(x)) + abs(ddy(x))` |
| `ddx_coarse(x)`, `ddy_coarse(x)` | coarse-quad derivative (2×2 once per quad) |
| `ddx_fine(x)`, `ddy_fine(x)` | fine derivative (per-pixel) |
| `fwidth_coarse(x)`, `fwidth_fine(x)` | matching width forms |

Per-backend lowering:

| OmegaSL | HLSL (SM 5.0+) | MSL | GLSL (4.50 core) |
|---------|----------------|-----|------------------|
| `ddx` | `ddx` | `dfdx` (rename) | `dFdx` (rename) |
| `ddy` | `ddy` | `dfdy` | `dFdy` |
| `fwidth` | `fwidth` | `fwidth` | `fwidth` |
| `ddx_coarse` | `ddx_coarse` | `dfdx` (widen — MSL has no coarse/fine) | `dFdxCoarse` |
| `ddx_fine` | `ddx_fine` | `dfdx` (widen) | `dFdxFine` |
| `ddy_coarse` | `ddy_coarse` | `dfdy` (widen) | `dFdyCoarse` |
| `ddy_fine` | `ddy_fine` | `dfdy` (widen) | `dFdyFine` |
| `fwidth_coarse` | `abs(ddx_coarse(t)) + abs(ddy_coarse(t))` (HLSL has no name) | `fwidth` (widen) | `fwidthCoarse` |
| `fwidth_fine` | `abs(ddx_fine(t)) + abs(ddy_fine(t))` | `fwidth` (widen) | `fwidthFine` |

Notes:

* **HLSL** has the four `ddx_coarse`/`ddx_fine`/`ddy_coarse`/`ddy_fine`
  intrinsics (SM 5.0+) but **no** `fwidth_coarse`/`fwidth_fine`. Those two
  lower inline via the existing statement-injection hook
  (`queuePendingStatement`, shared with `frexp`/`inverse`/`getDimensions`):
  capture the operand once into a temp, then emit
  `(abs(ddx_coarse(_t)) + abs(ddy_coarse(_t)))`.
* **MSL** has only `dfdx`/`dfdy`/`fwidth` — no coarse/fine variants. The
  coarse/fine OmegaSL names silently widen to the unqualified op (a
  hardware hint, not a hard contract — the unqualified form is whatever
  the GPU picks).
* **GLSL** has all nine in core 4.50 (was `GL_ARB_derivative_control` in
  earlier revs). The preamble already targets `#version 450`, so no
  extension line was needed.

**Stage restriction.** All nine are fragment-stage-only. The Sema check
mirrors §6.2 (`threadgroupBarrier`) / §2a (`setMeshOutputs`): an
`InvalidAttribute` diagnostic when `funcContext->shaderType !=
ast::ShaderDecl::Fragment`. No feature bit — derivatives are universal in
fragment stage on every backend, and the HLSL `fwidth_coarse`/`fine`
lowering keeps coverage uniform across backends without a `#requires`
gate.

(HLSL SM 6.6 added compute-shader `ddx_quads`/`QuadAny`/`QuadAll`;
deliberately out of scope. The portable surface is fragment-only.)

**Implementation.**

* `AST.def` — nine `BUILTIN_DDX_*` / `BUILTIN_DDY_*` / `BUILTIN_FWIDTH_*`
  macros under a `§5.4` comment block.
* `AST.cpp` — `isReservedBuiltinName` gets all nine.
* `Sema.cpp` — a new `isDerivative` bucket in `performSemForExpr`:
  `expectedArgs = 1`, fragment-stage guard, operand must be
  `float`/`float2`/`float3`/`float4` (rejected via `TypeError`
  otherwise), return type = operand type. Stamps
  `args[0]->resolvedType` so the HLSL `fwidth_coarse`/`fine` lowering
  can spell the operand-typed temp.
* `MSLTarget::renameBuiltin` — six renames (`ddx`/`ddx_coarse`/`ddx_fine`
  → `dfdx`, three `ddy*` → `dfdy`, `fwidth_coarse`/`fwidth_fine` →
  `fwidth`); `fwidth` itself passes through.
* `GLSLTarget::renameBuiltin` — nine renames to the `dFdx`/`dFdy`/
  `dFdxCoarse`/... spellings.
* `HLSLTarget::renameBuiltin` — no-op (the seven HLSL-native names pass
  through). `HLSLTarget::tryEmitBuiltinCall` adds a small
  `hlslEmitFwidthVariant` helper for `fwidth_coarse`/`fwidth_fine`:
  capture-into-temp via `queuePendingStatement`, then emit
  `(abs(ddx_<v>(t)) + abs(ddy_<v>(t)))`.

**Tests.** `derivatives.omegasl` (fragment shader with all nine builtins
on `float` and every `floatN`), `invalid_derivatives.omegasl` (multi-error
negative: `ddx` in a vertex shader / `ddy` of `int` / `fwidth` arg-count).

**Out of scope (intentional follow-ups).**

* **Compute-stage derivatives** (HLSL SM 6.6 `ddx_quads` / Metal quad simd
  ops). Backend-asymmetric and rarely needed; revisit if a real shader
  hits the wall.
* **GLSL precision hints** (`mediump`/`highp` on derivative output).
  Belongs with §11 precision modifiers.

### 5.5 Pack / unpack [COMPLETED — all three phases]

| Function | HLSL | MSL | GLSL |
|----------|------|-----|------|
| `f16tof32` / `f32tof16` | yes | `as_type<half2>(uint)` / reverse | `unpackHalf2x16` / `packHalf2x16` |
| `pack_snorm4x8` / `unpack_snorm4x8` | no direct, pattern | `pack_float_to_snorm4x8` | `packSnorm4x8` |
| `asint` / `asuint` / `asfloat` | yes | `as_type<>` | `floatBitsToInt` family |

Bit-level reinterpretation; needed to decode vertex/mesh buffer formats that
the CPU side packs into `uint` channels.

**Scope, naming, and ordering** (per Alex):

* **Naming.** Use the shorter / simpler spelling per row: HLSL names for the
  bit-reinterpret family (`asint` / `asuint` / `asfloat` — these spellings
  exist in HLSL verbatim; the GLSL `floatBitsToInt` / `intBitsToFloat`
  variants are routed inside the GLSL backend), and GLSL names for the
  pack/unpack families (`packHalf2x16` / `unpackHalf2x16` / `packSnorm4x8`
  / `packUnorm4x8` / `packSnorm2x16` / `packUnorm2x16` and their
  `unpack*` inverses — `pack_float_to_snorm4x8` is the longer MSL form).
  HLSL also keeps its scalar half-float forms (`f16tof32` / `f32tof16`)
  alongside the packed pair so the common scalar-decode path costs no
  extra arithmetic on HLSL.
* **Universal — no `#requires` gate.** Every operation is expressible on
  every backend; the holes (no native packed-half on HLSL; no native
  pack/unpack snorm/unorm on HLSL) are covered by per-backend inline
  lowerings, same pattern as §5.3 bitfieldExtract/Insert.
* **No stage restriction.** All three groups are valid in every stage
  (matches §5.3; the §5.4 fragment-only restriction was derivative-
  specific).
* **Phasing.** Three phases, smallest first, each builds on the prior
  phase's pattern for reviewability:
  * **Phase A** — `asint` / `asuint` / `asfloat` bit-reinterpret. No
    statement injection. Sema bucket `isBitReinterpret`; per-backend
    routing in `tryEmitBuiltinCall` because GLSL spells the function
    by operand type (`floatBitsToInt` vs `intBitsToFloat`).
  * **Phase B** — half-float pack/unpack. Four builtins:
    `packHalf2x16(float2)→uint`, `unpackHalf2x16(uint)→float2`,
    `f16tof32(uint)→float`, `f32tof16(float)→uint`. HLSL has the
    scalar pair native; the packed pair lowers inline via statement
    injection. GLSL has the packed pair native; the scalar pair lowers
    via `unpackHalf2x16` / `packHalf2x16` of a paired-zero. MSL uses
    `as_type<half2>` / `as_type<uint>` plus `half`/`float` casts.
  * **Phase C** — snorm / unorm pack family, all eight builtins
    (`pack{Snorm,Unorm}{4x8,2x16}` + the matching `unpack*`). GLSL
    native; MSL native (per-name `pack_float_to_*` / `unpack_*_to_float`
    via `renameBuiltin`); HLSL lowered via shared
    `CodeGen::emitHLSLPackNormalized` / `emitHLSLUnpackNormalized`
    helpers using statement injection (single-eval temp + clamp +
    multiply + round + mask + OR-shift, the GLSL-spec lowering host-
    verified before coding — same discipline as §5.3 Phase C).

Each phase adds its own `*.omegasl` compile test and an
`invalid_*.omegasl` fail test under `gte/omegasl/tests/`, wired into
`gte/omegasl/tests/CMakeLists.txt`. Phase B/C also pick up an
`omegagte_pack_ops` GPU runtime test (modeled on
`gte/tests/bitfield_ops_test.cpp`) that round-trips each lowering
through a real Metal dispatch and asserts the readback matches a
host-computed reference, so the HLSL lowering's bit-identity claim is
proven before the doc records it (the §5.3 Phase C / §2.4 follow-up
discipline — `-S` source inspection alone misses overload-ambiguity
and trailing-padding bugs).

**Phase A — `asint` / `asuint` / `asfloat` (landed).** All three are
1-arg, accept any 32-bit numeric scalar or vector
(`float`/`int`/`uint` and their `N`-component vector forms), and
return a same-arity scalar/vector of the target type fixed by the
builtin name (`asint`→`intN`, `asuint`→`uintN`, `asfloat`→`floatN`).
16-bit and 64-bit operands are rejected — only the 32-bit reinterpret
is uniform across backends. Implementation:

* **AST**: `BUILTIN_ASINT` / `BUILTIN_ASUINT` / `BUILTIN_ASFLOAT` in
  `AST.def`; added to `ast::isReservedBuiltinName`.
* **Sema**: a new `isBitReinterpret` bucket in `performSemForExpr` —
  `expectedArgs = 1`, operand-shape validated via `vectorComponentInfo`,
  return type built via `vectorTypeForScalarArity(targetScalar, arity)`.
  Stamps `args[0]->resolvedType` so the GLSL backend can pick the right
  spelling.
* **HLSL**: nothing to do — `asint` / `asuint` / `asfloat` are native HLSL
  intrinsics on scalar AND vector operands (verified by reading the dxc
  source), and the shared `renameBuiltin` pass-through + `(args)` print
  emits them verbatim.
* **MSL**: a single `tryEmitBuiltinCall` arm spelling
  `as_type<TargetN>(operand)`, with TargetN computed from the builtin
  name (`int`/`uint`/`float`) and operand arity (1/2/3/4). MSL accepts
  identity (`as_type<int>(intN)`) as a no-op.
* **GLSL**: a `tryEmitBuiltinCall` arm with a 3×3 dispatch table on the
  (source, target) signedness-family pair: float↔int/uint use the four
  named functions (`floatBitsToInt` / `floatBitsToUint` / `intBitsToFloat` /
  `uintBitsToFloat` — the same name overloads on scalar and `vecN`); int↔uint
  emit a constructor cast (`uvecN(x)` / `ivecN(x)` — per GLSL spec 4.1.3,
  int↔uint conversion preserves the bit pattern when in range and
  reinterprets otherwise, matching the `asint`/`asuint` cross-sign
  contract); same-target identity emits the operand bare (GLSL has no
  `intBitsToInt` / `floatBitsToFloat`).

Tests: `bit_reinterpret.omegasl` (every source/target pair across
scalar and float2/3/4, int2/3/4, uint2/3/4 — including identity and
cross-sign on every arity), `invalid_bit_reinterpret.omegasl` (multi-
error: arg-count, half operand, bool operand, matrix operand). Full
103-test omegasl ctest suite green; Metal compiles end-to-end via the
compile-test wrapper (it invokes `omegaslc -o .omegasllib`, which runs
the full pipeline including the Metal toolchain on this host).
HLSL/GLSL source emission verified by `-S` inspection across both
scalar and vector forms. No runtime feature bit (universal on every
backend, matching §5.3 Phase A and §5.4).

**Phase B — half-float pack / unpack (landed).** Four 1-arg builtins,
each with a fixed operand and fixed return type (no overloading):

|     OmegaSL                | Operand   | Returns | HLSL                              | MSL                                  | GLSL                              |
|----------------------------|-----------|---------|-----------------------------------|--------------------------------------|-----------------------------------|
| `f16tof32(x)`              | `uint`    | `float` | `f16tof32(x)` native              | `float(as_type<half>(ushort(x)))`    | `unpackHalf2x16(x).x`             |
| `f32tof16(x)`              | `float`   | `uint`  | `f32tof16(x)` native              | `uint(as_type<ushort>(half(x)))`     | `packHalf2x16(vec2(x, 0.0))`      |
| `packHalf2x16(v)`          | `float2`  | `uint`  | inline lowering, see below        | `as_type<uint>(half2(v))`            | `packHalf2x16(v)` native          |
| `unpackHalf2x16(u)`        | `uint`    | `float2`| inline lowering, see below        | `float2(as_type<half2>(u))`          | `unpackHalf2x16(u)` native        |

Implementation:

* **AST**: `BUILTIN_F16TOF32` / `BUILTIN_F32TOF16` / `BUILTIN_PACK_HALF_2X16`
  / `BUILTIN_UNPACK_HALF_2X16` in `AST.def`; added to
  `ast::isReservedBuiltinName`.
* **Sema**: a new `isHalfPackUnpack` bucket in `performSemForExpr` —
  `expectedArgs = 1`, fixed-shape per-builtin operand validation
  (`uint` / `float` / `float2`), fixed return type per builtin name.
  Stamps `args[0]->resolvedType` for the HLSL statement-injection
  lowering on the packed pair.
* **HLSL**: `f16tof32` / `f32tof16` are native (SM 5.0+) on scalar AND
  vector operands and pass through the shared `renameBuiltin` /
  `(args)` print path. `packHalf2x16` / `unpackHalf2x16` have no
  native form; lowered via two shared helpers `hlslEmitPackHalf2x16`
  / `hlslEmitUnpackHalf2x16` that queue a single-eval temp through
  statement injection, then emit the canonical formula over the
  scalar pair:
  * `packHalf2x16(v)`   → `(f32tof16(_ph<id>.x) | (f32tof16(_ph<id>.y) << 16))`
  * `unpackHalf2x16(u)` → `float2(f16tof32(_uh<id> & 0xFFFFu), f16tof32(_uh<id> >> 16))`
  Adjacent `pack`/`unpack` calls in the same statement get distinct
  temp ids and flush in dependency order (verified by `-S` inspection).
* **MSL**: all four lower inline via `as_type<>` of the matching
  half-typed value (no statement injection — each form is a single
  sub-expression). The `ushort(x)` cast in `f16tof32` truncates `x`
  to the low 16 bits naturally; `uint(as_type<ushort>(half(x)))` in
  `f32tof16` zero-extends the high 16 bits on promotion (matching the
  spec).
* **GLSL**: `packHalf2x16` / `unpackHalf2x16` are native (GLSL 4.20+,
  core in 4.50 — the preamble already targets `#version 450`) and
  pass through the shared `(args)` print path without a rename. The
  scalar pair lowers inline through the packed forms — `f16tof32(x)`
  uses `unpackHalf2x16(x).x` (discarding the high half), and
  `f32tof16(x)` uses `packHalf2x16(vec2(x, 0.0))` (the high half is
  zero by construction).

Tests:

* **`half_pack.omegasl`** — three compute kernels exercising all four
  builtins: scalar round-trip, packed round-trip, and the cross-form
  consistency case (`f32tof16(a) | (f32tof16(b) << 16)` should equal
  `packHalf2x16(float2(a, b))`, and `unpackHalf2x16` should be the
  inverse). The mixed-forms kernel surfaces the HLSL statement-
  injection adjacency case where two distinct temps must coexist in
  the same scope.
* **`invalid_half_pack.omegasl`** — multi-error fail test covering
  arg-count and per-builtin operand-type rejection (scalar form
  rejecting `float`/`uint` swap; packed form rejecting scalar
  operand).
* **`omegagte_pack_ops`** (`gte/tests/pack_ops_test.cpp` + Metal
  wiring under `gte/tests/metal/CMakeLists.txt`) — GPU runtime test
  modeled on `omegagte_bitfield_ops`. Round-trips four fp16-
  representable inputs (`0.5`, `1.0`, `2.0`, `-3.0` — covers sign-
  bit path, non-integer mantissa, and powers of two) through both
  the scalar and packed forms on Metal hardware, asserting bit-
  identity via on-GPU `asuint(recovered) == asuint(input)` so there
  is no float epsilon to tune. Also verifies cross-form consistency
  and the spec rule that `f32tof16(x) >> 16 == 0`. Host-side IEEE-754
  binary16 encoder included for an extra spot-check of the low 16
  bits of each `packedScalar` lane (catches a per-backend off-by-one
  before the abstract round-trip flag surfaces it).

Verification: full 105-test omegasl ctest suite green (Phase A's 103
plus 2 new); full 8-test GTE Metal GPU suite green (including
`omegagte_pack_ops`); HLSL/GLSL source verified by `-S` inspection
including the adjacency case. No `#requires` feature bit (universal
on every backend). D3D12/Vulkan runtime path is written from the
shared source map but compiles only on those platforms (per
[project-gte-backend-build-verification](memory/project-gte-backend-build-verification.md)).

**Phase C — normalized 4x8 / 2x16 pack / unpack (landed).** Eight 1-arg
builtins, two layouts × two signedness families × pack / unpack:

|     OmegaSL                  | Operand   | Returns  | HLSL                                       | MSL                                  | GLSL                            |
|------------------------------|-----------|----------|--------------------------------------------|--------------------------------------|---------------------------------|
| `packSnorm4x8(v)`            | `float4`  | `uint`   | inline lowering (clamp/×127/round/mask/OR) | `pack_float_to_snorm4x8(v)` rename   | `packSnorm4x8(v)` native        |
| `packUnorm4x8(v)`            | `float4`  | `uint`   | inline lowering (×255)                     | `pack_float_to_unorm4x8(v)` rename   | `packUnorm4x8(v)` native        |
| `packSnorm2x16(v)`           | `float2`  | `uint`   | inline lowering (×32767)                   | `pack_float_to_snorm2x16(v)` rename  | `packSnorm2x16(v)` native       |
| `packUnorm2x16(v)`           | `float2`  | `uint`   | inline lowering (×65535)                   | `pack_float_to_unorm2x16(v)` rename  | `packUnorm2x16(v)` native       |
| `unpackSnorm4x8(u)`          | `uint`    | `float4` | inline lowering (sign-extend/÷127/clamp)   | `unpack_snorm4x8_to_float(u)` rename | `unpackSnorm4x8(u)` native      |
| `unpackUnorm4x8(u)`          | `uint`    | `float4` | inline lowering (mask/÷255)                | `unpack_unorm4x8_to_float(u)` rename | `unpackUnorm4x8(u)` native      |
| `unpackSnorm2x16(u)`         | `uint`    | `float2` | inline lowering (÷32767)                   | `unpack_snorm2x16_to_float(u)` rename| `unpackSnorm2x16(u)` native     |
| `unpackUnorm2x16(u)`         | `uint`    | `float2` | inline lowering (÷65535)                   | `unpack_unorm2x16_to_float(u)` rename| `unpackUnorm2x16(u)` native     |

Implementation:

* **AST**: eight `BUILTIN_*` macros under §5.5 in `AST.def`; added to
  `ast::isReservedBuiltinName`.
* **Sema**: a new `isPackNorm` bucket in `performSemForExpr` —
  `expectedArgs = 1`, fixed-shape operand validation (pack-4x8 takes
  `float4`, pack-2x16 takes `float2`, every `unpack` takes `uint`),
  fixed return type per builtin. Stamps `args[0]->resolvedType` for
  the HLSL statement-injection lowering.
* **HLSL**: shared `CodeGen::emitHLSLPackNormalized` /
  `emitHLSLUnpackNormalized` helpers parameterized by a
  `PackNormKind` enum (`S4`/`U4`/`S2`/`U2`) that encodes per-lane bit
  width, scale (127/255/32767/65535), and signedness. The pack path
  emits clamp → multiply → round → cast to per-lane integer → mask to
  per-lane width → OR-shift assemble; the unpack path emits the
  sign-extend trick `int(u << leftShift) >> rightShift` (snorm) or
  mask + shift (unorm), divides by the scale, and `max(..., -1)`
  clamps the snorm floor (the GLSL spec mandates clamping `-128/127`
  etc. up to -1). Single-eval temp via the §5.3-style statement-
  injection hook, so the helpers compose with adjacent calls.
* **MSL**: eight per-name `renameBuiltin` entries pointing at the
  native `pack_float_to_*` / `unpack_*_to_float` spellings. Each
  signature shape matches OmegaSL's contract (T → uint / uint → T),
  so no `tryEmitBuiltinCall` arm is needed.
* **GLSL**: native — eight pass-throughs (the OmegaSL names already
  match the GLSL spelling exactly, so no `renameBuiltin` entry and no
  `tryEmitBuiltinCall` arm are needed; the shared `(args)` print path
  emits them verbatim).

Tests:

* **`pack_norm.omegasl`** — four compute kernels, one per
  (`snorm4x8`, `unorm4x8`, `snorm2x16`, `unorm2x16`) family, each
  packing a boundary-stressing vector (0, ±1, ½, ¼) and round-
  tripping through the unpack form.
* **`invalid_pack_norm.omegasl`** — multi-error fail test: arg-count,
  `float2` vs `float4` arity mismatch, `uint` vs `float` direction
  mismatch on the pack form, and the inverse on the unpack form.
* **`omegagte_pack_ops`** (extended with a second `packNormOps`
  kernel + dispatch) — host-computes the expected packed uint for
  each of the eight builtins using the GLSL/MSL spec formulas, passes
  them alongside the inputs to the GPU, and asserts:
  * The GPU's packed uints `==` the host-computed packed uints
    (host vs GPU bit-equal — proves the HLSL clamp/round/mask/OR
    chain matches spec, which `-S` source inspection alone cannot).
  * The GPU's unpacked floats round-trip bit-identically to the
    host's `unpack(pack(x))` reference (per-lane via on-GPU
    `asuint(recovered) == asuint(expected)`).
  Inputs are placed exactly on representable byte/short boundaries
  (`0`, `±1`, `0.5`, `0.25`) so host and GPU rounding agree by
  construction.

Verification: full 107-test omegasl ctest suite green (Phase B's 105
plus 2 new); full 8-test GTE Metal GPU suite green (including the
extended `omegagte_pack_ops` covering Phases B and C); HLSL source
verified by `-S` inspection across all eight lowerings (sign-extend
shift math + mask widths checked per lane). No `#requires` feature
bit (universal on every backend).

**Out of scope (intentional follow-ups).**

* **`pack_snorm4x8`-style MSL aliases.** OmegaSL exposes only the GLSL
  names; an MSL-style alias group would be the same rename in reverse
  and is rarely useful in portable code.
* **`packDouble2x32` / `unpackDouble2x32`** (GLSL 4.00+, MSL nonexistent,
  HLSL `asdouble` exists but only as a 2-uint→double bit cast). Belongs
  with a future double-precision pass (out of §5.5 scope).
* **Saturated-on-overflow integer constructors** (e.g. `uint4(saturate(...) * 255)`).
  The `packUnorm4x8` lowering's defensive 0xFFu mask handles the
  pathological-GPU-`round`-overshoot case at the per-lane level; a
  dedicated builtin can land if a real use case appears.

### 5.6 Atomic operations

```
atomic_add(buf[i].counter, 1);
atomic_exchange(buf[i].slot, newValue);
atomic_compare_exchange(buf[i].slot, expected, desired);
atomic_min/max/and/or/xor(...);
```

All three backends have native support; Vulkan additionally has
`atomic_*` on images and on 64-bit integers via extensions.

**The memory-model decision (per Alex).** MSL is the constraint that drives
the whole design: Metal *requires* atomic memory to be declared as an
`atomic_int` / `atomic_uint` type (`metal::atomic<int>` / `<uint>`). You
cannot run an atomic op on a plain `uint`, and once a slot is atomic you
cannot even plainly read it — every access goes through `atomic_*_explicit`.
HLSL (`Interlocked*` on plain `int`/`uint` in a UAV / `groupshared`) and GLSL
(`atomic*` on plain `int`/`uint` in an SSBO / `shared`) carry the atomicity in
the *operation*, not the *type*. So OmegaSL introduces two new builtin scalar
types — **`atomic_int` / `atomic_uint`** — that spell as `atomic_int` /
`atomic_uint` on MSL but as plain `int` / `uint` on HLSL / GLSL, and are only
ever touched through the `atomic_*` intrinsics (plus `atomic_load` /
`atomic_store`). This is the only model that satisfies Metal cleanly; the
alternative (plain `uint` fields + whole-program inference of which fields are
used atomically, rewriting the MSL type) was rejected as fragile.

**Return value (per Alex).** The fetch-ops return the *original* value, like
`atomic_add` on GLSL/MSL (`uint slot = atomic_add(b[0].count, 1);` is the
canonical "allocate-an-index" idiom). GLSL/MSL return it natively; HLSL's
`InterlockedAdd(dest, val, out orig)` writes it to an out-param, so HLSL uses
the existing statement-injection hook (`queuePendingStatement` /
`getDimensionsTempId`, same machinery as `frexp` / `GetDimensions`).

**No feature bit.** 32-bit integer atomics on device / threadgroup memory are
universal (SM5 `Interlocked*`, MSL `atomic_int`/`atomic_uint`, GLSL 4.3 /
Vulkan core), so this carries no `#requires` gate — matching `discard` / MRT /
the §6.2 barriers. 64-bit atomics and image atomics (extension-gated on
Vulkan) are explicitly out of scope.

**Compiler-only, no runtime change.** Like §5.3, this adds no resource form
and no runtime descriptor: the buffer is still a normal `buffer<T>`; an atomic
field is 4 bytes / 4-aligned exactly like `uint`, so `omegaSLStructStride` and
the `GEBufferWriter`/`Reader` path are unaffected (the C++ caller's CPU struct
just uses `uint`/`int`). The only new surface is the *shader-source* type
spelling.

**Phase A — atomic types + fetch-ops + load/store (LANDED)**

New scalar types `atomic_int` / `atomic_uint` and seven 2-arg fetch-ops plus
`atomic_load` / `atomic_store`:

| Builtin | Args | Returns | Notes |
|---------|------|---------|-------|
| `atomic_add(mem, v)` | (atomic, scalar) | original | |
| `atomic_min/max/and/or/xor(mem, v)` | (atomic, scalar) | original | |
| `atomic_exchange(mem, v)` | (atomic, scalar) | original | |
| `atomic_load(mem)` | (atomic) | scalar | |
| `atomic_store(mem, v)` | (atomic, scalar) | void | |

`mem` (arg0) must resolve to `atomic_int` / `atomic_uint` and be a writable
lvalue (walk-to-root + reject `const`, the `modf` out-param check); `load`
needs only the atomic-type check. `v` must match the underlying scalar
(`atomic_int`→`int`, `atomic_uint`→`uint`) or be an integer literal
(`canCoerceLiteralTo`). Ops are stage-agnostic (device-buffer atomics are
valid in any stage; `threadgroup atomic_uint` is already compute-gated by the
§6.1 storage-qualifier rule). Misusing an atomic slot in plain arithmetic /
read produces an ordinary type-mismatch (`atomic_uint` ≠ `uint`), which nudges
toward `atomic_load` — Phase A does not add a dedicated diagnostic for that.

Per-backend lowering (memory ordering is always `memory_order_relaxed` on MSL;
pair with §6.2 `deviceBarrier` when ordering is needed):

| Op | HLSL (statement-injected `out` temp) | MSL (inline, `&mem`) | GLSL (inline) |
|----|--------------------------------------|----------------------|---------------|
| add | `Interlocked Add(dest,v,_atmN)` → `_atmN` | `atomic_fetch_add_explicit` | `atomicAdd` |
| min/max | `InterlockedMin/Max(...)` | `atomic_fetch_min/max_explicit` | `atomicMin/Max` |
| and/or/xor | `InterlockedAnd/Or/Xor(...)` | `atomic_fetch_and/or/xor_explicit` | `atomicAnd/Or/Xor` |
| exchange | `InterlockedExchange(...)` | `atomic_exchange_explicit` | `atomicExchange` |
| load | plain read `mem` (32-bit aligned reads are atomic) | `atomic_load_explicit(&mem,…)` | plain read `mem` |
| store | plain write `(mem = v)` | `atomic_store_explicit(&mem,v,…)` | plain write `(mem = v)` |

The HLSL temp is `int`/`uint` per the operand's underlying type; the operand
is cast to that spelling on every backend to avoid the literal-overload
ambiguity §5.3 Phase C hit (`0xFFu`→`255`). The type spelling hooks into each
`writeTypeName` (HLSL/GLSL → `int`/`uint`; MSL → `atomic_int`/`atomic_uint`).

Touch points: `Toks.def` (two `KW_TY`), `Lexer.cpp` (`isKeywordType`),
`AST.def` (op-name macros), `AST.h`/`AST.cpp` (two builtin types +
`isReservedBuiltinName`), `Sema.cpp` (`builtinsTypeMap` + a CALL_EXPR
validation bucket + atomic-type→underlying helper), three `writeTypeName`,
three `tryEmitBuiltinCall`. Tests: `atomic_ops.omegasl` (every op on a
`buffer<T>` atomic field + a `threadgroup atomic_uint`, both `atomic_int` and
`atomic_uint`), `invalid_atomic_ops.omegasl` (op on a non-atomic operand,
mismatched operand type, wrong arg count), and a GPU runtime test
`atomic_ops_test.cpp` (a compute dispatch where N threads `atomic_add` a
shared counter and the readback equals N).

*Verification.* The full omegasl ctest suite is green (109 tests incl. the two
new ones); the `omegagte_atomic_ops` GPU test passes on real Metal hardware —
256 racing `atomic_add`s read back as exactly 256, and max/or/exchange/load/
store + the signed-atomic path all match (proving atomicity, not just "it
compiles"). The emitted HLSL compiles under `dxc -T cs_6_0` and the GLSL under
`glslc -fshader-stage=comp` (both installed here); D3D12/Vulkan *runtime* is
pending CI like §5.3. HLSL discards a fetch-op's result as a dead `_atmN;`
statement when the value is unused — a benign unused-value warning, the same
property the `frexp` / `GetDimensions` statement-injection has; the
`Interlocked*` side effect is preserved.

*Bug caught during verification (worth recording).* The MSL
`tryEmitBuiltinCall` initially resolved `args[0]->resolvedType`
*unconditionally* before checking the call was an atomic op, so every
non-atomic builtin that falls through to `renameBuiltin` on MSL (e.g.
`countbits`) dereferenced a null/unstamped `resolvedType` → segfault in
`resolveTypeWithExpr`. It slipped past the new `atomic_ops` test (whose calls
are all atomic) and only surfaced when the whole suite ran — `bitfield_ops`
and others crashed. Fixed by gating the resolve behind the atomic-name check
(HLSL/GLSL were already guarded inside their `if`). Lesson: run the *whole*
suite, not just the new test, when a change adds a fall-through branch to a
shared dispatch.

**Phase B — `atomic_compare_exchange` (strong) + `atomic_compare_exchange_weak` (LANDED)**

Two CAS builtins land together — a strong convenience form and the loop-free
weak primitive, per Alex (the weak primitive keeps the strong form's loop
emulation honest rather than a silent perf cliff; see the rule below and the
`Emulation.rst` §"Atomics" write-up).

`atomic_compare_exchange(mem, compare, desired)` → original value; the caller
checks `result == compare` for success. The contract is **strong** (no spurious
failure) on every backend — the only portable choice, since you cannot make a
strong CAS fail spuriously.

* **GLSL** — `atomicCompSwap(mem, compare, desired)` (native strong, returns
  original) inline; compare/desired cast to the underlying type.
* **HLSL** — `InterlockedCompareExchange(dest, compare, desired, out orig)`
  (native strong); the original goes to an out-param, so statement-injected
  like the fetch-ops (temp decl + call; expression value = the temp).
* **MSL** — Metal has **only** `atomic_compare_exchange_weak_explicit` (no
  strong form — verified against the toolchain headers). Strong is emulated
  with the canonical weak-in-a-loop: capture compare/desired into single-eval
  temps, seed `_exp = compare`, then
  `while(!atomic_compare_exchange_weak_explicit(&mem,&_exp,desired,relaxed,
  relaxed) && _exp == compare){}`. A spurious failure leaves `_exp` unchanged
  (== compare) so it retries; a genuine mismatch loads the differing value into
  `_exp` and exits. `_exp` ends holding the original value (the expression
  result). Statement-injected (decl + while-loop).

`atomic_compare_exchange_weak(mem, inout expected, desired)` → **bool** (did the
store happen). The weak primitive — may fail spuriously, so it is only correct
inside a caller retry loop. Shape matches Metal's native weak / C++
`std::atomic`: on failure it writes the current value back through `expected`
(which must be a writable **local** variable — a `device` buffer field can't
form Metal's `thread T*` `expected` pointer, so Sema rejects it). Per backend:
**MSL** native inline (`atomic_compare_exchange_weak_explicit`, bool + in-place
`expected`); **HLSL/GLSL** have no weak form, so it's emulated from the strong
intrinsic (strong satisfies weak) via statement injection — capture orig,
`ok = (orig == expected)`, write back `expected = orig`, value = `ok`.

*Portability constraint (documented in `Emulation.rst`):* because the weak CAS
expands to multiple statements on HLSL/GLSL, it must be called as a *statement*
(`done = atomic_compare_exchange_weak(...)` inside the loop body), not placed in
a bare `while`/`for` condition — in a condition the injection hoists out of the
loop and runs once. MSL (inline) works either way; the statement form is the
portable one.

Sema folds both into the §5.6 bucket: 3-arg, arg0 a writable atomic lvalue.
Strong: args1/2 (compare/desired) match the underlying scalar or are integer
literals, returns the underlying scalar. Weak: arg1 (`expected`) is a writable
local of the underlying type (not a literal), arg2 (`desired`) matches or is a
literal, returns `bool`.

*Verification.* Full omegasl ctest suite green (109 tests; CAS cases folded into
`atomic_ops.omegasl` / `invalid_atomic_ops.omegasl`); the `omegagte_atomic_ops`
GPU test passes on real Metal — strong CAS hit returns 5 & stores, miss returns
42 & leaves memory (both `atomic_uint` and `atomic_int`); a **contended**
weak-CAS increment loop across 256 threads converges to exactly 256 (proving the
in-place `expected` update + retry under genuine contention). Emitted
HLSL/MSL/GLSL for all five atomic shaders compile under dxc / `xcrun metal` /
glslc. **§5.6 complete** (64-bit + image atomics remain out of scope below).

**Out of scope (follow-ups)**

* 64-bit atomics (`atomic_long`/`atomic_ulong`) and image/texture atomics —
  Vulkan-extension-gated; would need a feature bit.
* Atomic resource-access enforcement (rejecting an atomic op on an `in`-only
  `buffer<T>` in Sema). Phase A mirrors the existing precedent that plain
  buffer *writes* aren't access-checked either — the backend compiler catches
  a write to a read-only resource. Land as a shared buffer-write rule later.
* Explicit memory-order arguments (`atomic_add(mem, v, acquire)`); Phase A
  pins `relaxed`, the right default for the overwhelming majority of GPU
  atomics.

---

## 6. Compute-shader features (P0 → P1)

### 6.1 Threadgroup / shared memory [COMPLETED]

Storage-class keyword `threadgroup` applied to a variable declared at the
top level of a compute shader body:

```
threadgroup float4 tile[16][16];
threadgroup float  scratch[256];
```

Maps to `groupshared` (HLSL), `threadgroup` (MSL), `shared` (GLSL).

**Landed.** `threadgroup` is a storage qualifier modelled on the §3.6
`const` prefix: a lexer keyword (`KW_THREADGROUP`) → a `VarDecl::isThreadgroup`
flag → a parser prefix → a Sema rule → backend emission. It and `const`
are mutually exclusive (immutable vs. mutable shared storage).

**The backend scoping split.** The three backends disagree on where the
declaration may live, which drove the design:

| Backend | Spelling | Where it must be declared | OmegaSL emission |
|---------|----------|---------------------------|------------------|
| MSL  | `threadgroup T name` | inside the kernel body | inline at kernel scope |
| HLSL | `groupshared T name`  | global scope only | hoisted to file scope |
| GLSL | `shared T name`       | global scope only | hoisted to file scope |

A function-local `groupshared` / `shared` is a hard compile error on
HLSL / GLSL, so those backends hoist each top-level `threadgroup` local
out of the kernel body to file scope (`Target::emitThreadgroupGlobals`,
called from the per-shader emission before the entry function) and the
body walk skips the original declaration. Because each shader is its own
translation unit, the hoist is collision-free. MSL keeps the inline
spelling via `MSLTarget::tryEmitVarDecl`.

**Scope / validation:**

* **Compute-only** — Sema rejects `threadgroup` anywhere but a compute
  shader (the discard-stage-check pattern: `funcContext->type ==
  SHADER_DECL && shaderType == Compute`).
* **Top-level only** — inherited for free from the parser structure: the
  function-body path (`parseStmt`) routes a leading `TOK_KW` to
  `parseGenericDecl`, but the nested-block path (`parseStmtFromBuffer`,
  used inside `if`/`for`/`while`/`switch`) does not. So a `threadgroup`
  (or `const`) decl is only parseable at the function-body top level —
  exactly where MSL needs it for kernel scope and where HLSL/GLSL hoisting
  reads from. No extra enforcement code.
* **No initializer** — shared memory is uninitialized storage on every
  backend; a `threadgroup` decl with `= …` is rejected at parse time.

**N-dimensional arrays (pulled in).** The doc's `tile[16][16]` example
required multi-dimensional arrays, which OmegaSL did not have:
`TypeExpr::arraySize` was a single `std::optional<unsigned>`. Replaced
with `OmegaCommon::Vector<unsigned> arrayDims` (outermost first); the
parser collects one `[N]` per dimension and every backend emits one `[d]`
suffix per entry.

This also exposed and fixed a latent gap: array-typed *locals* could be
**declared** but not **indexed** — the `INDEX_EXPR` Sema arm only knew
buffer / vector / matrix. It now peels one (outermost) dimension per
index *before* the vector/matrix checks (the base type of `float4
tile[16][16]` is still `float4`, but a single `[i]` selects an array
element, not a `.xyzw` component): `tile[lx]` → `float4[16]`,
`tile[lx][ly]` → `float4`, `tile[lx][ly][0]` → `float`.

**Verification.** All three backends verified on the macOS host: MSL via
the normal compile path (`box_blur.metal` + `metallib`), HLSL and GLSL via
`omegaslc --hlsl -S` / `--glsl -S` (source-only emission instantiates the
HLSL/GLSL `Target` without needing dxc/glslc). Sample outputs:

```hlsl
groupshared float4 tile[16][16];          // file scope
groupshared float scratch[256];
[numthreads(16,16,1)] void box_blur(...) { tile[lx][ly] = ...; }
```
```glsl
shared vec4 tile[16][16];                 // global scope
shared float scratch[256];
layout(local_size_x=16,...) in; void main() { tile[lx][ly] = ...; }
```
```metal
kernel void box_blur(...) {
    threadgroup float4 tile[16][16];      // kernel scope
    threadgroup float scratch[256];
}
```

Tests: `threadgroup_shared.omegasl` (2D `float4` tile + 1D `float`
scratch, indexed read/write through both), `invalid_threadgroup_outside_
compute.omegasl`, `invalid_threadgroup_init.omegasl`.

**Out of scope (named follow-ups):**

* **Barriers** — declaring shared memory is only half the story; a real
  reduction needs `threadgroupBarrier()` to sync writes before reads.
  That's §6.2, deliberately separate.
* **Array-of-matrix indexing on HLSL.** `threadgroup float4x4 m[8];`
  then `m[i]` is ambiguous against HLSL's matrix-column-synthesis path in
  `emitIndexExpr` (which keys off the base type being a matrix). Scalar /
  vector / struct shared arrays — the common case — are unaffected.
  Disambiguating would require `emitIndexExpr` to consult `arrayDims`.
* **Per-member std140-style alignment** does not apply (shared memory has
  no host-visible layout contract); a `threadgroup` struct array uses the
  backend's natural layout.

### 6.2 Barriers [COMPLETED]

Two zero-arg, void-returning, compute-only intrinsics:

- `threadgroupBarrier()` — sync all threads in the group, include memory.
- `deviceBarrier()` — memory barrier across the device.

**Landed.** Both are name-recognized in the same Sema math-intrinsic
dispatch the §5.1 builtins use — no AST `FuncType` registration needed
(that path keys off the name). They take the dispatch's first new 0-arg
shape: `expectedArgs = 0`, void return. The arg-count guard was widened
from `expectedArgs > 0` to `>= 0` so the 0-arg count is enforced (`-1`
still means "unknown function" and skips). Sema rejects either barrier
outside a compute shader (the same `funcContext->shaderType == Compute`
check §6.1 uses).

Per-backend emission:

| OmegaSL | HLSL | MSL | GLSL |
|---------|------|-----|------|
| `threadgroupBarrier()` | `GroupMemoryBarrierWithGroupSync()` | `threadgroup_barrier(mem_flags::mem_threadgroup)` | `barrier()` |
| `deviceBarrier()`      | `DeviceMemoryBarrier()`             | `threadgroup_barrier(mem_flags::mem_device)`      | `memoryBarrier()` |

HLSL/GLSL are simple `renameBuiltin` entries (0-arg → `name()` via the
shared `(args)` suffix). MSL needs `tryEmitBuiltinCall` because the
memory-scope flag is an *injected* argument (OmegaSL's call has none).

**`deviceBarrier` semantics — the one asymmetry.** HLSL `DeviceMemoryBarrier`
and GLSL `memoryBarrier` are **memory-only** (order device/UAV memory, no
thread execution sync). MSL's `threadgroup_barrier(mem_device)` additionally
syncs execution. The portable contract is therefore **"device-memory
ordering only — `deviceBarrier` does not make threads wait for each other"**:
that holds exactly on HLSL/GLSL, and MSL is merely stricter (extra
synchronization never breaks correctness, only costs a little perf). A
shader that needs threads to wait must use `threadgroupBarrier()`. Matching
the execution semantics precisely across backends would need the scoped /
acquire-release variants below.

**Verification.** MSL via the normal compile path, HLSL/GLSL via
`omegaslc --hlsl -S` / `--glsl -S`. Tests: `compute_barriers.omegasl` (a
reduction pattern: shared write → `threadgroupBarrier()` → shared read →
`deviceBarrier()`), `invalid_barrier_outside_compute.omegasl`,
`invalid_barrier_args.omegasl`.

**Out of scope (named follow-ups):** the extended variants —
`acquire`/`release` ordering and per-memory-type scoping (texture vs.
device vs. threadgroup). HLSL has the six-way `*MemoryBarrier[WithGroupSync]`
matrix, MSL has the `mem_flags` bitfield + `memory_order`, GLSL has the
`memoryBarrier{Shared,Buffer,Image}` family; exposing the cross product is
a separate design pass and there's no workload that needs it yet.

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

### 10.2 Push constants [COMPLETED — compile path + runtime binding (Metal verified)]

Vulkan push constants + D3D12 root constants + Metal inline `constant` under
4KB. Very performance-relevant for per-draw constants — small, frequently
updated, no buffer/descriptor allocation.

**Landed as `constant<T>`** — a third resource form parallel to `buffer<T>`
(§2.5/structured) and `uniform<T>` (§2.4/constant buffer). Read-only,
value-accessed (`pc.field`), never indexed, T must be a user struct. Declared
globally with an index and opted into per-shader via the resource map
(`[in pc]`) — same scoping as every other resource: the declaration is global
but access is *entry-scoped* (not an ambient global), so a helper function
takes the data as a parameter. That scoping is forced by Metal, which has no
global resources; it also means the HLSL-cbuffer-style `constant { ... }`
block form (which would make the fields ambient globals) was rejected — it
fits neither OmegaSL's model nor Metal. See
`Pipeline-Completion-Extension-Plan.md` §2.2 for the full decision record
(the resolved ALEX QUESTION) and the runtime (Phase B) side.

* **Token / type**: `constant` is a `KW_TY` (`Toks.def` `KW_TY_CONSTANT`,
  `Lexer.cpp` `isKeywordType`); `builtins::push_constant_type` is a
  one-type-arg builtin (`AST.h`, `AST.cpp`) registered in Sema's
  `builtinsTypeMap`. The generic resource-decl parser handles
  `constant<T> name : N;` with no change.
* **Sema**: `push_constant_type` joins the valid-resource-type set. Exactly
  one type arg, T must be a user struct (same contract as `uniform<T>` — root
  constants / push blocks have no scalar form across all three backends).
  In the resource map it is restricted to `In` access (read-only everywhere)
  and its name binds directly to T (so `pc.field` resolves through the normal
  struct path and `pc[i]` fails with the existing
  "indexing only on buffer/vector/matrix" diagnostic — shares the `uniform<T>`
  bind-to-T path). The element struct is registered for codegen emission.
* **Global resource index**: a push constant *does* take a `: N` slot from the
  shared resource index (it needs `[[buffer(N)]]` on Metal and `register(bN)`
  on D3D12). On GLSL it consumes **no** descriptor `binding` — the
  `push_constant` block lives outside descriptor sets — so the GLSL emitter
  sets `gpu_relative_loc = 0` and skips the `++binding` increment, keeping the
  other resources' bindings contiguous.
* **Runtime layout-desc**: new `OMEGASL_SHADER_PUSH_CONSTANT_DESC` appended at
  the tail of `omegasl_shader_layout_desc_type`, distinct from
  `OMEGASL_SHADER_UNIFORM_DESC` (CBV / bound uniform buffer) and from the
  pre-existing `OMEGASL_SHADER_CONSTANT_DESC` (the unrelated inline
  single-scalar push constant with a baked default value).
* **Per-backend emission**:
  * **HLSL**: `ConstantBuffer<T> pc : register(bN,spaceM);` — same `b`
    register class as `uniform<T>` (shares `bResourceCount`). The
    root-constants-vs-CBV distinction is purely in the root signature
    (Phase B); the source is identical to a constant buffer.
  * **MSL**: `constant T& pc [[buffer(N)]]` — byte-identical to a uniform.
    Metal has no separate push-constant construct;
    `setBytes:length:atIndex:` writes inline into the same buffer-index space
    a bound buffer would use. Only the layout-desc type differs.
  * **GLSL**: `layout(push_constant,std430) uniform pc_Layout { T pc; };` —
    no `set`/`binding`. std430 layout (push constants are the tight,
    128-byte-budgeted path); the Phase-B host-side writer must pack std430 to
    match, distinct from `uniform<T>`'s std140.
* **Tests**: `push_constant.omegasl` (vertex + fragment sharing one global
  `constant<PushData>` via their own resource maps, a helper that receives
  `pc.mvp` as a parameter, and a separate compute kernel with its own
  `constant<DispatchParams>`); `invalid_push_constant.omegasl` (multi-error:
  indexing a push constant, `out` access, non-struct element type). The Metal
  build compiles the generated MSL end-to-end on the macOS host; HLSL/GLSL
  source emission verified via `-S`.

**Phase B — runtime binding (done; see Pipeline-Completion §2.2).** The
`setRenderConstants` / `setComputeConstants` command-buffer API landed on
`GECommandBuffer`, with the per-backend inline-constant plumbing (Metal
`setBytes` — verified end-to-end via `gte/tests/push_constant_test.cpp`; D3D12
root-constants param + `SetGraphics/ComputeRoot32BitConstants`; Vulkan
`VkPushConstantRange` + `vkCmdPushConstants` — both written-from-source). No
host-side packing: the API takes raw bytes and the caller owns the std-layout.

**Out of scope (follow-ups):** threading the push block's exact struct size
through the layout desc (D3D12/Vulkan currently reserve the 128-byte cap);
Metal partial updates (`offset != 0`); compile-time / pipeline-creation
enforcement of the 128-byte limit and the one-push-constant-per-pipeline rule.

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

### 12.-1 Expression precedence — assignment/ternary + unary operands [FIXED 2026-07-01]

Two silent mis-parses, surfaced by AQUA's Phase 5e narrowphase kernels:

1. **`s = cond ? a : b` parsed as `(s = cond) ? a : b`.** The ternary check
   sat above the precedence climber, and the climber treats `=` as a plain
   binary op — so the assignment consumed the condition first. Fixed in the
   climb: an assignment's RHS is now a full recursive `parseExpr` (ternary
   included, and `a = b = c` gets correct right associativity).
2. **Prefix unary operands consumed a full expression:** `-a + b` parsed as
   `-(a + b)` — value-identical under `*` (sign commutes, why it survived),
   WRONG under `+`/`-`/comparisons (`x >= -a && ...` became `x >= -(a && …)`).
   Fixed: `!`/`-`/`++`/`--`/`~` (and `*`/`&` pointer forms) parse their
   operand at unary precedence (minPrec 11 — primary/postfix only) and the
   binary climb now also runs after a prefix expression.

Also fixed the same day: **scientific-notation float literals** — the lexer
had no `e`/`E` exponent support (`1e18` lexed as `1` + identifier `e18`,
silently dropping 18 orders of magnitude), and the parser classified
`1e18` (no decimal point) as an int (`std::stoi` stops at `e`). Tests:
`precedence_unary_ternary.omegasl` (the ternary case is discriminating —
the old parse fails Sema as float=bool), `sci_notation_literal.omegasl`.
NOTE: these fixes change codegen for pre-existing shaders; `add_omegasl_lib`
now depends on the omegaslc binary, so every shader lib rebuilds on a
compiler change (previously they went stale). That staleness fix immediately
exposed a LATENT Phase 8d bug: MSL static samplers emit no entry-function
parameter (they flush as in-body `constexpr sampler`) but still advanced
`paramIndex`, so the next resource emitted a stray leading comma
(`fragFunc (,texture2d...`). Fixed in `MSLTarget::emitResourceBinding`;
surfaced by the 2DTest shader lib finally rebuilding.

### 12.0 TU assembly — struct definitions vs. user functions [FIXED 2026-07-01]

Two related defects in per-shader TU assembly, surfaced by AQUA's Phase 5
kernels (whose math helpers pass a user struct — a 3x3 matrix-of-rows — that
no shader body names directly):

1. **User-function prototypes were emitted before struct definitions** on
   every backend, so a helper taking/returning a struct produced
   "unknown type name" in the downstream compiler.
2. **The used-struct set was shader-only.** A struct used exclusively in a
   user function's signature/locals was never emitted at all (Sema tracks
   used types per FuncDecl, but CodeGen consulted only the ShaderDecl's set).

Fix: `SemFrontend::addStructUseToFuncDecl` (new hook, implemented by `Sem`);
CodeGen's SHADER_DECL arm now merges every user function's used-struct set
into the shader's (the struct-use closure) and calls
`emitShaderUsedStructs` BEFORE the prototype pass. Follow-up (same day): the
FUNC_DECL Sema path never registered struct-typed PARAMS or RETURN types in
the per-decl used-type context (only locals were tracked), so a struct named
exclusively in helper signatures — AQUA's `out AQManifoldLocal m` — was
still dropped; both now register, mirroring the SHADER_DECL path. GLSL previously emitted
its plain structs inside `emitShaderEntryHeader` (after user functions) —
its `emitShaderUsedStructs` no-op now emits the plain (non-`internal`)
structs, and the entry header keeps only the interstage-varying emission.
Verified: metal + dxc + glslc all compile the AQUA kernels; golden HLSL
files unchanged (no user functions there); full test suite green.

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
2. Land the the API additions on
   `GTEShader.h`.
3. Implement matrix methods in Vulkan and Metal (already-column-major
   targets — pure additive work).
4. Implement matrix methods in D3D12, with the packing lock
   (`D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR`, `-Zpc`,
   `column_major` qualifier in HLSLCodeGen) in the same change so the
   guarantee is intact from day one.
5. Add the round-trip tests on all three backends.

#### Out of scope

- **`int` / `uint` matrix types** in the buffer API. **LANDED** — see
  §12.2.1 below. (Both the shader-language types and the buffer R/W API.)
- **`double` matrix types.** OmegaSL has no double type at the moment, but will add support in the future.
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

### 12.2.1 Integer / unsigned-integer matrix types [LANDED — GLSL verified; HLSL/MSL + D3D12/Metal written]

The §12.2 "out of scope" item. Adds `intCxR` / `uintCxR` (C, R ∈ {2,3,4} —
18 types, mirroring the 9 float matrix shapes) to both the OmegaSL shader
language and the host buffer R/W API.

**The wall, and the decision.** Unlike `float` matrices (which map 1:1 to
`mat4` / `float4x4` / `int4x4`), there is *no native integer matrix type* on
GLSL (`mat`/`dmat` only) or MSL (`metal::matrix` is float/half only) — only
HLSL has `int4x4`. Per the front-end-uniformity rule, a construct that can't
be expressed uniformly across all three backends must not silently diverge.
**Decision (Alex):** lower `intCxR` to an **array of C integer column
vectors** — `int4 m[C]` (HLSL/MSL) / `ivec4 m[C]` (GLSL) — *identically on
all three backends, including HLSL* (we deliberately skip native `int4x4`).
This keeps one codegen shape and makes column-major indexing `m[col][row]`
natural array indexing everywhere, so integer matrices never hit the HLSL
§12.1 row-major index swap.

**Shader language (front-end).**

* Tokens (`Toks.def`) + `isKeywordType` (`Lexer.cpp`); 18 builtin `Type`s
  (`AST.h`/`AST.cpp`); registered in Sema's `builtinsTypeMap`.
* Codegen: each backend's `writeTypeName` spells the column vector
  (`int4`/`uint4` on HLSL/MSL, `ivecR`/`uvecR` on GLSL); a shared
  `CodeGen::writeDeclTypeSuffix` appends the `[C]` array dimension at every
  declarator site (struct field / local / threadgroup), reusing the existing
  `arrayDims` emission. `CodeGen::integerMatrixShape` is the single
  (signed, C, R) decomposition both consume.
* Sema: integer-matrix `m[col]` indexing returns the `intR`/`uintR` column
  vector; `isIntegerMatrixType` is kept *out* of `isMatrixType` so the
  float-only algebra / §12.1-swap paths never fire on integer matrices.
* **Supported:** declaration (struct fields, locals, threadgroup), indexing
  (column / element read, column / element write), buffer round-trip.
* **Rejected (clear Sema/parser diagnostics):** any operator on a *whole*
  integer matrix (algebra / equality / whole-matrix assignment — not
  portable across the array lowering, and no backend has integer matrix
  algebra) and inline construction `int4x4(...)` (no portable array-rvalue
  form — the message points at per-column assignment).

**Buffer R/W API (host).**

* `omegasl_data_type` gains `OMEGASL_INT*x*` / `OMEGASL_UINT*x*`, **appended
  at the enum tail** so every pre-existing value is preserved (matrix data
  types are never serialized into `.omegasllib` — codegen does not emit them
  — so only the in-memory C++ ABI matters).
* `BufferIO.h`: `encodeFMatrix`/`decodeFMatrix` generalized to
  `encodeMatrix<T,C,R>`/`decodeMatrix<T,C,R>` over any 4-byte scalar (the
  float-named wrappers delegate, so existing call sites are byte-identical);
  `matrixDims` / `isMatrixDataType` / a new `intMatrixDataTypeFor` /
  `uintMatrixDataTypeFor` cover the new enumerators. An int/uint matrix's
  std430/std140 byte layout is *identical* to the same-shape float matrix
  (int/uint/float are all 4-byte), so all the column-padding math is reused
  unchanged.
* `GEBufferWriter` / `GEBufferReader` gain `writeInt<C>x<R>` /
  `writeUint<C>x<R>` / `getInt<C>x<R>` / `getUint<C>x<R>` (`IMatrix<C,R>` /
  `UMatrix<C,R>` from GTEMath.h). Implemented on all three backends' writers
  / readers (the per-backend `getMatrixImpl` was generalized on the element
  type).

**Verification (this Linux/Vulkan host).**

* **GLSL: fully verified** — `int_matrix.omegasl` compiles and `glslc`
  validates the generated GLSL into SPIR-V; the emitted struct fields are
  `ivec4 m[4]` / `uvec3 m[3]` / `ivec2 m[3]` with natural `m[c][r]` access.
* **HLSL / MSL codegen: C++-compile-checked** (all three `*Target.cpp`
  always compile into `omegaslc`) but their *source emission* is unverified
  here — the `--hlsl`/`--msl` flags are `TARGET_*`-gated off on Linux.
* **D3D12 / Metal buffer writers/readers: written, compile-unverified**
  (`GED3D12.cpp` / `GEMetal.mm` do not build off-platform). The Vulkan
  writer/reader compiles and links.
* Layout parity is asserted on any host by `gte/tests/std140_layout_test.cpp`
  (int/uint matrix `matrixDims` + std140 struct strides equal the same-shape
  float). Tests: `int_matrix.omegasl`, `invalid_int_matrix_op.omegasl`,
  `invalid_int_matrix_ctor.omegasl`.

**Follow-up (open):** a GPU round-trip test for integers (`writeInt4x4` →
compute shader → `getInt4x4`), mirroring the float `matrix_ops_test`. Not
blocking — the byte layout is provably identical to the float path (asserted
by the std140 parity test) and the float GPU round-trip already exercises the
binding path — but a dedicated integer round-trip would confirm in-shader
`ivec4 m[C]` indexing semantics on real hardware.

### 12.3 Next phase — `column_major` source qualifier on HLSL struct fields

§12.1 and §12.2 landed: source-level matrix indexing is locked
column-first (with the HLSL index swap + non-square type-spelling flip),
matrix R/W methods exist on `GEBufferWriter`/`GEBufferReader`, and the
HLSL backend pins storage to column-major via the runtime
`D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR` flag plus the offline `/Zpc` flag.
Step 5c of §12.2 — the `column_major` source qualifier on every
matrix-typed HLSL struct field — was deferred and lands here.

**Why it didn't fit in §12.2.** The straightforward implementation
("emit `column_major ` from `HLSLTarget::writeTypeName` when the type
is a matrix") was tried and reverted at
`gte/omegasl/src/HLSLTarget.cpp`. `writeTypeName` is shared with cast
expressions (`writeCast`) and function param types, where HLSL forbids
storage qualifiers — `(column_major float3x3)x` does not compile. A
clean fix needs a struct-field-context-aware emit path, which doesn't
exist as a `Target` hook today. The deferred-comment in `writeTypeName`
points to this section.

**Why it's still worth doing.** The compile-flag pair
(`D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR`, `/Zpc`) covers the storage in
the current toolchain, but a future `dxc` default flip or a downstream
recompile with a different flag set would silently break the layout.
The source qualifier travels with the source and survives flag drift —
that's why the original §12.2 step 5c preferred it over the flags
when both feel redundant.

#### Patch

**1. New `Target` hook.** Add in `gte/omegasl/src/Target.h`, paralleling
the existing `emitMemberExpr`/`emitIndexExpr` hooks:

```cpp
/// Emit a struct field's type. Default delegates to
/// `cg.writeTypeExpr` — what GLSL/MSL want. `HLSLTarget` overrides
/// to prefix `column_major ` when the field type is a matrix, so
/// the storage layout is locked at the source level instead of
/// relying solely on `D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR` /
/// `/Zpc`. The hook is called *only* from struct-field emission —
/// not from casts or function params, where HLSL forbids storage
/// qualifiers. See §12.3.
virtual void emitStructFieldType(CodeGen &cg,
                                 ast::TypeExpr *typeExpr,
                                 std::ostream &out);
```

Default implementation in `gte/omegasl/src/CodeGen.cpp`:

```cpp
void Target::emitStructFieldType(CodeGen &cg,
                                 ast::TypeExpr *typeExpr,
                                 std::ostream &out) {
    cg.writeTypeExpr(typeExpr, out);
}
```

**2. HLSL override.** In `gte/omegasl/src/HLSLTarget.cpp`:

```cpp
void HLSLTarget::emitStructFieldType(CodeGen &cg,
                                     ast::TypeExpr *typeExpr,
                                     std::ostream &out) {
    auto *t = cg.typeResolver->resolveTypeWithExpr(typeExpr);
    if (t && isOmegaSLMatrixType(t)) {
        out << "column_major ";
    }
    cg.writeTypeExpr(typeExpr, out);
}
```

(`isOmegaSLMatrixType` already exists as a static helper next to
`emitIndexExpr`.) Declaration goes in the `HLSLTarget` block of
`Target.h`.

**3. Single call site.** Switch `HLSLTarget::emitStructDecl` at
`gte/omegasl/src/HLSLTarget.cpp` to call the new hook in place of
`cg.writeTypeExpr` for the field-type emission. `writeCast` and the
function-parameter / return-type sites in `emitShaderEntryHeader`
keep using the unqualified path.

**4. Drop the deferred-comment.** Remove the "deferred — see §12.3"
note from the matrix branch of `HLSLTarget::writeTypeName` once this
lands.

#### Tests

- Cross-emit `matrix_index.omegasl`, `matrix_buffer.omegasl`,
  `matrix_ops.omegasl` to HLSL via `omegaslc --hlsl -S` and grep for
  `column_major` on each matrix-typed struct field. Add a
  `golden/matrix_buffer.hlsl` golden file under
  `gte/omegasl/tests/golden/` and wire a `RunGoldenCodegenTest.cmake`
  invocation in the `TARGET_DIRECTX` block of
  `gte/omegasl/tests/CMakeLists.txt`.
- Existing `omegasl_compile_matrix_*` and
  `omegasl_invalid_matrix_column_write` tests must still pass — the
  qualifier is additive at struct-field sites and shouldn't perturb any
  other path.
- Confirm the Vulkan `runMatrixRoundtrip` in
  `gte/tests/vulkan/ComputeTest/main.cpp` still passes (it should be
  unaffected — the change is HLSL-only).

#### Out of scope

- **`row_major` qualifier as an explicit per-field opt-out.** OmegaSL
  has no surface for "this matrix is row-major in storage" — every
  matrix is column-major. If a future use case needs an opt-out (e.g.
  loading a D3D9-era asset format directly into a uniform buffer), it
  pairs with the §12.2 "Out of scope: Runtime transposition fallback"
  item.
- **GLSL `layout(row_major)` / `layout(column_major)` decorations.**
  GLSL uses storage qualifiers on the *block*, not per-field. The std430
  default is already column-major (`mat4` columns at `MatrixStride = 16`)
  so no extra emit is needed today. If the engine ever adds std140
  uniform blocks with mixed layouts, the per-block qualifier can land
  on `GLSLTarget::emitStructDecl` directly, but that's separate from
  this section.
- **MSL.** Metal's matrix layout is already column-major by spec, no
  qualifier exists or is needed.

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

**Cross-cutting prerequisite:** §14 (feature gating) should land before any
of the asymmetric features (RT, mesh shaders, bindless, geometry shaders,
tessellation re-enable — see §16) so each one has a clean way to declare its backend
requirements at the source level and the runtime can per-shader-reject
when the device can't satisfy them.

---

## 14. Cross-cutting: Backend feature gating

> **Status (Phases 1–4 landed):** file-scope `#requires(...)` parses and
> predefines `OMEGASL_BACKEND_<X>` + `OMEGASL_FEATURE_<NAME>` macros for the
> active backend; shaders carry an `omegasl_shader.requiredFeatures` bitfield
> through library write + read; the Sema-time portability scan emits the
> undeclared-use + partition-suggestion warnings from §14.2; features the
> active backend can't express produce a header-only stub shader entry
> instead of a hard compile error (per the user's twist — see §14.1 below).
> The runtime side is now closed end-to-end:
> `GTEDeviceFeatures::featuresAsBitmask()` synthesizes the device's
> `OMEGASL_FEATURE_BIT_*` mask (§14.4 Phase A); the library loader masks
> per-shader `requiredFeatures` against that mask and inserts an
> unsupported-shader sentinel + diagnostic for any shader the device can't
> run; pipeline builders detect the sentinel and surface the diagnostic at
> `makeRenderPipelineState` / `makeComputePipelineState` time. Per-backend
> probes already populate `GTEDEVICE_FEATURE_*` flags. Remaining work is
> bounded by §14.7.1 tasks E–G — extending `GTEDeviceFeatures` with the
> Phase B bits (cube R/W, MS write, subpass inputs), trigger-table entries
> for features whose language constructs haven't shipped yet (DOUBLE,
> FLOAT16, INT64, RT intrinsics, subgroup ops), and the Sema acceptance +
> codegen for cube R/W and 2D-MS write.

Several features in this survey are **backend-asymmetric** — supported on
some targets but not others. Examples already on the roadmap:

| Feature | HLSL | MSL | GLSL/Vulkan |
|---------|------|-----|-------------|
| Ray tracing / RayQuery | SM 6.5+ | Metal 3.0+ | KHR_ray_tracing |
| Mesh / amplification shaders | SM 6.5+ | Metal 3.0+ | EXT_mesh_shader |
| Geometry shaders | yes | **no** (removed in MSL 2.x) | yes |
| Tessellation hull/domain (completion: §16) | API yes; codegen incomplete (bug 4) | rejected at codegen (bug 3) | API yes; codegen untested |
| `texturecube` `read` / `write` | partial (via aliasing) | **no** | **no** |
| `texture2d_ms` `write` | **no** | **no** | **no** |
| Subgroup / wave ops | SM 6.0+ | Metal 2.0+ | KHR_shader_subgroup |
| Bindless / descriptor indexing | SM 6.6 / `register space` | Argument buffers | EXT_descriptor_indexing |
| `half` / 16-bit types | SM 6.2 + `-enable-16bit-types` | native | KHR_shader_float16_int8 |
| 64-bit integers | SM 6.0 | `long`/`ulong` | ARB_gpu_shader_int64 |
| Variable rate shading | SM 6.4 | (limited) | KHR_fragment_shading_rate |
| Subpass inputs | **no** | **no** (tile shading is similar) | yes |
| Specialization constants | (use `#define`) | (use function constants) | yes |

Today the only escape hatch is "compile fails on backend X with a confusing
codegen error." That's brittle and hides the *why* of the failure. This
section proposes a three-layer feature-gating system so authors can declare
intent, the compiler can warn before they get burned, and the runtime can
per-shader-reject when a library is loaded on a device that can't satisfy
the requirements.

### 14.1 Layer 1 — source-level `#requires` macro [LANDED — modified]

> **Modified rule:** the original "hard fail at compile" is replaced
> with **highest-capability + null-stub**. The active backend is
> assumed to express every feature it *can possibly* express (RT, mesh
> shaders, etc. always compile; the runtime decides whether the device
> can execute them). Features the backend can't express at all (e.g.
> `DOUBLE` on MSL, `SUBPASS_INPUTS` on HLSL/MSL) do **not** raise a
> compile error — instead the affected shader transpiles to a header-
> only stub: type, name, and `requiredFeatures` bitfield are recorded
> in the library; no source file is emitted; no downstream compiler is
> invoked. The runtime rejects pipeline creation against the stub with
> a precise diagnostic (Layer 3, §14.3).
>
> Per-shader / per-function `#requires` (annotation immediately
> preceding a decl) was considered for Phase 1.2 and dropped in favor
> of the Sema-time portability scan (§14.2). `#requires` is currently
> **file-scope only** — the union applies to every shader in the file.


OmegaSL already has a Preprocessor (`gte/omegasl/src/Preprocessor.cpp`).
Add a new directive, `#requires`, that names one or more features the
shader requires to be supported by the target backend:

```omegasl
#requires(OMEGASL_FEATURE_RAYTRACING)

[in tlas, in scene]
compute(x=8,y=8,z=1)
void rtKernel(uint3 gid : GlobalThreadID){
    /// ... ray-query work ...
}
```

Behavior:

- The preprocessor predefines `OMEGASL_BACKEND_<HLSL|MSL|GLSL>` for the
  active target backend (one of the three is always defined; the other
  two are not).
- The preprocessor also predefines `OMEGASL_FEATURE_<NAME>` for every
  feature the active backend supports (table in §14.5).
- `#requires(X)` expands to a Sema-level check: if `OMEGASL_FEATURE_X` is
  not defined, the compiler emits a hard error keyed on the file location,
  with the form:

  > error: shader 'rtKernel' in 'foo.omegasl' requires feature
  > `OMEGASL_FEATURE_RAYTRACING`, which is not available on the GLSL/Vulkan
  > backend (requires VK_KHR_ray_tracing_pipeline). Consider partitioning
  > shaders that use ray-tracing into a separate library, or guard the
  > kernel with `#if defined(OMEGASL_FEATURE_RAYTRACING)`.

- `#requires(A, B, C)` is the conjunction; all three must be defined.
- `#requires` at file scope applies to every shader in the file.
  `#requires` immediately preceding a `vertex`/`fragment`/`compute`/`hull`/
  `domain` declaration applies to that shader only.

`#if defined(OMEGASL_FEATURE_X)` / `#else` / `#endif` already follows from
the existing preprocessor; no new directive needed for the soft-conditional
path.

### 14.2 Layer 2 — portability scan (warning) [LANDED — modified]

> **Modified mechanism:** runs at **Sema time** as an AST walker
> (`omegasl::FeatureScanner`), not as backend codegen instrumentation.
> Both Phase A (undeclared-use) and Phase B (partition suggestion)
> emit advisory warnings to stderr. Compilation is not gated on the
> scanner's output. The trigger table maps AST constructs →
> `OMEGASL_FEATURE_BIT_*`; cube `read`/`write` and `texture2d_ms`
> `write` are seeded today, but those constructs are still rejected
> at Sema as compile-path-only (§2.1). New trigger entries land
> alongside future feature implementations (DOUBLE, FLOAT16, INT64,
> RT intrinsics, subgroup ops). The scanner folds callee
> `usedFeatures` into callers via a fixed-point closure over the
> user-function call graph, so a shader sees every feature its
> transitive callees use without per-callee annotations.


`#requires` is the explicit path. Most authors won't know they're using
something that's backend-asymmetric until they compile against the
unsupported target. Phase A of this layer:

- During codegen, each backend tracks which features the *generated source*
  actually exercised — ray-query intrinsics, subgroup ops, mesh-shader
  attributes, cube `read`, etc.
- After codegen completes, the compiler diff-checks the *used* feature
  set against the *declared* set (the union of all `#requires` directives
  that applied to that shader).
- If a feature was used but not declared, and that feature is **not
  universally supported across all three backends**, emit a warning:

  > warning: shader 'envProbe' uses cube-texture sampling, which is
  > universal — no action needed.
  > warning: shader 'rtKernel' uses ray-query intrinsics, which are not
  > supported on every backend (HLSL ✓ SM 6.5+ / MSL ✓ Metal 3 / GLSL ✓
  > KHR_ray_tracing). Add `#requires(OMEGASL_FEATURE_RAYTRACING)` so this
  > shader fails fast on backends without it, and consider partitioning
  > the file so non-RT shaders aren't gated by RT.

Phase B (optional): emit a *partition suggestion* when a single .omegasl
file contains a mix of universal-feature shaders and gated-feature
shaders. The suggestion text proposes a concrete split — moving the
gated shaders into `<filename>_<feature>.omegasl` — so the universal
ones still compile when the device can't satisfy the gated set.

### 14.3 Layer 3 — runtime per-shader feature flags (per-shader rejection) [LANDED]

> **Done:** the `omegasl_shader_feature_flags` enum + `requiredFeatures`
> field on `omegasl_shader` are in `omegasl.h`; the library writer
> (`linkShaderObjects`) appends the bitfield to every shader record
> and emits header-only entries (`dataSize == 0`, no per-stage
> decoration) for stub shaders; the library reader
> (`OmegaGraphicsEngine::loadShaderLibraryFromInputStream`) accepts
> `dataSize == 0`, skips the per-stage decoration block in that case,
> and reads `requiredFeatures` for every entry. The loader masks each
> shader's `requiredFeatures` against
> `_deviceFeatures = device->features.featuresAsBitmask()` and
> substitutes a base-class `GTEShader` sentinel
> (`isUnsupported = true`, `unsupportedDiagnostic` populated) instead
> of calling `_loadShaderFromDesc` when bits are missing; the
> diagnostic is also stashed on `GTEShaderLibrary::unsupportedDiagnostics`
> and logged to stderr at load. Pipeline builders
> (`makeRenderPipelineState` / `makeComputePipelineState` on D3D12,
> Metal, Vulkan) call `OmegaGraphicsEngine::_checkPipelineShader` at
> the top of each builder and surface the stored diagnostic at
> pipeline-creation time, returning `nullptr`. Sibling shaders /
> sibling pipelines load and build normally.
>
> The formal `OMEGASL_SHADER_LOAD_UNSUPPORTED` status / `loadDiagnostic`
> fields from the spec sketch below were not added; the implementation
> uses the `GTEShader` sentinel + `GTEShaderLibrary::unsupportedDiagnostics`
> map instead, which carries the same information without growing the
> public C ABI.


Even with `#requires`, a `.omegasllib` produced for one backend can be
loaded onto a device that doesn't satisfy a particular shader's
requirements. The library load must not fail wholesale — other shaders in
the same library may still be perfectly usable.

**Add to `omegasl.h`:**

```c
/// Per-shader feature requirements. Each bit names a runtime feature that
/// the *generated* shader requires. The library writer sets these from the
/// `#requires` declarations + the codegen-time portability scan; the
/// loader masks them against the device's `GTEDeviceFeatures` and rejects
/// only the shaders whose required bits are not satisfied. Other shaders
/// in the same library load normally.
using omegasl_shader_feature_flags = enum : uint64_t {
    OMEGASL_FEATURE_BIT_NONE              = 0,
    OMEGASL_FEATURE_BIT_RAYTRACING        = 1ull << 0,
    OMEGASL_FEATURE_BIT_MESH_SHADERS      = 1ull << 1,
    OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS  = 1ull << 2,
    OMEGASL_FEATURE_BIT_TESSELLATION      = 1ull << 3,
    OMEGASL_FEATURE_BIT_SUBGROUP_OPS      = 1ull << 4,
    OMEGASL_FEATURE_BIT_BINDLESS          = 1ull << 5,
    OMEGASL_FEATURE_BIT_FLOAT16           = 1ull << 6,
    OMEGASL_FEATURE_BIT_INT64             = 1ull << 7,
    OMEGASL_FEATURE_BIT_VARIABLE_RATE     = 1ull << 8,
    OMEGASL_FEATURE_BIT_SUBPASS_INPUTS    = 1ull << 9,
    OMEGASL_FEATURE_BIT_SPEC_CONSTANTS    = 1ull << 10,
    OMEGASL_FEATURE_BIT_TEXTURECUBE_RW    = 1ull << 11,  // cube read/write (deferred)
    OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE = 1ull << 12,
    OMEGASL_FEATURE_BIT_DOUBLE            = 1ull << 13
    /// New bits append at the tail; existing bits never get reused.
};

/// Add to omegasl_shader struct:
struct omegasl_shader {
    /// ... existing fields ...
    uint64_t requiredFeatures;   /// Bitfield of OMEGASL_FEATURE_BIT_*
};
```

The library file format already encodes per-shader records; appending one
`uint64_t` to each record is forward-compatible (older readers ignore the
trailing bytes).

**Loader semantics** (engine-side, in `OmegaSLLibrary::load` or
equivalent):

```cpp
void load(const omegasl_lib &lib, GTEDevice &device) {
    uint64_t deviceFeatures = device.featuresAsBitmask();
    for (auto &shader : lib.shaders) {
        uint64_t missing = shader.requiredFeatures & ~deviceFeatures;
        if (missing) {
            shader.loadStatus = OMEGASL_SHADER_LOAD_UNSUPPORTED;
            shader.loadDiagnostic = formatMissingFeatureMessage(missing);
            continue;  /// Other shaders in the library load normally.
        }
        bindShader(shader);
    }
}
```

Pipeline creation against an `OMEGASL_SHADER_LOAD_UNSUPPORTED` shader
fails *at pipeline creation time* with the stored diagnostic — not at
library load. This way a library with a mix of supported and unsupported
shaders is still *usable*; only the specific draw/dispatch that names an
unsupported shader errors.

### 14.4 GTEDeviceFeatures bridge [LANDED — Phase A]

> **Done.** `GTEDeviceFeatures::featuresAsBitmask() -> uint64_t` ships in
> `gte/include/OmegaGTE.h` and synthesizes the active device's
> `OMEGASL_FEATURE_BIT_*` bits from existing `GTEDEVICE_FEATURE_*` flags
> + the `ShaderModel` tier per the Phase A mapping below. Each backend
> caches the result on `OmegaGraphicsEngine::_deviceFeatures` during
> construction; the loader and pipeline builders consume it. Phase B
> (the four bits currently without backing storage) remains future
> work — see §14.7.1 task E.
>
> Mapping (Phase A — uses existing `GTEDeviceFeatures` data only):
>
> | OMEGASL_FEATURE_BIT_       | Source on GTEDeviceFeatures                                                                                            |
> |---------------------------|------------------------------------------------------------------------------------------------------------------------|
> | RAYTRACING                | `flags & GTEDEVICE_FEATURE_RAYTRACING`                                                                                  |
> | MESH_SHADERS              | `flags & GTEDEVICE_FEATURE_MESH_SHADER`                                                                                 |
> | GEOMETRY_SHADERS          | `flags & GTEDEVICE_FEATURE_GEOMETRY_SHADER`                                                                             |
> | TESSELLATION              | `flags & GTEDEVICE_FEATURE_TESSELLATION_SHADER`                                                                         |
> | SUBGROUP_OPS              | `shaderModel >= SM_6_0`                                                                                                  |
> | BINDLESS                  | `flags & GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING`                                                                         |
> | FLOAT16                   | `flags & GTEDEVICE_FEATURE_SHADER_FLOAT16`                                                                              |
> | INT64                     | `flags & GTEDEVICE_FEATURE_SHADER_INT64`                                                                                |
> | VARIABLE_RATE             | `flags & GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING`                                                                       |
> | DOUBLE                    | `flags & GTEDEVICE_FEATURE_SHADER_FLOAT64`                                                                              |
> | SUBPASS_INPUTS            | (Vulkan-only — needs new `GTEDEVICE_FEATURE_SUBPASS_INPUTS` flag, or always-true on the GLSL/Vulkan backend)             |
> | SPEC_CONSTANTS            | (Vulkan-native; HLSL/MSL emulate via `#define` / function constants — Phase B can synthesize true on all)               |
> | TEXTURECUBE_RW            | (no current `GTEDeviceFeatures` flag — Phase B addition)                                                                |
> | TEXTURE2D_MS_WRITE        | (no current `GTEDeviceFeatures` flag — Phase B addition)                                                                |
>
> Phase A delivers the bridge against today's `GTEDeviceFeatures`
> shape; Phase B extends the struct itself with the missing bits
> (cube R/W, MS write, subpass inputs as a generic flag) once we need
> them — gated behind feature implementations landing.


`GTEDeviceFeatures` already exists in `gte/include/omegaGTE/GTEBase.h` (or
similar). Phase A of §14 adds a `featuresAsBitmask() -> uint64_t` method
that walks the existing `GTEDeviceFeatures` flags and synthesizes the
matching `OMEGASL_FEATURE_BIT_*` bits.

Phase B (later): extend `GTEDeviceFeatures` itself with the bits that
aren't yet tracked (cube R/W, MS write, etc.) so the bridge is total
rather than synthesized.

### 14.5 Predefined feature macros [LANDED]

> Implemented in `Preprocessor::setBackend(...)` via a static
> `kFeatureTable` keyed on `(name, bit, hlslExpressible,
> mslExpressible, glslExpressible)`. A backend defines
> `OMEGASL_FEATURE_<NAME>` only when the feature is expressible at
> highest capability on that backend; otherwise the macro is omitted
> and any `#requires(NAME)` against it triggers the §14.1 stub path.
> The table mirrors the matrix below.


Initial mapping (which `OMEGASL_FEATURE_<NAME>` macros are predefined per
backend):

| Macro | HLSL | MSL | GLSL |
|---|---|---|---|
| `OMEGASL_BACKEND_HLSL` | ✓ | — | — |
| `OMEGASL_BACKEND_MSL` | — | ✓ | — |
| `OMEGASL_BACKEND_GLSL` | — | — | ✓ |
| `OMEGASL_FEATURE_RAYTRACING` | ✓ (SM 6.5+) | ✓ (Metal 3+) | ✓ (KHR_ray_tracing) |
| `OMEGASL_FEATURE_MESH_SHADERS` | ✓ (SM 6.5+) | ✓ (Metal 3+) | ✓ (EXT_mesh_shader) |
| `OMEGASL_FEATURE_GEOMETRY_SHADERS` | ✓ | **—** | ✓ |
| `OMEGASL_FEATURE_TESSELLATION` | ✓ | **—** (runtime gap) | ✓ |
| `OMEGASL_FEATURE_SUBGROUP_OPS` | ✓ (SM 6.0+) | ✓ (Metal 2+) | ✓ |
| `OMEGASL_FEATURE_BINDLESS` | ✓ (SM 6.6) | ✓ (argument buffers) | ✓ |
| `OMEGASL_FEATURE_FLOAT16` | ✓ (SM 6.2 + flag) | ✓ | ✓ (KHR_shader_float16_int8) |
| `OMEGASL_FEATURE_INT64` | ✓ (SM 6.0) | ✓ | ✓ |
| `OMEGASL_FEATURE_VARIABLE_RATE` | ✓ (SM 6.4) | partial | ✓ (KHR_fragment_shading_rate) |
| `OMEGASL_FEATURE_SUBPASS_INPUTS` | **—** | **—** | ✓ |
| `OMEGASL_FEATURE_SPEC_CONSTANTS` | (via #define) | (function constants) | ✓ |
| `OMEGASL_FEATURE_TEXTURECUBE_RW` | partial | **—** | **—** |
| `OMEGASL_FEATURE_TEXTURE2D_MS_WRITE` | **—** | **—** | **—** |
| `OMEGASL_FEATURE_DOUBLE` | ✓ | **—** | ✓ |
| `OMEGASL_FEATURE_TEXTURE1D_MIP_SAMPLE` | ✓ | **—** | ✓ |
| `OMEGASL_FEATURE_CULL_DISTANCE` | ✓ (SV_CullDistance) | **—** (no Metal equivalent) | ✓ (gl_CullDistance) |

A backend defines the macro for a feature only when the feature is
*reliably* supported on the lowest-common-denominator hardware that
backend targets. The macro definition is gated behind the device-feature
probe at runtime, but at compile time it's static — it tracks "is this
expressible in the generated source for this backend at all," not "does
the user's hardware support it."

### 14.6 Concrete example

**Source:**

```omegasl
#requires(OMEGASL_FEATURE_MESH_SHADERS)

struct MeshOut internal {
    float4 pos : Position;
    float3 normal : TexCoord;
};

mesh(threads=64, vertices=64, primitives=128)
void modelMesh(uint3 gid : GlobalThreadID){
    /// ... mesh-shader body ...
}

#if defined(OMEGASL_FEATURE_RAYTRACING)
[in tlas]
compute(x=8,y=8,z=1)
void shadowRay(uint3 gid : GlobalThreadID){
    /// RT-only shadow query path
}
#else
[in shadowMap]
compute(x=8,y=8,z=1)
void shadowMap(uint3 gid : GlobalThreadID){
    /// fallback shadow-map sample path
}
#endif
```

**Compile output (HLSL backend, SM 6.5):** both shaders compile.
`OMEGASL_BACKEND_HLSL` defines `OMEGASL_FEATURE_MESH_SHADERS` and
`OMEGASL_FEATURE_RAYTRACING`, so the preprocessor selects the
`shadowRay` branch. Library records
`modelMesh.requiredFeatures = MESH_SHADERS` (file-scope `#requires`)
and `shadowRay.requiredFeatures = MESH_SHADERS` (file-scope only —
`shadowRay` itself doesn't add bits because per-shader `#requires`
isn't a feature in the current model; if file-scope had also
declared `RAYTRACING` the bit would propagate to every shader).

**Compile output (MSL backend, highest-capability assumption):** the
preprocessor selects the `shadowRay` branch (MSL defines
`OMEGASL_FEATURE_RAYTRACING`). Both shaders compile fully. Library
records `modelMesh.requiredFeatures = MESH_SHADERS`,
`shadowRay.requiredFeatures = MESH_SHADERS`. Whether the user's
specific Metal device supports MESH_SHADERS or RT is decided at
runtime. (Pre-§14.1 modification this would have been a hard error;
the new "transpile as null + tag" rule defers to the runtime.)

**Compile output (any backend, hypothetical `#requires(DOUBLE)` on
MSL):** `OMEGASL_FEATURE_DOUBLE` is *not* defined on MSL — DOUBLE is
not expressible at all in MSL. The shader transpiles to a header-
only stub: no `.metal` file is emitted, no `xcrun` is invoked, the
library record carries `dataSize == 0` plus
`requiredFeatures = OMEGASL_FEATURE_BIT_DOUBLE`. The Layer-2
scanner emits a portability warning (when DOUBLE has a trigger
entry) suggesting partitioning.

**Runtime load (planned, blocked on §14.7.1 tasks A–C):** the loader
masks each shader's `requiredFeatures` against the device's
capability bitmask synthesized by `featuresAsBitmask()`. Shaders
with non-zero `missing` bits are inserted as a sentinel into
`lib->shaders` with a stored diagnostic. Pipeline creation against
the sentinel fails with the diagnostic; sibling shaders in the same
library load and run normally.

### 14.7 Implementation phases — current status

1. **Macro + `#requires` directive** — preprocessor predefines
   `OMEGASL_BACKEND_<X>` and the static `OMEGASL_FEATURE_<NAME>` set;
   `#requires(...)` is parsed at file scope; resolved bits drive the
   stub-emission path. `#if defined(...)` / `#elif` / `#else`
   evaluation added so source-level fallback paths work. **[LANDED]**

2. **Portability scan** — runs as a Sema-time AST walker
   (`omegasl::FeatureScanner`) instead of backend codegen
   instrumentation. Phase A undeclared-use + Phase B partition-
   suggestion warnings both emit to stderr. The trigger table is
   seeded with cube `read`/`write` and `texture2d_ms` `write` (both
   currently rejected at Sema as compile-path-only — entries are
   live-wired but inert until those compile paths land). **[LANDED;
   trigger table grows as features land]**

3. **`omegasl_shader.requiredFeatures` field + library writer/reader**
   — `omegasl.h` gained the `omegasl_shader_feature_flags` enum and
   the `requiredFeatures` field. Writer (`linkShaderObjects`) emits
   header-only entries for stub shaders (`dataSize == 0`, no per-stage
   block) and appends the bitfield to every record. Reader
   (`OmegaGraphicsEngine::loadShaderLibraryFromInputStream`) accepts
   `dataSize == 0`, skips the per-stage block in that case, and reads
   the trailing `requiredFeatures`. **No** `lib_version` byte was
   added — the format change is breaking against pre-§14 readers, but
   no shipped lib predates the gating system. **[LANDED]**

4. **`GTEDeviceFeatures::featuresAsBitmask` + per-shader load
   rejection** — **[LANDED]**. Three pieces:
   - **(4a) Bridge**: `GTEDeviceFeatures::featuresAsBitmask() -> uint64_t`
     in `gte/include/OmegaGTE.h`. Phase A only — covers the bits backed
     by existing `GTEDEVICE_FEATURE_*` flags + the `ShaderModel` tier;
     Phase B extension for `TEXTURECUBE_RW` / `TEXTURE2D_MS_WRITE` /
     `SUBPASS_INPUTS` is §14.7.1 task E and stays open. **[LANDED]**
   - **(4b) Loader rejection**: each backend caches the bitmask on
     `OmegaGraphicsEngine::_deviceFeatures`. The library loader
     (`loadShaderLibraryFromInputStream`) and the runtime loader
     (`loadShaderLibraryRuntime`) compute
     `missing = shader.requiredFeatures & ~_deviceFeatures`; when
     non-zero they skip `_loadShaderFromDesc` and substitute an
     unsupported-shader sentinel (base `GTEShader` with
     `isUnsupported = true` + `unsupportedDiagnostic`) plus a
     side-channel entry in `GTEShaderLibrary::unsupportedDiagnostics`.
     The diagnostic is also logged to stderr at load time. **[LANDED]**
   - **(4c) Pipeline-builder diagnostic surface**: each backend's
     `makeRenderPipelineState` / `makeComputePipelineState` (D3D12,
     Metal, Vulkan) calls
     `OmegaGraphicsEngine::_checkPipelineShader(shader, role,
     pipelineName)` at the top; on a null shader or a sentinel the
     helper writes a precise message to stderr and the builder
     returns `nullptr` cleanly without touching shader bytecode.
     **[LANDED]**

5. **Partition-suggestion warning (Phase B of layer 2)** — Sema-time
   scanner emits both undeclared-use and partition warnings;
   stderr-formatted with file basename and recommended split filename
   (`<file>_<lower_feature>.omegasl`). **[LANDED]**

### 14.7.1 Remaining work for full operation

A through D have all landed; the source → library → loader → pipeline
path now rejects unsupported shaders cleanly with a precise
diagnostic. The remaining tasks (E–G) are gated behind specific
feature implementations rather than the gating system itself.

| # | Task | Where | Effort | Blocks | Status |
|---|------|-------|--------|--------|--------|
| A | `GTEDeviceFeatures::featuresAsBitmask` (Phase A — uses existing flags) | `gte/include/OmegaGTE.h` | small | runtime rejection | **LANDED** |
| B | Loader-side mask + sentinel insertion | `gte/src/GE.cpp` (`loadShaderLibraryFromInputStream` + `loadShaderLibraryRuntime`) | small | runtime rejection | **LANDED** |
| C | Pipeline-builder sentinel handling + diagnostic surface | `makeRenderPipelineState` / `makeComputePipelineState` on D3D12 / Metal / Vulkan | small | clean error at draw/dispatch time | **LANDED** |
| D | Per-backend probe of `GTEDEVICE_FEATURE_*` flags | D3D12 / Metal / Vulkan device init | medium | accurate device feature reporting | **LANDED** |
| E | `GTEDeviceFeatures` extension for `TEXTURECUBE_RW`, `TEXTURE2D_MS_WRITE`, `SUBPASS_INPUTS` | `gte/include/OmegaGTE.h` | small | gating those specific features (Phase B of §14.4) | open |
| F | Trigger-table entries for DOUBLE, FLOAT16, INT64, RT intrinsics, subgroup ops | `FeatureScanner.cpp` | per-feature | scanner reporting on those features | open |
| G | Cube `read`/`write` + 2D-MS `write` Sema acceptance + codegen | `Sema.cpp:1330,1447`, all three Targets | medium | making the existing trigger entries fire | open |

E unblocks the four `OMEGASL_FEATURE_BIT_*` bits that have no backing
`GTEDEVICE_FEATURE_*` flag today; once the struct grows the
corresponding fields, `featuresAsBitmask` can synthesize them and the
runtime can stop conservatively rejecting shaders that name those
bits. F adds undeclared-use scanner reporting for the language
features that haven't shipped yet — entries land alongside each
feature implementation. G is the language work itself for cube R/W
and 2D-MS write, which currently bounce at Sema and so make their
trigger entries inert.

### 14.7.2 Format note

The `.omegasllib` format changed shape in step 3:

- Stub shaders write `type, name_size, name, dataSize=0, nLayout=0,
  requiredFeatures` and skip the per-stage block.
- Real shaders write the existing layout + per-stage block, then
  append `requiredFeatures` (uint64_t) at the tail.

The reader detects stubs by `dataSize == 0` and branches accordingly.
This is a hard format break against any pre-§14 reader, accepted on
the grounds that no shipping lib predates the change.

### 14.8 Out of scope

- **Per-shader / per-function `#requires`.** Per-shader annotation was
  considered for Phase 1.2 and dropped: when a user function uses a
  feature, the cleanest way to gate the shader that calls it is the
  Sema-time scanner (§14.2), not by hand-decorating each shader.
  `#requires` remains file-scope only; if mixing happens, the partition
  suggestion warns the author to split the file.

- **Conditional resource declarations.** `#if defined(OMEGASL_FEATURE_X)`
  around a `buffer<...>` or `texture2d` declaration is technically
  expressible but produces resource-binding-table drift across backends
  (the binding numbers shift). Recommend documenting as "supported but
  fragile" — the partition pattern is cleaner.

- **Version-numbered features.** Things like "SM 6.6 minimum" or "Metal
  2.4+" don't appear as feature flags here — they appear as the version
  the backend was *invoked at*. If `dxc -T cs_6_6` is used and the user
  declares `#requires(OMEGASL_FEATURE_BINDLESS)`, the feature macro is
  defined and the shader compiles. There's no "minimum SM" macro because
  that's not a feature, it's a compiler invocation parameter.

- **Optional feature-set negotiation.** Vulkan extensions can be requested
  optionally — "use ray-query if available, otherwise fall back." The
  `#if defined(...)` path covers source-level fallback. Per-shader
  *runtime* fallback (load-time substitution of an alternate shader if
  the primary fails) is a higher-level engine concern, not a language
  feature.

---

## 15. Cross-cutting: Diagnostics & multi-error recovery

The compiler is already wired to report *multiple* errors per compile:
`DiagnosticEngine` accumulates up to `kMaxErrorsBeforeStop` (50) errors
(`Error.h` / `Error.cpp`), and the driver reports the whole set and exits
non-zero at end of run (`main.cpp` `if(diagnostics.hasErrors())`). The
limiting factor is not the diagnostics store — it is *how far the semantic
walk recovers* before giving up.

### 15.1 Function-body statement recovery [LANDED]

`Sem::performSemForBlock` recovers past a failed statement instead of
returning on the first one: it records the failure, keeps checking the
remaining statements in the body, and signals failure to the caller only
after the whole block has been walked (skipping the return-type
consistency check, which would be spurious once statements were
abandoned). A "fail loud" guard still fires — if a failing statement
somehow recorded no diagnostic of its own, a generic one is added — so a
sem failure can never slip through as a silent success that emits a broken
library. The walk also stops at the 50-error cap.

Effect: a single shader/function body with several independent errors now
surfaces all of them in one compile. The pre-existing "multi-error"
negative tests that live in one body (`invalid_sample_variants`,
`invalid_texture_query`, `invalid_modf_frexp`, …) previously only ever
exercised their *first* case — a regression in cases 2..N would have left
the test passing on case 1. They now report every case.

### 15.2 Cross-declaration recovery [DEFERRED — gap]

The top-level driver loop (`Parser::parseContext`) deliberately **stops at
the first failed global declaration** rather than continuing to the next
one. This is the known limitation:

- A file whose independent errors are spread across *separate* decls
  (e.g. `invalid_uniform`'s three compute shaders, or a resource decl plus
  the shader that binds it) still reports only the first.

The reason it is not simply "remove the `break`": unlike a statement in a
function body, a global decl registers **shared state** — resource maps,
struct/type registrations — that *later* decls read. A decl that fails
sem partway can leave that state half-registered, and a subsequent decl
that depends on it then dereferences the missing piece and **crashes**.
The concrete case is `invalid_buffer_no_args`: `buffer noElement : 0;`
(no element type) fails sem, but the shader that binds `[out noElement]`
reaches an out-of-bounds `args[0]` for the element type — the exact crash
that decl's test comment documents. Naively continuing the decl loop
re-introduces that segfault on `invalid_uniform` and `invalid_buffer_no_args`.

**Scope when picked up.** Either (a) give the driver dependency tracking so
it skips decls that reference a failed decl while still semming
independent ones, or (b) harden every global-decl partial-registration
path so a failed decl leaves no half-state for dependents to trip on (and
register failed resources/structs as error-typed placeholders so
references resolve to a clean "depends on an invalid declaration"
diagnostic instead of a crash). Both are larger than the §15.1 change and
carry cascade-noise risk; deferred until a real need appears. Until then,
author multi-error negative tests *within a single shader body* (§15.1
recovers those) rather than across multiple decls.

---

## 16. Tessellation — finishing the backends

The `hull` / `domain` stages are the one place where OmegaSL ships a
**language surface with no working backend**. The parser, Sema, and
`ShaderDecl::TessellationDesc` (domain / partitioning / outputtopology /
outputcontrolpoints) all exist, resource maps bind, and every backend's
`emitShaderEntryHeader` has a hull/domain arm — but none of the three
produces a tessellation pipeline that actually runs:

- **HLSL** — RESOLVED for codegen (§16-A + §16-B landed). The hull entry now
  carries `[patchconstantfunc("<patchfn>")]` naming the patchfn (emitted as an
  ordinary function whose return struct fields carry `SV_TessFactor` /
  `SV_InsideTessFactor`), takes `InputPatch<CP, N>` + `uint : SV_OutputControlPointID`,
  and the domain takes `const OutputPatch<CP, N>` + `float3/2 : SV_DomainLocation`.
  Control-point buffers are lifted out of the file-scope `register(...)` set
  into those patch params (the hull `[out]` buffer is dropped — the hull emits
  its per-CP output via `return`). The generated HLSL compiles with `dxc`;
  `omegasl_compile_tessellation` is expected-pass on `TARGET_DIRECTX`. (was
  `OmegaSL-Reference.md` bug 4.)
- **Metal** — RESOLVED for codegen (§16-A + §16-D landed). A `hull` lowers
  to a compute kernel (per-CP output store + a tess-factor epilogue that runs
  the `patchfn` and writes `MTLTriangle/QuadTessellationFactorsHalf`), and a
  `domain` lowers to a `[[patch(...)]]` post-tessellation vertex consuming
  `patch_control_point<CP>` + `[[position_in_patch]]`. The generated MSL
  compiles with `metal`; `omegasl_compile_tessellation` is expected-pass on
  `APPLE`. Runtime pipeline plumbing (`drawPatches`, factor-buffer binding) is
  still Phase E. (was `OmegaSL-Reference.md` bug 3.)
- **GLSL** — RESOLVED for codegen (§16-A + §16-C landed). The `.tesc` now
  emits `layout(vertices=N) out`, writes `gl_out[gl_InvocationID].gl_Position`,
  and — guarded on `if (gl_InvocationID == 0)` — writes
  `gl_TessLevelOuter/Inner[]` from the patchfn (called as a plain function).
  The `.tese` emits `layout(domain,spacing,winding) in` and binds the
  `DomainLocation` param to `gl_TessCoord` (`.xy` for quad). The patch-constant
  struct is emitted as a **plain data struct** (not destructured into
  inter-stage varyings — the fix that unbroke the skeleton). Verified against
  `glslc` (`tesscontrol`/`tesseval`); `omegasl_compile_tessellation` is
  expected-pass on the Vulkan/GLSL backend.
- **Runtime** has *zero* tessellation plumbing on all three backends.
  `RenderPipelineDescriptor` exposes only `vertexFunc` / `fragmentFunc`
  (`GEPipeline.h`); there is no patch primitive topology, no `drawPatches`,
  and `TessellationDesc` never reaches the runtime `omegasl_shader`.

There is also a **latent honesty bug**: D3D12 advertises
`GTEDEVICE_FEATURE_TESSELLATION_SHADER` unconditionally (`GED3D12.cpp:173`)
and Vulkan advertises it whenever the GPU reports `tessellationShader`
(`GEVulkan.cpp:512`), even though neither runtime can build a tessellation
pipeline. A `#requires(TESSELLATION)` shader therefore *passes* the §14.3
per-shader gate, loads, and then trips the vertex/fragment-only assertions
in `makeRenderPipelineState` — a crash, not the clean rejection §14.3 is
supposed to give. Metal correctly does **not** advertise it
(`GEMetal.mm:116`).

This section is the plan to close all of that. It depends on the §14
feature-gating machinery (landed) and resolves `OmegaSL-Reference.md` bugs
3 and 4.

### 16.0 The core design problem — expressing the patch-constant output

A tessellated patch produces **two** outputs from the control stage, and
OmegaSL can express only one of them today:

1. **Per-control-point** data — runs once per output control point. This is
   exactly what the current `hull` body computes (returns the hull-output
   struct per invocation).
2. **Per-patch ("patch-constant")** data — computed once per patch, and
   *always* carries the tessellation factors (per-edge `TessFactor`s and
   the `InsideTessFactor`(s)) that tell the fixed-function tessellator how
   finely to subdivide. OmegaSL has **no way to express this**, which is
   the root cause of bugs 3 and 4.

Each backend wants the patch-constant computation as a distinct unit:

| Backend | Patch-constant realization |
|---------|----------------------------|
| HLSL | a separate function named by `[patchconstantfunc("…")]`, returning a struct with `SV_TessFactor[]` / `SV_InsideTessFactor[]` fields |
| GLSL | writes to `gl_TessLevelOuter[]` / `gl_TessLevelInner[]` (+ optional `patch out` vars), conventionally guarded by `if (gl_InvocationID == 0)` |
| MSL  | a **compute kernel** that writes `MTLTriangle/QuadTessellationFactorsHalf` into the tessellation-factor buffer |

**Design (confirmed by Alex).** Model the patch-constant computation as a
*named companion function* of the hull, plus three new attribute semantics
— mirroring exactly how §1.2–§1.4 added `Depth`, `Color(N)`, and
`OutputCoverage` as new field/param semantics rather than new syntax forms:

- New hull descriptor property `patchfn=<name>`, e.g.
  `hull(domain=tri, partitioning=integer, outputtopology=triangle_cw, outputcontrolpoints=3, patchfn=triConstants)`.
- New internal-struct **field** semantics `TessFactor(N)` and
  `InsideTessFactor(N)` (on `float` fields), valid only on the struct a
  patch-constant function returns.
- New **parameter** semantic `DomainLocation` (`float3` for tri, `float2`
  for quad) on the `domain` shader, lowering to `SV_DomainLocation` /
  `gl_TessCoord`.

The patch-constant function is an ordinary user function returning that
internal struct; the hull names it. This keeps the per-CP hull body and
the per-patch factor body as two clearly separated pieces of source —
which is what *every* backend needs — and avoids asking the compiler to
*split* one interleaved body.

Alternatives considered:
- **Option 2 — one hull struct mixing per-CP and `TessFactor` fields.**
  Cleaner authoring, but the compiler would have to separate per-CP from
  per-patch logic out of a single body; impossible in general when the two
  computations interleave.
- **Option 3 — a dedicated `patch` stage keyword** instead of `patchfn=`.
  Equivalent power; rejected only because `patchfn=` keeps the
  patch-constant fn bound to its hull at the declaration site and adds no
  new top-level keyword. Revisit if multiple hulls want to share one
  patch-constant fn.

Phases A–D below implement this design.

### Phase A — language: tess-factor / domain-location semantics [design]

Land the §16.0 surface: parse `patchfn=` in the hull descriptor
(`Parser.cpp`, the `KW_HULL` arm), add the `TessFactor(N)` /
`InsideTessFactor(N)` field semantics and the `DomainLocation` param
semantic (the attribute-context tables in `Sema.cpp`, alongside
`Color`/`Depth`), and Sema validation: a `patchfn` must name a function
returning an internal struct whose only attributed fields are
`TessFactor`/`InsideTessFactor`; factor counts must match the domain
(tri → 3 edge + 1 inside; quad → 4 + 2); `DomainLocation` rank must match
the domain. No codegen yet — Phase A is purely the front-end contract that
B–D lower.

### Phase B — HLSL codegen → `dxc`-valid (resolves bug 4) [DONE 2026-07-05]

Emit the patch-constant function (its return-struct fields tagged
`SV_TessFactor`/`SV_InsideTessFactor`), add `[patchconstantfunc("…")]` to
the hull entry, give the hull an `SV_OutputControlPointID` param and an
`InputPatch<CP, N>`, and give the domain its `SV_DomainLocation` +
`OutputPatch<CP, N>`. Add a golden test; flip
`omegasl_compile_tessellation` to expected-pass on `TARGET_DIRECTX`
(§16-F).

**Landed (`HLSLTarget.cpp`), `dxc`-verified (`hs_6_0`/`ds_6_0`).** Unlike
Metal, the HLSL hull keeps its `return <expr>;` (per-CP output flows through
the return, not a store), so no return-rewrite was needed. Details:
- `writeAttribute`: `TessFactor`→`SV_TessFactor`, `InsideTessFactor`→
  `SV_InsideTessFactor`, `DomainLocation`→`SV_DomainLocation`. The patchfn is
  emitted as a normal user function; `[patchconstantfunc("…")]` names its
  mangled `osl_user_…` spelling (`spellUserFuncName`). A **zero-arg** patch-
  constant function and a **domain with no patch-constant input param** are
  both accepted by `dxc` (verified) — so the OmegaSL surface maps 1:1.
- `emitResourceBinding`: a hull/domain `buffer<T>` is **not** a file-scope
  `register(...)` global — the hull `[in]`/domain `[in]` buffer is injected in
  `emitShaderEntryHeader` as an `InputPatch<CP,N>` / `const OutputPatch<CP,N>`
  parameter, and the hull `[out]` buffer is dropped.
- `emitShaderUsedStructs`: control-point structs (the `[in]` buffer element
  and the hull's return struct) are plain in OmegaSL, but HLSL requires
  semantics on `InputPatch`/`OutputPatch`/return I/O — so they are re-emitted
  with positional `TEXCOORD<N>` semantics (mesh-vertex re-emit is the
  precedent). A hull's `VertexID` param → `SV_OutputControlPointID`.
- Uses the same `tessellation.omegasl` (tri+quad); `dxc` compiles all four
  hull/domain entries. `omegasl_compile_tessellation` gate is now
  `NOT APPLE AND NOT TARGET_DIRECTX` (Metal + DirectX pass, GLSL pending).

### Phase C — GLSL codegen → `glslc`-valid [DONE 2026-07-05]

In the `.tesc`: write per-CP output to the `out` control-point array and,
under `if (gl_InvocationID == 0)`, emit the patch-constant fn's body into
`gl_TessLevelOuter/Inner[]` (plus any `patch out` user fields). In the
`.tese`: bind `gl_TessCoord` to the `DomainLocation` param and read the
per-CP `in[]` array. Verify against `glslc`; add a compile/golden test.

**Landed (`GLSLTarget.cpp`), `glslc`-verified (`tesscontrol`/`tesseval`).**
The existing skeleton was broken (it destructured the patch-constant struct
into `out` varyings, mangled the patchfn body, wrote no tess levels, and left
`gl_TessCoord`/the return unwired). Fixes:
- **Patch-constant struct as plain data** — `isPatchConstantStruct` (internal +
  a `TessFactor`/`InsideTessFactor` field) is kept OUT of `internalStructs`, so
  it emits as a plain `struct`, its locals aren't added to
  `internalStructVarMap`, and its field refs aren't rerouted to varyings. This
  was the root cause of the mangled skeleton.
- **Tess-level writes** — a hull-body epilogue emits
  `if (gl_InvocationID == 0) { <PatchStruct> osl_pc = <patchfn>();
  gl_TessLevelOuter[i] = osl_pc.<edge>[i]; gl_TessLevelInner[j] =
  osl_pc.<inside>[…]; }` (tri: 3 outer + 1 inner scalar; quad: 4 + 2).
- **DomainLocation → `gl_TessCoord`** — the param bridge (which only handled
  `VertexID`) now maps `DomainLocation` to `gl_TessCoord` (whole for tri,
  `.xy` for quad).
- **Return** — a hull/domain `return <internalStructVar>` (e.g. the domain's
  `DomainOut` whose `Position` already routed to `gl_Position`) now emits a
  bare `return;` instead of a dangling `gl_Position = o.pos` on the suppressed
  local.
- Control points stay SSBOs (`buffer<CP>` → `layout(std430) buffer`), matching
  OmegaSL's buffer model; the hull's `[out]` buffer emits as an (unused) SSBO.
  Correct per-patch SSBO indexing is a Phase-E runtime concern, same as the
  Metal per-patch-dispatch decision.
- Identifier note: the epilogue local is `osl_pc` (not `__osl_pc`) — GLSL
  reserves `__` (double-underscore) identifiers, which `glslc` warns on.

### Phase D — Metal codegen: kernel + post-tessellation vertex (resolves bug 3)

The large one. Split one `hull`/`domain` pair into two MSL entry points:
(a) a `[[kernel]]` that runs the patch-constant fn and writes
`MTLTriangle/QuadTessellationFactorsHalf` to the factor buffer, and (b) a
post-tessellation `[[patch(triangle|quad, N)]] vertex` built from the
`domain` body that consumes `patch_control_point<CP>` +
`[[position_in_patch]]`. This needs the shader-map to expose **two** Metal
entry points for one logical hull/domain pair (the "metadata" note under
`OmegaSL-Reference.md` bug 3) — best done as per-backend stage-expansion in
the shader-map writer rather than leaking the split into the public
`omegasl_shader` API. Remove the `supportsStage()` rejection once it
compiles.

#### Phase A + D implementation detail (this pass — Metal codegen only)

Scope of this pass (confirmed by Alex): land the shared front-end (Phase A)
and Metal codegen (Phase D) **only** — no runtime pipeline plumbing (Phase E
is a follow-up). The verification bar is: the generated MSL compiles with the
real `metal` compiler (`xcrun -sdk macosx metal`), driven by the existing
`omegasl_compile_tessellation` ctest (flip its `APPLE` `WILL_FAIL` off).
HLSL/GLSL stay `WILL_FAIL` (Phases B/C unimplemented); the non-Apple non-DX
config is *added* to the `WILL_FAIL` set because the rewritten test file now
uses the full new surface that GLSL cannot yet honor.

**Phase A — front-end surface (backend-agnostic).**

* `AST.def`: three new attribute macros — `ATTRIBUTE_TESS_FACTOR "TessFactor"`,
  `ATTRIBUTE_INSIDE_TESS_FACTOR "InsideTessFactor"`,
  `ATTRIBUTE_DOMAIN_LOCATION "DomainLocation"`.
* `AST.h`: `TessellationDesc::patchFn` (`OmegaCommon::String`, empty ⇒ none).
* `Parser.cpp` (`KW_HULL` arm): parse `patchfn=<ident>` into
  `tessDesc.patchFn`, alongside the existing `domain/partitioning/
  outputtopology/outputcontrolpoints` props.
* `Sema.h` (`isValidAttributeInContext`): allow `TessFactor` /
  `InsideTessFactor` as `StructField` semantics; allow `DomainLocation` as a
  `DomainShaderArgument` param semantic.
* `Sema.cpp` (SHADER_DECL, hull arm): a hull **must** name a `patchfn`; the
  named function must exist and return an `internal` struct whose only
  attributed fields are `TessFactor`/`InsideTessFactor`; edge/inside factor
  counts must match the domain (tri → 3 edge + 1 inside; quad → 4 + 2). The
  hull must have exactly **one** `out` buffer resource (the per-CP output
  store target on Metal). A `DomainLocation` param's rank must match the
  domain (`float3` tri / `float2` quad). First-cut restriction: the patchfn
  takes **zero parameters** (multi-arg patchfn — passing control points /
  a uniform — is a follow-up); Sema enforces the zero-arg rule.

**Phase D — Metal lowering model: per-control-point dispatch (INTERIM).**

> **Superseded target (Alex, 2026-07-05): per-*patch* dispatch.** The final
> Metal design dispatches the hull/factor kernel **one thread per patch**, so
> the dispatch aligns with the per-patch tessellation config the shader
> declares (domain / control-point count / partitioning), read by the runtime
> from the tessellation descriptor serialized into the `.omegasllib` metadata.
> The per-control-point shape below is the **interim** cut that compiles and
> is `metal`-verified today; moving to per-patch dispatch is Phase-E work and
> requires a **codegen revision in lockstep** (it is *not* runtime-only) —
> see "Phase E — Metal per-patch dispatch" below. The per-CP kernel would
> produce wrong results if dispatched per-patch (its `thread_position_in_grid`
> is a control-point index, not a patch index), so codegen and dispatch must
> change together.

Each of the two OmegaSL decls emits exactly one MSL entry point (no shader-map
stage-expansion is needed for codegen; the runtime-facing "two entries for one
pair" metadata is a Phase E concern):

* **`hull` → `[[kernel]]`**, dispatched one thread per output control point
  across all patches. `[[thread_position_in_grid]]` is the hull's `VertexID`
  param = global control-point index (matches the pre-existing hull→kernel
  mapping and the `uint vid : VertexID` idiom).
  * The per-CP hull body is emitted verbatim, except a trailing
    `return <expr>;` is rewritten to `<outBuffer>[<vidParam>] = <expr>;`
    (the single `out` buffer resource is the store target).
  * A **synthesized** factor-buffer param is appended after the user
    resources/params: `device <FactorT>* __osl_tess_factors [[buffer(K)]]`,
    K = next free MSL buffer slot. `<FactorT>` is
    `MTLTriangleTessellationFactorsHalf` (tri) /
    `MTLQuadTessellationFactorsHalf` (quad) — the exact spelling is
    verified against the `metal` compiler before commit.
  * Factor epilogue, guarded so only the first CP of each patch writes:
    `if ((vid % N) == 0u) { auto __pc = <patchfn>(); __osl_tess_factors[vid / N]
    .edgeTessellationFactor[i] = half(__pc.<edgeField>[i]); …
    .insideTessellationFactor[j] = half(__pc.<insideField>[j]); }`
    (quad inside is a 2-array; tri inside is scalar
    `insideTessellationFactor`).
* **`domain` → `[[patch(triangle|quad, N)]] vertex`** (the existing arm, now
  reached):
  * Each `[in]` buffer resource whose element is the control-point struct is
    emitted as `patch_control_point<CP> <name> [[stage_in]]` instead of
    `constant CP*` — the body's `name[i]` subscript then reads a control
    point. (Selected by: domain stage + `In` buffer.)
  * The `DomainLocation` param → `float3/float2 <name> [[position_in_patch]]`.
  * Return value routes to the rasterizer output struct exactly like a
    vertex shader.

**supportsStage**: drop the Hull/Domain rejection in `MSLTarget::supportsStage`.

**Test**: rewrite `gte/omegasl/tests/tessellation.omegasl` to the new surface
(patchfn + `TessFactor`/`InsideTessFactor`/`DomainLocation`), covering tri and
quad. `CMakeLists.txt`: remove `APPLE` from the tessellation `WILL_FAIL` gate,
add the non-Apple-non-DX config to it (GLSL Phase C pending). A negative test
(`invalid_tessellation.omegasl`) covers: hull with no patchfn, wrong factor
count, `DomainLocation` rank mismatch.

**Deferred to Phase E (runtime) / follow-ups**: multi-arg patchfn, per-patch
user varyings, the `omegasl_shader` tessellation metadata + the two-entry-point
runtime binding, any real tessellated draw, and — per Alex — **switching the
Metal factor kernel from the interim per-CP shape to per-patch dispatch driven
by that metadata** (codegen + runtime together; see the Phase E Metal note).

### Phase E — runtime: pipeline plumbing + public API

Public API (`GEPipeline.h` / `GERenderTarget.h` / `omegasl.h`):
- `RenderPipelineDescriptor`: add `hullFunc` / `domainFunc` and
  `uint32_t patchControlPoints`. **[DONE 2026-07-05]**
- `PrimitiveTopologyCategory`: add `Patch` **[DONE]**; `PolygonType`: add a patch
  topology + a `drawPatches(controlPointCount, …)` command on
  `GECommandBuffer`.
- Serialize `TessellationDesc` into `omegasl_shader` (a new
  `omegasl_tessellation_desc`) so the runtime can read
  partitioning / winding / control-point count — it is AST-only today.
  **[DONE 2026-07-05]**

**Tessellation render pass — a dedicated `startTessRenderPass` (decision: Alex,
2026-07-05, supersedes "auto inside `drawPatches`").** A tessellated draw is
started with a **new** command-buffer entry point, `startTessRenderPass`, NOT the
regular `startRenderPass`. This makes the two-stage structure (hull/factor
compute → fixed-function tessellator + domain/fragment draw) explicit at the API
boundary instead of hiding encoder gymnastics inside `drawPatches`:

- **`startRenderPass` does not support tessellation stages.** Binding a
  tessellation pipeline (one with `hullFunc` / `domainFunc`) inside a plain
  render pass is a caller-contract violation (surface it via `DEBUG_CRITICAL` +
  assert, like the existing pipeline-kind guards). A plain render pass stays a
  single render encoder with no compute pre-pass — byte-identical to today.
- **`startTessRenderPass` opens a deferred tessellation pass.** The engine runs
  the hull/factor **compute dispatch first** (one thread per patch, writing the
  per-control-point output + the `MTL{Triangle,Quad}TessellationFactorsHalf`
  factor buffer) and **defers the render encoder until that compute kernel has
  finished — without halting the engine.** The deferral is ordered by the GPU's
  own dependency tracking (Metal: the render encoder is created after the
  compute encoder ends on the same command buffer, and the factor / control-point
  buffers carry the hazard so the driver serializes them; the CPU never blocks on
  the dispatch). This is why a dedicated entry point is cleaner than the earlier
  "suspend and re-open the render encoder inside `startRenderPass`" idea — there
  is no encoder to suspend, and the ordering is a normal compute-before-render
  sequence the backend already knows how to schedule asynchronously.
- The engine owns the internal hull-output + factor buffers (sized from the
  patch count + the `omegasl_tessellation_desc` control-point count); the caller
  binds only its input control points and issues `drawPatches` inside the tess
  pass. `finishRenderPass` closes it.
- Non-Metal backends map the same surface onto their native model: D3D12 / Vulkan
  run HS/DS inside the one graphics pipeline, so `startTessRenderPass` there is a
  thin variant of `startRenderPass` that permits the tessellation pipeline and
  patch topology (no separate compute pre-pass) — but keeping the dedicated
  entry point keeps the "plain `startRenderPass` rejects tessellation" contract
  uniform across all three backends.

Per backend (`makeRenderPipelineState`):
- **D3D12** (`GED3D12.cpp:1438`): set `.HS`/`.DS`, build the root signature
  over four stages, `PrimitiveTopologyType = …_PATCH`,
  `IASetPrimitiveTopology(…_N_CONTROL_POINT_PATCHLIST)`; drop the
  vertex/fragment-only `type ==` assertions (~line 1453).
- **Vulkan** (`GEVulkan.cpp:2669`): add the tess-control/eval
  `VkPipelineShaderStageCreateInfo`s (stageCount 2 → 4),
  `VkPipelineTessellationStateCreateInfo.patchControlPoints`,
  `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`, and **enable** the `tessellationShader`
  device feature at device creation (currently only probed).
- **Metal** (`GEMetal.mm:1209`): `startTessRenderPass` runs the hull/factor
  compute dispatch on a compute encoder, then (deferred, non-blocking — see the
  `startTessRenderPass` note above) opens the render encoder with the
  `MTLRenderPipelineDescriptor` tessellation properties
  (`tessellationFactorBuffer`, `tessellationPartitionMode` ← partitioning,
  `tessellationOutputWindingOrder` ← outputtopology, `maxTessellationFactor`)
  and issues `drawPatches`. The standalone hull dispatch + factor write this
  builds on is already verified (16-E4a / `TessellationDispatchTest`).

**Phase E — Metal per-patch dispatch (decision: Alex, 2026-07-05).** The hull
kernel is dispatched **one thread per patch** (grid = patch count), configured
from the tessellation descriptor read out of the `.omegasllib` metadata, so the
dispatch matches what the shader declared rather than a codegen assumption. This
supersedes the interim per-control-point kernel from Phase D and is **not
runtime-only** — it needs a paired Metal-codegen change:

- **Metadata (`omegasl_shader`)**: serialize the `TessellationDesc` (domain,
  partitioning, output topology, `outputControlPoints`, and the hull↔patchfn /
  hull↔domain linkage) into a new `omegasl_tessellation_desc` on the hull's
  `omegasl_shader` record, written by the shader-map writer and read at pipeline
  build / dispatch time. Today the descriptor is AST-only.
- **Codegen change** (`MSLTarget.cpp`): re-shape the hull kernel so
  `[[thread_position_in_grid]]` is the **patch id**, wrap the per-CP body in a
  `for (uint __cp = 0; __cp < N; ++__cp)` loop with the `VertexID` param bound
  to `patchId * N + __cp` (global CP index), and write the tess factors
  **unconditionally** (once per thread = once per patch), dropping the
  `vid % N == 0` guard. The current per-CP kernel (`inHullShader` return→store +
  guarded epilogue in `emitShaderEntryHeader`/`emitShaderEntryBody`/
  `tryEmitReturnDecl`) is what gets revised.
- **Runtime** (`GEMetal.mm`): dispatch the kernel with `threadsPerGrid = patch
  count` (from the draw's patch count) using the metadata's control-point count,
  then `drawPatches`. The factor buffer is `MTL{Triangle,Quad}TessellationFactorsHalf`
  sized to the patch count.

Rationale: per-patch dispatch has no wasted threads (the interim per-CP shape
runs N threads per patch but only 1 writes factors), and the granularity then
mirrors the fixed-function tessellator's per-patch factor model and HLSL's
separate per-patch patch-constant function.

### Phase F — feature-flag honesty + re-enable

Until Phase E lands per backend, **stop advertising**
`GTEDEVICE_FEATURE_TESSELLATION_SHADER` where the runtime can't honor it:
gate D3D12 (`GED3D12.cpp:173`) and Vulkan (`GEVulkan.cpp:512`) so they
advertise only once their Phase E work is in (Metal already abstains). This
converts the current load-then-crash into the clean per-shader rejection
§14.3 already provides. Re-advertise each backend as its codegen + runtime
pair completes; the `omegasl_compile_tessellation` `WILL_FAIL` markings come
off per backend in lockstep.

### Phase G — mandatory vertex stage for tessellation (`vertex(tess=true)`) [design — Alex, 2026-07-06]

**Why.** The interim tessellation model (§16.0 → Phase D/E) has **no vertex
stage**: the hull reads its control points directly from an SSBO indexed by
`VertexID`, and Metal's runtime lowers the hull to a compute dispatch. That
works for Metal — but a **Vulkan/D3D12 graphics tessellation pipeline requires
a vertex stage** feeding the tessellation-control stage. A pipeline with only
`tesc`/`tese`/`fragment` and no `VK_SHADER_STAGE_VERTEX_BIT` is invalid. So the
Vulkan runtime (16-E3) cannot be built on the SSBO-only model.

**Decision (Alex, 2026-07-06).** Require a vertex stage for a tessellation
pipeline **everywhere**, authored as a new stage descriptor
`vertex(tess=true)`. `tess` defaults to `false`, so a bare `vertex` is
byte-identical to today. The vertex stage reads the raw input vertices and
**outputs a control-point struct that the hull consumes** — the standard
`vertex → hull → domain → fragment` dataflow (D3D11/GL/Vulkan all model it this
way). This supersedes the SSBO-only hull input.

Per backend (target model):

| Backend | `vertex(tess=true)` lowers to | hull input |
|---------|-------------------------------|-----------|
| Vulkan / D3D12 | the real vertex stage (`.vert` / VS) feeding the tessellator | tesc reads `gl_in[]` / HS reads `InputPatch` |
| Metal (**deferred**) | a **compute** pass that runs *before* the hull compute, writing a per-vertex control-point buffer the hull compute reads | hull compute reads that buffer |

**Scope this session (Alex): Vulkan-first.** Land the shared front-end, the
GLSL codegen for the standard dataflow, the Vulkan runtime, and a
Vulkan-runnable tess draw test. **Metal keeps its current landed path** (SSBO
hull-compute, no vertex stage) until a follow-up migrates it to the
compute-before-hull model — so this pass must not break Metal's existing
`omegasl_compile_tessellation` / `TessellationDispatchTest` / `TessellationDrawTest`.
D3D12 vertex-stage wiring is a follow-up too. Because only Vulkan compiles/runs
on the Linux host, Metal/HLSL codegen for the *new* surface is out of scope
here; their `omegasl_compile_tessellation` gate stays `WILL_FAIL` until reworked.

**Data-flow / control-point model (recorded).**

- The `vertex(tess=true)` stage reads control points through the **standard
  OmegaSL vertex-input path** (vertex attributes described by
  `RenderPipelineDescriptor::vertexInputDescriptor`, indexed by `VertexID`),
  and returns the control-point struct — exactly like a normal vertex shader,
  just tagged for tessellation.
- The hull's input becomes a **control-point array parameter** (`CP cp[N]`,
  N = `outputcontrolpoints`) in place of the old `uint vid : VertexID` + `[in]`
  SSBO. `cp[i]` is the i-th post-vertex control point. GLSL maps it to
  `gl_in[i]`; the hull's `[out]` per-CP buffer stays (unused on GLSL, per §16-C).
- `drawPatches(patchCount, controlPointBuffer, startPatch)` binds
  `controlPointBuffer` as **vertex buffer 0** on Vulkan and issues
  `vkCmdDraw(patchCount * N, …)` under `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`.

#### Sub-phases

- **G.1 — front-end (shared).** Parser: `vertex(tess=true)` in the `KW_VERTEX`
  arm (one-token peek/putback, mirroring `fragment(early_depth)`); reject any
  other token inside the parens. AST: `ShaderDecl::tessVertex` (`bool`, default
  `false`). Sema: the hull input model gains the `CP cp[N]` array-parameter
  form; a tessellation pipeline (hull present) requires a `vertex(tess=true)`
  companion. Keep the old hull `uint vid : VertexID` form accepted so Metal's
  current shaders still compile (additive, not a hard cutover) — the mandatory
  rule is enforced only where the runtime needs it, until Metal migrates.
- **G.2 — GLSL codegen.** Emit `.vert` from `vertex(tess=true)` (reads
  attributes by `gl_VertexIndex`, writes `gl_Position` + CP-struct varyings);
  rework the `.tesc` hull input from the SSBO read to `gl_in[i]`; carry the
  control-point struct's non-Position fields as vert→tesc→tese varyings.
  Verify with `shaderc`/`glslc` (`vertex`/`tesscontrol`/`tesseval`).
- **G.3 — Vulkan runtime (16-E3 + 16-E4-Vulkan).** `makeRenderPipelineState`:
  when `hullFunc && domainFunc`, build a 4-stage pipeline (VS = `vertexFunc`,
  tesc = `hullFunc`, tese = `domainFunc`, fragment), set
  `pTessellationState.patchControlPoints`, `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`;
  map `OMEGASL_SHADER_HULL/DOMAIN` → tess-control/eval stage flags in
  `createPipelineLayoutFromShaderDescs` (both switches). `GEVulkanCommandBuffer`:
  override `startTessRenderPass` (a `startRenderPass` variant that permits the
  tess pipeline + patch topology — no compute pre-pass on Vulkan) and
  `drawPatches` (bind CP buffer as vertex buffer, `vkCmdDraw(patchCount*N)` with
  patch topology). Plain `startRenderPass` rejects a tess pipeline (uniform
  contract). `tessellationShader` is already enabled at device creation
  (features2 copies all core feats) — no device change needed.
- **G.4 — feature honesty (16-F, Vulkan slice).** Once G.3 runs, Vulkan may keep
  advertising `GTEDEVICE_FEATURE_TESSELLATION_SHADER`; before that it should not
  claim it. (D3D12 stays gated off until its Phase-E lands.)
- **G.5 — test.** A `vertex(tess=true)` tessellation shader + a Vulkan-runnable
  draw test (one triangle patch → offscreen RGBA8 → center pixel green), built
  and run against the warm `./build`.

### Phase H — D3D12 / HLSL tessellation runtime (`vertex(tess=true)`) [Alex, 2026-07-06]

The D3D12 analog of Phase G (Vulkan): make a tessellated draw run on D3D12 on
the mandatory vertex-stage model. Task 16-E2 was the original placeholder, but
Phase G superseded the SSBO-only hull input, so D3D12 lands the *same*
`vertex → hull → domain → fragment` dataflow the Vulkan runtime uses. Two pieces
were open: HLSL codegen for the Phase-G surface (only GLSL had it — §16-G2), and
the D3D12 runtime itself. Unlike the Vulkan work, this runs and is verifiable on
the native-Windows agent (the `build/` clang-cl DirectX config); `dxc` validates
codegen directly.

Scope note — two HLSL tessellation surfaces coexist. The pre-Phase-G SSBO model
(`tessellation.omegasl` **compile** test: hull reads an `[in]` buffer injected as
`InputPatch`) was already handled by §16-B and stays `dxc`-passing. Phase H is
purely **additive**: it teaches HLSL the Phase-G **vertex-stage** surface
(`vertex(tess=true)` + `CP cp[N]` array param) used by the
`tessellation_vtx_draw_test.cpp` **runtime** test, without disturbing the
compile-test path.

#### Sub-phases

- **H.1 — HLSL codegen for the Phase-G surface** (`HLSLTarget.cpp`). A hull/domain
  control-point-array parameter (`CP cp[N]`, element = an internal struct) is
  emitted as `InputPatch<CP,N>` (hull) / `const OutputPatch<CP,N>` (domain) in
  place of the plain array param; the body's `cp[i].field` then indexes it
  directly — **no member-expr rewrite** is needed (HLSL indexes `InputPatch`
  natively, unlike GLSL's `gl_in[]`). The element struct is added to the
  positional-semantic re-emit set (`cpStructNames`) so patch I/O carries HLSL
  semantics; a `Position` field rides `SV_Position` consistently across
  VS-out / HS-in / HS-out / DS-in. `vertex(tess=true)` needs no special HLSL
  handling — it is a normal VS reading its control points by `SV_VertexID`.
  `fillTessellationDesc` is now called from the hull/domain arms (mirroring MSL)
  so the serialized `omegasl_tessellation_desc` is accurate for the runtime. The
  pre-Phase-G `[in]`-buffer `InputPatch` injection is left intact.
- **H.2 — D3D12 pipeline** (`GED3D12.cpp`, `GED3D12Pipeline.h`).
  `makeRenderPipelineState`: `isTess = hullFunc && domainFunc`, feature-gate on
  `GTEDEVICE_FEATURE_TESSELLATION_SHADER`, build the root signature over the four
  stages {VS,HS,DS,PS} (all root params are `SHADER_VISIBILITY_ALL` and no stage
  access is denied, so no per-stage split), set `d.HS`/`d.DS` on the classic
  `D3D12_GRAPHICS_PIPELINE_STATE_DESC` (no stream build needed, unlike mesh),
  `PrimitiveTopologyType = …_PATCH`, and stamp `isTess`/`patchControlPoints`
  (from the serialized descriptor, falling back to the API field / 3) on
  `GED3D12RenderPipelineState`.
- **H.3 — D3D12 command buffer** (`GED3D12CommandQueue.cpp/.h`).
  `startTessRenderPass` (= `startRenderPass` + `tessPassActive`), `drawPatches`
  (bind CP buffer at VS slot 0, `IASetPrimitiveTopology(…_N_CONTROL_POINT_PATCHLIST)`
  from the contiguous enum, `DrawInstanced(patchCount*N, …)`),
  `setRenderPipelineState` rejects a tess pipeline outside a tess pass, and
  `finishRenderPass` clears the flag — the same contract as the Vulkan backend.
- **H.4 — feature honesty (D3D12 slice of 16-F).** D3D12 already advertised
  `TESSELLATION_SHADER` (`GED3D12.cpp:179`) — dishonestly, since the runtime
  would crash. With H.2/H.3 landing, the advertisement becomes honest, so it
  stays as-is (no gate needed). This closes the load-then-crash the honesty bug
  described.
- **H.5 — test.** Register the backend-independent `tessellation_vtx_draw_test.cpp`
  under `gte/tests/directx/CMakeLists.txt` (`add_d3d12_cli_test`); one triangle
  patch → offscreen RGBA8 → center pixel green. Built + run on the native-Windows
  DirectX config.

### Task table

| # | Task | Where | Effort | Blocks | Status |
|---|------|-------|--------|--------|--------|
| 16-A | `patchfn=` descriptor + `TessFactor`/`InsideTessFactor`/`DomainLocation` semantics + Sema | `Parser.cpp` (KW_HULL), `Sema.cpp` attribute tables, `AST.h`, `AST.def`, `Sema.h` | medium | B, C, D | **DONE** |
| 16-B | HLSL patch-constant fn + `[patchconstantfunc]` + patch types | `HLSLTarget.cpp` | medium | E2 | **DONE** — `dxc`-verified (`hs_6_0`/`ds_6_0`); `omegasl_compile_tessellation` expected-pass on `TARGET_DIRECTX`. No return-rewrite (hull returns per-CP output directly). |
| 16-C | GLSL tess-level writes + `gl_TessCoord` read (+ patch-constant struct as plain data) | `GLSLTarget.cpp` | medium | E3 | **DONE** — `glslc`-verified (`tesscontrol`/`tesseval`); `omegasl_compile_tessellation` now expected-pass on all three backends (WILL_FAIL gate removed). |
| 16-D | Metal kernel (per-CP store + factor epilogue) + post-tess vertex (`patch_control_point`/`position_in_patch`) | `MSLTarget.cpp`, `Target.h` | large | E4 | **DONE (codegen, interim per-CP)** — `metal`-verified; `omegasl_compile_tessellation` flipped to expected-pass on `APPLE`. Dispatch shape to be revised to **per-patch** in Phase E (16-E4a) per Alex — the per-CP kernel is the interim cut. |
| 16-E4a | **Metal per-patch dispatch**: serialize `omegasl_tessellation_desc` into `omegasl_shader`; re-shape hull kernel to thread=patch id (CP loop + unconditional factor write); dispatch `threadsPerGrid = patchCount` | `MSLTarget.cpp` (codegen), shader-map writer, `GEMetal.mm` (runtime) | medium | — | **CODEGEN + DISPATCH VERIFIED** 2026-07-05 — hull kernel re-shaped to per-patch (`__osl_patch_id [[thread_position_in_grid]]` → `for(__osl_cp<N)` binding `vid = patchId*N+cp` → unconditional factor write to `__osl_tess_factors[__osl_patch_id]`). Also gave the hull a valid `threadgroupDesc` (32×1×1, serialized in `ShaderArchive.cpp`) so it builds as a `MTLComputePipelineState` and dispatches. **RUNTIME-VERIFIED on real Metal**: `TessellationDispatchTest` dispatches triHull+quadHull as compute kernels (one thread per patch), the synthesized factor buffer binds at the fall-through slot (id 2 → Metal buffer 2), and the readback confirms per-CP output echo + factor half-bits (1.0=0x3C00 tri / 2.0=0x4000 quad). The auto-`drawPatches` render command (16-E4, compute-then-render encoder + tess render pipeline) is still open. |
| 16-E1 | Public API: descriptor fields, patch topology, `drawPatches`, `omegasl_tessellation_desc` | `GEPipeline.h`, `GERenderTarget.h`, `omegasl.h` | medium | E2–E4 | **PARTIAL** 2026-07-05 — DONE: `omegasl_tessellation_desc` (struct in `omegasl.h`; serialized in `ShaderArchive.cpp` read+write for HULL/DOMAIN; populated by `fillTessellationDesc` in `Target.h`, called from `MSLTarget.cpp` hull+domain arms); `RenderPipelineDescriptor::hullFunc/domainFunc/patchControlPoints`; `PrimitiveTopologyCategory::Patch`. TODO: `GECommandBuffer::PolygonType::Patch` + `drawPatches` command (blocked on the E4 draw-model plumbing below). |
| 16-E2 | D3D12 HS/DS pipeline + patch topology | `GED3D12.cpp` | medium | — | **DONE (via Phase H, 16-H2/H3)** 2026-07-06 — superseded by the Phase-G vertex-stage model. See 16-H* rows. |
| 16-E3 | Vulkan tess stages + tess state + device-feature enable | `GEVulkan.cpp` | medium | — | **DONE** 2026-07-06 — subsumed by Phase G (16-G3), which requires the `vertex(tess=true)` stage. `tessellationShader` was already enabled at device creation. |
| 16-E4 | Metal `startTessRenderPass` + tessellation pipeline + `drawPatches` | `GEMetal.mm`, `GEMetalCommandQueue.*`, `GEMetalPipeline.h`, `GERenderTarget.h` | large | — | **DONE (Metal)** 2026-07-06 — `startTessRenderPass` (deferred: records the descriptor, opens no encoder) + `drawPatches` (opens a compute encoder for the hull dispatch → engine-owned Private hullOut + factor buffers, ends it, then opens the render encoder via the stored descriptor, sets the half tess-factor buffer + `hullOut` as the per-patch-control-point stage-in, `[rp drawPatches:...]`). `makeRenderPipelineState` builds a `MTLComputePipelineState` from the hull + a render pipeline from domain/fragment with `MTLRenderPipelineDescriptor` tess state (partition/winding/half-factor + `MTLVertexStepFunctionPerPatchControlPoint` stage-in). Plain `startRenderPass` rejects tessellation pipelines. **RUNTIME-VERIFIED on real Metal** under the API + GPU validation layer: `TessellationDrawTest` draws one triangle patch to a texture → center pixel green (0,255,0,255). Metal now advertises `GTEDEVICE_FEATURE_TESSELLATION_SHADER` (Metal's slice of 16-F). |
| 16-F | Feature-flag honesty gate + per-backend re-enable | `GED3D12.cpp`, `GEVulkan.cpp`, `GEMetal.mm`, tests `CMakeLists.txt` | small | — | **DONE** 2026-07-06 — all three backends now honor `TESSELLATION_SHADER`: Metal (16-E4), Vulkan (16-G4), and D3D12 (16-H4 — the runtime lands, so its pre-existing unconditional advertisement is now honest, closing the load-then-crash). |
| 16-G1 | Front-end: `vertex(tess=true)` descriptor + `tessVertex` AST + hull `CP cp[N]` input + Sema (Phase G.1) | `Parser.cpp` (KW_VERTEX), `AST.h`, `Sema.cpp` | medium | G2–G3 | **DONE** 2026-07-06 — `vertex(tess=true)` parses (peek/putback like `fragment(early_depth)`); `ShaderDecl::tessVertex`. Array function params (`cp[N]`) already parsed + indexable in Sema, so the hull/domain CP-array input type-checks with no new Sema rule; hull `out`-buffer requirement relaxed "exactly one"→"at most one" (the store buffer is Metal-only; GLSL/HLSL write `gl_out[]`/return). |
| 16-G2 | GLSL codegen: emit `.vert`, tesc reads `gl_in[]`, hull writes `gl_out[]`, tess-level epilogue reachable (Phase G.2) | `GLSLTarget.cpp`, `Target.h` | medium | G3 | **DONE** 2026-07-06 — `glslc`/`shaderc`-verified (vert/tesc/tese/frag). A hull/domain CP-array param (`controlPointArrayParams`) is suppressed as a `layout(location) in` varying and `cp[i].<Pos>` rewrites to `gl_in[i].gl_Position`; `writeInternalFieldRef` routes hull Position → `gl_out[gl_InvocationID].gl_Position`; the hull's internal-struct `return` emits no mid-body `return;` so the factor epilogue stays reachable. **Also fixed the runtime `compileShaderRuntime` shaderc kind: Hull→`tess_control`, Domain→`tess_evaluation` (was defaulting to compute → invalid SPIR-V).** Non-Position CP varyings deferred (tests are Position-only). |
| 16-G3 | Vulkan runtime: 4-stage tess pipeline + tess state + PATCH_LIST + HULL/DOMAIN stage flags; `startTessRenderPass` + `drawPatches` (Phase G.3, = 16-E3 + Vulkan slice of 16-E4) | `GEVulkan.cpp`, `GEVulkanCommandQueue.cpp/.h`, `GEVulkanPipeline.h` | large | G5 | **DONE** 2026-07-06 — **RUNTIME-VERIFIED under VVL.** `makeRenderPipelineState` builds vertex+tesc+tese+fragment with `pTessellationState.patchControlPoints` (from the serialized `omegasl_tessellation_desc`) + static `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`; HULL/DOMAIN → tess-control/eval stage flags in both `createPipelineLayoutFromShaderDescs` switches; `isTess`/`patchControlPoints` on the PSO. `startTessRenderPass` (thin `startRenderPass` variant + `tessPassActive`), `drawPatches` (binds control points as the VS SSBO at slot 0, `vkCmdDraw(patchCount*N)`), plain `startRenderPass`/`setRenderPipelineState` reject a tess pipeline, `finishRenderPass` clears the flag. `tessellationShader` is enabled implicitly at device creation (features2 copies all core feats) — no device change. |
| 16-G4 | Feature honesty: Vulkan advertises `TESSELLATION_SHADER` only once G3 runs (Vulkan slice of 16-F) | `GEVulkan.cpp` | small | — | **DONE** (no-op) 2026-07-06 — Vulkan already advertised it when the GPU reports `tessellationShader`; with G3 the runtime now honors it, so the advertisement is honest and no change is needed. D3D12 stays dishonest until its Phase-E lands (still open). |
| 16-G5 | `vertex(tess=true)` tess shader + Vulkan-runnable tess draw test | `gte/tests/tessellation_vtx_draw_test.cpp`, `gte/tests/vulkan/CMakeLists.txt` | medium | — | **DONE** 2026-07-06 — `omegagte_tessellation_vtx_draw` (backend-independent source, registered under Vulkan) draws one triangle patch to a 64² RGBA8 texture via `startTessRenderPass`/`drawPatches`; center pixel reads back green (0,255,0,255) under the Vulkan validation layer with zero VVL errors. Skips cleanly if the device lacks tessellation. Full omegasl suite (136/136) + Vulkan runtime suite (9/9) green. Metal/D3D12 can register the same source once their vertex-stage runtime lands. |
| 16-H1 | HLSL codegen for the Phase-G surface: `CP cp[N]` → `InputPatch`/`OutputPatch`, CP-struct positional re-emit, `fillTessellationDesc` on hull/domain, runtime `compileShaderRuntime` tess profiles, FXC domain patch-constant input | `HLSLTarget.cpp` | medium | H2 | **DONE — RUNTIME-VERIFIED on real D3D12** 2026-07-06 — additive over §16-B (pre-G `[in]`-buffer path preserved). HLSL indexes `InputPatch` natively (no member-expr rewrite, unlike GLSL). Bring-up fixes: (a) `compileShaderRuntime` (FXC/`D3DCompile`) had no Hull/Domain target profile → empty target → `X3506`; added `hs_5_1`/`ds_5_1`. (b) FXC (unlike dxc) requires the DS to consume the patch-constant struct → `X3502: ds_5_1 tessfactor inputs missing`; the domain now injects the patch-constant struct (matched to the domain by edge-factor count) as its first param. |
| 16-H2 | D3D12 tess pipeline: 4-stage root sig, `.HS`/`.DS`, `_PATCH` topology, `isTess`/`patchControlPoints` on PSO, feature-gate | `GED3D12.cpp`, `GED3D12Pipeline.h` | medium | — | **DONE — RUNTIME-VERIFIED** 2026-07-06 — classic `D3D12_GRAPHICS_PIPELINE_STATE_DESC` HS/DS (no stream build); root sig unions {VS,HS,DS,PS} at `VISIBILITY_ALL`. |
| 16-H3 | D3D12 command buffer: `startTessRenderPass` + `drawPatches` (N-CP patch-list IA + `DrawInstanced(patchCount*N)`) + reject-in-plain-pass | `GED3D12CommandQueue.cpp/.h` | medium | — | **DONE — RUNTIME-VERIFIED** 2026-07-06 — same contract as the Vulkan backend; CP buffer bound at VS slot 0 SRV. |
| 16-H4 | Feature honesty (D3D12 slice of 16-F): advertisement now backed by the runtime | `GED3D12.cpp` | small | — | **DONE (no-op)** 2026-07-06 — D3D12 already advertised `TESSELLATION_SHADER`; with H2/H3 it is honest. |
| 16-H5 | Register `tessellation_vtx_draw_test.cpp` under D3D12 | `gte/tests/directx/CMakeLists.txt`, `gte/tests/tessellation_vtx_draw_test.cpp` | small | — | **DONE — PASSES on real D3D12** 2026-07-06 — `add_d3d12_cli_test`; one triangle patch → offscreen RGBA8 → **center pixel (0,255,0,255)**. The test deliberately holds every GPU resource handle across `Close(gte)` (destructs at scope exit, after the engine) so it doubles as the regression test for 16-H6. Separately fixed a pre-existing Windows-portability break in `te_index_buffer_test.cpp` (its `using Polygon =` alias clashed with `<wingdi.h>`'s `Polygon`). |
| 16-H6 | D3D12 teardown-order hardening: resources may outlive the engine | `GED3D12.h`, `GED3D12.cpp`, `GED3D12Texture.h/.cpp` | small | — | **DONE** 2026-07-06 — the first headless D3D12 render-to-texture test surfaced that a `GETexture`/`GESamplerState` outliving the engine (handle dropped after `Close()` ran `~GED3D12Engine`) faults derefing its raw `owningEngine` back-pointer in the descriptor-slot-free path. Hardened as a sibling to `GED3D12AllocatorOwner` (which already covered the D3D12MA path): a `GED3D12EngineBackRef` interface + an engine-side registry; `~GED3D12Engine` nulls every still-live resource's back-pointer (`onEngineDestroyed`), so its destructor skips the (moot — GPU idle, heaps releasing) slot-free. Buffers already tolerate it via `GED3D12AllocatorOwner`; render targets delegate to their texture. |

### Tests

- Per-backend golden / compile tests as each codegen phase lands; flip
  `omegasl_compile_tessellation` from `WILL_FAIL` to expected-pass per
  backend (§16-F).
- **`gte/tests/tessellation_dispatch_test.cpp`** (`omegagte_tessellation_dispatch`,
  registered under `gte/tests/metal/CMakeLists.txt`) — Metal runtime hull-dispatch
  test, DONE + passing 2026-07-05. Runtime-compiles a tri+quad hull/domain shader,
  dispatches each hull as a compute kernel over one patch, and reads back the
  per-control-point output (echo check) + the tessellation-factor buffer (half
  bits 0x3C00 / 0x4000). Metal-only: only Metal models a hull as a compute
  dispatch with an intermediate factor buffer; on D3D12/Vulkan the hull runs
  inside the draw pipeline (verified via `drawPatches` when 16-E2/E3 land).
- **`gte/tests/tessellation_draw_test.cpp`** (`omegagte_tessellation_draw`,
  Metal) — DONE + passing 2026-07-06. Full `startTessRenderPass` + `drawPatches`
  path: a hull/domain/fragment pipeline draws one triangle patch to an offscreen
  RGBA8 texture; the center pixel is read back and asserted green (the shaded
  patch) vs the black clear. Passes under the Metal API + GPU validation layer.
  D3D12/Vulkan tessellated-draw tests land with their Phase-E runtime.

### Out of scope (follow-ups)

- **Multiple hulls sharing one patch-constant function** — `patchfn=` is
  1:1 in this cut (the Option 3 revisit).
- **Per-patch user data beyond tess factors** (`patch out` user varyings /
  a Metal patch-data buffer). The design leaves room — extra non-factor
  fields on the patch-constant struct — but the first cut wires only the
  factors.
- **Isoline domains** and quad `gl_TessCoord.z` handling beyond tri/quad.
- **Tessellation × geometry stage (§9)** and tessellation × instancing.

#### Cross-backend visual parity (opened 2026-07-06)

The domain math + factor *values* are identical across MSL/HLSL/GLSL (same
OmegaSL source). Divergence can only come from how each API's fixed-function
tessellator interprets factors + parameterizes the domain. With **uniform**
factors and a position-only domain (both current tests), all three agree. With
**non-uniform per-edge factors** they may not, and this is unverified — the
D3D12/Vulkan runtime is still pass-2, so no empirical three-way comparison
exists yet. Two follow-ups:

- **Cross-backend tessellation conformance test.** Once the D3D12/Vulkan
  runtime lands (16-E2/E3), add a test that draws the SAME patch with
  **non-uniform** per-edge factors (e.g. `edges = {8,1,1}`) and compares the
  tessellated vertex output (or a pixel readback) across backends. This is what
  actually proves/disproves parity rather than arguing it from the specs.
- **Normalize tessellator conventions in codegen** if that test diverges. Two
  known mismatch sources, both because OmegaSL currently passes the shader's
  `edges[i]` / `DomainLocation` straight through without remapping:
  1. **Per-edge factor → physical-edge mapping.** Metal + HLSL share the D3D11
     tessellator's edge numbering; Vulkan/GLSL uses the OpenGL convention, which
     differs. `edges[0]` can refine a *different* physical edge on GLSL. Fix:
     remap the edge index when emitting `gl_TessLevelOuter[]` in `GLSLTarget.cpp`
     so `edges[i]` means the same edge on all three.
  2. **Domain-coordinate orientation.** The quad `[0,1]²` domain UV can be
     **v-flipped** between the GL lineage (Vulkan `gl_TessCoord`) and the D3D
     lineage (Metal `[[position_in_patch]]` / HLSL `SV_DomainLocation`). Fix:
     flip `gl_TessCoord` (`.y`, and the tri barycentric assignment) in the
     `.tese` so `DomainLocation` is backend-independent.
  The right layer is codegen — keep the developer-facing OmegaSL semantics
  backend-independent rather than pushing the difference onto shader authors.

- **Vulkan front-face / cull-mode normalization (general render parity, NOT
  tessellation-specific) — IMPLEMENTED 2026-07-06 (write-only; pending a Vulkan
  build).** CONFIRMED mechanism (Alex): the Vulkan backend sets a
  **negative-height viewport** in `GEVulkanCommandBuffer::setViewports`
  (`GEVulkanCommandQueue.cpp:1433`) to flip NDC-Y so `y=+1` maps to the
  framebuffer top, matching Metal/D3D12 — NDC coords themselves get no special
  treatment (universal). That negative-height flip **reverses the apparent
  triangle winding** the rasterizer sees, so applying `polygonFrontFaceRotation`
  straight through classifies the *wrong* faces as front on Vulkan vs
  Metal/D3D12. **Fix applied:** the Vulkan pipeline rasterization state now uses
  the **OPPOSITE `VkFrontFace`** (`GEVulkan.cpp` ~3429 graphics + tessellation,
  ~4031 mesh — the blit pipeline routes through the graphics path). Chosen over a
  literal cull-mode swap because the frontFace flip *normalizes front-facing*
  (the stated goal): it culls the same physical faces AND keeps `gl_FrontFacing`
  / two-sided stencil consistent, whereas swapping only the cull mode fixes
  culling but leaves those inverted. **Latent today** — every pipeline uses
  `RasterCullMode::None` (the `setViewports` comment notes the winding reversal
  is "harmless" precisely because of that) — so behavior is unchanged until
  culling is enabled, at which point it is correct-by-construction. Not
  compile-verifiable on the macOS host; build + confirm on a Vulkan platform,
  ideally with the first culled-draw test (e.g. a tessellated closed mesh).

#### Metal `startTessRenderPass` / `drawPatches` first-cut constraints (2026-07-06)

- **Caller-supplied control-point layout.** The Metal tess pipeline requires
  `RenderPipelineDescriptor::vertexInputDescriptor` to describe the
  control-point struct (Metal's `patch_control_point` stage-in needs the field
  layout, which is not in the shader's serialized reflection). A
  reflection-derived default would remove that caller burden.
- **Domain/fragment resource binding in the deferred pass.** `drawPatches`
  currently binds only the input control-point buffer + built-ins; domain /
  fragment resources (textures, uniforms) beyond the stage-in are not yet
  routed through the deferred render encoder. Fine for a constant-shaded patch;
  a real material needs the bind path wired into `startTessRenderPass`.
