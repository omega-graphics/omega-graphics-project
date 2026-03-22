# OmegaSL Completion Plan

## Current State

OmegaSL is a cross-platform shading language that transpiles to HLSL, MSL, and GLSL (Vulkan / SPIR-V). The compiler (`omegaslc`) and an optional runtime compilation path (`OmegaSLCompiler`) both exist and can compile simple vertex, fragment, and compute shaders end-to-end for all three backends.

### What works

- **Lexer**: tokenizes keywords, operators, identifiers, numeric and string literals, comments. Populates `line`/`colStart`/`colEnd` on every token.
- **Preprocessor**: `#define`, `#ifdef`/`#ifndef`/`#endif`, `#include "file"` with depth-limited recursion. Integrated into `omegaslc` before lexing.
- **Parser**: parses global declarations (struct, resource, shader, user function), expressions (binary, unary, call, member access, index, cast, literals, identifiers), variable declarations (including fixed-size arrays), control flow (`if`/`else if`/`else`, `for`, `while`), and return statements inside shader/function bodies.
- **Semantic analysis (`Sem`)**: resolves types, validates builtin function arguments (`make_float2/3/4`, `make_int2/3/4`, `make_uint2/3/4`, `sample`, `read`, `write`, `dot`, `cross`), resolves swizzle member access on vector types, validates struct field/parameter uniqueness, builds vertex input descriptors and resource layout descriptors. Extracted to `Sema.h`/`Sema.cpp` (2026-03-21).
- **Error handling**: `DiagnosticEngine` with structured error types (`TypeError`, `UndeclaredIdentifier`, `DuplicateDeclaration`, `ArgumentCountMismatch`, `UnexpectedToken`, `InvalidAttribute`), source location tracking, and `^` underline code views.
- **Code generation**: HLSL, MSL, and GLSL backends all emit struct declarations, resource bindings, shader entry points, user function definitions, and function bodies for all supported statement and expression types.
- **Offline compilation**: `omegaslc` invokes platform compilers (`dxc` / `xcrun metal` / `glslc`) and serialises compiled bytecode + layout metadata into `.omegasllib` binary archives.
- **Runtime compilation**: `OmegaSLCompiler::compile()` parses OmegaSL, generates target source, and compiles in-process via `D3DCompile` / `newLibraryWithSource:` / `shaderc`.
- **Types**: `void`, `bool`, `int`, `int2/3/4`, `uint`, `uint2/3/4`, `float`, `float2/3/4`, `float2x2/3x3/4x4`, `double`, `double2/3/4`, `buffer<T>`, `texture1d/2d/3d`, `sampler1d/2d/3d`.
- **Shader stages**: vertex, fragment, compute. Hull/domain parsed and partially codegen'd.
- **Builtins**: `make_float2/3/4`, `make_int2/3/4`, `make_uint2/3/4`, `make_float2x2/3x3/4x4`, `dot`, `cross`, `sample`, `read`, `write`. Math functions (`cos`, `sin`, `sqrt`, etc.) pass through to the target language.

### What is missing or broken

