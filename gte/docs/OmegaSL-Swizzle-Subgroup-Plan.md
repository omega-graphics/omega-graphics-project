# OmegaSL Extension Plan: Texture Swizzles & Subgroup/Wave Operations

## Motivation

Two adjacent capability gaps in OmegaSL today:

1. **Texture swizzles.** `texture-swizzle-proposal.md` adds a runtime `TextureSwizzle` and a layout-descriptor field (`omegasl_texture_swizzle_desc`) so the binary format is forward-compatible, but explicitly defers the OmegaSL language syntax (`texture2d myTex : 2 (swizzle=bgra)`) and the corresponding `AST.h` / codegen work. This plan lands that deferred language piece.

2. **Subgroup / wave operations.** `GTEDeviceFeatures-Extension-Plan.md` calls these out under "What's NOT Included": *"OmegaSL doesn't expose subgroup ops yet; add when it does."* Wave intrinsics (ballot, broadcast, prefix sum, reductions) are first-class on every backend OmegaGTE targets — D3D12 SM6 wave ops, Metal `simdgroup_*` / `quad_*`, and GLSL `GL_KHR_shader_subgroup_*`. They're a strict prerequisite for any non-trivial compute pass (parallel reductions, occupancy queries, hierarchical culling) and the only thing currently blocking them is OmegaSL surface area.

These two extensions are bundled because they share the same change surface (lexer keywords → parser → AST → Sema → three codegens → reference doc → layout descriptor → device feature bit) and would otherwise duplicate the same edit pass twice in a row.

---

## Scope

| In scope | Out of scope |
|---|---|
| `swizzle=` argument on resource declarations | Per-call swizzle override inside shader source |
| `omegasl_texture_swizzle_desc` field already in layout desc — wire into parser/codegen | A full `TextureView` type (subresource ranges, format reinterpret) |
| Subgroup builtins covering ballot, broadcast, any/all, reductions, prefix scans, quad ops | Cooperative matrix / wave-matrix intrinsics (SM 6.8 / `VK_KHR_cooperative_matrix`) |
| `GTEDeviceFeatures::subgroupOps` capability bit + tier | Variable subgroup size control (`VK_EXT_subgroup_size_control`) |
| Sema diagnostics that reject subgroup ops outside compute/fragment, swizzle outside texture decls | Backwards-compat shims for older shader binaries lacking the swizzle field |

---

## Part A — Texture Swizzles in OmegaSL

### A.1 Surface syntax

Resource declarations gain an optional parenthesized argument list, mirroring the existing `static sampler2d s(filter=linear, address_mode=wrap)` form:

```omegasl
// All four channels — full form
texture2d diffuse : 1 (swizzle=bgra);

// Broadcast a single channel
texture2d shadowMask : 2 (swizzle=rrrr);

// Constants are allowed
texture2d normalMap : 3 (swizzle=rg01);

// Identity is the default — no parens needed
texture2d albedo : 4;
```

The token grammar for the swizzle value is exactly four characters from `{r, g, b, a, 0, 1}`, case-insensitive. Validation lives in Sema, not the lexer, so the error message can point at the offending character.

### A.2 Lexer / Toks.def

Add one new contextual keyword:

```c
#define KW_SWIZZLE KW("swizzle")
```

The four-character swizzle value is parsed as a `TOK_ID` (e.g. `bgra`) — no new token class. Sema validates the characters; this keeps the lexer regular and lets `rgba01`-style typos surface as a clean diagnostic instead of a lex error.

### A.3 AST.h

Extend `ResourceDecl`:

```cpp
struct ResourceDecl : public Decl {
    TypeExpr *typeExpr;
    OmegaCommon::String name;
    size_t registerNumber;
    bool isStatic = false;
    std::unique_ptr<StaticSamplerDesc> staticSamplerDesc;

    /// NEW — present iff a (swizzle=...) clause was supplied.
    /// Each entry is one of: R, G, B, A, Zero, One, Identity.
    /// Absent => identity (no codegen change, no descriptor field set).
    struct SwizzleDesc {
        unsigned char r, g, b, a;  // matches omegasl_texture_swizzle_desc encoding
    };
    std::optional<SwizzleDesc> swizzleDesc;
};
```

