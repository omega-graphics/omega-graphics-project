# OmegaSL Completion Plan

## Current State

OmegaSL is a cross-platform shading language that transpiles to HLSL, MSL, and GLSL (Vulkan / SPIR-V). The compiler (`omegaslc`) and an optional runtime compilation path (`OmegaSLCompiler`) both exist and can compile simple vertex, fragment, and compute shaders end-to-end for all three backends.

### What works

- **Lexer**: tokenizes keywords, operators, identifiers, numeric and string literals, comments.
- **Parser**: parses global declarations (struct, resource, shader), expressions (binary, unary, call, member access, index, literals, identifiers), variable declarations, and return statements inside shader bodies.
- **Semantic analysis (`Sem`)**: resolves types, validates builtin function arguments (`make_float2/3/4`, `sample`, `write`, `dot`, `cross`), resolves swizzle member access on vector types, builds vertex input descriptors and resource layout descriptors.
- **Code generation**: HLSL, MSL, and GLSL backends all emit struct declarations, resource bindings, shader entry points, and function bodies for the supported statement and expression types.
- **Offline compilation**: `omegaslc` invokes platform compilers (`dxc` / `xcrun metal` / `glslc`) and serialises compiled bytecode + layout metadata into `.omegasllib` binary archives.
- **Runtime compilation**: `OmegaSLCompiler::compile()` parses OmegaSL, generates target source, and compiles in-process via `D3DCompile` / `newLibraryWithSource:` / `shaderc`.
- **Types**: `void`, `int`, `uint`, `uint2`, `uint3`, `float`, `float2`, `float3`, `float4`, `double`, `double2`, `double3`, `double4`, `buffer<T>`, `texture1d/2d/3d`, `sampler2d/3d`.
- **Shader stages**: vertex, fragment, compute.
- **Builtins**: `make_float2/3/4`, `dot`, `cross`, `sample`, `write`. Math functions (`cos`, `sin`, `sqrt`, etc.) pass through to the target language.

### What is missing or broken

| Area | Issue |
|------|-------|
| **Control flow** | `if`/`else`/`for`/`while` are defined as tokens and AST node IDs (`IF_DECL`, `ELSE_DECL`, `ELSEIF_DECL`) but are **never parsed**. No AST node structs exist for them in `AST.h`. Shaders requiring any branching or looping cannot be written. |
| **Bool type** | No `bool` type. Cannot express boolean variables or conditions. |
| **Matrix types** | No `float2x2`, `float3x3`, `float4x4` (or equivalent). Matrix operations (multiply, transpose, inverse) are not supported. |
| **Integer vector constructors** | No `make_int2/3/4` or `make_uint2/3/4`. |
| **`read` builtin** | Declared in `AST.h` (`builtins::read`) and in `AST.def` but **not implemented** in any codegen backend. |
| **`sampler1d`** | Token (`KW_TY_SAMPLER1D`) exists but no builtin type is registered. |
| **HLSL `RW_TEXTURE2D` typo** | `HLSLCodeGen.cpp` line 13: `#define RW_TEXTURE2D "RWTexture1D"` — should be `"RWTexture2D"`. |
| **`UnaryOpExpr` codegen** | Parsed by the parser (prefix `++`/`--`/`!`/`-`, postfix `++`/`--`) but **no codegen backend handles `UNARY_EXPR`**. |
| **`PointerExpr` codegen** | AST node exists but codegen does not handle `POINTER_EXPR`. |
| **Error handling** | `Error.h`/`Error.cpp` define `ErrorLoc`, `Error`, `SourceFile`, `DiagnosticEngine` but all methods are empty stubs. Errors go to `std::cout` with no source location. |
| **Lexer source tracking** | `Tok` has commented-out `line`/`colStart`/`colEnd` fields — never populated. |
| **Semantic validation gaps** | `make_float4()` argument checks are incomplete (TODO at Parser.cpp:562). Type-checking for function calls is a TODO (line 705). Struct field uniqueness (line 794) and parameter uniqueness (line 892) checks are missing. Unexpected keyword error is a TODO (line 1303). |
| **`InterfaceGen`** | The C++ struct header generator (`InterfaceGen`) is fully commented out in `CodeGen.h`. No `interface.h` / `structs.h` output. |
| **Tessellation stages** | No `tessellation_control` / `tessellation_evaluation` (or `hull` / `domain`) shader stages. |
| **Preprocessor** | No `#include`, `#define`, `#ifdef`, or any preprocessor directives. |
| **User functions** | `FuncDecl` exists in the AST but non-shader functions are not fully supported (no cross-function calls, no function overloading). |
| **Array types** | No array variable declarations (e.g. `float arr[4]`). |
| **Casting / conversion** | No explicit type casts. |
| **Vulkan tess shaders** | `VulkanTessSpirv.inc` contains hand-maintained SPIR-V placeholders rather than compiled OmegaSL. |
| **Compiler tests** | No dedicated unit or integration tests for the compiler; correctness is only verified indirectly via GTE app tests. |

