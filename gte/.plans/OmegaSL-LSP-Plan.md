# OmegaSL Language Server (`omegasl-lsp`) Plan

A compiler-driven Language Server Protocol implementation that reuses the
existing OmegaSL **parser** and **semantic analyzer** (`Sem`) to provide basic
editor features for `.omegasl` / `.omegaslh` files. The server speaks LSP over
stdio (JSON-RPC 2.0, `Content-Length` framing) and runs the real compiler
front-end on every edit — diagnostics are exactly what `omegaslc` would report,
because they come from the same `DiagnosticEngine`.

The executable is named **`omegasl-lsp`** and lives under `gte/omegasl/lsp/`.

## Status — IMPLEMENTED (2026-06-19)

All v1 scope below is implemented and verified on this macOS (Metal) host:
diagnostics, document symbols, hover, completion, and the full lifecycle /
text-sync surface. `omegaslc`'s 124-test suite still passes (the compiler
changes are additive), and the golden codegen tests confirm transpiler output
is byte-identical.

Two compiler-side seams emerged during implementation beyond Phase 1.1/1.2:

- **`DiagnosticEngine::getErrors()`** (`Error.h`) — the engine kept its error
  list private (only `report(ostream&)` exposed it as combined text). Added a
  read-only accessor so the server maps each `Error::loc` / `Error::format` to
  a structured LSP diagnostic.
- **`Preprocessor::setLinePreserving(bool)`** (`Preprocessor.{h,cpp}`) — the
  preprocessor dropped directive lines and `#if`-skipped regions, shifting
  every line number below them. That would misplace diagnostics in the editor.
  The new mode emits one blank line per consumed/skipped input line so output
  stays aligned 1:1 with the source (assumes includes are rejected, which the
  server does). Default off ⇒ `omegaslc` output unchanged. Verified: an error
  below `#define` + a false `#ifdef` block lands on its true source line.

Known v1 robustness gap (documented, not yet addressed): a malformed incoming
message makes `OmegaCommon::JSON::parse` call `std::exit(1)` (RapidJSON's
failure path), which would kill the server. Conforming LSP clients send
well-formed JSON, so this is low-risk in practice; a future hardening pass
would pre-validate framing or swap in a non-aborting parse.

## Why this approach

The value proposition of a *compiler-driven* server is that the editor sees the
compiler's own truth, not a re-implemented approximation. OmegaSL already has a
production `Lexer` → `Parser` → `Sem` pipeline that fills a `DiagnosticEngine`.
The server drives that pipeline directly and reads its outputs. It deliberately
does **not** re-lex/re-parse the language a second way for semantic facts.

The one thing the front-end pipeline also does — code generation and shader
toolchain invocation — is exactly what an editor must *not* trigger on every
keystroke (it writes temp files and shells out to `dxc`/`metal`/`glslc`, and on
some hosts needs a GPU device). So the server runs the pipeline in a new
**frontend-only** mode that stops after semantic analysis.

## Scope (v1 — "basic features")

Implemented:

1. **Lifecycle** — `initialize` (capability advertisement), `initialized`,
   `shutdown`, `exit`.
2. **Text sync (full)** — `textDocument/didOpen`, `didChange` (full document
   sync), `didClose`; an in-memory document store keyed by URI.
3. **Diagnostics** — `textDocument/publishDiagnostics`, pushed after every
   open/change. Sourced from the `Preprocessor` errors + `DiagnosticEngine`
   errors produced by parse + sema. *(flagship feature)*
4. **Document symbols** — `textDocument/documentSymbol`: an outline of the
   top-level declarations (structs, functions, shader stages, resources,
   global constants).
5. **Hover** — `textDocument/hover`: the identifier under the cursor resolved
   against the document's symbol index plus the builtin type / function /
   keyword catalog.
6. **Completion** — `textDocument/completion`: keywords, builtin scalar/vector/
   matrix/resource types, builtin intrinsics, and the document's own top-level
   symbol names.

Explicitly out of scope for v1 (documented as follow-ups, not silently
dropped):

- Go-to-definition / find-references (needs per-identifier-use `loc`, which the
  parser does not populate today — a pervasive parser change).
- Scope-aware hover/completion for locals (the symbol index is top-level only).
- Incremental / range text sync (full-document sync only).
- Formatting, rename, semantic tokens, signature help.
- Cross-file resolution through `#include` (each document is analyzed alone;
  the preprocessor still expands includes it can resolve on disk).

## Known limitations (surfaced, not hidden)

- **AST allocation lifetime.** AST nodes are `new`-allocated by the parser with
  no arena or ownership, and `omegaslc` relies on process exit to reclaim them.
  A long-lived server re-parses on every edit, so each analysis leaks its AST.
  v1 accepts this; a bounded fix (arena allocator on the AST, or an explicit
  AST teardown walk) is a separate compiler-core change. Tracked here so it is
  not mistaken for "covered."
- **First-fatal-global-decl stop.** `parseContext` stops at the first global
  decl that fails semantic analysis (registering shared state past a
  half-formed resource/struct can crash — see the comment in `parseContext`).
  The server inherits this: diagnostics are complete up to the first fatal
  global decl. This matches `omegaslc` exactly.
