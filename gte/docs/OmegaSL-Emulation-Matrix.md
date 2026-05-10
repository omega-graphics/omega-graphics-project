# OmegaSL Cross-Backend Emulation Matrix

Companion to `OmegaSL-Reference.md` and `OmegaSL-Feature-Gap-Survey.md`. This
catalogues features where the OmegaSL surface is *uniform* across backends but
at least one target lacks a native intrinsic / type / construct, and the
compiler bridges the gap by lowering to a backend-specific equivalent.

Scope rule: a row only appears here if the feature **can** be expressed
identically (semantically, not lexically) across all three backends with a
mechanical rewrite. Features that are structurally different on one backend
(geometry shaders on Metal, subpass inputs off Vulkan, tessellation on Metal)
belong to the feature-gating system in §14 of the gap survey, not here.

## Status legend

| Symbol | Meaning |
|--------|---------|
| ✓ landed | Emulation is in CodeGen today; verified by tests |
| ◔ partial | Lowering exists for some shapes; remaining shapes deferred |
| ☐ planned | Lowering is well-defined; not yet implemented |
| ⊘ skip   | Listed for completeness; intentionally not emulated (see notes) |

`Native` columns: ✓ = first-class intrinsic / keyword; — = absent; ≈ = present
but with a different shape (out-params, return-type asymmetry, etc.) that
makes raw passthrough unsafe.

---

## 1. Math intrinsics

| OmegaSL surface | HLSL | MSL | GLSL | Emulation site / lowering | Status |
|---|---|---|---|---|---|
| `saturate(x)` | ✓ | ✓ | — | GLSL: `clamp(x, 0.0, 1.0)` (broadcasted for `vecN`) | ✓ landed (§5.1.1) |
| `fmod(a, b)` (truncation) | ✓ | ✓ (`fmod`) | ≈ (`mod` is floor-based) | GLSL: `(x - y * trunc(x / y))` inline | ✓ landed (§5.1) |
| `rsqrt(x)` | ✓ | ✓ | ≈ (`inversesqrt`) | GLSL: rename `rsqrt` → `inversesqrt` | ✓ landed (§5.1) |
| `degrees(r)` / `radians(d)` | ✓ | — | ✓ | MSL: `(x) * 57.2957…` / `(x) * 0.01745…` (scalar broadcast handles vectors) | ✓ landed (§5.1.2) |
| `fma(a, b, c)` | ≈ (double-only on SM 5; `mad` is fp32 path) | ✓ | ✓ | HLSL: rename `fma` → `mad`. Precision contract loosens on HLSL — document at the surface | ✓ landed (§5.1) |
| `inverse(m)` | — | ✓ | ✓ | HLSL: emit per-size cofactor expansion (`float2x2`/`float3x3`/`float4x4`); reject `Cx2`/`Cx3` non-square (no inverse exists for those anyway) | ☐ planned (§5.2) |
| `any(v)` / `all(v)` | ✓ | ✓ | ✓ | None — passthrough on every backend | ☐ planned (just register in builtin map) |
| `f16tof32(u)` / `f32tof16(f)` | ✓ | ≈ (`as_type<half2>(uint)`) | ≈ (`unpackHalf2x16` / `packHalf2x16`) | MSL: `as_type<half2>(u)`; GLSL: `unpackHalf2x16` (returns `vec2`, take `.x` for scalar form). Document that the scalar form discards the upper half | ☐ planned (§5.5) |
| `pack_snorm4x8(v)` / `unpack_snorm4x8(u)` | ≈ (manual bit ops) | ✓ | ✓ | HLSL: emit a 4-line bit-pack helper (`f * 127.0` clamped + bitwise OR). Inline at every call site is acceptable; downstream FXC/DXC fold it | ☐ planned (§5.5) |
| `asint(f)` / `asuint(f)` / `asfloat(u)` | ✓ | ≈ (`as_type<>`) | ≈ (`floatBitsToInt` family) | MSL: `as_type<int>(x)` etc.; GLSL: matching `*BitsTo*`. Generic enough to cover via `Target::renameBuiltin` when the dest type is fixed — return-type-driven, not arg-driven | ☐ planned (§5.5) |
| `modf(x, out i)` / `frexp(x, out e)` | ✓ (native out-param) | ≈ (separate accessors) | ≈ (separate forms) | All three: blocked on the statement-injection hook (also blocking `getDimensions` Phase B). Land the hook once, all three become a few lines | ☐ planned — blocked (§5.1.0) |
| `clip(x)` | ✓ | — | — | MSL/GLSL: `if (any(x < 0)) discard_fragment();` / `if (any(x < 0)) discard;`. Sugar over `discard`; keep for HLSL ergonomics | ☐ planned (§1.1) |
| `select(cond, a, b)` (vector ternary) | ✓ (vector cond OK) | ≈ (no native vector ternary) | ≈ (`mix(a, b, bvec)`) | MSL: emit `select(b, a, cond)` (Metal stdlib has it, arg order is flipped vs HLSL); GLSL: `mix(a, b, bvec)`. Sema needs to enforce same-rank `bvec` cond | ☐ planned (§3.2 follow-up) |