The encoding deliberately matches `omegasl_texture_swizzle_desc` 1:1 so the codegen path is a `memcpy`-equivalent assignment.

### A.4 Parser.cpp

The static-sampler arm at `Parser.cpp:467` already parses a `(key=value, ...)` argument list. Refactor that into a small helper `parseResourceArgList()` and call it from both arms — the static sampler arm and the new non-static arm at `Parser.cpp:661`. The shared helper recognizes:

- `filter=`, `min_filter=`, `mag_filter=`, `mip_filter=`, `address_mode=`, `max_anisotropy=` — only valid on samplers (sema check)
- `swizzle=` — only valid on `texture1d`/`texture2d`/`texture3d` (sema check)

This avoids two divergent argument parsers and lets future descriptor fields (e.g. mip range) land in one place.

### A.5 Sema.cpp

Three checks:

1. **Type check.** `swizzle=` is rejected on non-texture resources with the existing diagnostic infrastructure: `swizzle is only valid on texture1d/2d/3d resources`.
2. **Character set.** Reject any character outside `{r,g,b,a,0,1}` (case-insensitive) at the *position of the bad character*, not the whole identifier.
3. **Length.** Exactly 4 characters. `swizzle=rgb` and `swizzle=rgbaa` both error with `swizzle must be exactly 4 channels`.

Identity swizzles (`rgba`) are silently normalized to `swizzleDesc = nullopt` so the rest of the pipeline can use a single "is identity?" check.

### A.6 Code generation

For non-identity swizzles, the **shader source itself doesn't change** — the swizzle is realized at the descriptor / view level by the runtime (`texture-swizzle-proposal.md` §3). The compiler's job is to emit the metadata.

| Backend | What changes in emitted source |
|---|---|
| HLSL (`HLSLCodeGen.cpp`) | Nothing. SRV `Shader4ComponentMapping` is set by the host runtime via `omegasl_texture_swizzle_desc` in the layout. |
| MSL (`MetalCodeGen.cpp`) | Nothing. `MTLTextureSwizzleChannels` is applied at bind time. |
| GLSL (`GLSLCodeGen.cpp`) | Nothing. `VkComponentMapping` is set on the `VkImageView`. |

**The single shared change is in the layout-descriptor emission path** (the place where each codegen writes the `omegasl_shader_layout_desc` array into the `.omegasllib` archive). When `ResourceDecl::swizzleDesc` is present, populate the `swizzle_desc` field; otherwise zero-initialize (which the runtime treats as identity per `texture-swizzle-proposal.md`).

This means **A.6 is a single ~10-line edit per codegen file**, all writing the same field — the heavy lifting is the parser/AST changes in A.3–A.5.

### A.7 Files touched (Part A)

| File | Change |
|---|---|
| `gte/omegasl/src/Toks.def` | Add `KW_SWIZZLE` |
| `gte/omegasl/src/Lexer.cpp` | Recognize `swizzle` as contextual keyword (no new token class) |
| `gte/omegasl/src/AST.h` | Add `ResourceDecl::SwizzleDesc` and `swizzleDesc` optional |
| `gte/omegasl/src/Parser.cpp` | Extract `parseResourceArgList()`; accept `swizzle=` on texture decls |
| `gte/omegasl/src/Sema.cpp` | Validate type, length, character set; normalize identity to nullopt |
| `gte/omegasl/src/HLSLCodeGen.cpp` | Populate `swizzle_desc` in emitted layout |
| `gte/omegasl/src/MetalCodeGen.cpp` | Populate `swizzle_desc` in emitted layout |
| `gte/omegasl/src/GLSLCodeGen.cpp` | Populate `swizzle_desc` in emitted layout |
| `gte/docs/OmegaSL-Reference.md` | Add `swizzle=` to §3 Resources |
| `gte/include/omegasl.h` | (Already added by texture-swizzle proposal — no change here) |

