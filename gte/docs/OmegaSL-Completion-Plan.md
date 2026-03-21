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

| Area | Status | Issue |
|------|--------|-------|
| **Control flow** | **DONE** | ~~`if`/`else`/`for`/`while` are never parsed.~~ `IfStmt`, `ForStmt`, `WhileStmt` AST nodes exist. Parser and all three codegen backends handle them. Used in production tessellation shaders. |
| **Bool type** | **DONE** | ~~No `bool` type.~~ `bool` is a recognized type keyword, parsed and emitted by all backends. |
| **Matrix types** | **DONE** | ~~No `float2x2`, `float3x3`, `float4x4`.~~ Parser supports these types. Constructor builtins `make_float2x2/3x3/4x4` exist. GLSL emits `mat2/3/4`. |
| **Integer vector constructors** | **DONE** | ~~No `make_int2/3/4` or `make_uint2/3/4`.~~ All integer and unsigned vector constructors are implemented in parser and codegen. |
| **`read` builtin** | Open | Declared in `AST.h` (`builtins::read`) and in `AST.def` but **not implemented** in any codegen backend. |
| **`sampler1d`** | Open | Token (`KW_TY_SAMPLER1D`) exists but no builtin type is registered. |
| **HLSL `RW_TEXTURE2D` typo** | Open | `HLSLCodeGen.cpp` line 13: `#define RW_TEXTURE2D "RWTexture1D"` — should be `"RWTexture2D"`. |
| **`UnaryOpExpr` codegen** | Open | Parsed by the parser (prefix `++`/`--`/`!`/`-`, postfix `++`/`--`) but **no codegen backend handles `UNARY_EXPR`**. |
| **Postfix unary parser bug** | **FIXED** | Parser consumed ALL `TOK_OP` tokens in the postfix unary check but only processed `++`/`--`. The `=` operator was silently eaten, breaking all assignment expressions in shader bodies. Fixed 2026-03-21. |
| **Compound assignment operators** | Open | `+=`, `-=`, `/=` are lexed as tokens (`OP_PLUSEQUAL`, `OP_MINUSEQUAL`, `OP_DIVEQUAL`) but not handled in `getBinaryPrecedence`. `*=` has no token definition. |
| **Bare `return;`** | Open | `return;` (void return without expression) is not supported by the parser — it always expects an expression after `return`. |
| **`PointerExpr` codegen** | Open | AST node exists but codegen does not handle `POINTER_EXPR`. |
| **Error handling** | Open | `Error.h`/`Error.cpp` define `ErrorLoc`, `Error`, `SourceFile`, `DiagnosticEngine` but all methods are empty stubs. Errors go to `std::cout` with no source location. |
| **Lexer source tracking** | Open | `Tok` has commented-out `line`/`colStart`/`colEnd` fields — never populated. |
| **Semantic validation gaps** | Open | `make_float4()` argument checks are incomplete. Type-checking for function calls is a TODO. Struct field uniqueness and parameter uniqueness checks are missing. |
| **`InterfaceGen`** | Open | The C++ struct header generator (`InterfaceGen`) is fully commented out in `CodeGen.h`. No `interface.h` / `structs.h` output. |
| **Tessellation stages** | Open | `hull` / `domain` shader types declared in AST but no parser validation or codegen test cases. |
| **Preprocessor** | Open | No `#include`, `#define`, `#ifdef`, or any preprocessor directives. |
| **User functions** | Open | `FuncDecl` exists in the AST but non-shader functions are not fully supported (no cross-function calls, no function overloading). |
| **Array types** | Open | No array variable declarations (e.g. `float arr[4]`). |
| **Casting / conversion** | Open | `CastExpr` exists in AST and codegen, but parser support for C-style casts is incomplete. |
| **Vulkan tess shaders** | Open | `VulkanTessSpirv.inc` contains hand-maintained SPIR-V placeholders rather than compiled OmegaSL. |
| **Compiler tests** | **PARTIAL** | CTest suite exists with tokenizer, compile, error, and HLSL golden-file tests. Expanded with operators, control flow, vector math, resource types, and gradient compute kernel tests (2026-03-21). |

---

## Phase 1: Core Language Completeness

These changes make OmegaSL capable of expressing real-world shaders.

### 1.1 Control flow: `if` / `else if` / `else` — DONE

- `IfStmt` AST node with condition, then-block, else-if chain, and else-block.
- Parser handles `if`/`else if`/`else` blocks.
- All three codegen backends emit correct control flow.
- Used in production tessellation shaders (`tess_path2d.omegasl`, `tess_rounded_rect.omegasl`).

### 1.2 Control flow: `for` loop — DONE

- `ForStmt` AST node with init, condition, increment, and body block.
- Parser handles full `for(init; cond; inc)` syntax.
- All three codegen backends emit correct for loops.

### 1.3 Control flow: `while` loop — DONE

