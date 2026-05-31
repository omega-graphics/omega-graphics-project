# Plan — C++ Semantic Indexer for omega-codedb

> **Status: FUTURE WORK — deferred.** The current regex-based `codedb.py` works well
> enough for the navigation queries we run today (`find`, `where`, `show`). This
> plan is parked until that boundary actually binds — when an agent or human keeps
> needing dependency-direction answers (`what does X reference?`, `who uses Y?`)
> that regex can't produce. Revisit when that pain shows up, not before.
>
> The document below remains a proposal, not an approved design. Decisions in §5
> are still open; the phasing in §6 still requires user sign-off.

## 0. TL;DR

The current `codedb.py` is regex-driven. That is fine for OmegaSL — shaders are flat,
self-contained, no preprocessor, no inheritance — and fine for "where is `Foo`
defined?" lookups in C++. It is **not** fine for any question that needs the C++
semantic model: what types a class references, which subsystems a file actually pulls
in, what a forward declaration resolves to, what a macro expands to.

Proposal: keep the regex tool for what it already does well (find/where/show over
type definitions and OmegaSL), and add a **Clang LibTooling indexer** that emits
**dependency information** for C++ — type→type, file→file, area→area edges. Two
indexes, one tool, separated by the languages they're suited for.

The indexer is **platform-generic**: one unified parse profile, every Omega
feature-gating macro defined simultaneously, every code branch visited. We are not
reproducing the build — we are walking the AST to extract references. Linting,
verification, and per-platform fidelity are explicit non-goals.

The hard prerequisite is a small CMake-emitted aggregate listing project include
paths, project macros, and source files. The hard runtime cost is several
seconds-to-minutes for a cold build of the semantic index.

---

## 1. Why now

The README already concedes the boundary: *"It indexes the definitions of types and
shaders — the anchors you navigate **to**. It deliberately does not try to index
every free function, method, or variable."*

That boundary is starting to bind:

- The agent guidance in `AGENTS.md` calls codedb the canonical alternative to
  `grep -r`. But for "what does `EditorViewport` depend on?", neither `find` nor
  `where` answers it — the agent still falls back to grep.
- Regex is brittle on C++. `_ATTR` already exists to skip all-caps attribute macros;
  the next platform header that uses a non-uppercase export macro will silently
  drop symbols. Template specialisations, partial specialisations, nested types,
  `typename T::Inner` aliases — all currently invisible.
- LLVM/Clang is already the toolchain on every dev machine. The cost of pulling in
  LibTooling is a CMake target, not a new dependency stack.

## 2. What stays, what changes