---

## Part B — Subgroup / Wave Operations in OmegaSL

### B.1 Concept mapping

A "subgroup" is the SIMD execution unit a shader stage runs on. Vendor terminology differs but the operations are identical:

| OmegaSL term | HLSL SM6 | MSL | GLSL |
|---|---|---|---|
| subgroup | wave | simdgroup | subgroup |
| quad | quad | quad | subgroup quad |

OmegaSL adopts **subgroup** as the cross-platform term — it matches the SPIR-V / Vulkan vocabulary and is the most googleable for someone reading the language reference cold.

### B.2 New builtins

Added to `AST.def` and registered in `ast::builtins::Initialize()`. Each is a recognized intrinsic with type-checked args, mirroring how `dot` / `sample` already work:

**Lane-identity / size queries** (no args, return `uint`):
```omegasl
uint subgroup_size();          // total lanes in the subgroup
uint subgroup_lane_id();       // 0..subgroup_size()-1
```

**Vote / ballot** (1-arg `bool` in):
```omegasl
bool subgroup_any(bool p);     // true if any lane has p == true
bool subgroup_all(bool p);     // true if all lanes have p == true
uint4 subgroup_ballot(bool p); // bitmask of lanes where p == true (up to 128 lanes)
```

**Broadcast** (value + lane index):
```omegasl
T subgroup_broadcast(T value, uint lane);     // T ∈ scalar/vector arith types
T subgroup_broadcast_first(T value);          // broadcast from lowest active lane
```

**Reductions** (1-arg, return same type):
```omegasl
T subgroup_add(T x);   T subgroup_min(T x);   T subgroup_max(T x);
T subgroup_and(T x);   T subgroup_or(T x);    T subgroup_xor(T x);
```

**Prefix scans** (exclusive variants only — match GLSL/MSL semantics):
```omegasl
T subgroup_prefix_add(T x);
T subgroup_prefix_min(T x);
T subgroup_prefix_max(T x);
```

**Quad ops** (2x2 fragment-shader subset):
```omegasl
T quad_broadcast(T value, uint quad_lane);    // 0..3
T quad_swap_horizontal(T x);
T quad_swap_vertical(T x);
T quad_swap_diagonal(T x);
```

Type constraints: `T` is restricted to OmegaSL's existing arithmetic builtins (`int`, `uint`, `float`, and their 2/3/4 vectors). Sema rejects non-arith T with the existing "no matching overload" diagnostic.

### B.3 Stage restrictions

Sema enforces:

- **Reductions, scans, ballot, broadcast, vote, lane queries** — allowed in `compute` and `fragment`. (Vertex/hull/domain stages have undefined or implementation-defined wave behavior on at least one backend; the safe portable subset is compute + fragment.)
- **Quad ops** — fragment only on Metal/HLSL by spec; Sema enforces fragment-only.
- Using any subgroup intrinsic in `vertex`/`hull`/`domain` produces `subgroup operations are not supported in <stage> shaders`.

### B.4 Backend codegen