- `WhileStmt` AST node with condition and body block.
- Parser and all codegen backends handle while loops.

### 1.4 Bool type — DONE

- `bool` is a recognized type keyword, parsed, and emitted by all backends.

### 1.5 Matrix types — DONE

- Parser supports `float2x2`, `float3x3`, `float4x4`.
- Constructor builtins `make_float2x2`, `make_float3x3`, `make_float4x4` exist.
- GLSL codegen emits `mat2`, `mat3`, `mat4`.

### 1.6 Missing expression codegen

- **`UNARY_EXPR`**: add codegen for prefix/postfix unary operators in all three backends. Emit the operator and operand in the correct order.
- **`POINTER_EXPR`**: add codegen for address-of (`&`) and dereference (`*`) if pointer semantics are desired, or remove from the AST if not needed.
- **Files**: `HLSLCodeGen.cpp`, `MetalCodeGen.cpp`, `GLSLCodeGen.cpp`.

### 1.7 Integer vector constructors — DONE

- `make_int2/3/4` and `make_uint2/3/4` builtins implemented in parser and all codegen backends.

### 1.8 `read` builtin — DONE

- Codegen already existed in all three backends:
  - HLSL: `texture.Load(coord)`
  - MSL: `texture.read(coord)`
  - GLSL: `texelFetch(texture, coord, 0)`
- Added semantic validation in Parser.cpp: validates 2 arguments, checks texture type (1d/2d/3d) matches coordinate type (int/uint, int2/uint2, int3/uint3).

### 1.9 `sampler1d` — DONE

- `builtins::sampler1d_type` was already registered in `AST.h`/`AST.cpp`.
- Codegen already handled `sampler1d` in all three backends (GLSL: `sampler1D`, HLSL: `SamplerState`, Metal: sampler binding).
- Added `sampler1d_type` to the `sample()` semantic validation: accepts `sampler1d` + `texture1d` + `float` coordinate.

### 1.10 Bug fix: HLSL `RW_TEXTURE2D` — DONE (was already correct)

- `HLSLCodeGen.cpp` line 13 already has `#define RW_TEXTURE2D "RWTexture2D"`. The plan's description was stale.

---

## Phase 2: Error Handling and Diagnostics — DONE

### 2.1 Source location tracking — DONE

- Lexer populates `line`, `colStart`, `colEnd` on every `Tok` via `advanceChar()` tracking and `PUSH_TOK` macro.
- Fixed `SEEK_TO_NEXT_CHAR()` to update `currentCol` (was missing, causing incorrect column numbers on multi-char operators).
- `Tok` struct carries location; `Stmt` base has `std::optional<ErrorLoc> loc`.

### 2.2 Diagnostic engine — DONE

- `SourceFile::buildLinePosMap`, `toLine`, `toCol`, `getLine` all implemented.
- `DiagnosticEngine::generateCodeView` prints source context with `^` underline spans.
- `DiagnosticEngine::report` prints all accumulated errors with `(line:col)` locations.
- All `std::cout` error messages in `Parser.cpp` converted to `diagnostics->addError()`.
- All debug trace `std::cout` messages removed (token dumps, parse state, semantic progress).

### 2.3 Structured error types — DONE

- Error subclasses: `TypeError`, `UndeclaredIdentifier`, `DuplicateDeclaration`, `ArgumentCountMismatch`, `UnexpectedToken`, `InvalidAttribute` — all defined and used.
- Errors accumulate up to `kMaxErrorsBeforeStop` (50).
- `omegaslc` returns non-zero exit code on errors (line 238-241 in main.cpp).

### 2.4 Semantic validation — DONE

- User-defined function argument count validation with `ArgumentCountMismatch`.
- Struct field type resolution emits `TypeError` when type unknown.
- Resource declaration use-after-free fixed (missing `return nullptr` after `delete`).
- Undefined resource in shader resource map emits `UndeclaredIdentifier` (was a segfault).
- Duplicate struct/resource/shader/parameter name checks emit `DuplicateDeclaration`.

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

### 4.2 Consistent `generateBlock` handling of control flow — DONE

- All three `generateDecl` implementations dispatch `IF_STMT`, `FOR_STMT`, `WHILE_STMT` alongside `VAR_DECL` / `RETURN_DECL` / expressions.

### 4.3 Operator precedence — DONE

- Parser uses precedence-climbing with explicit levels: multiplicative (3) > additive (2) > comparison (1) > assignment (0).
- **Bug fixed 2026-03-21**: postfix unary check was consuming all `TOK_OP` tokens (including `=`) but only processing `++`/`--`. This silently broke assignment expressions in shader bodies.

### 4.4 Constant folding (optional)

- Evaluate constant expressions at compile time (e.g. `2.0 * 3.14159` → `6.28318`) to simplify generated code.
- **Files**: new optimization pass or integrated into codegen.

