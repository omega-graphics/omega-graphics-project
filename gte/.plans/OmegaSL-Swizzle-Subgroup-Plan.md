# OmegaSL Extension Plan: Texture Swizzles, Subgroup/Wave Operations & Cooperative Matrices/Vectors

## Motivation

Three adjacent capability gaps in OmegaSL today:

1. **Texture swizzles.** `texture-swizzle-proposal.md` adds a runtime `TextureSwizzle` and a layout-descriptor field (`omegasl_texture_swizzle_desc`) so the binary format is forward-compatible, but explicitly defers the OmegaSL language syntax (`texture2d myTex : 2 (swizzle=bgra)`) and the corresponding `AST.h` / codegen work. This plan lands that deferred language piece.

2. **Subgroup / wave operations.** `GTEDeviceFeatures-Extension-Plan.md` calls these out under "What's NOT Included": *"OmegaSL doesn't expose subgroup ops yet; add when it does."* Wave intrinsics (ballot, broadcast, prefix sum, reductions) are first-class on every backend OmegaGTE targets — D3D12 SM6 wave ops, Metal `simdgroup_*` / `quad_*`, and GLSL `GL_KHR_shader_subgroup_*`. They're a strict prerequisite for any non-trivial compute pass (parallel reductions, occupancy queries, hierarchical culling) and the only thing currently blocking them is OmegaSL surface area.

3. **Cooperative matrices and vectors.** The hardware matrix-core / tensor-core abstractions — both gated through the existing OmegaSL feature gate (Part F). A cooperative **matrix** (Part D) is a matrix fragment shared across one subgroup with a single `D = A·B + C` multiply-accumulate — the GEMM primitive. A cooperative **vector** (Part E) gives every thread its own vector against a shared weight matrix (`out = M·in + bias`) — the per-thread MLP primitive behind neural materials, neural texture compression, and neural upscaling (NVIDIA RTX Neural Shaders / DirectX Cooperative Vectors use it directly). Cooperative *matrix* is broadly supported (Vulkan `VK_KHR_cooperative_matrix`, Metal `simdgroup_matrix`, HLSL Wave Matrix SM 6.8); cooperative *vector* is newer and narrower (Vulkan `VK_NV_cooperative_vector`, HLSL `dx::linalg` SM 6.9, **no native Metal primitive — emulated there via `simdgroup_matrix`**, E.6.1) — which is exactly why both run through the feature gate, rejected gracefully only where neither a native nor an emulated path exists. Unlike Parts A/B these introduce **new opaque types**, not just builtins, so they are the deepest changes here and are sequenced last. (This is the "OmegaSL cooperative-vector support" gating dependency flagged in §7.4 of `Super-Resolution-And-Frame-Generation-Plan.md`.)

These three extensions are bundled because they share the same change surface (lexer/type-system → parser → AST → Sema → three codegens → reference doc → layout descriptor → device feature bit) and would otherwise duplicate the same edit pass. The cooperative types (Parts D/E) build directly on the subgroup scope Part B introduces, so they belong in the same plan.

---

## Scope

| In scope | Out of scope |
|---|---|
| `swizzle=` argument on resource declarations | Per-call swizzle override inside shader source |
| `omegasl_texture_swizzle_desc` field already in layout desc — wire into parser/codegen | A full `TextureView` type (subresource ranges, format reinterpret) |
| Subgroup builtins covering ballot, broadcast, any/all, reductions, prefix scans, quad ops | Training / backprop ops (outer-product & reduce-sum accumulate) for either cooperative type |
| Cooperative matrix type `coopmat<T,Use,M,N>` + load / store / multiply-accumulate for in-shader MMA (compute) | Matrix/vector shapes & element types beyond device-reported support; scopes other than subgroup |
| Cooperative vector type `coopvec<T,N>` + matmul/matmul-add (compute + fragment), incl. **Metal emulation via `simdgroup_matrix`** (E.6.1) | fp8 microscaling formats beyond device report; cooperative-vector *training* ops |
| OmegaSL feature gating of both cooperative types via `#requires(coopmatrix/coopvector)` + `OMEGASL_FEATURE_BIT_*` (Part F) | A *new* gate mechanism — Part F reuses the existing mesh-shader/raytracing gate |
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

## Part D — Cooperative Matrices in OmegaSL

### D.1 Concept

A cooperative matrix is a matrix *fragment* whose storage and arithmetic are distributed across the lanes of a single **subgroup** (Part B). One `coopmat_mad` performs a full tile `D = A·B + C` on the hardware matrix core — an order of magnitude faster than an open-coded loop. The shape (`M×N×K`), element types, and scope are **compile-time constants**, and only the combinations the device reports as supported are legal (queried at runtime — D.7).