| OmegaSL builtin | HLSL (SM 6.0+) | MSL | GLSL (`GL_KHR_shader_subgroup_*`) |
|---|---|---|---|
| `subgroup_size()` | `WaveGetLaneCount()` | `[[threads_per_simdgroup]]` value | `gl_SubgroupSize` |
| `subgroup_lane_id()` | `WaveGetLaneIndex()` | `[[thread_index_in_simdgroup]]` | `gl_SubgroupInvocationID` |
| `subgroup_any(p)` | `WaveActiveAnyTrue(p)` | `simd_any(p)` | `subgroupAny(p)` |
| `subgroup_all(p)` | `WaveActiveAllTrue(p)` | `simd_all(p)` | `subgroupAll(p)` |
| `subgroup_ballot(p)` | `WaveActiveBallot(p)` | `simd_ballot(p)` (→ `uint4`) | `subgroupBallot(p)` |
| `subgroup_broadcast(v, l)` | `WaveReadLaneAt(v, l)` | `simd_broadcast(v, l)` | `subgroupBroadcast(v, l)` |
| `subgroup_broadcast_first(v)` | `WaveReadLaneFirst(v)` | `simd_broadcast_first(v)` | `subgroupBroadcastFirst(v)` |
| `subgroup_add(x)` | `WaveActiveSum(x)` | `simd_sum(x)` | `subgroupAdd(x)` |
| `subgroup_min(x)` | `WaveActiveMin(x)` | `simd_min(x)` | `subgroupMin(x)` |
| `subgroup_max(x)` | `WaveActiveMax(x)` | `simd_max(x)` | `subgroupMax(x)` |
| `subgroup_and(x)` | `WaveActiveBitAnd(x)` | `simd_and(x)` | `subgroupAnd(x)` |
| `subgroup_or(x)` | `WaveActiveBitOr(x)` | `simd_or(x)` | `subgroupOr(x)` |
| `subgroup_xor(x)` | `WaveActiveBitXor(x)` | `simd_xor(x)` | `subgroupXor(x)` |
| `subgroup_prefix_add(x)` | `WavePrefixSum(x)` | `simd_prefix_exclusive_sum(x)` | `subgroupExclusiveAdd(x)` |
| `subgroup_prefix_min(x)` | `WavePrefixProduct` is the closest available — emit a software fallback using ballot+broadcast | `simd_prefix_exclusive_min` (Apple7+); else fallback | `subgroupExclusiveMin(x)` |
| `subgroup_prefix_max(x)` | (same — software fallback) | (same — Apple7+ or fallback) | `subgroupExclusiveMax(x)` |
| `quad_broadcast(v, l)` | `QuadReadLaneAt(v, l)` | `quad_broadcast(v, l)` | `subgroupQuadBroadcast(v, l)` |
| `quad_swap_horizontal(x)` | `QuadReadAcrossX(x)` | `quad_shuffle_xor(x, 1)` | `subgroupQuadSwapHorizontal(x)` |
| `quad_swap_vertical(x)` | `QuadReadAcrossY(x)` | `quad_shuffle_xor(x, 2)` | `subgroupQuadSwapVertical(x)` |
| `quad_swap_diagonal(x)` | `QuadReadAcrossDiagonal(x)` | `quad_shuffle_xor(x, 3)` | `subgroupQuadSwapDiagonal(x)` |

GLSL backend additionally emits:

```glsl
#extension GL_KHR_shader_subgroup_basic       : require
#extension GL_KHR_shader_subgroup_vote        : require
#extension GL_KHR_shader_subgroup_ballot      : require
#extension GL_KHR_shader_subgroup_arithmetic  : require
#extension GL_KHR_shader_subgroup_quad        : require
```

These are emitted **only when the shader actually uses one of the corresponding builtins** — Sema sets a per-codegen-unit `usedSubgroupCategories` bitset that the GLSL emitter consults. The HLSL backend bumps the target profile from `cs_5_0` / `ps_5_0` to `cs_6_0` / `ps_6_0` under the same condition. The MSL backend includes `<metal_simdgroup>` only when used.

### B.5 Open-coded fallbacks

Two subgroup ops have no portable HLSL primitive (`prefix_min`, `prefix_max`) and no MSL primitive on Apple6 and below. The emitter falls back to a small inline expansion using `WaveActiveBallot` + per-lane broadcast — the slow path is correct on every device, and the fast path is taken automatically when the backend supports it. The fallback is gated on the `subgroupOps` tier from Part C, not on a runtime branch in the shader.

