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
| `getDimensions(t)` | query mip level dimensions | Phase B — pending |
| `calculateLOD(s, t, c)` | query LOD chosen by hardware | Phase B — pending |

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

**Phase B — query intrinsics (planned)**

`getDimensions` and `calculateLOD` are query-style intrinsics that pose
distinct challenges from the composable sampling variants in Phase A.
Both are deferred until the underlying issues are resolved:

* `getDimensions(t)` / `getDimensions(t, lod)` — query mip-level dimensions.
  The three backends have radically different surface shapes:

  | Backend | Surface |
  |---------|---------|
  | HLSL | `tex.GetDimensions(out width, out height [, out levels])` — out-params, no return value |
  | MSL | `tex.get_width(lod)`, `tex.get_height(lod)`, `tex.get_depth(lod)`, `tex.get_num_mip_levels()` — separate accessors |
  | GLSL | `textureSize(samplerND(t, s), int(lod))` — returns `int`/`ivec2`/`ivec3` |

  Two design problems beyond the simple "pick a spelling" pattern Phase A
  used:

  1. **Return-shape inference.** The result type depends on the texture's
     spatial rank: `uint` for 1D, `uint2` for 1D-array / 2D / cube,
     `uint3` for 2D-array / 3D / cube-array. None of the existing builtins
     return shape-dependent types — Sema's `func_found->returnType`
     mechanism returns a fixed `TypeExpr`. A new dispatch path is needed,
     either by per-call return-type synthesis (similar to how `transpose`
     handles its NxM → MxN return on lines 719–731 of `Sema.cpp`) or by
     introducing per-shape fan-out builtins (`getDimensions1D`,
     `getDimensions2D`, ...).

  2. **Out-param synthesis on HLSL.** `tex.GetDimensions(out w, out h)`
     can't be expressed inline as a sub-expression. We'd need to emit a
     temporary statement before the use site and rewrite the source-level
     call to read from the temporary, e.g.

     ```hlsl
     uint _gd_w, _gd_h;
     tex.GetDimensions(_gd_w, _gd_h);
     uint2 dims = uint2(_gd_w, _gd_h);
     ```

     CodeGen currently has no statement-injection hook — every builtin
     emitter writes inline into the expression stream. This is the
     blocking architectural change for Phase B.

  Recommended approach when Phase B is picked up:

  * Add a `Target::synthesizeStatementBefore(...)` hook that backends
    can use to inject lines before the current statement. Default no-op
    on MSL/GLSL (they don't need it). HLSL uses it to emit the
    `GetDimensions` temporary and rewrite the call site to a swizzle
    constructor.
  * Add per-shape return-type resolution for these query builtins —
    specialize on the resolved texture type at the per-call validation
    site, the same way `transpose` does.
  * Decide whether `lod` is required or optional. Recommendation: make
    it required for now (`getDimensions(tex, 0)`) — the optional form
    requires HLSL out-param overload selection that adds another moving
    part for marginal benefit. Mip 0 is the default everywhere if a user
    passes `0`.

* `calculateLOD(s, t, c)` — query the LOD the hardware would choose.
  Less invasive than `getDimensions`, but introduces the first
  "spec-says-scalar / GLSL-returns-vec2" wart in the codebase:

  | Backend | Form | Return |
  |---------|------|--------|
  | HLSL | `tex.CalculateLevelOfDetail(s, c)` | `float` |
  | MSL | `tex.calculate_clamped_lod(s, c)` (or `_unclamped_lod`) | `float` |
  | GLSL | `textureQueryLod(samplerND(t, s), c)` | `vec2` (clamped, unclamped) |

  Source-level signature: `calculateLOD(s, t, c) → float`. GLSL emits
  `textureQueryLod(...).x` to discard the unclamped component. Document
  the semantics as "implementation may return clamped or unclamped LOD;
  treat as advisory" — every backend has a different trade-off here and
  we shouldn't force a runtime branch to normalize them.

  Stage restriction: `calculateLOD` needs derivatives on every backend,
  so it's fragment-only in practice. `sample` already has the same
  implicit constraint and isn't enforced today; Phase B can either
  follow that precedent or land stage-checking as a uniform rule across
  `sample`, `sampleBias`, `gather`, and `calculateLOD` together.

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
don't support it, emit nothing. Rare to need the others.

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

Out of scope (intentional, deferred):
  - **`const` on function parameters.** Overlaps with §3.7's `in` /
    `out` / `inout` work and is cleaner to land alongside that pass.
  - **Postfix `T const x` form.** C-family backends accept it; OmegaSL
    parser only recognizes the prefix form today. Add when there's
    real demand.

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
  - **`const T` parameters.** §3.6 noted this as 3.7-adjacent. Still
    unimplemented; the parser does not yet accept `const` in front
    of a parameter type. Add when a shader actually wants it — every
    backend already accepts the spelling, so the patch is small.

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

### 4.3 `double` — 

We can do this but we must feature gate it with our proposed feature gating.

---

## 5. Intrinsics — common functions currently missing (P0 → P1)

Grouped by how often engines reach for them.

### 5.1 Core math not yet listed [PARTIALLY COMPLETED]

| Function | Status |
|----------|--------|
| `sign(x)` | **landed** — passthrough on every backend |
| `saturate(x)` | **landed** — passthrough HLSL/MSL; GLSL rewrites to `clamp(x, 0.0, 1.0)`. See §5.1.1 |
| `fma(a,b,c)` | **landed** — passthrough MSL/GLSL; HLSL lowers to `mad` (precision contract looser; see notes below) |
| `mad(a,b,c)` | not added — `fma` is the canonical name in this spec; pick one |
| `fmod(a,b)` | **landed** — truncation semantics, matching C/HLSL/MSL. GLSL rewrites to `(x - y*trunc(x/y))` because GLSL's `mod` has different (floor-based) semantics |
| `mod(a,b)` | not added — would alias `fmod` here, picking one canonical spelling |
| `trunc(x)` | **landed** — passthrough on every backend |
| `rsqrt(x)` | **landed** — passthrough HLSL/MSL; GLSL renames to `inversesqrt` |
| `degrees(r)` / `radians(d)` | **landed** — passthrough HLSL/GLSL; MSL inlines as a multiplication by the matching π constant (Metal stdlib has no `degrees`/`radians`). See §5.1.2 |
| `sinh` / `cosh` / `tanh` | **landed** — passthrough on every backend |
| `ldexp(x, e)` | **landed** — passthrough on every backend |
| `modf(x, out integerPart)` | not added — out-param synthesis blocker (see §2.3 Phase B for the same issue with `getDimensions`) |
| `frexp(x, out exp)` | not added — same out-param blocker |

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
  affected backend's `renameBuiltin`.
* **`modf` / `frexp`**. Both have an out-parameter for the integer part /
  exponent. HLSL exposes them with native out-params (`modf(in, out)`),
  MSL has separate accessors, GLSL has separate forms. Inlining the
  emission requires the same statement-injection hook proposed for
  §2.3 Phase B's `getDimensions`. Land that hook once and these
  builtins become a few lines each.
* **Sema reservation of intrinsic names** (rejecting user `func sin(...)`,
  `func saturate(...)`, etc.). Currently a user definition shadows the
  builtin. The §5.1.1 doc proposes this; doing it together with §5.1
  is a backwards-compat hazard for any existing shader that defines its
  own `saturate` workaround. Land as a separate, clearly-flagged change.
* **HLSL `osl_user_` user-function prefix**. MSL already prefixes user
  funcs to avoid stdlib collisions; HLSL has the same exposure but
  hasn't adopted the prefix yet. Same separate-PR rationale: it's a
  source-shape change that the existing HLSL goldens would need to
  re-baseline. (This should apply to all backends.)

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

- **`int` / `uint` matrix types** in the buffer API. The shader language
  only declares `float` matrices today (§2.3 of the reference). Adding
  the integer matrix path is straightforward when needed.
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
tessellation re-enable) so each one has a clean way to declare its backend
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
| Tessellation hull/domain | yes | **emulation-only**; runtime not wired | yes |
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