### Notes

- The `tryEmitBuiltinCall` hook used for `saturate` / `fmod` / `degrees` /
  `radians` is the right vehicle for everything in this section that needs
  more than a name swap. New entries should follow the `saturate` shape
  (`Target` overrides, no statement-level rewriting needed).
- The cluster blocked on statement injection (`modf`, `frexp`, `getDimensions`)
  is best landed in one pass — the hook design is the same for all three.

---

## 2. Texture / sampling

| OmegaSL surface | HLSL | MSL | GLSL | Emulation site / lowering | Status |
|---|---|---|---|---|---|
| `read(tex, coord)` coord cast | ≈ (`Load(intN+1)` signed) | ≈ (read takes `uintN` only) | ≈ (`texelFetch` signed `ivecN`) | MSL: cast coord to matching `uintN` per-dim. HLSL/GLSL: cast to signed `intN`/`ivecN` per-dim | ✓ landed for MSL (bug 2); HLSL/GLSL ◔ partial (bug 4 / bug 5 in reference) |
| `write(tex, coord, val)` coord cast | ≈ (`uintN` for RW operator-`[]`) | ≈ (`uintN` only) | ≈ (`imageStore` signed `ivecN`) | Same as read; current GLSL hardcodes `ivec2` regardless of dim — broken for 1D/3D | ◔ partial (bug 5 — real bug, no test exercises it) |
| `sampleGrad` on `texture1d`/`texture1d_array` | ✓ | — (no `gradient1d`) | ✓ | MSL: feature-gated via `OMEGASL_FEATURE_BIT_TEXTURE1D_MIP_SAMPLE` — emit header-only stub. **Cannot be emulated** (no mip pyramid on Apple 1D textures); listed as ⊘ skip rather than ☐ planned | ⊘ skip (§2.3 Phase A.1) |
| `getDimensions(tex, lod)` | ≈ (out-params) | ≈ (separate accessors) | ✓ (`textureSize`) | HLSL: blocked on statement-injection hook (synthesize temp `uint w, h; tex.GetDimensions(w, h); uint2 dims = uint2(w, h);`). MSL: combine `tex.get_width(lod)`/`tex.get_height(lod)` into a vector constructor. GLSL: passthrough | ☐ planned — blocked (§2.3 Phase B) |
| `calculateLOD(s, t, c)` → `float` | ✓ | ✓ (`calculate_clamped_lod`) | ≈ (returns `vec2`) | GLSL: `textureQueryLod(...).x` to drop the unclamped channel | ☐ planned (§2.3 Phase B) |
| `sampleCmp(s, t, c, ref)` | ✓ | ✓ (`sample_compare`) | ≈ (coord and ref packed into one vec) | GLSL: `texture(samplerNDShadow, vecN+1(c, ref))` — pack ref as the trailing component | ☐ planned (§2.2) |
| Cube `read` / `write` | ≈ (TextureCubeArray indexing only) | ≈ (write only, limited) | — (no `imageCube`) | Not portably emulable. Stay as `OMEGASL_FEATURE_BIT_TEXTURECUBE_RW` (gated) | ⊘ skip (§2.1 deferred) |
| `texture2d_ms` `write` | — | — | — | Not supported anywhere. Gated as `TEXTURE2D_MS_WRITE` | ⊘ skip (§2.1 deferred) |