| Area | Status | Issue |
|------|--------|-------|
| **Control flow** | **DONE** | ~~`if`/`else`/`for`/`while` are never parsed.~~ `IfStmt`, `ForStmt`, `WhileStmt` AST nodes exist. Parser and all three codegen backends handle them. Used in production tessellation shaders. |
| **Bool type** | **DONE** | ~~No `bool` type.~~ `bool` is a recognized type keyword, parsed and emitted by all backends. |
| **Matrix types** | **DONE** | ~~No `float2x2`, `float3x3`, `float4x4`.~~ Parser supports these types. Constructor builtins `make_float2x2/3x3/4x4` exist. GLSL emits `mat2/3/4`. |
| **Integer vector constructors** | **DONE** | ~~No `make_int2/3/4` or `make_uint2/3/4`.~~ All integer and unsigned vector constructors are implemented in parser and codegen. |
| **`read` builtin** | **DONE** | ~~Not implemented in any codegen backend.~~ All three backends implement `read`: HLSL `texture.Load()`, MSL `texture.read()`, GLSL `texelFetch()`. Semantic validation checks argument count and texture/coordinate type matching. |
| **`sampler1d`** | **DONE** | ~~No builtin type registered.~~ `builtins::sampler1d_type` registered in `AST.cpp`. All three codegen backends handle it. Added to `sample()` semantic validation. |
| **HLSL `RW_TEXTURE2D` typo** | **DONE** | ~~`RWTexture1D` typo.~~ `HLSLCodeGen.cpp` already has `#define RW_TEXTURE2D "RWTexture2D"`. Plan description was stale. |
| **`UnaryOpExpr` codegen** | **DONE** | All three codegen backends handle `UNARY_EXPR` (prefix and postfix). Parser prefix check was missing `-` (negation) — fixed 2026-03-21. |
| **Postfix unary parser bug** | **FIXED** | Parser consumed ALL `TOK_OP` tokens in the postfix unary check but only processed `++`/`--`. The `=` operator was silently eaten, breaking all assignment expressions in shader bodies. Fixed 2026-03-21. |
| **Compound assignment operators** | **DONE** | ~~`+=`, `-=`, `/=` not handled in precedence; `*=` no token.~~ Lexer now produces `++`/`--` tokens (was broken — two separate `+`/`-` tokens before). Added `OP_MULEQUAL` (`*=`) token. All four compound assignments (`+=`, `-=`, `*=`, `/=`) added to `getBinaryPrecedence` at precedence 0. Fixed 2026-03-21. |
| **Bare `return;`** | **DONE** | ~~Parser always expects expression after `return`.~~ Both `parseStmtFromBuffer` and `parseGenericDecl` now check for semicolon/brace/EOF after `return` and produce `ReturnDecl` with `expr = nullptr`. All three codegen backends and Sema handle null expr. Fixed 2026-03-21. |
| **`PointerExpr` codegen** | **DONE** | All three codegen backends handle `POINTER_EXPR` (address-of `&` and dereference `*`). |
| **Error handling** | **DONE** | ~~Empty stubs.~~ `DiagnosticEngine` fully implemented with `addError()`, `generateCodeView()`, `report()`. Source locations tracked via `ErrorLoc`. Structured error types in use throughout `Sema.cpp`. |
| **Lexer source tracking** | **DONE** | ~~`line`/`colStart`/`colEnd` commented out.~~ `Tok` struct has active `line`, `colStart`, `colEnd` fields populated by the lexer. |
| **Semantic validation gaps** | **DONE** | ~~`make_float4()` checks incomplete, no struct field uniqueness.~~ All builtin function argument validation implemented in `Sema.cpp`. Struct field uniqueness, parameter uniqueness, and duplicate declaration checks all present. |
| **`InterfaceGen`** | Open | The C++ struct header generator (`InterfaceGen`) is fully commented out in `CodeGen.h`. No `interface.h` / `structs.h` output. |
| **Tessellation stages** | **PARTIAL** | Parser handles `hull`/`domain` keywords. All three codegen backends have Hull/Domain cases (HLSL emits `[domain("tri")]` attributes, GLSL uses `.tesc`/`.tese` extensions, Metal emits `vertex`). No test cases or full tessellation attribute support (patch topology, control point count, etc.). |
| **Preprocessor** | **DONE** | ~~No directives.~~ `Preprocessor.h`/`.cpp` implements `#define`, `#ifdef`/`#ifndef`/`#endif`, `#include "file"` with depth-limited recursion. Integrated into `main.cpp` before lexing. |
| **User functions** | **DONE** | ~~Not fully supported.~~ Parser, semantic analysis, and all three codegen backends support user-defined functions with cross-function calls. Metal codegen bug fixed 2026-03-21 (missing function name in non-builtin calls). Function overloading not implemented. |
| **Array types** | **DONE** | ~~No array variable declarations.~~ Parser handles `type name[size]` syntax. `TypeExpr::arraySize` field stores the size. All three codegen backends emit `name[size]` in variable declarations. |
| **Casting / conversion** | **DONE** | ~~Parser support incomplete.~~ Parser handles C-style casts `(type)expr` via `CastExpr` AST node. All three codegen backends emit functional-cast syntax `type(expr)`. Sema validates target type exists. |
| **Vulkan tess shaders** | Open | `VulkanTessSpirv.inc` contains hand-maintained SPIR-V placeholders rather than compiled OmegaSL. |
| **Compiler tests** | **PARTIAL** | CTest suite exists with tokenizer, compile, error, and HLSL golden-file tests. Expanded with operators, control flow, vector math, resource types, gradient compute kernel, and user function tests. |

---

## Phase 1: Core Language Completeness — DONE

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

### 1.6 Missing expression codegen — DONE

- **`UNARY_EXPR`**: All three codegen backends already handle prefix/postfix unary operators correctly.
- **`POINTER_EXPR`**: All three codegen backends already handle address-of (`&`) and dereference (`*`).
- **Parser fix**: The prefix unary check was missing `-` (negation). Only `!`, `++`, `--` were allowed. Added `OP_MINUS` to the allowed prefix operators (2026-03-21).
- **Test expansion**: `operators.omegasl` `test_unary` shader now covers prefix `-`, prefix `++`/`--`, postfix `++`/`--`, and logical `!`.

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

## Phase 3: Language Extensions — MOSTLY DONE

### 3.1 User-defined functions — DONE