### B.6 Files touched (Part B)

| File | Change |
|---|---|
| `gte/omegasl/src/AST.def` | Add `BUILTIN_SUBGROUP_*` and `BUILTIN_QUAD_*` macros |
| `gte/omegasl/src/AST.h` | Declare new `FuncType *` builtins in `namespace builtins` |
| `gte/omegasl/src/Sema.cpp` | Register builtins; enforce stage restrictions; track `usedSubgroupCategories` |
| `gte/omegasl/src/CodeGen.cpp` | Common dispatch table from builtin name → backend-specific emitter hook |
| `gte/omegasl/src/HLSLCodeGen.cpp` | Wave intrinsic emission; bump SM target when used; fallbacks for `prefix_min/max` |
| `gte/omegasl/src/MetalCodeGen.cpp` | `simd_*` / `quad_*` emission; include `<metal_simdgroup>`; Apple6 fallback path |
| `gte/omegasl/src/GLSLCodeGen.cpp` | `subgroup*` emission; conditional `#extension` lines |
| `gte/docs/OmegaSL-Reference.md` | New §7 subsection "Subgroup operations" with the table from B.4 |

---

## Part C — Wiring to `GTEDeviceFeatures`

`GTEDeviceFeatures-Extension-Plan.md` listed *"Shader subgroup / wave operations detail"* as deferred. This is the moment to add it. Two new fields:

```cpp
struct GTEDeviceFeatures {
    // ... existing fields ...

    // ── Subgroup / wave operations ─────────────────────────────
    /// Highest tier of subgroup operations the device supports.
    /// Maps to D3D12 shader model 6.x wave ops, Metal SIMD-group
    /// support, and Vulkan VkPhysicalDeviceSubgroupProperties.
    enum class SubgroupTier : uint8_t {
        None = 0,         // device cannot run any subgroup intrinsic safely
        Basic,            // size, lane_id, broadcast, vote, ballot
        Arithmetic,       // + reductions and exclusive prefix scans
        Quad,             // + quad ops in fragment shaders
    } subgroupOps = SubgroupTier::None;

    /// Number of lanes per subgroup. 0 if subgroupOps == None.
    /// Variable on AMD (typically 32 or 64); fixed on NVIDIA (32),
    /// Apple (32), Intel (8/16/32 depending on shader complexity).
    /// Treat as advisory — use subgroup_size() in-shader for ground truth.
    unsigned subgroupSize = 0;
};
```

### Backend population

| Backend | Tier query | Size query |
|---|---|---|
| Metal | `supportsFamily(.apple7)` → Quad; `.apple6` → Arithmetic; `.apple4` → Basic | `device.maxThreadsPerThreadgroup` heuristic; final value comes from `[[threads_per_simdgroup]]` at runtime |
| D3D12 | `CheckFeatureSupport(SHADER_MODEL)` ≥ 6_0 → Basic; ≥ 6_0 with `WaveOps` capability → Arithmetic+Quad | `CheckFeatureSupport(OPTIONS1).WaveLaneCountMin` |
| Vulkan | `VkPhysicalDeviceSubgroupProperties.supportedOperations` bitmask: `BASIC` → Basic; `+ARITHMETIC` → Arithmetic; `+QUAD` → Quad | `VkPhysicalDeviceSubgroupProperties.subgroupSize` |

### Compiler integration

The `omegaslc` runtime compiler (`OmegaSLCompiler::Create(device)`) reads `device->features().subgroupOps` at compile time and:

- **Tier = None** → reject any shader using a subgroup builtin with `device does not support subgroup operations`.
- **Tier = Basic** → reject `subgroup_add`/`prefix_*`/quad ops; allow vote/ballot/broadcast.
- **Tier = Arithmetic** → reject quad ops only.
- **Tier = Quad** → no rejection.