The cross-platform vocabulary maps cleanly because all three backends model the same SPIR-V-derived concept:

| OmegaSL term | Vulkan/GLSL | Metal | HLSL |
|---|---|---|---|
| cooperative matrix | `coopmat` (`GL_KHR_cooperative_matrix`) | `simdgroup_matrix` (`<metal_simdgroup_matrix>`) | Wave Matrix (`WaveMatrix*`, SM 6.8) |

OmegaSL adopts **`coopmat`** as the type spelling — it matches the Vulkan/SPIR-V term that has the largest documentation surface, consistent with the `subgroup_` choice in Part B.

### D.2 Surface syntax — a new opaque type

Unlike Parts A/B, this introduces a **type**, parameterized by element type, *use*, and dimensions (all compile-time):

```omegasl
// coopmat<ElementType, Use, Rows, Cols>
// Use ∈ { MatrixA, MatrixB, Accumulator }
compute void gemm_tile(
    buffer<half>  matA : 0,
    buffer<half>  matB : 1,
    buffer<float> matC : 2,
    [[thread]] uint3 tid)
{
    coopmat<half,  MatrixA,     16, 16> a;
    coopmat<half,  MatrixB,     16, 16> b;
    coopmat<float, Accumulator, 16, 16> acc;

    coopmat_fill(acc, 0.0);
    coopmat_load(a, matA, /*offset*/ 0, /*stride*/ 16, row_major);
    coopmat_load(b, matB, 0, 16, row_major);
    acc = coopmat_mad(a, b, acc);          // acc = a·b + acc
    coopmat_store(acc, matC, 0, 16, row_major);
}
```

`MatrixA`, `MatrixB`, `Accumulator`, `row_major`, and `col_major` are new contextual keywords (lexer), valid only inside a `coopmat<...>` type or a load/store call.

### D.3 Operations (builtins over the new type)

| OmegaSL builtin | Meaning |
|---|---|
| `coopmat_fill(m, scalar)` | Broadcast-initialize every element. |
| `coopmat_load(m, buf, offset, stride, layout)` | Load a tile from a `buffer<T>` (device) or `threadgroup` array. |
| `coopmat_store(m, buf, offset, stride, layout)` | Store a tile. |
| `coopmat_mad(a, b, c) → coopmat` | `a·b + c`. `a:MatrixA<M,K>`, `b:MatrixB<K,N>`, `c/result:Accumulator<M,N>`. |
| `m + n`, `m * scalar` | Element-wise add / scalar multiply on same-shape/use matrices. |
| `coopmat_convert(dst, src)` | Element-type conversion (e.g. `Accumulator<float>` → `MatrixA<half>` to chain layers). |

### D.4 Type system / AST

This is the part with no Part A/B analog:

- **`AST.h`** — add a `CoopMatrixType` `TypeExpr` subclass carrying `{ TypeExpr *element; enum Use; unsigned rows, cols; }`. Cooperative matrices are first-class types: usable as local variable types and function parameter types (but **not** as resource/buffer element types — they have no memory layout outside the subgroup).
- **`Parser.cpp`** — parse `coopmat < type , Use , int , int >` in the type-parsing path. The K dimension is implied by pairing A's `Cols` with B's `Rows` at the `coopmat_mad` call, matching the Vulkan model.

### D.5 Constraints (Sema)

1. **Stage** — compute only (initial scope). Vulkan `coopmat` is subgroup-scoped and practically compute-only; Metal/HLSL are more permissive but the portable subset is compute. Use in any other stage: `cooperative matrices are only valid in compute shaders`.
2. **Shape/type legality** — the `{A-type, B-type, C-type, M, N, K}` tuple must be in the device-reported supported set (runtime compiler) or the `--target-coopmat-shapes` set (offline). Reject with a message naming the requested tuple and the nearest supported one.
3. **Use agreement** — `coopmat_mad` requires `MatrixA × MatrixB → Accumulator` with matching inner K; `+`/`*` require identical `{type, use, M, N}`. Mismatches use the existing "no matching overload" diagnostic.
4. **Load/store targets** — pointer must resolve to a `buffer<T>` or `threadgroup` array of the element type; `layout` is `row_major`/`col_major` only.

### D.6 Backend codegen