---

## Phase 1: Core Language Completeness

These changes make OmegaSL capable of expressing real-world shaders.

### 1.1 Control flow: `if` / `else if` / `else`

- **AST**: add `IfStmt` (condition expr, then-block, optional else-if chain, optional else-block) to `AST.h`.
- **Parser**: in `parseGenericDecl` or `parseStmt`, when `first_tok.str == KW_IF`, parse the condition expression in parentheses, then parse the then-block. Loop to consume `else if` / `else` branches. Produce an `IfStmt` node.
- **Sem**: type-check the condition (must be bool or numeric/implicit-bool).
- **Codegen** (all 3 backends): emit `if (...) { ... } else if (...) { ... } else { ... }`. The syntax is identical in HLSL, MSL, and GLSL.
- **Files**: `AST.h`, `AST.def`, `Parser.cpp`, `HLSLCodeGen.cpp`, `MetalCodeGen.cpp`, `GLSLCodeGen.cpp`.

### 1.2 Control flow: `for` loop

- **AST**: add `ForStmt` (init decl/expr, condition expr, increment expr, body block).
- **Parser**: when `first_tok.str == KW_FOR`, parse `(init; condition; increment)` then the body block.
- **Sem**: validate init, condition, increment types.
- **Codegen**: emit `for (init; cond; inc) { ... }` — identical syntax on all targets.
- **Files**: same as 1.1.

### 1.3 Control flow: `while` loop

- **AST**: add `WhileStmt` (condition expr, body block).
- **Parser**: when `first_tok.str == KW_WHILE`, parse `(condition)` then body block.
- **Codegen**: emit `while (cond) { ... }`.
- **Files**: same as 1.1.

### 1.4 Bool type

- **AST**: add `builtins::bool_type`.
- **Lexer / Token defs**: add `KW_TY_BOOL` (`"bool"`).
- **Parser / Sem**: recognise `bool` as a type keyword, resolve it.
- **Codegen**: emit `bool` on all three backends.
- **Files**: `Toks.def`, `AST.h`, `AST.cpp`, `Lexer.cpp`, `Parser.cpp`, codegen files.

### 1.5 Matrix types

Add `float2x2`, `float3x3`, `float4x4` (plus integer and double variants as needed).

- **AST**: add `builtins::float2x2_type`, `float3x3_type`, `float4x4_type`.
- **Tokens**: add `KW_TY_FLOAT2X2`, `KW_TY_FLOAT3X3`, `KW_TY_FLOAT4X4`.
- **Codegen**:
  - HLSL: `float2x2`, `float3x3`, `float4x4`.
  - MSL: `float2x2`, `float3x3`, `float4x4`.
  - GLSL: `mat2`, `mat3`, `mat4`.
- Add constructor builtins (e.g. `make_float4x4(...)`) or support direct construction syntax.
- **Files**: `Toks.def`, `AST.h`, `AST.cpp`, `Lexer.cpp`, `Parser.cpp`, codegen files.

### 1.6 Missing expression codegen

