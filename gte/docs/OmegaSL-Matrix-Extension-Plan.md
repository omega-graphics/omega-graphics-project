# OmegaSL Matrix Extension Plan

## Current State

OmegaSL supports three square matrix types: `float2x2`, `float3x3`, `float4x4` with constructors `make_float2x2/3x3/4x4`. Matrix-vector multiplication works via the `*` operator (pass-through to backends), and basic arithmetic (`+`, `-`, scalar `*`) works. No matrix-specific builtin functions exist (`transpose`, `determinant`, etc.).

## Goal

Extend OmegaSL with the full set of matrix types and operations that are portable across all three target languages (HLSL, MSL, GLSL).

## Backend Comparison

### Matrix types

All three backends support NxM float matrices for N,M in {2,3,4}:

| OmegaSL | HLSL | MSL | GLSL |
|---------|------|-----|------|
| `float2x2` | `float2x2` | `float2x2` | `mat2` |
| `float2x3` | `float2x3` | `float2x3` | `mat2x3` |
| `float2x4` | `float2x4` | `float2x4` | `mat2x4` |
| `float3x2` | `float3x2` | `float3x2` | `mat3x2` |
| `float3x3` | `float3x3` | `float3x3` | `mat3` |
| `float3x4` | `float3x4` | `float3x4` | `mat3x4` |
| `float4x2` | `float4x2` | `float4x2` | `mat4x2` |
| `float4x3` | `float4x3` | `float4x3` | `mat4x3` |
| `float4x4` | `float4x4` | `float4x4` | `mat4` |

**Naming convention**: OmegaSL uses `floatNxM` matching HLSL/MSL. GLSL uses `matNxM`. The existing codegen already maps `float2x2` → `mat2`, etc.

**Column-major storage**: All three backends default to column-major. OmegaSL adopts this convention — `floatNxM` has N columns of M-element vectors.

### Matrix operations

| Operation | HLSL | MSL | GLSL | Portable? |
|-----------|------|-----|------|-----------|
| `M + M` | `+` | `+` | `+` | Yes |
| `M - M` | `-` | `-` | `-` | Yes |
| `scalar * M` | `*` | `*` | `*` | Yes |
| `M * M` (linear algebra) | `mul(M,M)` | `*` | `*` | **Divergent** — HLSL uses `mul()`, others use `*` |
| `M * v` (matrix-vector) | `mul(M,v)` | `*` | `*` | **Divergent** — same issue |
| `v * M` (vector-matrix) | `mul(v,M)` | `*` | `*` | **Divergent** |
| `M * M` (component-wise) | `*` | N/A | `matrixCompMult()` | Not portable |

### Matrix builtin functions

| Function | HLSL | MSL | GLSL | Portable? |
|----------|------|-----|------|-----------|
| `transpose(M)` | Yes | Yes | Yes | **Yes** |
| `determinant(M)` | No (manual) | Yes | Yes | **Emittable** — generate inline for HLSL |
| `inverse(M)` | No (manual) | No (manual) | Yes | **Emittable** — generate inline for HLSL/MSL |

### Column/element access

| Access | HLSL | MSL | GLSL |
|--------|------|-----|------|
| Column: `M[i]` | Returns column vector | Returns column vector | Returns column vector |
| Element: `M[i][j]` | Element at column i, row j | Element at column i, row j | Element at column i, row j |

Already works in OmegaSL via `IndexExpr`.

## Proposed Changes

### Phase 1: Non-square matrix types

Add the 6 missing rectangular matrix types:

**New types**: `float2x3`, `float2x4`, `float3x2`, `float3x4`, `float4x2`, `float4x3`

**New constructors**: `make_float2x3`, `make_float2x4`, `make_float3x2`, `make_float3x4`, `make_float4x2`, `make_float4x3`

Files:
- `Toks.def`: Add `KW_TY_FLOAT2X3`, `KW_TY_FLOAT2X4`, `KW_TY_FLOAT3X2`, `KW_TY_FLOAT3X4`, `KW_TY_FLOAT4X2`, `KW_TY_FLOAT4X3`
- `Lexer.cpp`: Add to `isKeywordType`
- `AST.h`/`AST.cpp`: Register builtin types and constructor FuncTypes
- `AST.def`: Add `BUILTIN_MAKE_FLOAT*` macros
- `Sema.cpp`: Add to `builtinsTypeMap` and `builtinFunctionMap`
- `HLSLCodeGen.cpp`: Map to `float2x3`, etc.
- `MetalCodeGen.cpp`: Map to `float2x3`, etc.
- `GLSLCodeGen.cpp`: Map to `mat2x3`, `mat2x4`, `mat3x2`, `mat3x4`, `mat4x2`, `mat4x3`

### Phase 2: Matrix multiplication codegen

The `*` operator between matrices (and between matrix and vector) currently passes through to the target language. This works for MSL and GLSL but **not for HLSL**, where `M * N` does Hadamard (component-wise) multiplication, and `mul(M, N)` does linear algebra multiplication.