---

## 3. Matrix ops

| OmegaSL surface | HLSL | MSL | GLSL | Emulation site / lowering | Status |
|---|---|---|---|---|---|
| `m[col][row]` two-level read | row-first natively | column-first | column-first | HLSL: swap subscripts at codegen — emit `m[row][col]` so source-level `m[col][row]` reads the same element on every backend | ✓ landed (§12.1) |
| `m[col]` single-level read | row-first natively | column-first | column-first | HLSL: synthesize column with `floatN(m[0][a], m[1][a], …)` constructor | ✓ landed (§12.1) |
| `m[col] = vec` single-level write | not portable | not portable | not portable | **Rejected at Sema** — write per-element with `m[col][row] = …`. Statement expansion is feasible (`m[0][a] = v.x; m[1][a] = v.y; …`) once a "lower expression to statement sequence" pass exists | ⊘ skip — surface deliberately restricted (§12.1 Out of scope) |
| Column-major storage | ≈ (default flag, implicit) | ✓ (spec) | ✓ (std140/std430 default) | HLSL: pin via `D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR` (runtime) + `/Zpc` (offline) + `column_major` qualifier on struct fields | ✓ landed (§12.2 + §12.3) |
| `Cx3` matrix std430 column padding | (column-major + struct field qualifier) | (column-major spec) | (std430 pads vec3 column to 16B) | All three: shared `std430MatrixStride(R)` helper inside `GEBufferWriter::writeFloat<C>x<R>` inserts 4 bytes of padding per `Cx3` column at upload | ✓ landed (§12.2) |

---

## 4. Numeric types — feature-gated, not emulated

These types are **not** emulated when the backend lacks native support — they
trip the feature gate and produce a header-only stub. Listed here so the
distinction "could be emulated but isn't" is explicit.

| Type | HLSL | MSL | GLSL | Why no emulation |
|---|---|---|---|---|
| `half` / `half2..4`, `short`, `ushort` | ✓ (SM 6.2 + flag) | ✓ | ✓ (ext) | Lowering to `float` would silently double bandwidth and break the entire reason to use 16-bit. Gate via `OMEGASL_FEATURE_BIT_FLOAT16` |
| `long` / `ulong` (64-bit ints) | ✓ (SM 6.0) | ✓ | ✓ (ext) | Lowering via `uint2` carry-add is feasible but lossy at the ABI boundary (atomics, buffer interop). Gate via `OMEGASL_FEATURE_BIT_INT64` |
| `double` | ✓ | — | ✓ | Metal has no double precision at any shape. Gate via `OMEGASL_FEATURE_BIT_DOUBLE`; MSL produces a stub |

---

## 5. User function name collisions

| Concern | HLSL | MSL | GLSL | Emulation site / lowering | Status |
|---|---|---|---|---|---|
| User function shadowing stdlib | global namespace; same-arity overload errors | `metal::saturate` collides with user `saturate` | user wins for non-reserved names | All backends: prefix user-defined function names with `osl_user_` at codegen. MSL needs it; HLSL needs it for parity; GLSL gets it for predictability | ✓ landed on MSL; ☐ planned on HLSL (§5.1.0) |

---

## 6. Loop / branch attributes