- **`UNARY_EXPR`**: add codegen for prefix/postfix unary operators in all three backends. Emit the operator and operand in the correct order.
- **`POINTER_EXPR`**: add codegen for address-of (`&`) and dereference (`*`) if pointer semantics are desired, or remove from the AST if not needed.
- **Files**: `HLSLCodeGen.cpp`, `MetalCodeGen.cpp`, `GLSLCodeGen.cpp`.

### 1.7 Integer vector constructors

- Add `make_int2/3/4`, `make_uint2/3/4` builtins and wire them through Sem and all codegen backends (emit `int2(...)`, `uint3(...)`, etc.).
- **Files**: `AST.h`, `AST.cpp`, `Parser.cpp` (Sem), codegen files.

### 1.8 `read` builtin

- Implement codegen for `read(texture, coord)`:
  - HLSL: `texture[coord]` or `texture.Load(coord)`.
  - MSL: `texture.read(coord)`.
  - GLSL: `imageLoad(texture, coord)`.
- **Files**: codegen files, `Parser.cpp` (Sem validation).

### 1.9 `sampler1d`

- Register `builtins::sampler1d_type` in `AST.cpp`.
- Add codegen for 1D sampler binding and `sample` calls with `sampler1d` + `texture1d` + `float` coord.
- **Files**: `AST.h`, `AST.cpp`, codegen files.

### 1.10 Bug fix: HLSL `RW_TEXTURE2D`

- Change `HLSLCodeGen.cpp` line 13 from `"RWTexture1D"` to `"RWTexture2D"`.
- **Files**: `HLSLCodeGen.cpp`.

---

## Phase 2: Error Handling and Diagnostics

### 2.1 Source location tracking

- **Lexer**: populate `line`, `colStart`, `colEnd` on every `Tok` (uncomment and implement the tracking in `Lexer.cpp`).
- **AST**: propagate source location through `Stmt` / `Expr` / `Decl` nodes.
- **Files**: `Lexer.h`, `Lexer.cpp`, `AST.h`.

### 2.2 Diagnostic engine

- Implement `SourceFile::buildLinePosMap`, `toLine`, `toCol`.
- Implement `DiagnosticEngine::generateCodeView` to print source context with underlined error span (similar to Clang/Rust diagnostics).
- Replace all `std::cout << "error..."` calls in `Parser.cpp` (Sem) and codegen with proper `DiagnosticEngine` invocations that include source location.
- **Files**: `Error.h`, `Error.cpp`, `Parser.cpp`, codegen files.

### 2.3 Structured error types

- Define concrete `Error` subclasses for common issues: `TypeError`, `UndeclaredIdentifier`, `DuplicateDeclaration`, `ArgumentCountMismatch`, `UnexpectedToken`, `InvalidAttribute`.
- Accumulate errors instead of aborting on the first one; report all at the end (or after a threshold).
- Return a non-zero exit code from `omegaslc` on error.
- **Files**: `Error.h`, `Error.cpp`, `Parser.cpp`, `main.cpp`.

### 2.4 Complete semantic validation

Address the five known TODOs in `Parser.cpp`:

1. **Line 562**: finish `make_float4()` argument checks for all overload patterns (e.g. `make_float4(float2, float, float)`).
2. **Line 705**: implement general function call type-checking (validate argument types against function parameter types).
3. **Line 794**: enforce struct field name uniqueness.
4. **Line 892**: enforce function parameter name uniqueness.
5. **Line 1303**: emit a proper error on unexpected keyword in block context.

- **Files**: `Parser.cpp`.

---

## Phase 3: Language Extensions

### 3.1 User-defined functions

- Allow non-shader `FuncDecl` functions that can be called from shaders or other functions.
- Parser already has `FuncDecl`; ensure the codegen backends emit helper functions above the shader entry point when referenced.
- Add function overload resolution if multiple functions share a name.
- **Files**: `Parser.cpp`, `AST.h`, codegen files.

### 3.2 Explicit type casts

- Support cast syntax (e.g. `(float)intVal` or a functional cast `float(intVal)`).
- Codegen: HLSL/MSL use functional casts directly; GLSL uses constructor syntax.
- **Files**: `Parser.cpp`, `AST.h` (new `CastExpr`), codegen files.

### 3.3 Array declarations