| Capability | Current (regex) | After (hybrid) |
| --- | --- | --- |
| Find a C++ type by name → file:line | regex | **semantic** (regex retained as fallback when the indexer hasn't run) |
| Find an OmegaSL shader / struct | regex | regex (unchanged) |
| List areas, partition lookup | curated JSON | curated JSON (unchanged) |
| `where <topic>` | regex + curated tags | regex + curated tags (unchanged) |
| **What types does `Foo` reference?** | not available | semantic |
| **What subsystems does this file depend on?** | not available | semantic, derived from type→file→area |
| **Reverse: who references `Foo`?** | not available | semantic |
| Function / method definitions | not indexed | **semantic, opt-in** (see §5 Decision D) |

The Python frontend remains the user-facing CLI. The semantic indexer is a separate
build artefact (a clang-based binary) that emits JSON into `.cache/`.

## 3. The three dependency layers

There are three distinct things people casually call "dependencies":

1. **Include dependency.** File A `#include`s File B. Cheap, preprocessor-only.
   Useful for build-order questions, less useful for "what does this code actually
   need." Falls out for free as a side effect of parsing.
2. **Symbol / type dependency.** Type A names Type B in its members, bases, return
   types, parameters, friends, or function-local declarations. This is the layer
   AGENTS.md actually cares about — "which subsystems does this code touch?" — and
   it requires AST-level analysis. LibTooling.
3. **Runtime / call dependency.** Function A calls Function B. Strictly more
   expensive (needs full function bodies parsed, not just declarations) and noisier
   (inlined helpers, lambda captures). Worth indexing later, not in the first cut.

The proposal scopes layer 2 as the primary deliverable, layer 1 as a cheap free
side effect, and layer 3 as a deferred phase (see §6 Phase 5).

## 4. Architecture

```
        ┌──────────────────────────────────────────────────────┐
        │  CMake configure (top-level)                          │
        │   - aggregates project include dirs across targets    │
        │   - aggregates project macros across targets          │
        │   - aggregates source list                            │
        │   - writes build/cxx-index-profile.json               │
        └─────────────────────────┬────────────────────────────┘
                                  │
                  ┌───────────────▼──────────────┐
                  │  cxx-index-profile.json      │
                  │   - includes[]               │  (one unified set)
                  │   - defines[]                │  (all platforms ON)
                  │   - sources[]                │
                  │   - cxx_std                  │
                  └───────────────┬──────────────┘
                                  │
                       ┌──────────▼──────────┐
                       │  omega-cxxindex     │  (new C++ binary, LibTooling)
                       │   - FixedCompilDB   │  one, shared
                       │   - ASTMatchers     │
                       │   - emits JSON      │
                       └──────────┬──────────┘
                                  │
              ┌───────────────────▼────────────────────┐
              │  .cache/cxx-semantic-index.json        │
              │   - typedefs[]                         │
              │   - type_refs[]                        │
              │   - file_refs[]                        │
              │   - area_refs[]                        │
              │   - (optional) call_edges[]            │
              └───────────────────┬────────────────────┘
                                  │
              ┌───────────────────▼────────────────────┐
              │  codedb.py  (existing)                 │
              │   - find/where/show/stats unchanged    │
              │   - new: deps, users                   │
              └────────────────────────────────────────┘
```

Key points:

- The indexer is **out-of-process** from `codedb.py`. Python never links LLVM.
- **No per-TU `compile_commands.json`, no per-target profile.** CMake emits one
  flat aggregate at configure time. Every file is parsed under the same flags.
- **Platform-generic.** Every Omega feature-gating macro (`OMEGA_HAS_METAL`,
  `OMEGA_HAS_VULKAN`, `OMEGA_HAS_D3D12`, `OMEGA_PLATFORM_*`, etc.) is defined as 1
  in the unified profile, so the parser visits every code branch regardless of
  host OS. Host compiler macros (`_WIN32`, `__APPLE__`, `__linux__`) come from
  whatever Clang sees natively — we don't try to fake those (system headers would
  contradict each other), but most of our dependency edges are between Omega types
  anyway.
- Two cache files coexist: `.cache/symbol-index.json` (regex, OmegaSL + C++ fallback)
  and `.cache/cxx-semantic-index.json` (LibTooling, C++ only). Python prefers the
  semantic one for C++ when present, falls back to regex when absent (e.g. on a
  fresh checkout before build configure).
- Incremental invalidation reuses the same (mtime, size) key per source file.
  **Header changes invalidate every file including them** — we capture the include
  closure per source at index time, so a header touch dirties the right set.

## 5. Decisions to confirm

These are decisions I need you to make before I start writing the plan into phases.
Each is genuinely a fork — I have a recommendation but not enough context to
overrule yours.

### Decision A — Hybrid or full replacement?

**Recommend: hybrid (keep regex for OmegaSL + as C++ fallback).**

The regex tool is fast, zero-dependency, and works on a fresh checkout with no
configured build. Removing it would mean agents can't navigate the codebase on a
brand-new clone until the user has run CMake. Hybrid lets `find <Symbol>` keep
working everywhere.

Alternative: full replacement. Cleaner conceptually, but breaks the "zero
dependency" guarantee for the most common operation.

### Decision B — Where does the indexer binary live?