**Problem**: `float4x4 mvp = proj * view * model;` generates correct code for Metal/GLSL but incorrect code for HLSL.

**Solution**: The Sema already knows the types of both sides of a `BinaryExpr`. When both operands are matrix types (or one is matrix and the other is vector), the HLSL codegen should emit `mul(lhs, rhs)` instead of `(lhs * rhs)`.

Files:
- `HLSLCodeGen.cpp` `generateExpr` for `BINARY_EXPR`: check if `*` operand types are matrices/vectors, emit `mul()` wrapper
- `Sema.cpp`: Add type inference for matrix * matrix (result dimensions: NxM * MxP = NxP), matrix * vector, vector * matrix

This requires the Sema to propagate type information to the codegen for binary expressions. Options:
- **Option A**: Store resolved types on AST `Expr` nodes (new field `TypeExpr *resolvedType`)
- **Option B**: Have the HLSL codegen call back into the Sema's type resolver
- **Option C**: Annotate `BinaryExpr` with a flag during Sema indicating it's a matrix multiply

Option A is cleanest — it also benefits constant folding and other future passes.

### Phase 3: Matrix builtin functions

**`transpose(M)`** — portable across all three backends. Register as a math intrinsic in Sema. Return type is the transposed matrix type (e.g. `transpose(float3x4)` returns `float4x3`). Straightforward pass-through.

**`determinant(M)`** — supported in MSL and GLSL, **not in HLSL**. Two options:

- **Option A (recommended)**: Emit inline determinant calculation for HLSL. For `float2x2`: `a*d - b*c`. For `float3x3`: cofactor expansion (6 multiplications). For `float4x4`: Laplace expansion via 3x3 cofactors.
- **Option B**: Emit a helper function in the HLSL output.

Return type is always `float`.

**`inverse(M)`** — supported **only in GLSL**. Needs inline generation for both HLSL and MSL.

- For `float2x2`: `(1/det) * adj` where adj is the 2x2 adjugate (4 elements).
- For `float3x3`: adjugate via cofactors (9 cofactors) divided by determinant.
- For `float4x4`: 4x4 adjugate (16 cofactors) divided by determinant. This generates ~100 operations — significant but standard for shader compilers.

Return type is the same matrix type.

**`inverse` should be clearly documented as expensive** — developers should prefer pre-computing inverses on the CPU when possible.

### Phase 4 (optional): Additional matrix functions

These are GLSL-only and would need emulation on HLSL/MSL:

- **`outerProduct(v, u)`** — vector outer product producing a matrix. Emitable as column-vector construction.
- **`matrixCompMult(M1, M2)`** — component-wise multiplication. HLSL uses `*` natively; MSL/GLSL would need element-wise code or the GLSL builtin.

Low priority — rarely used in practice.

## Implementation Order

| Step | Description | Difficulty |
|------|-------------|------------|
| 1 | Add 6 non-square matrix types + constructors | Low — mechanical, follows existing pattern |
| 2 | Register `transpose` as math intrinsic with transposed return type | Low — add to intrinsic table |
| 3 | Register `determinant` as intrinsic (returns `float`), emit inline for HLSL | Medium — needs HLSL-specific codegen |
| 4 | Add resolved type tracking to Expr nodes | Medium — AST change with Sema integration |
| 5 | HLSL `mul()` emission for matrix `*` operations | Medium — requires type-aware codegen |
| 6 | Register `inverse` with inline generation for HLSL and MSL | High — substantial inline math generation |

Steps 1-3 are independent and can be done in any order. Steps 4-5 are a paired change. Step 6 depends on step 3 (reuses determinant).

## Type Inference Rules for Matrix Operations

For the Sema to validate matrix expressions:

| Expression | LHS | RHS | Result |
|------------|-----|-----|--------|
| `M * N` | `floatAxB` | `floatBxC` | `floatAxC` |
| `M * v` | `floatAxB` | `floatB` | `floatA` |
| `v * M` | `floatA` | `floatAxB` | `floatB` |
| `M + M` | `floatAxB` | `floatAxB` | `floatAxB` (must match) |
| `s * M` | `float` | `floatAxB` | `floatAxB` |
| `transpose(M)` | — | `floatAxB` | `floatBxA` |
| `determinant(M)` | — | `floatNxN` | `float` (square only) |
| `inverse(M)` | — | `floatNxN` | `floatNxN` (square only) |

## Risks

- **HLSL `mul()` wrapping** is the most complex change because it requires type-aware codegen. If the type information isn't propagated to the codegen layer, the HLSL backend can't distinguish `float * float` from `mat4 * mat4` to decide whether to emit `*` or `mul()`.
- **`inverse()` for 4x4** generates a lot of inline code. Consider whether the code bloat is acceptable or whether a helper function should be emitted instead.
- **Non-square matrix constructors** need clear documentation on column-vs-row ordering to avoid confusion.