- Support fixed-size array variables: `float arr[4]`.
- Support array indexing (already handled by `IndexExpr`).
- Codegen: direct mapping on all backends.
- **Files**: `Parser.cpp`, `AST.h` (extend `VarDecl` or `TypeExpr`), codegen files.

### 3.4 Preprocessor directives

- Implement a lightweight preprocessor pass before lexing:
  - `#define NAME VALUE` — simple text macros (no function-like macros initially).
  - `#ifdef` / `#ifndef` / `#endif` — conditional compilation.
  - `#include "file.omegasl"` — file inclusion.
- This enables sharing struct definitions across shader files and conditional platform code.
- **Files**: new `Preprocessor.h`/`.cpp`, integrate before `Lexer` in `Parser::parseContext` and `main.cpp`.

### 3.5 Tessellation shader stages

- Add `hull` (or `tessellation_control`) and `domain` (or `tessellation_evaluation`) shader types.
- Tokens: `KW_HULL`, `KW_DOMAIN` (or `KW_TESS_CONTROL`, `KW_TESS_EVAL`).
- AST: extend `ShaderDecl::Type` with the new stages.
- Codegen:
  - HLSL: emit `[domain("tri")]`, `[partitioning("...")]`, `[outputtopology("...")]`, `[outputcontrolpoints(...)]`, `[patchconstantfunc("...")]` attributes and hull/domain shader signatures.
  - MSL: emit `[[patch(triangle, N)]]` kernel signatures.
  - GLSL: emit `layout(vertices = N) out;` for tess control and `layout(triangles, ...) in;` for tess evaluation.
- **Files**: `Toks.def`, `AST.h`, `AST.def`, `Parser.cpp`, codegen files.

---

## Phase 4: Codegen Quality and Backend Parity

### 4.1 GLSL combined image-sampler support

- GLSL (Vulkan) uses combined image-samplers (`sampler2D`) differently from the separate texture + sampler model in HLSL/MSL. The current GLSL codegen emits separate `sampler` and `image2D`/`texture2D` bindings. Ensure that the `sample()` builtin correctly combines them using `sampler2D(texture, sampler)` in the generated GLSL.
- **Files**: `GLSLCodeGen.cpp`.

### 4.2 Consistent `generateBlock` handling of control flow

- Once `if`/`for`/`while` AST nodes exist, ensure all three `generateBlock` implementations dispatch to the correct codegen for control-flow statements (not just `VAR_DECL` / `RETURN_DECL` / expressions).
- **Files**: `HLSLCodeGen.cpp`, `MetalCodeGen.cpp`, `GLSLCodeGen.cpp`.

### 4.3 Operator precedence

- The parser currently builds binary expression trees without explicit precedence handling. Implement standard operator precedence (multiplicative > additive > comparison > logical) using a precedence-climbing or Pratt parser approach.
- **Files**: `Parser.cpp`.

### 4.4 Constant folding (optional)

- Evaluate constant expressions at compile time (e.g. `2.0 * 3.14159` → `6.28318`) to simplify generated code.
- **Files**: new optimization pass or integrated into codegen.

### 4.5 Vulkan tessellation shaders from OmegaSL

- Replace the hand-maintained SPIR-V in `VulkanTessSpirv.inc` with shaders compiled from `gte/src/shaders/*.omegasl` via `omegaslc` (once compute shaders compile cleanly to GLSL/SPIR-V for Vulkan).
- **Files**: `VulkanTEContext.cpp`, `VulkanTessSpirv.inc`, CMake build rules.

---

## Phase 5: Tooling and Testing

### 5.1 Compiler unit tests

- Create a test suite under `gte/omegasl/tests/` that:
  - Lexes sample inputs and asserts token sequences.
  - Parses sample inputs and asserts AST structure (or at least no errors).
  - Compiles sample shaders to each backend and checks that the output compiles with the respective platform compiler (or at minimum that codegen does not crash and produces non-empty output).
  - Tests error cases: invalid syntax, type mismatches, undeclared identifiers — asserts that appropriate errors are emitted.
- Use CTest or a simple test runner invoked from CMake.
- **Files**: `gte/omegasl/tests/`, `gte/CMakeLists.txt`.