**Recommend: `utils/omega-codedb/cxx-indexer/` with its own `CMakeLists.txt`,
linking `LLVM` + `clangTooling` + `clangASTMatchers`. Built out-of-tree like the
rest of the toolchain.**

The binary is `OmegaCxxIndex` (PascalCase, per AGENTS.md). It is **not** part of the
main Omega build — it builds against the system Clang/LLVM the user already has for
the toolchain, and lives next to the Python script. `python3 codedb.py index
--semantic` shells out to it.

Alternative: a separate repo. Too heavy for a utility.

### Decision C — Platform-generic profile (no `compile_commands.json`)

**Recommend: a single CMake-emitted `cxx-index-profile.json` that lists project
includes, project macros (all platforms defined as 1), source files, and the C++
standard. Indexer applies that one profile to every source.**

Modelled on how Doxygen consumes input: not per-TU compile commands, but a
high-level "this is what it takes to read this codebase" config. CMake has the
data natively — `INCLUDE_DIRECTORIES`, `COMPILE_DEFINITIONS`, `SOURCES`,
`CXX_STANDARD` are all readable as target properties at configure time.

A small CMake module (`cmake/CodedbExport.cmake`) gets `include()`'d from top-level.
It walks every target, takes the union of their includes, macros, and sources,
adds the platform-gating macros explicitly set ON, and writes JSON to
`${CMAKE_BINARY_DIR}/cxx-index-profile.json`.

Shape (proposed; needs review):

```json
{
  "version": 1,
  "generator": "CodedbExport.cmake",
  "root": "/abs/path/to/repo",
  "cxx_std": "17",
  "includes": [
    "wtk/include", "wtk/src",
    "gte/include", "gte/src",
    "gte/src/d3d12", "gte/src/metal", "gte/src/vulkan",
    "common/include", "common/src"
  ],
  "defines": [
    "OMEGA_HAS_METAL=1",
    "OMEGA_HAS_VULKAN=1",
    "OMEGA_HAS_D3D12=1",
    "OMEGA_PLATFORM_DARWIN=1",
    "OMEGA_PLATFORM_LINUX=1",
    "OMEGA_PLATFORM_WIN32=1",
    "OMEGAWTK_BUILD=1"
  ],
  "sources": ["wtk/src/UI/Widget.cpp", "..."]
}
```

The indexer builds a single `clang::tooling::FixedCompilationDatabase` from this,
runs the AST tool over the union of sources.

This is **platform-generic by design**: we're not compiling, we're indexing. All
Omega feature-gating macros are ON simultaneously so every `#if OMEGA_HAS_*`
branch is parsed. System headers (which require mutually-exclusive
compiler-supplied macros like `_WIN32`) come from the host's native Clang — those
branches lose fidelity off-platform, but the dependency edges we care about live
in Omega types, not in `<windows.h>`.

Why this beats `compile_commands.json` for our use case:
- Cleaner data model. A navigation tool doesn't need per-TU command granularity.
- Stable across rebuilds. `compile_commands.json` re-emits whenever any source
  list changes; this one only changes when project shape does.
- No requirement that the user has fully configured every backend's build — every
  platform's macros are forced ON, regardless of which platforms the host can
  build.
- Smaller. ~one block vs one row per TU.

Why it's worse:
- Loses per-target / per-file flag granularity. Acceptable for navigation; would
  not be acceptable for full compilation reproduction.