| OmegaSL | Vulkan/GLSL (`GL_KHR_cooperative_matrix`) | Metal (`<metal_simdgroup_matrix>`) | HLSL (Wave Matrix, SM 6.8) |
|---|---|---|---|
| `coopmat<T,MatrixA,M,N>` | `coopmat<T, gl_ScopeSubgroup, M, N, gl_MatrixUseA>` | `simdgroup_matrix<T, M, N>` | `WaveMatrixLeft<T, M, N>` |
| `coopmat<T,MatrixB,M,N>` | `…, gl_MatrixUseB>` | `simdgroup_matrix<T, M, N>` | `WaveMatrixRight<T, M, N>` |
| `coopmat<T,Accumulator,M,N>` | `…, gl_MatrixUseAccumulator>` | `simdgroup_matrix<T, M, N>` | `WaveMatrixAccumulator<T, M, N>` |
| `coopmat_fill(m, s)` | `m = coopmat<…>(s)` | `m = make_filled_simdgroup_matrix<…>(s)` | `m.Fill(s)` |
| `coopmat_load(m,b,o,st,lay)` | `coopMatLoad(m, b, o, st, layout)` | `simdgroup_load(m, b + o, st)` | `m.Load(b, o, st, lay)` |
| `coopmat_store(m,b,o,st,lay)` | `coopMatStore(m, b, o, st, layout)` | `simdgroup_store(m, b + o, st)` | `m.Store(b, o, st, lay)` |
| `coopmat_mad(a,b,c)` | `coopMatMulAdd(a, b, c)` | `simdgroup_multiply_accumulate(d, a, b, c)` | `WaveMatrixMultiplyAccumulate(c, a, b)` |

Conditional emission (same `usedCategories` mechanism as B.4): GLSL emits `#extension GL_KHR_cooperative_matrix : require` only when a `coopmat` is used; HLSL bumps the target profile to `cs_6_8` and sets the experimental Wave-Matrix flag; MSL includes `<metal_simdgroup_matrix>`.

> **HLSL is the in-flux backend — feasibility spike required first.** HLSL Wave Matrix shipped as an **SM 6.8 preview** and its exact type names/methods have changed across DXC revisions (and the newer SM 6.9 *Cooperative Vector* path is a separate, matrix×vector feature, out of scope here). The names in the table above are the SM 6.8 preview spelling and **must be verified against the DXC version this repo vendors** before Part D's HLSL arm is committed — per the backend-build-verification rule, compile the generated HLSL with the local `dxc`, do not trust `-S` text alone. If shipping DXC has dropped/renamed Wave Matrix, the HLSL arm degrades to "unsupported on this device" (D.7 tier `None`) rather than emitting code that won't compile.

### D.7 Wiring to `GTEDeviceFeatures`

Cooperative-matrix support is **shape-dependent**, so a single bit is insufficient — add a coarse capability plus a runtime-queried supported-shape list:

```cpp
struct GTEDeviceFeatures {
    // ... existing fields, including the Part C subgroup additions ...

    // ── Cooperative matrices ───────────────────────────────────
    /// True if the device exposes any cooperative-matrix shape.
    bool coopMatrix = false;

    /// One device-supported {types, dims} combination. Element types
    /// reuse the OmegaSL scalar enum (half/float/int8/int32). The
    /// engine exposes the list so a caller (or the runtime compiler)
    /// can pick a legal tile shape; empty when coopMatrix == false.
    struct CoopMatrixShape {
        uint8_t aType, bType, cType;   // element types of A, B, accumulator
        uint8_t M, N, K;
    };
    OmegaCommon::Vector<CoopMatrixShape> coopMatrixShapes;
};
```

| Backend | Query |
|---|---|
| Vulkan | `VkPhysicalDeviceCooperativeMatrixFeaturesKHR` → `coopMatrix`; enumerate `vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` → `coopMatrixShapes`. |
| Metal | `supportsFamily(.apple7)` (M1/A14+) → `coopMatrix`; the supported set is the documented `simdgroup_matrix` shapes (8×8 half/float, etc.) — seed `coopMatrixShapes` from the family tier. |
| D3D12 | `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS…)` Wave-MMA / SM-6.8 tier (preview — gate behind the same spike as D.6); shapes from the documented Wave Matrix dimension set. |

> **The Apple7+ gate is not a real coverage limit.** Cooperative matrix (and the Part E coopvec emulation) need `simdgroup_matrix`, which is Apple GPU Family 7+ = Apple silicon **M1 / A14 and later**. The upcoming macOS release **drops support for Intel-based Macs**, so every Mac the engine targets going forward is Apple silicon (Apple7+). The "`false` below Apple7" branch therefore only covers legacy/Intel Macs that are already falling out of support — in practice cooperative matrix and coopvec emulation are available across the whole supported Mac install base, and the Part F gate's Metal-reject path is effectively dead on supported OS versions.