Offline compilation (`omegaslc` CLI) cannot query a device, so it accepts a `--target-subgroup-tier=quad|arithmetic|basic|none` flag. The default is `arithmetic` — the most common subset across desktop GPUs released since 2020.

---

## Test plan

Per-feature smoke tests written as `.omegasl` fixtures under `gte/test/omegasl/`:

**Swizzle**:
- `swizzle_identity.omegasl` — `texture2d t : 0 (swizzle=rgba);` should emit no `swizzle_desc` field (normalized to nullopt).
- `swizzle_bgra.omegasl` — verify the layout binary contains `{2,1,0,3}` in `swizzle_desc`.
- `swizzle_constants.omegasl` — `swizzle=rg01` round-trips.
- `swizzle_invalid_char.omegasl` — `swizzle=rgbz` produces a diagnostic pointing at column of `z`.
- `swizzle_on_buffer.omegasl` — `buffer<T> b : 0 (swizzle=rgba);` rejected.

**Subgroup**:
- `subgroup_reduction.omegasl` — compute shader summing `subgroup_add(thread_value)`. Verify HLSL emits `cs_6_0` and `WaveActiveSum`, MSL emits `simd_sum`, GLSL emits `subgroupAdd` plus the arithmetic extension line.
- `subgroup_quad_in_vertex.omegasl` — should be rejected by Sema.
- `subgroup_with_unsupported_tier.omegasl` — runtime compile against a fake `GTEDeviceFeatures{subgroupOps=Basic}` rejects `subgroup_add`.
- `subgroup_prefix_min_fallback.omegasl` — HLSL output should contain the open-coded fallback expansion, not `WavePrefixMin` (which doesn't exist).

---

## Sequencing

This plan can land as four PRs of decreasing risk:

1. **Part A end-to-end** — swizzle parser + sema + codegen + reference doc. Self-contained, no runtime dependency beyond the texture-swizzle proposal's already-landed layout-descriptor field.
2. **Part C struct extension only** — add `subgroupOps` and `subgroupSize` to `GTEDeviceFeatures`, populate on all three backends. Ships independently and is useful even before Part B (callers can branch on it).
3. **Part B without fallbacks** — subgroup builtins for the tier-Quad happy path only. Refuse to compile on lower tiers. Lets the engine start using wave ops.
4. **Part B fallbacks** — `prefix_min` / `prefix_max` software paths for HLSL and pre-Apple7 MSL. Pure quality-of-life, can land any time.

---

## Open questions

1. **Swizzle on writes.** `texture-swizzle-proposal.md` deferred swizzle on UAV/storage-image writes because all three APIs are restrictive. Should the OmegaSL parser actively *reject* `swizzle=` on textures bound `out` / `inout`, or accept it and let the runtime apply only the read view? Reject is the safer default — flips silently to read-only behavior would be a footgun in compute shaders.

2. **Subgroup uniform-control-flow requirement.** SPIR-V's subgroup arithmetic ops must be called from uniform control flow. HLSL is more permissive; Metal sits in between. Should Sema attempt a conservative uniform-flow check (rejecting calls inside data-dependent `if`), or leave that as a runtime-validation-layer concern? The check is hard to do precisely, and false positives would block legitimate code — recommend deferring with a doc note.

3. **Subgroup-aware threadgroup sizing.** Should `compute(x=64, ...)` warn when `x` is not a multiple of `subgroup_size()`? On AMD this is a real performance cliff, but the multiplier varies by device — making it a compile-time warning requires the runtime-compiler path. Probably belongs in a separate "OmegaSL diagnostics" pass, not this one.

4. **Naming: `subgroup_` vs `wave_`.** OmegaSL has historically biased toward HLSL terminology (`make_float4`, `lerp`). Switching to SPIR-V's `subgroup_` for these is a deliberate departure — chosen because the documentation surface area for subgroup operations is overwhelmingly Vulkan/SPIR-V. If you'd prefer consistency over searchability, the rename to `wave_*` is a single sed-pass through this plan.