### 4.5 Vulkan tessellation shaders from OmegaSL

- Replace the hand-maintained SPIR-V in `VulkanTessSpirv.inc` with shaders compiled from `gte/src/shaders/*.omegasl` via `omegaslc` (once compute shaders compile cleanly to GLSL/SPIR-V for Vulkan).
- **Files**: `VulkanTEContext.cpp`, `VulkanTessSpirv.inc`, CMake build rules.

---

## Phase 5: Tooling and Testing

### 5.1 Compiler unit tests — PARTIAL

- CTest suite exists under `gte/omegasl/tests/` with:
  - Tokenizer test (`omegasl_tokens_shaders`)
  - Positive compilation tests: `shaders.omegasl`, `operators.omegasl`, `control_flow.omegasl`, `vector_math.omegasl`, `resource_types.omegasl`, `compute_gradient.omegasl` (linear + radial gradient kernels)
  - Negative tests: `invalid_phase2.omegasl`, `invalid_type_mismatch.omegasl`, `invalid_undefined_resource.omegasl`
- **Remaining**: some test files depend on features not yet implemented (compound assignments, bare `return;`, C-style casts). Tests compile on Vulkan/GLSL backend via runtime shaderc; offline `glslc` path needs the tool on PATH.

### 5.2 Golden-file tests for codegen — PARTIAL

- HLSL golden file infrastructure exists with `RunGoldenCodegenTest.cmake` and tests for `myVertex`, `myFrag`, `myVertex2`, `myFrag2`.
- **Remaining**: GLSL and MSL golden files not yet created.
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

| Step | Phase | Description | Status |
|------|-------|-------------|--------|
| 1 | 1.10 | Bug fix: HLSL `RW_TEXTURE2D` typo. | **DONE** (was already correct) |
| 2 | 1.6 | Codegen for `UNARY_EXPR` (all backends). | Open |
| 3 | 1.4 | `bool` type. | **DONE** |
| 4 | 1.1–1.3 | Control flow (`if`/`else`, `for`, `while`). | **DONE** |
| 5 | 1.5 | Matrix types (`float2x2/3x3/4x4`). | **DONE** |
| 6 | 1.7 | Integer vector constructors (`make_int2/3/4`, `make_uint2/3/4`). | **DONE** |
| 7 | 1.8–1.9 | `read` builtin and `sampler1d`. | **DONE** |
| 8 | 2.1–2.2 | Source location tracking and diagnostic engine. | **DONE** |
| 9 | 2.3–2.4 | Structured errors and semantic validation completion. | **DONE** |
| 10 | 3.1 | User-defined functions. | Open |
| 11 | 3.2–3.3 | Type casts and array declarations. | Open |
| 12 | 4.1–4.3 | Codegen quality (GLSL image-samplers, control flow dispatch, operator precedence). | **MOSTLY DONE** (4.2, 4.3 done; 4.1 open) |
| 13 | 3.4 | Preprocessor. | Open |
| 14 | 3.5 | Tessellation shader stages. | Open |
| 15 | 4.5 | Vulkan tess shaders from OmegaSL. | Open |
| 16 | 5.1–5.2 | Compiler unit tests and golden-file tests. | **PARTIAL** |
| 17 | 5.4 | Language reference documentation. | Open |
| — | — | Parser bug: postfix unary consuming `=` operator. | **FIXED** (2026-03-21) |
| — | — | Compound assignment operators (`+=`, `-=`, `/=`, `*=`). | Open (new) |
| — | — | Bare `return;` (void return without expression). | Open (new) |

Steps 3–6 are complete. Steps 1–2 and 7 remain on the critical path for core language completeness.

---

## Summary

| Area | Current | Target | Status |
|------|---------|--------|--------|
| **Control flow** | `if`/`else if`/`else`, `for`, `while` | Done | **DONE** |
| **Types** | Scalars, vectors, `bool`, matrices, resources | + arrays | Mostly done |
| **Builtins** | `make_float/int/uint 2/3/4`, `sample`, `write`, `dot`, `cross`, matrix constructors | + `read` | Mostly done |
| **Expressions** | Binary, call, member, index, literal | + unary codegen, casts, compound assignment | Partially done |
| **Shader stages** | vertex, fragment, compute | + hull/domain (tessellation) | Open |
| **Error handling** | `std::cout` messages, no source locations | Structured errors with source context | Open |
| **Semantic checks** | Partial (5 TODOs) | Complete type-checking and validation | Open |
| **Preprocessor** | None | `#define`, `#ifdef`, `#include` | Open |
| **User functions** | AST exists, not fully wired | Full support with cross-function calls | Open |
| **Testing** | CTest suite with positive/negative/golden tests | + GLSL/MSL golden files, runtime validation | **PARTIAL** |
| **Documentation** | 1-line README | Language reference | Open |