- **Column units.** LSP positions are UTF-16 code units; OmegaSL token columns
  are byte offsets. Shader source is overwhelmingly ASCII, so these coincide in
  practice. Non-ASCII columns may be off; acceptable for v1.

## Phase 1 — Frontend-only seam in the compiler

Two small, contained changes to the existing front-end. Both are additive and
leave the `omegaslc` flow byte-identical.

### 1.1 `ParseContext` gains a frontend-only mode

`ParseContext` (in `Parser.h`) gains:

- `bool frontendOnly = false;`
- `std::vector<ast::Decl *> *collectedDecls = nullptr;`

`Parser::parseContext` (in `Parser.cpp`), inside its parse loop, when
`frontendOnly` is set: run `performSemForGlobalDecl` + `foldConstantsInDecl`,
push the decl into `collectedDecls` (when non-null), and **skip** `generateDecl`
+ `generateInterfaceAndCompileShader`. The error-recovery `break` behavior is
unchanged. When `frontendOnly` is false the path is exactly as today.

This is < 15 lines and needs no change to `Sem` (which never references
`CodeGen` — it works through `ast::SemFrontend`). The server still constructs a
real `CodeGen` + host-default `Target` to satisfy the `Parser` constructor and
`gen->setTypeResolver(sem)`, but the target's emission hooks are never called.

### 1.2 Top-level decls capture a source location

`Parser::parseGlobalDecl` already holds the declaration's name token where it
assigns `decl->name`. Set `decl->loc` from that token for the four top-level
decl kinds the outline surfaces:

- `STRUCT_DECL` — at the `_decl->name = t.str;` site.
- `FUNC_DECL` / `SHADER_DECL` — at the `funcDecl->name = id_for_decl;` site
  (capture the name token alongside `id_for_decl`).
- `RESOURCE_DECL` — at the `resourceDecl->name = id_for_decl;` /
  `res_decl->name = id_for_decl;` sites.

`loc = ErrorLoc{ nameTok.line, nameTok.line, nameTok.colStart, nameTok.colEnd }`.
Accurate, compiler-sourced positions for document symbols; no heuristics.

## Phase 2 — LSP module (`gte/omegasl/lsp/`)

Three modules, object-oriented and separated by concern (transport vs.
compiler bridge), matching the repo's modular style.

### 2.1 `Analysis.{h,cpp}` — the compiler bridge

The only module that touches the front-end. Given a document's text it:

1. Runs the `Preprocessor` (host-default backend) and collects preprocessor
   errors.
2. Builds a `SourceFile` + `DiagnosticEngine`, constructs a host-default
   `CodeGen`/`Target`, and runs `Parser::parseContext` in **frontend-only**
   mode, collecting decls.
3. Produces an `AnalysisResult`:
   - `Vector<Diagnostic>` — `{ ErrorLoc, message }`, from preprocessor +
     `DiagnosticEngine`.
   - `Vector<SymbolInfo>` — `{ name, kind, detail, ErrorLoc }` for each
     top-level decl (kind from `ASTType`, detail = a rendered signature/type).
   - The symbol index is reused by hover/completion.

`Analysis` owns the `ast::builtins::Initialize()` / `Cleanup()` lifetime (called
once by the server, not per document — builtins are global singletons).

### 2.2 `LspServer.{h,cpp}` — transport + dispatch + document store

- **Framing** — read one `Content-Length`-delimited message (read exactly N
  bytes, then `OmegaCommon::JSON::parse`); write a framed response.
- **Dispatch** — switch on the JSON-RPC `method`; requests return a `result`,
  notifications return nothing.
- **Document store** — `Map<uri, text>`; updated by didOpen/didChange, cleared
  by didClose.
- **Handlers** — lifecycle, sync (→ re-analyze → `publishDiagnostics`),
  documentSymbol, hover, completion. Hover/completion run a `Lexer` pass over
  the stored text to find the token at a position and map LSP positions
  (0-based line / char) from OmegaSL `Tok` (1-based line / 0-based col).

### 2.3 `main.cpp` — entry point

Initializes builtins, constructs an `LspServer` on `std::cin` / `std::cout`,
and runs the message loop until `exit`.

## Phase 3 — Build wiring

Add an `omegasl-lsp` tool target in `gte/CMakeLists.txt`, mirroring `omegaslc`:
the shared front-end/codegen sources (`OMEGASL_SRCS` minus `main.cpp`) plus the
`omegasl/lsp/*` sources, linking `OmegaCommonCore` (which provides
`OmegaCommon::JSON`) and the same host deps `omegaslc` uses (Metal/Foundation on
Apple, shaderc on Vulkan) since it compiles the same `Target` sources.

## Phase 4 — Verification

- Build `omegasl-lsp` on this host (Metal).
- Drive it with scripted JSON-RPC sessions over stdio: `initialize`, `didOpen`
  of a valid shader and a deliberately broken one, assert `publishDiagnostics`
  matches `omegaslc`'s diagnostics; `documentSymbol`, `hover`, `completion`
  smoke tests.
- Confirm `omegaslc` still builds and its tests are unaffected (the Phase 1
  change is additive).