The CMake module is small (~30 lines, since there's no per-target loop). Phase 1
ships it.

### Decision D — Scope: types only, or types + functions?

**Recommend: phase 2 ships types-and-type-refs only; phase 5 adds functions/calls
behind an `--include-functions` flag.**

Layer 2 (type refs) gives ~80% of the value at ~20% of the cost. Function bodies
require the parser to descend into every TU's full body — minutes vs seconds on a
100k-line repo. Defer.

If your intuition is "I'll want function-level deps inside a month anyway, do it
now" — say so and I'll fold it into phase 2.

### Decision E — Source list construction

How does the CMake exporter know which sources to include?

**Recommend: union of every target's `SOURCES` property, plus a recursive glob of
`*.h`/`*.hpp` under each include directory.**

The `SOURCES` union catches every `.cpp` the build actually compiles. The header
glob catches public headers that aren't in any target's `SOURCES` (some libraries
list only `.cpp` in their target, with headers picked up via include search).
Without the header glob, the indexer wouldn't see definitions in header-only
declarations.

Alternative: only `SOURCES`. Misses header-only types.

Alternative: just glob the repo. Picks up generated code, third-party,
abandoned files. Worse.

---

## 6. Multi-phase implementation

Each phase is a reviewable increment. Phase 1 lands the harness; everything after
adds capabilities to it. No phase below depends on the next.

### Phase 1 — CMake exporter + skeleton indexer (target: ~30 LoC CMake + ~150 LoC C++)

- Add `cmake/CodedbExport.cmake`. Walks every target, unions
  `INCLUDE_DIRECTORIES`, `COMPILE_DEFINITIONS`, and `SOURCES`, layers on the
  forced-ON platform macros, writes JSON to
  `${CMAKE_BINARY_DIR}/cxx-index-profile.json`. Wired in at top-level via
  `include(cmake/CodedbExport.cmake)`.
- Create `utils/omega-codedb/cxx-indexer/` with `CMakeLists.txt` that locates
  `LLVM` + `Clang` via `find_package(LLVM CONFIG)` / `find_package(Clang CONFIG)`.
  This indexer builds **out-of-tree** from the main Omega build — it has its own
  configure step (the user runs it once per host).
- Skeleton `OmegaCxxIndex.cpp` reads `cxx-index-profile.json`, builds a
  `FixedCompilationDatabase`, runs an empty `ClangTool` over the source list.
  Emits `{"files_seen": [...], "version": 1}`.
- `codedb.py index --semantic --dry-run` shells out to the indexer, captures
  output, prints a summary.

**Exit criterion:** indexer runs against a freshly configured build, its
`files_seen` equals (or explains its diff from) `codedb.py stats`' file list
today.

### Phase 2 — Type definitions (parity + the gaps regex misses)

- ASTMatcher: `cxxRecordDecl(isDefinition()).bind("type")` + analogues for
  `enumDecl`, `typedefDecl`, `typeAliasDecl`, `namespaceDecl`. Emit
  `{name, qualified_name, kind, file, line, is_template, template_kind}` per
  match.
- Add `qualified_name` (`omega::wtk::ui::Widget`) — regex can't produce these and
  they're the natural disambiguator for `find` when two unrelated types share a
  base name across modules.
- `codedb.py` change: when the semantic index is present, C++ symbols come from
  there. The `find` output gains a `qualified_name` column.

**Exit criterion:** `codedb.py find <Symbol>` returns ≥ the symbols the regex
version returns for a sampled list of 20 known types. Mismatches are explained
(e.g., regex misses are bugs we're fixing; semantic misses are real and need
investigation).

### Phase 3 — Type → type references (the headline new capability)

- ASTMatcher on `typeLoc` inside `cxxRecordDecl` members, base specifiers, return
  types, parameters, friend decls. For each, record the referenced
  `CXXRecordDecl` or `TypedefDecl`'s file:line.
- Emit `type_refs[]`: `{from: "WTK::Compositor", to: "GTE::CommandQueue",
  from_file: "...", to_file: "..."}`.
- Aggregate into `file_refs[]` (file-to-file, deduped) and `area_refs[]`
  (area-to-area, derived from the existing partition resolver).
- New command: `codedb.py deps <Symbol|file|Area>` returns the referenced types,
  files, or areas respectively.

**Exit criterion:** `codedb.py deps "OmegaWTK Composition Engine"` produces a
non-empty, sensible set of area edges. Spot-check against the curated
`depends_on` for that area, but disagreements are *not* failures — they're data.

### Phase 4 — Reverse lookup

- Reverse index: `users(symbol)` walks `type_refs[]` in reverse. Cheap once the
  forward index exists.
- New command: `codedb.py users <Symbol>` returns every type/file referencing the
  argument.

**Exit criterion:** `codedb.py users GECommandQueue` returns every D3D12/Metal/Vulkan
backend that names the public type, matching what `find CommandQueue` already
shows by convention.

### Phase 5 — Functions, methods, calls (deferred)

Only if Decision D goes the "yes, do it" route, or once we have a concrete need.

- Adds `function_defs[]`, `call_edges[]`. Much larger cache; consider a separate
  cache file so the type-only path stays fast.
- New command: `codedb.py calls <Symbol>` and `--include-functions` flag on
  `find`.

---

## 7. Risks & open questions

- **LLVM/Clang version drift.** The indexer pins to whatever LLVM is on the host.
  ASTMatcher API has changed across LLVM 14 → 18. Plan: lock the indexer's minimum
  to LLVM 17 (current toolchain), document the assumption, fail loudly if older.
- **Template-heavy code.** Heavy templates (variadic, CRTP, expression templates)
  blow up TU parse time. If the cold-build cost lands at 5+ minutes, we may need
  to scope the indexer to **headers only**, or to a configurable subset.
- **Platform-only system headers.** Code that includes `<windows.h>` will fail to
  parse on macOS/Linux (and vice versa). The indexer's strategy: **continue on
  fatal errors**. Clang has `-ferror-limit=0` and partial AST recovery; we set the
  diagnostic engine to keep going. Whatever type info Clang *did* extract before
  the unresolvable include gets indexed. Edges from those branches will be
  partial, but Omega-type-to-Omega-type edges (the ones we care about) survive.
- **Out-of-tree third-party.** `PRUNE_DIRS` in `codedb.py` already excludes
  `third_party`, `deps`, `.automdeps`. The CMake exporter needs the same
  exclusion when unioning sources — filter target sources whose path falls inside
  those dirs. Otherwise we'd index Vulkan-Headers, ICU, etc.
- **Generated code.** OmegaSL emits intermediate C++; ditto any codegen step. Need
  to confirm where these land and whether they should be indexed.
- **Modules.** If/when the codebase adopts C++20 modules, the indexer changes
  shape. Out of scope for now, but worth knowing.
- **Macro contradictions.** Defining `OMEGA_HAS_METAL=1` AND `OMEGA_HAS_VULKAN=1`
  simultaneously may trip code that assumes exactly one is set (e.g.
  `#if defined(OMEGA_HAS_METAL) && defined(OMEGA_HAS_VULKAN) #error mutual
  exclusion`). If any such guards exist, the exporter needs to either suppress
  them or the indexer needs a small ignore-list.

## 8. Not in scope (deliberately)

- LSP / editor integration. Not what this tool is for.
- Symbol renaming, refactoring. Not navigation, not our job.
- Public API stability tracking. Different tool.
- Replacing `OMEGA-Project.json`. The curated map is a design statement, not
  derivable from code.
- **Linting curated `depends_on` against observed edges.** Explicitly out of scope
  per the user's direction; the curated edges and the observed edges live side by
  side without one validating the other.
- **Per-platform fidelity.** The indexer is platform-generic. If a question
  genuinely needs per-platform answers, ask a per-platform build of the indexer.

---

## 9. What I'm surfacing vs proposing

Surfaced (your call):
- A, C, D, E — these are real forks with non-trivial consequences.

Proposed (my call, override if wrong):
- B (binary lives in `utils/omega-codedb/cxx-indexer/`).
- Phase order: harness → types → refs → reverse → functions.

Not yet investigated:
- Exact LLVM versions installed on each host platform of this repo.
- Whether any Omega code uses mutual-exclusion guards across platform macros
  (Risks §7).
- Whether the union of every target's includes/defines actually fits in one
  Clang invocation without hitting include-depth or macro-redefinition limits.

I'd want answers on those before writing phase 1, but I don't think they change
the shape of the plan.