**Compiler integration.** The `omegaslc` runtime path rejects any `coopmat` whose tuple is not in `device->features().coopMatrixShapes`. Offline, `omegaslc` takes `--target-coopmat-shapes=<list>` (default: the portable `16×16×16 half→float` and `8×8×8 half→float` tiles common to all three vendors since ~2020). `featuresAsBitmask()` gains an advisory `OMEGASL_FEATURE_BIT_COOPMATRIX` so a shader requiring cooperative matrices is rejected at library-load on a device without them (same mechanism as the existing feature bits — full gate wiring in **Part F**).

### D.8 Files touched (Part D)

| File | Change |
|---|---|
| `gte/omegasl/src/Toks.def` | Add `MatrixA`/`MatrixB`/`Accumulator`/`row_major`/`col_major` contextual keywords |
| `gte/omegasl/src/AST.def` | Add `BUILTIN_COOPMAT_*` macros (fill/load/store/mad/convert) |
| `gte/omegasl/src/AST.h` | Add `CoopMatrixType` `TypeExpr` subclass; declare coopmat builtins |
| `gte/omegasl/src/Parser.cpp` | Parse `coopmat<type, Use, M, N>` in the type path |
| `gte/omegasl/src/Sema.cpp` | Type/use/shape/stage checks; track `usedCoopMatrix` for conditional emission |
| `gte/omegasl/src/CodeGen.cpp` | Dispatch coopmat builtins → backend emitter hooks |
| `gte/omegasl/src/HLSLCodeGen.cpp` | Wave Matrix emission; `cs_6_8` + experimental flag (gated on the D.6 spike) |
| `gte/omegasl/src/MetalCodeGen.cpp` | `simdgroup_matrix` emission; include `<metal_simdgroup_matrix>` |
| `gte/omegasl/src/GLSLCodeGen.cpp` | `coopmat`/`coopMatMulAdd` emission; conditional `#extension` |
| `gte/include/omegaGTE/GTEDevice.h` | Add `coopMatrix` + `CoopMatrixShape`/`coopMatrixShapes`; advisory bit in `featuresAsBitmask()` |
| `gte/src/{metal,d3d12,vulkan}/…` | Populate `coopMatrix` + `coopMatrixShapes` per the D.7 query table |
| `gte/docs/OmegaSL-Reference.md` | New §8 "Cooperative matrices" with the D.2/D.3/D.6 tables |
| `gte/include/omegasl.h` | Add `OMEGASL_FEATURE_BIT_COOPMATRIX` |

---

## Part E — Cooperative Vectors in OmegaSL

### E.1 Concept

A cooperative **vector** is a per-invocation vector of length `N` on which the subgroup cooperatively evaluates a matrix–vector product `out[N] = M[N×K] · in[K] (+ bias)` on the matrix-core hardware. Where a cooperative *matrix* (Part D) shares one tile across the whole subgroup (ideal for GEMM), a cooperative *vector* gives **every thread its own vector** against a shared weight matrix in memory — the natural shape for "each pixel/sample runs a small MLP." That is precisely how neural materials, neural texture compression (NTC), and neural upscaling perform inference, and it is the primitive NVIDIA RTX Neural Shaders and DirectX **Cooperative Vectors** (SM 6.9) expose directly.

**Availability is narrow — narrower than cooperative matrix — which is why the feature gate (Part F) is still mandatory.** Today cooperative vector is an NVIDIA extension on Vulkan (`VK_NV_cooperative_vector`) and an SM 6.9 preview on D3D12 (`dx::linalg`). **Metal has no shader-language cooperative-vector type — but it does not need to be gated off there:** a coopvec across a subgroup is a GEMM with the lane axis as the batch dimension, so Metal **emulates** it on the `simdgroup_matrix` matrix units (E.6.1). The Part F gate therefore only rejects a `coopvec` shader where *neither* a native path nor the emulation exists — a non-NVIDIA D3D12/Vulkan GPU without the extension, or pre-Apple7 Metal lacking `simdgroup_matrix` — never emitting code that won't compile.

### E.2 Surface syntax — a new opaque type

