# Plan: Extract `Sem` class from `Parser.cpp` into `Sema.h` / `Sema.cpp`

## Current state

The `Sem` class lives entirely inside `Parser.cpp` (lines 59–1264, ~1200 lines). It's defined in an anonymous-ish fashion — not in any header — with `SemContext` just above it. The parser's only interaction with it is:

1. **Construction**: `Parser::Parser()` creates `std::make_unique<Sem>()` and passes it to codegen via `gen->setTypeResolver(sem.get())`
2. **Context setup**: `sem->setSemContext(semContext)` and `sem->setDiagnostics(diagnostics)`
3. **Per-decl validation**: `sem->performSemForGlobalDecl(decl)` — the single entry point from the parse loop

This is a clean seam. The parser produces AST nodes, hands them to Sem, and Sem returns pass/fail. There are no callbacks from Sem back into the parser.

## What moves

| Destination | Content | Current lines |
|---|---|---|
| **`Sema.h`** | `SemContext` struct, `Sem` class declaration | 59–72 (SemContext), 76–87 + method signatures |
| **`Sema.cpp`** | `Sem` constructor, all method bodies | 89–1263 |

Specifically, these methods move to `Sema.cpp`:

- **Type resolution**: `resolveTypeWithExpr`, `resolveFuncTypeWithName`, `addTypeToCurrentContext`
- **FuncDecl context helpers**: `hasTypeNameInFuncDeclContext`, `addTypeNameToFuncDeclContext`, `getStructsInFuncDecl`
- **Validation core**: `performSemForExpr` (~600 lines, the bulk), `performSemForDecl`, `performSemForStmt`, `performSemForBlock`
- **Top-level entry**: `performSemForGlobalDecl` (STRUCT_DECL, RESOURCE_DECL, FUNC_DECL, SHADER_DECL validation)
- **SemFrontend overrides**: `evalExprForTypeExpr`
- **Context management**: `setSemContext`, `setDiagnostics`

## What stays in `Parser.cpp`

Everything else (~1520 lines): lexer interaction, token buffering, `parseExpr`, `parseBlock`, `parseStmt`, `parseGlobalDecl`, `parseContext`, all the `parseXxxFromBuffer` methods. The parser continues to `#include "Sema.h"` and hold a `std::unique_ptr<Sem>`.

## Steps

1. **Create `Sema.h`** — Move `SemContext` and the `Sem` class declaration (public interface + private members). It needs to include `AST.h` and `Error.h` (already transitively included). The header exposes:
   - `SemContext` struct (unchanged)
   - `class Sem : public ast::SemFrontend` with all current public methods declared but not defined

2. **Create `Sema.cpp`** — `#include "Sema.h"`. Move all method bodies verbatim. No logic changes, just `Sem::methodName(...)` qualification on each definition. The builtin type/function initializer lists in the constructor move here.

3. **Update `Parser.cpp`** — Replace the inline `SemContext` + `Sem` class with `#include "Sema.h"`. No other changes — the parser already accesses Sem through `std::unique_ptr<Sem> sem` and the three call sites listed above.

4. **Update `Parser.h`** — The forward declaration `class Sem;` on line 18 stays as-is (or becomes `#include "Sema.h"` if needed for `unique_ptr` destruction, but a forward decl + destructor-in-cpp pattern works too).

5. **No CMakeLists.txt change needed** — the glob `file(GLOB OMEGASL_SRCS "omegasl/src/*.h" "omegasl/src/*.cpp")` picks up new files automatically.

6. **Verify** — Build and run the existing CTest suite. Zero logic changes, so any test failure would indicate a missed dependency.

## Risk

Low. This is a pure mechanical extraction — no logic changes, no API changes, no new dependencies. The seam between parser and Sem is already clean (one entry point, no circular calls). The only thing to watch for is that `SemContext` is currently defined in the same translation unit as both `Sem` and `Parser`, so moving it to the header makes it visible to anything that includes `Sema.h`. That's fine — codegen already interacts with Sem via `SemFrontend`, and `SemContext` is just data.

## Result

`Parser.cpp` drops from ~2786 lines to ~1520 lines. `Sema.cpp` is ~1200 lines. Each file has a single responsibility: parsing syntax vs. validating semantics.