| OmegaSL surface | HLSL | MSL | GLSL | Emulation site / lowering | Status |
|---|---|---|---|---|---|
| `[unroll]` / `[unroll(N)]` | ✓ | — (compiler-driven) | ≈ (`[[unroll]]` via SPV ext) | MSL: emit nothing (compiler decides); GLSL: emit the SPIR-V loop-control hint when supported, else nothing | ☐ planned (§3.4) |
| `[loop]` / `[branch]` / `[flatten]` | ✓ | — | — | MSL/GLSL: emit nothing. Document as advisory, not load-bearing | ☐ planned (§3.4) |

---

## 7. Compute / pipeline plumbing

| OmegaSL surface | HLSL | MSL | GLSL | Emulation site / lowering | Status |
|---|---|---|---|---|---|
| Flat local thread index | ≈ (`SV_GroupIndex`) | ✓ (`[[thread_index_in_threadgroup]]`) | ✓ (`gl_LocalInvocationIndex`) | All three are native on their respective semantic — no emulation, just an attribute alias. Not emulation per se; listed because authors will want a uniform name (`LocalThreadIndex`) | ☐ planned (§6.4) |
| `SV_VertexID` base normalization | starts at zero (excludes `firstVertex`) | starts at zero | includes `firstVertex` (`gl_VertexID`) | GLSL: subtract `firstVertex` push-constant at vertex shader entry; or rebrand attribute as `gl_BaseVertex`-relative. Needs runtime cooperation (push the base in) | ☐ planned (§8.5) |
| Push constants | root constants | inline `constant` <4KB | `layout(push_constant)` | OmegaSL surface (`raw<T>` per Alex's note in §2.5, or a dedicated `constant<T>`) lowers to the matching native form on each backend. No runtime emulation needed; mechanical lowering | ☐ planned (§10.2) |

---

## 8. Skip / not portably emulable

Listed for the record so future questions resolve to a stable answer.

| Feature | Reason |
|---|---|
| Geometry shaders on Metal | Removed in MSL 2.x. Emulation via compute + append-buffer is feasible but expensive and complex; the project policy is to skip in favor of mesh shaders (§9 of survey) |
| Tessellation on Metal | Structurally different model (compute kernel + post-tess vertex). Held as a feature-gated stub; revisit when a real workload demands it (§ Known failing tests entry 3 in reference) |
| Subpass inputs off Vulkan | No D3D12 / Metal equivalent (Metal tile shading is similar but not identical). Gate via `OMEGASL_FEATURE_BIT_SUBPASS_INPUTS` (§10.1) |
| `texturecube` `read` / `write` | Backend-asymmetric and rare in practice. Gate, don't emulate (§2.1) |
| `texture2d_ms` `write` | Not supported on any backend. Reject at Sema (§2.1) |
| `texture1d` mip sampling on MSL | Apple GPUs don't store a mip pyramid for 1D textures. Gate via `TEXTURE1D_MIP_SAMPLE`; emit stub on MSL (§2.3 Phase A.1) |
| Vector-condition ternary | GLSL has no syntactic form; HLSL/MSL semantics differ. Surface as `select(...)` (row in §1) instead of overloading `?:` (§3.2) |
| Single-level matrix column write (`m[a] = v`) | All three backends would need different lowerings (HLSL needs statement expansion). Reject at Sema until a generic statement-lowering pass exists (§12.1) |

---

## 9. When to add a row here vs. gate it

The decision rule, stated for new feature work:

1. If every backend can express the operation **with the same observable
   semantics** after a mechanical rewrite — add an emulation row. The
   compiler hides the divergence; user code is portable.
2. If at least one backend **cannot** express the operation at all (no API,
   no extension, structurally different model) — gate it via
   `#requires(...)` and produce a stub on the unsupported backend. User
   code declares intent; the runtime decides whether to load.
3. If the lowering would silently change performance characteristics
   (16-bit → 32-bit, atomics → CAS loops over non-atomic memory) — gate,
   don't emulate. Silent perf cliffs are worse than a clean rejection.

Rule of thumb: emulation is for **syntax / spelling** divergence. Gating is
for **capability** divergence.