```omegasl
// coopvec<ElementType, N>
#requires(coopvector)                       // file-scope feature gate (Part F)

compute void mlp_layer(
    buffer<half> weights : 0,               // [outN x inK], row-major
    buffer<half> bias    : 1,
    buffer<half> io      : 2,
    [[thread]] uint3 tid)
{
    coopvec<half, 32> x;
    coopvec_load(x, io, tid.x * 32);

    coopvec<half, 64> h;
    coopvec_matmul_add(h, x, weights, bias, /*K*/ 32, /*N*/ 64, row_major);
    h = max(h, coopvec<half, 64>(0.0));      // ReLU activation, element-wise

    coopvec_store(h, io, tid.x * 64);
}
```

`N`, the element type, and the matrix dims are compile-time. `row_major`/`col_major` reuse the Part D layout keywords.

### E.3 Operations

| OmegaSL builtin | Meaning |
|---|---|
| `coopvec_load(v, buf, offset)` | Load `N` elements into the per-thread vector. |
| `coopvec_store(v, buf, offset)` | Store the vector. |
| `coopvec_matmul(out, in, mat, K, N, layout)` | `out = M · in`. |
| `coopvec_matmul_add(out, in, mat, bias, K, N, layout)` | `out = M · in + bias`. |
| `max`/`min`/`+`/`*`, `coopvec_convert(dst, src)` | Element-wise activations (ReLU, etc.) and per-layer type changes. |

Outer-product / reduce-sum *accumulate* (the **training** ops) are out of scope — inference only.

### E.4 Type system / AST

Add a `CoopVectorType` `TypeExpr` subclass `{ TypeExpr *element; unsigned n; }`, reusing the Part D type-parsing path (`coopvec < type , int >`). A first-class local/parameter type, not a resource element type.

### E.5 Constraints (Sema)

1. **Stage** — compute **and fragment** (neural materials run in the fragment stage), broader than coopmat's compute-only. Other stages: rejected.
2. **Element type** must be in the device-reported coopvec type set (half/float/int8; fp8 only if reported).
3. **Dim agreement** — `coopvec_matmul*` requires `in.N == K` and `out.N == N`; the matrix buffer element type must be compatible.
4. **Feature bit** — any shader using `coopvec` must carry the `coopvector` feature bit; Sema auto-sets it (Part F) and verifies device support at compile time on the runtime path.

### E.6 Backend codegen

| OmegaSL | Vulkan/GLSL (`GL_NV_cooperative_vector`) | HLSL (`dx::linalg`, SM 6.9) | Metal (emulated — E.6.1) |
|---|---|---|---|
| `coopvec<T,N>` | `coopvecNV<T, N>` | `dx::linalg::Vector<T, N>` | per-lane vector staged into a `simdgroup_matrix` tile |
| `coopvec_matmul(...)` | `coopVecMatMulNV` | `dx::linalg::MatVecMul` | subgroup GEMM: `simdgroup_multiply` (lane axis = batch) |
| `coopvec_matmul_add(...)` | `coopVecMatMulAddNV` | `dx::linalg::MatVecMulAdd` | `simdgroup_multiply_accumulate` + bias |

Conditional emission (same `usedCategories` mechanism as B.4 / D.6): GLSL emits `#extension GL_NV_cooperative_vector : require`; HLSL bumps to `cs_6_9` / `ps_6_9` and sets the Agility/preview flag (**same feasibility-spike posture as Wave Matrix in D.6** — verify against the vendored DXC first); Metal includes `<metal_simdgroup_matrix>` and emits the emulation (E.6.1).

#### E.6.1 Metal emulation via `simdgroup_matrix`

A cooperative vector evaluated across a subgroup *is* a GEMM with the **lane axis as the batch dimension**: stacking the subgroup's per-lane inputs `in[K]` gives a `K×subgroupSize` matrix, and `M[N×K] · that = N×subgroupSize`, whose columns scatter back to each lane's `out[N]`. Metal lowers `coopvec_matmul` to:
1. stage the per-lane input vectors into `threadgroup` memory in tile layout,
2. load the weight tile and the staged inputs as `simdgroup_matrix` (the **Part D** path),
3. `simdgroup_multiply[_accumulate]` on the matrix units,
4. scatter the result columns back to each lane's output vector.

