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

---

## Phase 5 — Include resolution (relative + `omegasl_commands.json`)

**Status — IMPLEMENTED (2026-07-05).** All five sub-phases landed and are
verified: the preprocessor source-map host test (`omegasl_preprocessor_sourcemap`)
and the end-to-end server test (`omegasl_lsp_include_resolution`) pass, the full
omegasl suite is 135/135, and `omegasl_commands.json` generates at the build
root with one entry per `add_omegasl_lib` call. v1 rejects `#include` outright: an editor
buffer was assumed to have no filesystem anchor. But every open document has a
`file://` URI — its own directory *is* an anchor — and shared `.omegaslh`
headers are resolved at compile time via `add_omegasl_lib`'s `INCLUDE_DIRS`
(`-I`). So a symbol declared in a header shows up in the editor as a bogus
"undeclared identifier". This phase resolves includes in the server so header
declarations participate in analysis, from two sources:

1. **Relative** — the open document's own directory (percent-decoded from its
   `file://` URI), passed as the preprocessor's `currentPath`.
2. **`omegasl_commands.json`** — a compile-commands database, generated by
   `add_omegasl_lib`, mapping each compiled source to its `-I` include dirs.

### The line-mapping constraint (why this is not a one-liner)

`setLinePreserving` keeps LSP diagnostics aligned 1:1 with the editor buffer by
emitting exactly one output line per source line — and its contract *assumes
`#include` expansion is off*, because an expanded header emits many lines and
shifts every main-file line below the directive. Enabling includes therefore
requires replacing "diagnostics map by identity" with an explicit **source-line
map** (`outputLine → editorLine`, 0 = "from an included file"). Includes are
inlined **in place**, exactly as `omegaslc` does, so the server sees precisely
what the compiler sees (forward-reference / declaration-order behavior cannot
diverge from a real compile).

### 5.1 Preprocessor — source-map mode

Add to `Preprocessor`:

- `void setSourceMap(bool enable)` — when set, `process()` records a dense
  `std::vector<unsigned> sourceMap_`: one entry per output line, holding the
  **1-based top-level source line** that produced it, or **0** when the output
  line came from included (foreign) content.
- `const std::vector<unsigned> & sourceMap() const` — valid after `process()`.

Built only at `includeDepth == 0`. `process()` clears it. The rule is uniform
and include-agnostic: after handling each source line, count the output lines it
appended; the **first** maps to that source line, **any others** map to 0. A
normal line adds 1 (→ its line); a directive under line-preservation adds 1
blank (→ its line); an `#include` adds 1 blank (the directive site, → its line)
followed by *K* expanded content lines (→ 0). `out` becomes a `std::string`
accumulator (from `ostringstream`) so the per-line output delta is measurable in
O(n) total. Line-preservation stays **on** in this mode so the map is dense and
the no-include case is byte-identical to today (identity map). The
`setLinePreserving` doc comment is updated: its "includes must be off" caveat is
lifted specifically when a source map is being built.

### 5.2 Analyzer — thread the path + include dirs, remap results

`Analyzer::analyze` gains two parameters:

```
AnalysisResult analyze(const std::string & text,
                       const std::string & documentPath,          // abs path, or ""
                       const std::vector<std::string> & includeDirs);
```

- Derive `docDir` from `documentPath` (strip the filename). *Anchored* =
  `docDir` non-empty **or** `includeDirs` non-empty.
- `setLinePreserving(true)`, `setSourceMap(true)`, `setRejectIncludes(!anchored)`
  (unanchored keeps today's loud rejection), `addIncludeDir` for each dir,
  `process(text, docDir)`.
- Pull `sourceMap()`. Remap every diagnostic and symbol line through it:
  - **Diagnostics** — a diagnostic whose start line maps to 0 (originates inside
    a header) is **suppressed** (decision: header-internal diagnostics belong to
    the header's own buffer). Otherwise remap start/end to editor lines.
  - **Symbols** — a decl whose loc maps to 0 (header-declared) is **kept out of
    the document outline** but **still added to the index** (so hover and
    completion resolve header symbols; hover computes its range from the cursor,
    not the decl loc, so it is unaffected). Main-file decls have their range
    remapped.
- A failed include *resolution* (wrong path / missing `-I`) is a main-file
  problem, so the existing coarse file-scope "preprocessor error" diagnostic is
  retained for it.

The no-include path is unchanged (identity map, reject on when unanchored).

### 5.3 LSP server — URI→path, load the compile DB, feed the analyzer

- `uriToPath(uri)` — decode a `file://` URI: strip the scheme, percent-decode
  (`%20` → space, …), and drop the leading slash before a Windows drive letter
  (`file:///C:/x` → `C:/x`).
- **Compile-DB discovery** — from the document's directory, walk parent
  directories until an `omegasl_commands.json` is found (typically the build
  root or project root — mirrors clang's `compile_commands.json` upward search).
  Parse it with **`OmegaCommon::JSON::TryParse`** (graceful on a malformed DB —
  log + ignore, never crash). Schema (purpose-built):

  ```json
  [ { "file": "/abs/x.omegasl", "includeDirs": ["/abs/inc", ...] }, ... ]
  ```

  Build `absFile → includeDirs`. Cache the parsed DB by its resolved path (v1
  does not hot-reload a changed DB — noted as a limitation).
- `analyzeAndPublish` — `docPath = uriToPath(uri)`; look up its include dirs in
  the DB (exact path match); call `analyze(text, docPath, dirs)`. The relative
  case needs no DB entry (it flows through `docDir`).

### 5.4 CMake — `add_omegasl_lib` emits `omegasl_commands.json`

- In `add_omegasl_lib`, resolve `_SRC` and each `INCLUDE_DIRS` entry to absolute
  (CMake normalizes to forward slashes, so the JSON strings need no
  backslash-escaping even on Windows) and append one entry to a
  `CACHE INTERNAL` list `OMEGASL_COMPILE_COMMANDS` (the same cross-directory-
  scope trick already used for `OMEGASLC_EXE`, so `gte/`, `wtk/`, and `aqua/`
  calls all accumulate into one list).
- A companion `omegasl_write_compile_commands()` function `file(GENERATE)`s
  `${CMAKE_BINARY_DIR}/omegasl_commands.json` from the accumulated list. It is
  called **once at the very end of the top-level `CMakeLists.txt`**, after every
  `add_subdirectory`, so the list is fully populated when its `CONTENT` is
  expanded.

### 5.5 Verification

- Preprocessor: a focused unit test for the source map — a buffer with a
  multi-line `#include` and directives above/below it; assert every main line
  maps back to itself and expanded lines map to 0.
- Server: scripted stdio session with a doc that `#include`s a header from a
  sibling dir (relative) and one resolved only via a synthetic
  `omegasl_commands.json` (`-I`); assert the previously-"undeclared" header
  symbol now resolves (no diagnostic), appears in hover/completion, and does
  **not** appear in the document outline; assert a malformed DB is ignored.
- CMake: configure the tree, assert `omegasl_commands.json` exists at the build
  root with one entry per `add_omegasl_lib` call carrying absolute paths.
- Confirm `omegaslc` output is byte-identical (all Phase 5 preprocessor changes
  are gated behind `setSourceMap`, which `omegaslc` never sets).