- Parser, semantic analysis, and all three codegen backends already support user-defined functions:
  - Parser creates `FuncDecl` nodes for non-shader functions.
  - Semantic analysis validates parameter uniqueness, type resolution, body, and registers `FuncType` in `functionMap` for call resolution.
  - All three codegen backends accumulate `userFuncDecls` and emit them via `emitUserFunction()` before each shader entry point.
  - `CALL_EXPR` codegen falls through to a default branch that emits the user function name and arguments.
- **Bug fix (2026-03-21)**: `MetalCodeGen.cpp` `CALL_EXPR` was missing an `else` branch for non-builtin function names. Calls to user functions (and `dot`/`cross`) would emit `(args)` without the function name. Added the missing `else { shaderOut << func_name; }`.
- **Test**: `user_functions.omegasl` tests single-param, multi-param, and cross-function calls (function A calling function B).
- **Not implemented**: Function overload resolution (multiple functions with the same name but different parameter types). Functions must be declared before use (no forward declarations).

### 3.2 Explicit type casts — DONE

- Parser handles C-style casts `(type)expr` in `parseOpExpr`: detects `(` followed by a type name and `)`, constructs `CastExpr` AST node.
- `CastExpr` AST node with `targetType` and `expr` fields.
- All three codegen backends handle `CAST_EXPR`, emitting functional-cast syntax `type(expr)`.
- Sema validates the target type exists via `resolveTypeWithExpr`.

### 3.3 Array declarations — DONE

- Parser handles `type name[size]` syntax after variable name in `parseGenericDecl`. Reads `[`, numeric literal, `]` and stores in `TypeExpr::arraySize`.
- `TypeExpr` has `std::optional<unsigned> arraySize` field.
- All three codegen backends emit `name[size]` when `arraySize.has_value()`.
- Array indexing already handled by `IndexExpr`.

### 3.4 Preprocessor directives — DONE

- `Preprocessor.h`/`Preprocessor.cpp` implements:
  - `#define NAME VALUE` — simple text macros with word-boundary-aware expansion.
  - `#ifdef` / `#ifndef` / `#endif` — conditional compilation with nesting via skip stack.
  - `#include "file.omegasl"` — file inclusion with `kMaxIncludeDepth = 10`.
- Integrated into `main.cpp`: source file is preprocessed before being passed to the parser.
- Not yet wired into the runtime compilation path (`Parser::parseContext`).

### 3.5 Tessellation shader stages — PARTIAL

- Parser handles `hull` and `domain` keywords, creates `ShaderDecl` with `Hull`/`Domain` type.
- AST: `ShaderDecl::Type` includes `Hull` and `Domain`.
- Codegen (basic):
  - HLSL: emits `[domain("tri")]`, `[partitioning("integer")]`, `[outputtopology("triangle_cw")]`, `[outputcontrolpoints(3)]` for hull shaders.
  - GLSL: uses `.tesc`/`.tese` file extensions.
  - Metal: emits `vertex` qualifier for both.
- **Remaining**: No configurable patch topology, control point count, or partition mode. No `[patchconstantfunc]` support. No MSL `[[patch(triangle, N)]]` or GLSL `layout(vertices = N) out;` emission. No test cases.

---

## Phase 4: Codegen Quality and Backend Parity — MOSTLY DONE

### 4.1 GLSL combined image-sampler support — DONE

- `GLSLCodeGen.cpp` `CALL_EXPR` for `BUILTIN_SAMPLE` already combines separate texture + sampler into `sampler1D`/`sampler2D`/`sampler3D`:
  ```glsl
  texture(sampler2D(textureVar, samplerVar), coord)
  ```
- Resolves sampler type from the resource store to select the correct combined type.

### 4.2 Consistent `generateBlock` handling of control flow — DONE

- All three `generateDecl` implementations dispatch `IF_STMT`, `FOR_STMT`, `WHILE_STMT` alongside `VAR_DECL` / `RETURN_DECL` / expressions.

### 4.3 Operator precedence — DONE

- Parser uses precedence-climbing with explicit levels: multiplicative (3) > additive (2) > comparison (1) > assignment (0).
- **Bug fixed 2026-03-21**: postfix unary check was consuming all `TOK_OP` tokens (including `=`) but only processing `++`/`--`. This silently broke assignment expressions in shader bodies.

### 4.4 Constant folding — DONE

- `ConstFold.h`/`ConstFold.cpp` implements a recursive AST pass that runs after semantic analysis, before codegen.
- Folds binary operations (`+`, `-`, `*`, `/`) on same-type numeric literals (`float`, `int`, `uint`, `double`).
- Folds prefix unary negation (`-`) on numeric literals.
- Division by zero is left unfolded (passed through to the target compiler).
- Recursively walks all expression slots: variable initializers, return values, call arguments, conditions, for-loop init/increment, array elements, member/index sub-expressions.
- Wired into `Parser::parseContext` between `performSemForGlobalDecl` and `generateDecl`.
- **Test**: `const_fold.omegasl` exercises float multiplication chains, negation addition, and integer division.