This runs on the Apple-silicon matrix units, not a scalar loop. Caveats — the reason this is a **feasibility spike** rather than a certainty (the developer's "probably"): `K`/`N` must be padded to the `simdgroup_matrix` tile dims; the threadgroup staging costs shared memory; and `simdgroup_matrix` in the **fragment** stage may be restricted on some Apple families — if so, Metal-emulated `coopvec` is **compute-only** and fragment-stage coopvec falls back to the Part F gate there. Gated on Apple7+ (`simdgroup_matrix` support); below that, `coopVector = false` and the gate rejects — but Apple7+ = M1/A14+, i.e. every Mac on the upcoming macOS that drops Intel support, so this floor is effectively the entire supported install base (see the D.7 note).

### E.7 Wiring to `GTEDeviceFeatures`

```cpp
struct GTEDeviceFeatures {
    // ... Part D cooperative-matrix fields ...

    // ── Cooperative vectors ────────────────────────────────────
    bool coopVector = false;                 // device exposes any coopvec path
    /// Element types the device accepts for coopvec (OmegaSL scalar enum).
    OmegaCommon::Vector<uint8_t> coopVectorTypes;
};
```

| Backend | Query |
|---|---|
| Vulkan | `VkPhysicalDeviceCooperativeVectorFeaturesNV` → `coopVector`; properties enumerate the supported component types. |
| D3D12 | SM 6.9 cooperative-vector tier via `CheckFeatureSupport` (Agility-SDK preview — gate behind the E.6 spike). |
| Metal | `coopVector = true` on Apple7+ (emulated via `simdgroup_matrix`, E.6.1); `false` below. |

### E.8 Files touched (Part E)

Same shape as D.8 — `Toks.def` (reuses Part D layout keywords), `AST.def` (`BUILTIN_COOPVEC_*`), `AST.h` (`CoopVectorType`), `Parser.cpp`, `Sema.cpp` (stage = compute+fragment), `CodeGen.cpp`, the three target emitters, `GTEDevice.h` (`coopVector`/`coopVectorTypes`), `gte/src/{vulkan,d3d12,metal}/…` (populate), `OmegaSL-Reference.md` (§9 "Cooperative vectors"), and `omegasl.h` (`OMEGASL_FEATURE_BIT_COOPVECTOR`).

---

## Part F — OmegaSL feature gating for the cooperative types

The cooperative types are device-gated through the **existing** OmegaSL feature gate — the same machinery mesh shaders and raytracing already use (OmegaSL-Feature-Gap-Survey §14.3 / §14.7.1). **No new mechanism is introduced**; Parts D/E only register two new bits into it. Grounded in the real symbols:

1. **Feature bits** — `gte/include/omegasl.h:263-292` defines the `OMEGASL_FEATURE_BIT_*` enum (the last assigned bit is `OMEGASL_FEATURE_BIT_CULL_DISTANCE = 1ull << 15`). Add:
   ```c
   OMEGASL_FEATURE_BIT_COOPMATRIX = 1ull << 16,
   OMEGASL_FEATURE_BIT_COOPVECTOR = 1ull << 17,
   ```
2. **`#requires` directive** — `gte/omegasl/src/Preprocessor.cpp` (~`:226`, `requiredFeatures_ |= f->bit`) maps a `#requires(name)` feature name → bit. Register `coopmatrix` and `coopvector` in that name→bit table so `#requires(coopmatrix)` / `#requires(coopvector)` light the bits. **Sema additionally auto-sets** the bit whenever a `coopmat`/`coopvec` type is used, so an author who omits `#requires` still produces a correctly-gated library (with a Sema note suggesting the directive) — the bit can never silently go unset.
3. **Per-shader serialization** — the bit lands in `omegasl_shader::requiredFeatures` (`omegasl.h:349`), serialized into every `.omegasllib`.
4. **Device capability mask** — `GTEDeviceFeatures::featuresAsBitmask()` (`gte/include/omegaGTE/GTEDevice.h:108`) ORs in `OMEGASL_FEATURE_BIT_COOPMATRIX` when `coopMatrix` is set and `OMEGASL_FEATURE_BIT_COOPVECTOR` when `coopVector` is set (D.7 / E.7 populate those bools per backend).
5. **Load-time rejection** — the engine caches the mask in `_deviceFeatures` (`GE.h:375`); `loadShaderLibraryFromInputStream` / `loadShaderLibraryRuntime` already mask each shader's `requiredFeatures` and, when a required bit is missing, substitute `_makeUnsupportedShaderSentinel(...)` (`GE.h:396`). `_checkPipelineShader` (`GE.h:407`) then surfaces the precise diagnostic at pipeline-build time.

**Net effect:** a `coopvec`/`coopmat` shader loaded on a device lacking the capability — e.g. a `coopvec` library on a non-NVIDIA D3D12/Vulkan GPU without the extension, or on pre-Apple7 Metal that cannot even emulate it (E.6.1) — is rejected at load with a precise message, its sibling shaders still load, and any attempt to build a pipeline from it fails loudly via the sentinel. This is **identical** to how a mesh shader is gated on `OMEGASL_FEATURE_BIT_MESH_SHADERS` today. The shape/type-specific checks (legal tile in D.5, legal vector type in E.5) layer on top at compile time; the feature bit is the coarse "can this device run it at all" gate underneath.

| File | Change |
|---|---|
| `gte/include/omegasl.h` | Add `OMEGASL_FEATURE_BIT_COOPMATRIX` / `OMEGASL_FEATURE_BIT_COOPVECTOR` |
| `gte/omegasl/src/Preprocessor.cpp` | Register `coopmatrix` / `coopvector` in the `#requires` name→bit table |
| `gte/omegasl/src/Sema.cpp` | Auto-set the feature bit on cooperative-type use; note-if-missing `#requires` |
| `gte/include/omegaGTE/GTEDevice.h` | OR the two bits into `featuresAsBitmask()` from `coopMatrix`/`coopVector` |

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

**Cooperative matrix**:
- `coopmat_gemm.omegasl` — the D.2 tile GEMM. Verify GLSL emits `coopMatMulAdd` + the `GL_KHR_cooperative_matrix` extension, MSL emits `simdgroup_multiply_accumulate` + `<metal_simdgroup_matrix>`, HLSL emits `WaveMatrixMultiplyAccumulate` at `cs_6_8` (or, if the D.6 spike found Wave Matrix unavailable in the vendored DXC, the fixture is `xfail` with the unsupported-tier diagnostic).
- `coopmat_in_fragment.omegasl` — `coopmat` used in a fragment shader; rejected by Sema (compute-only).
- `coopmat_unsupported_shape.omegasl` — request `coopmat<half,MatrixA,7,7>`; offline compile with `--target-coopmat-shapes=16x16x16:half` rejects it, naming the requested vs nearest-supported tuple.
- `coopmat_use_mismatch.omegasl` — `coopmat_mad` with two `MatrixA` operands; rejected with the no-matching-overload diagnostic.

**Cooperative vector**:
- `coopvec_mlp.omegasl` — the E.2 MLP layer. Verify GLSL emits `coopVecMatMulAddNV` + `GL_NV_cooperative_vector`, HLSL emits `dx::linalg::MatVecMulAdd` at `cs_6_9` (or `xfail` if the E.6 spike found `dx::linalg` unavailable in the vendored DXC).
- `coopvec_in_vertex.omegasl` — `coopvec` in a vertex shader; rejected by Sema (compute+fragment only).
- `coopvec_dim_mismatch.omegasl` — `coopvec_matmul` where `in.N != K`; rejected.

**Feature gate (Part F)**:
- `coopvec_metal_emulated.omegasl` — `coopvec_matmul` compiled for Metal on an Apple7+ device: verify it emits the `simdgroup_matrix` emulation (E.6.1) — `<metal_simdgroup_matrix>`, threadgroup staging, `simdgroup_multiply[_accumulate]` — and does **not** hit the Part F gate.
- `coopvec_unsupported.omegasl` — a `#requires(coopvector)` library loaded against a device with no coopvec path (`GTEDeviceFeatures{coopVector=false}` — pre-Apple7 Metal or a non-NVIDIA D3D12/Vulkan GPU without the extension): the shader is replaced by the unsupported sentinel, sibling shaders still load, and building a pipeline from it fails via `_checkPipelineShader` with a precise diagnostic.
- `coopmat_requires_autoset.omegasl` — a shader using `coopmat` *without* `#requires(coopmatrix)`: verify Sema auto-sets `OMEGASL_FEATURE_BIT_COOPMATRIX` in the serialized `requiredFeatures` (and emits the suggest-directive note).

---

## Sequencing

This plan can land as six PRs of decreasing risk:

1. **Part A end-to-end** — swizzle parser + sema + codegen + reference doc. Self-contained, no runtime dependency beyond the texture-swizzle proposal's already-landed layout-descriptor field.
2. **Part C struct extension only** — add `subgroupOps` and `subgroupSize` to `GTEDeviceFeatures`, populate on all three backends. Ships independently and is useful even before Part B (callers can branch on it).
3. **Part B without fallbacks** — subgroup builtins for the tier-Quad happy path only. Refuse to compile on lower tiers. Lets the engine start using wave ops.
4. **Part B fallbacks** — `prefix_min` / `prefix_max` software paths for HLSL and pre-Apple7 MSL. Pure quality-of-life, can land any time.
5. **Part D — cooperative matrices** — depends on Parts B + C (subgroup scope and the device-feature plumbing) and on the **Part F** feature-gate bits. Gate the start on the **D.6 HLSL feasibility spike**: confirm the vendored DXC still exposes Wave Matrix before committing all three backends. Vulkan + Metal arms can land first (Vulkan on Linux CI, Metal `simdgroup_matrix` on the macOS host); the HLSL/D3D12 arm is now **testable on the developer's RTX 50 box** (D3D12 build still handed off per the WSL constraint, but verifiable on real HW).
6. **Part E — cooperative vectors** — highest risk and narrowest support; depends on Part D's type-system work + Part F. Gate on the **E.6 spike** (`dx::linalg` / `VK_NV_cooperative_vector` availability in the vendored toolchains). The **RTX 50 card exercises both the Vulkan `VK_NV_cooperative_vector` and the HLSL `dx::linalg` paths**, so the native arms are fully verifiable on the developer's hardware. The **Metal arm emulates** via `simdgroup_matrix` (E.6.1) — gated on its own small spike (tile padding + fragment-stage support) and testable on the macOS host; pre-Apple7 falls to the Part F gate.

---

## Open questions

1. **Swizzle on writes.** `texture-swizzle-proposal.md` deferred swizzle on UAV/storage-image writes because all three APIs are restrictive. Should the OmegaSL parser actively *reject* `swizzle=` on textures bound `out` / `inout`, or accept it and let the runtime apply only the read view? Reject is the safer default — flips silently to read-only behavior would be a footgun in compute shaders.

1. Reject. Follow what each of the 3 backends do.

2. **Subgroup uniform-control-flow requirement.** SPIR-V's subgroup arithmetic ops must be called from uniform control flow. HLSL is more permissive; Metal sits in between. Should Sema attempt a conservative uniform-flow check (rejecting calls inside data-dependent `if`), or leave that as a runtime-validation-layer concern? The check is hard to do precisely, and false positives would block legitimate code — recommend deferring with a doc note.

Look at what each one does. I recommend a stricter uniform-flow check, and then render to each target properly.

3. **Subgroup-aware threadgroup sizing.** Should `compute(x=64, ...)` warn when `x` is not a multiple of `subgroup_size()`? On AMD this is a real performance cliff, but the multiplier varies by device — making it a compile-time warning requires the runtime-compiler path. Probably belongs in a separate "OmegaSL diagnostics" pass, not this one.

Compile-Time warning. Depending on the algorithim if people need a kernel that requires that exact subgroup size, we will allow it but it may only run best on certain devices.

4. **Naming: `subgroup_` vs `wave_`.** OmegaSL has historically biased toward HLSL terminology (`make_float4`, `lerp`). Switching to SPIR-V's `subgroup_` for these is a deliberate departure — chosen because the documentation surface area for subgroup operations is overwhelmingly Vulkan/SPIR-V. If you'd prefer consistency over searchability, the rename to `wave_*` is a single sed-pass through this plan.

Subgroup might be more accurate semantically.

5. **Cooperative matrix vs cooperative vector (Part D/E scope). — RESOLVED: both in scope.** Cooperative **matrix** (Part D, broadly portable) *and* cooperative **vector** (Part E, the neural matrix×vector path) are both planned, with proper feature gating (Part F) so the narrower coopvec path is rejected gracefully where unsupported. The developer's **RTX 50-class GPU** makes both the Vulkan `VK_NV_cooperative_vector` and HLSL `dx::linalg` (SM 6.9) paths testable on real hardware, so co-scoping is no longer blocked on HW access.

   *Follow-up — RESOLVED: Metal emulates coopvec.* Rather than gate it off, Metal lowers `coopvec` onto `simdgroup_matrix` (a subgroup coopvec is a GEMM with the lane axis as the batch dimension — E.6.1), so neural shaders run on Apple7+ too. `coopVector = true` on Apple7+. Remaining open detail is only the spike outcome: whether the **fragment** stage supports `simdgroup_matrix` on the target families, or whether Metal-emulated coopvec is compute-only.

6. **`coopmat` shape portability vs. performance.** The portable default tile (`16×16×16` / `8×8×8`, `half→float`) runs everywhere but is not always the fastest shape on a given vendor. Should the engine expose a "pick the best supported shape" helper over `coopMatrixShapes`, or leave shape selection entirely to the shader author (who then writes per-device variants)? Leaning toward a helper, since the device list is already enumerated — but it adds a small selection API.

7. **HLSL Wave Matrix availability (the D.6 spike).** This is the one genuinely open *feasibility* question, not just a design choice: does the DXC revision this repo vendors still expose Wave Matrix, and under which type names? The answer determines whether Part D ships on all three backends or Vulkan+Metal only with HLSL deferred. Must be resolved before Part D starts.