### 5.2 Golden-file tests for codegen

- For each backend, maintain golden output files (e.g. `tests/golden/rect_shader.hlsl`). After codegen, diff against the golden file. This catches unintended regressions.
- **Files**: `gte/omegasl/tests/golden/`.

### 5.3 `InterfaceGen` revival (optional)

- Uncomment and complete the `InterfaceGen` class to emit a C++ header (`interface.h` / `structs.h`) with struct layouts matching the OmegaSL declarations. This lets C++ code share struct definitions with shaders without manual duplication.
- **Files**: `CodeGen.h`, new `InterfaceGen.cpp`.

### 5.4 Language reference documentation

- Write a concise OmegaSL language reference covering:
  - Types (scalar, vector, matrix, resource, sampler).
  - Declarations (struct, resource, shader, function).
  - Statements (variable, return, if/else, for, while).
  - Expressions (arithmetic, comparison, logical, call, member, index, unary, cast).
  - Builtins (constructors, math pass-throughs, `sample`, `read`, `write`, `dot`, `cross`).
  - Attributes (`VertexID`, `Position`, `TexCoord`, etc.).
  - Resource maps (`[in ...]`, `[out ...]`, `[inout ...]`).
  - Static samplers.
- Update `gte/docs/OmegaSL.rst` or create `gte/docs/OmegaSL-Reference.md`.

---

## Implementation Order

| Step | Phase | Description |
|------|-------|-------------|
| 1 | 1.10 | Bug fix: HLSL `RW_TEXTURE2D` typo. |
| 2 | 1.6 | Codegen for `UNARY_EXPR` (all backends). |
| 3 | 1.4 | `bool` type. |
| 4 | 1.1–1.3 | Control flow (`if`/`else`, `for`, `while`). |
| 5 | 1.5 | Matrix types (`float2x2/3x3/4x4`). |
| 6 | 1.7 | Integer vector constructors (`make_int2/3/4`, `make_uint2/3/4`). |
| 7 | 1.8–1.9 | `read` builtin and `sampler1d`. |
| 8 | 2.1–2.2 | Source location tracking and diagnostic engine. |
| 9 | 2.3–2.4 | Structured errors and semantic validation completion. |
| 10 | 3.1 | User-defined functions. |
| 11 | 3.2–3.3 | Type casts and array declarations. |
| 12 | 4.1–4.3 | Codegen quality (GLSL image-samplers, control flow dispatch, operator precedence). |
| 13 | 3.4 | Preprocessor. |
| 14 | 3.5 | Tessellation shader stages. |
| 15 | 4.5 | Vulkan tess shaders from OmegaSL. |
| 16 | 5.1–5.2 | Compiler unit tests and golden-file tests. |
| 17 | 5.4 | Language reference documentation. |

Steps 1–7 are the critical path: they make OmegaSL functional enough to write real shaders with branching, loops, matrices, and all resource types. Steps 8–9 make the compiler usable by producing actionable error messages. Steps 10+ are extensions that round out the language.

---

## Summary

| Area | Current | Target |
|------|---------|--------|
| **Control flow** | Not parsed | `if`/`else if`/`else`, `for`, `while` |
| **Types** | Scalars, vectors, resources | + `bool`, matrices (`float2x2/3x3/4x4`), arrays |
| **Builtins** | `make_float2/3/4`, `sample`, `write`, `dot`, `cross` | + `read`, `make_int/uint` variants, matrix constructors |
| **Expressions** | Binary, call, member, index, literal | + unary codegen, pointer codegen, casts |
| **Shader stages** | vertex, fragment, compute | + hull/domain (tessellation) |
| **Error handling** | `std::cout` messages, no source locations | Structured errors with source context |
| **Semantic checks** | Partial (5 TODOs) | Complete type-checking and validation |
| **Preprocessor** | None | `#define`, `#ifdef`, `#include` |
| **User functions** | AST exists, not fully wired | Full support with cross-function calls |
| **Testing** | None (indirect via GTE apps) | Unit tests, golden-file tests |
| **Documentation** | 1-line README | Language reference |