### 4.5 Vulkan tessellation shaders from OmegaSL

- Replace the hand-maintained SPIR-V in `VulkanTessSpirv.inc` with shaders compiled from `gte/src/shaders/*.omegasl` via `omegaslc` (once compute shaders compile cleanly to GLSL/SPIR-V for Vulkan).
- **Files**: `VulkanTEContext.cpp`, `VulkanTessSpirv.inc`, CMake build rules.

---

## Phase 5: Tooling and Testing

### 5.1 Compiler unit tests — PARTIAL

- CTest suite exists under `gte/omegasl/tests/` with:
  - Tokenizer test (`omegasl_tokens_shaders`)
  - Positive compilation tests: `shaders.omegasl`, `operators.omegasl`, `control_flow.omegasl`, `vector_math.omegasl`, `resource_types.omegasl`, `compute_gradient.omegasl`, `user_functions.omegasl`
  - Negative tests: `invalid_phase2.omegasl`, `invalid_type_mismatch.omegasl`, `invalid_undefined_resource.omegasl`
- **Remaining**: some test files depend on features not yet implemented (compound assignments, bare `return;`). Tests compile on Vulkan/GLSL backend via runtime shaderc; offline `glslc` path needs the tool on PATH.

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
| 2 | 1.6 | Codegen for `UNARY_EXPR` (all backends) + parser prefix `-` fix. | **DONE** |
| 3 | 1.4 | `bool` type. | **DONE** |
| 4 | 1.1–1.3 | Control flow (`if`/`else`, `for`, `while`). | **DONE** |
| 5 | 1.5 | Matrix types (`float2x2/3x3/4x4`). | **DONE** |
| 6 | 1.7 | Integer vector constructors (`make_int2/3/4`, `make_uint2/3/4`). | **DONE** |
| 7 | 1.8–1.9 | `read` builtin and `sampler1d`. | **DONE** |
| 8 | 2.1–2.2 | Source location tracking and diagnostic engine. | **DONE** |
| 9 | 2.3–2.4 | Structured errors and semantic validation completion. | **DONE** |
| 10 | 3.1 | User-defined functions. | **DONE** |
| 11 | 3.2–3.3 | Type casts and array declarations. | **DONE** |
| 12 | 4.1–4.3 | Codegen quality (GLSL image-samplers, control flow dispatch, operator precedence). | **DONE** |
| 13 | 3.4 | Preprocessor. | **DONE** |
| 14 | 3.5 | Tessellation shader stages. | **PARTIAL** |
| 15 | 4.5 | Vulkan tess shaders from OmegaSL. | Open |
| 16 | 5.1–5.2 | Compiler unit tests and golden-file tests. | **PARTIAL** |
| 17 | 5.4 | Language reference documentation. | Open |
| — | — | Parser bug: postfix unary consuming `=` operator. | **FIXED** (2026-03-21) |
| — | — | Compound assignment operators (`+=`, `-=`, `/=`, `*=`) + `++`/`--` lexer fix. | **DONE** (2026-03-21) |
| — | — | Bare `return;` (void return without expression). | **DONE** (2026-03-21) |

Steps 1–13 are complete. Remaining work is tessellation polish, Vulkan tess shader migration, test expansion, and documentation.

---

## Summary

| Area | Current | Target | Status |
|------|---------|--------|--------|
| **Control flow** | `if`/`else if`/`else`, `for`, `while` | Done | **DONE** |
| **Types** | Scalars, vectors, `bool`, matrices, arrays, resources | Done | **DONE** |
| **Builtins** | `make_float/int/uint 2/3/4`, `make_float 2x2/3x3/4x4`, `sample`, `read`, `write`, `dot`, `cross` | Done | **DONE** |
| **Expressions** | Binary, unary, call, member, index, literal, pointer, cast, compound assignment (`+=`/`-=`/`*=`/`/=`) | Done | **DONE** |
| **Shader stages** | vertex, fragment, compute, hull/domain (basic) | + full tessellation attributes | **PARTIAL** |
| **Error handling** | Structured errors with source location and code views | Done | **DONE** |
| **Semantic checks** | Type-checking, uniqueness, argument validation | Done | **DONE** |
| **Preprocessor** | `#define`, `#ifdef`/`#ifndef`/`#endif`, `#include` | Done | **DONE** |
| **User functions** | Parsing, semantic analysis, codegen all working | + overloading (optional) | **DONE** |
| **Testing** | CTest suite with positive/negative/golden tests | + GLSL/MSL golden files | **PARTIAL** |
| **Documentation** | 1-line README | Language reference | Open |
