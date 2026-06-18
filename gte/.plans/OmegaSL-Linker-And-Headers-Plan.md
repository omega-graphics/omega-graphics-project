# OmegaSL Linker + `.omegaslh` Headers — Implementation Plan

**Status:** Proposed (no code written yet)
**Module:** `gte/omegasl`
**Author:** design pass, 2026-06-17
**Scope:** two related additions to the OmegaSL toolchain —
1. a **link tool** that merges several `*.omegasllib` archives into one larger archive, and
2. a **preprocessor rule** that rejects `#include`d headers (`.omegaslh`, the new recommended header extension) which contain shader entry points.

The two are designed together because they form one workflow: split shaders across
translation units, share declarations through `.omegaslh` headers, compile each unit to
its own `.omegasllib`, then link the units into a shippable archive. This is the
C/C++ header / translation-unit / static-archive discipline applied to OmegaSL.

---

## 1. Background — current state (verified against source)

### 1.1 What an `.omegasllib` actually is

The archive is a flat, hand-rolled binary container. The writer is
`CodeGen::linkShaderObjects()` (`gte/omegasl/src/CodeGen.h:497`) and the reader is
`OmegaGraphicsEngine::loadShaderLibraryFromInputStream()` (`gte/src/GE.cpp:143`). The
on-disk layout, reconstructed from both sides, is:

```
[size_t]              libname_size
[bytes × libname_size] libname
[unsigned]            entry_count
  ── per shader entry ──────────────────────────────────────────────
  [int]               type                         (omegasl_shader_type)
  [size_t]            name_len
  [bytes × name_len]  name
  [size_t]            dataSize                     (0 ⇒ "stub", no body)
  [bytes × dataSize]  data                         (compiled GPU object)
  [unsigned]          nLayout
  [omegasl_shader_layout_desc × nLayout]           (raw struct dump, zero-padded)
  if dataSize > 0:                                 (stubs skip this block)
     VERTEX:   [bool useVertexID][unsigned nParam]
               per param: [size_t name_len][bytes name][int type][size_t offset]
     COMPUTE:  [unsigned x][unsigned y][unsigned z]
     MESH:     [unsigned x][y][z][unsigned max_vertices][unsigned max_primitives][int topology]
     FRAGMENT / HULL / DOMAIN: (nothing)
  [unsigned long long] requiredFeatures            (always written, even when 0)
```

The in-memory mirror of this is `omegasl_shader` / `omegasl_shader_lib`
(`gte/include/omegasl.h:330` and `:379`).

### 1.2 The critical insight: linking is a *container merge*, not symbol resolution

Each `omegasl_shader.data` blob is a **fully self-contained compiled object for exactly
one entry point** (one DXIL blob, one SPIR-V module, or one metallib per shader). User
helper functions are **inlined** into every shader before transpilation — see
`lambert_diffuse` / `blinn_specular` in `gte/omegasl/tests/blinn_phong.omegasl:22-42`,
which are emitted into each shader's source, not shared as external symbols. The writer
loop confirms this: each `shaderMap` key is one compiled object file on disk, read in
whole as that entry's `data` (`CodeGen.h:521-553`).

**Consequence:** there are no unresolved cross-entry references between shader objects.
"Linking" two `.omegasllib` files is therefore a pure archive merge — concatenate the
entry lists, write a new header with the combined count. It is `ar`, not `ld`. No symbol
table, no relocation, no dead-strip. This keeps the tool small and, importantly, means
**the linker needs no platform shader toolchain** (no dxc / metal / glslc) and can run on
any host, unlike compilation, which is platform-gated (`main.cpp:98-133`).

### 1.3 Gaps the linker exposes in today's format

1. **No magic number / version field.** The file begins immediately with `libname_size`.
   A link tool cannot tell a real `.omegasllib` from an arbitrary file, nor detect a
   format change across tool versions.
2. **No backend tag.** A DXIL archive and a SPIR-V archive are byte-structurally
   identical; only the opaque `data` blobs differ. Merging libraries built for *different*
   backends would silently produce a corrupt archive that explodes at load time with a
   cryptic backend error. This is exactly the kind of silent-corruption / 3am failure the
   project's "fail loud" principle exists to prevent.
3. **The format is hand-coded in two places that must stay byte-identical** (writer in
   `CodeGen.h`, reader in `GE.cpp` — the code comments at `CodeGen.h:616` and
   `GE.cpp:384` explicitly warn they must stay "in lockstep"). A linker would be a *third*
   copy. Three hand-rolled copies of an unversioned binary format is a drift hazard.

### 1.4 How includes work today

`Preprocessor::processInternal()` (`gte/omegasl/src/Preprocessor.cpp:212`) is a
line-based text pass. `#include "path"` (`:331-364`) reads the file and **textually
inlines** its recursively-processed content into the output stream. There is already an
include-rejection switch — `setRejectIncludes(true)` — but it is used only for *runtime*
compilation, where there is no filesystem context (`Preprocessor.h:36-44`,
`Preprocessor.cpp:332-346`). Offline compilation resolves includes normally.

Because includes are textually inlined, a header that declares a shader entry point and
is `#include`d into two translation units produces that shader **twice** — duplicate
entries, name collisions at link time, and binary bloat. That is the problem Feature 2
prevents.

### 1.5 Stage-keyword detection is cheap and exact

The shader-stage keywords `vertex`, `fragment`, `compute`, `hull`, `domain`, `mesh` are
**hard keywords** matched by exact string content in `isKeyword()`
(`gte/omegasl/src/Lexer.cpp:12-43`). They can never be identifiers. So "does this file
contain a shader function?" reduces to "does a stage keyword appear as a real token
(not inside a comment or string)?" The existing `Lexer` already tokenizes correctly
(comments, strings, whole-word boundaries, line numbers), so detection should **reuse the
Lexer** rather than re-implement comment/string skipping — single source of truth for
what a stage keyword is.

---

## 2. Goals / non-goals

**Goals**
- A tool that merges N `.omegasllib` archives into one, with loud rejection of
  incompatible inputs (wrong format, mismatched backend) and duplicate shader names.
- Establish `.omegaslh` as the recommended header extension.
- Make the preprocessor reject any `#include`d file that declares a shader entry point,
  with a precise diagnostic (path + line).
- Remove the writer/reader lockstep hazard by extracting one shared container
  (de)serializer used by the compiler, the engine loader, and the new linker.

**Non-goals**
- No cross-shader symbol linking / dead-stripping (architecturally unnecessary — §1.2).
- No change to how individual shaders are compiled or transpiled.
- No new language features. `.omegaslh` is a *convention + a guard*, not new grammar; a
  header is ordinary OmegaSL minus shader entry points.
- No runtime-compilation path change (the runtime `OmegaSLCompiler` already rejects
  includes wholesale).

---

## 3. Feature 1 — the `.omegasllib` linker

### Phase 0 — Format versioning + backend tag (foundation)

Prepend a small fixed header to the container so it can be identified and validated:

```
[char × 4]   magic = "OSLL"
[uint32]     format_version            (start at 1)
[uint8]      backend_id               (0 = hlsl, 1 = metal, 2 = glsl)
[uint8 × 3]  reserved (zero)
... existing container body (libname_size, libname, entry_count, entries...)
```

- Update the writer (`CodeGen::linkShaderObjects`, `CodeGen.h:497`) to emit the prefix.
  The backend id comes from the active `Target` (the codegen already knows whether it is
  HLSL/MSL/GLSL — `main.cpp:215-221` constructs the matching `PPBackend`/Target).
- Update the reader (`GE.cpp:143`) to read and validate the prefix: reject on bad magic,
  reject on a `format_version` it does not understand. The reader does **not** need to act
  on `backend_id` (the engine only ever loads its own backend's lib), but it must skip the
  field correctly.
- No committed `.omegasllib` binaries exist in the tree (verified: `git ls-files
  '*.omegasllib'` is empty), and the CTest suite regenerates libs into the build temp dir
  (`gte/omegasl/tests/CMakeLists.txt:5-18`). So this is a clean break — no on-disk
  migration is required, only a rebuild.
- Update the format documentation in `gte/.plans/OmegaSL-Reference.md` and
  `gte/docs/api/Shaders.rst`.

**Why this is Phase 0 and not optional:** it is the only thing that lets the linker fail
*loudly* on a backend mismatch instead of producing a corrupt archive (§1.3.2). See
**Open Decision D1** for the fallback if the format change is deferred.

> Cross-backend callout: the writer change touches the single shared writer; the reader
> change touches `GE.cpp`, which is backend-neutral but feeds all three backends. On this
> Linux host only the **GLSL/Vulkan** path is build- and run-verifiable. The **D3D12** and
> **Metal** readers compile the same `GE.cpp` and must be confirmed by the user on those
> platforms (per repo multi-backend policy).

### Phase 1 — Extract a shared container (de)serializer

New module: `gte/omegasl/src/LibContainer.{h,cpp}` (linked into both `omegaslc` and the
engine; pure stdlib + `omegasl.h`, no GTE device dependency).

```cpp
struct OmegaSLLibContainer {            // owns all backing storage
    std::string name;
    uint8_t backendId = 0;
    uint32_t formatVersion = 1;
    std::vector<omegasl_shader> shaders;     // pointers into the buffers below
    // owning buffers: names, data blobs, layout arrays, vertex-param arrays
};

bool ReadLibContainer (std::istream&, OmegaSLLibContainer& out, std::string& err); // pure parse, no device
bool WriteLibContainer(std::ostream&, const OmegaSLLibContainer&,  std::string& err);
```

- `ReadLibContainer` parses the container into owned memory and performs **no** device
  feature-gating and **no** `_loadShaderFromDesc` — it stops at raw records. This is the
  reusable core the linker needs (it must read a lib with no GPU device present).
- Refactor `CodeGen::linkShaderObjects` (`CodeGen.h:497`) to: load each compiled object
  file's bytes into a record, then call `WriteLibContainer`.
- Refactor `GE.cpp:143` loader to: `ReadLibContainer`, then per record run the existing
  feature-gating (`GE.cpp:349-360`) and `_loadShaderFromDesc`. The fiddly manual
  `.release()` ownership transfer in the current reader (`GE.cpp:362-369`) is replaced by
  the container owning its buffers.
- This collapses the three-copies-must-match hazard (§1.3.3) into one tested module.

**Verification:** a byte-identical round-trip golden test — `ReadLibContainer` an existing
generated lib, `WriteLibContainer` it back, assert the two byte streams are identical.

This phase is the largest single change and may be split into **1a** (writer extraction)
and **1b** (reader extraction) so each lands and is verified independently.

> Cross-backend callout: same as Phase 0 — the `GE.cpp` reader refactor is exercised on
> Vulkan here; D3D12/Metal compile-and-load must be user-confirmed.

### Phase 2 — The link command

Add a `--link` mode to `omegaslc` (recommended — reuses the serializer TU already linked
into the compiler; see **Open Decision D2** for the separate-executable alternative):

```
omegaslc --link in1.omegasllib in2.omegasllib [in3...] -o out.omegasllib [--lib-name NAME]
```

Behavior:
1. `ReadLibContainer` each input (no toolchain invoked — pure container reads).
2. Validate compatibility across inputs: identical `magic`, identical `format_version`,
   identical `backend_id`. Any mismatch ⇒ precise error naming the offending file, exit
   nonzero. (This is the payoff of Phase 0.)
3. Merge entry lists. On a **duplicate shader name**, error by default (see
   **Open Decision D3** for an override policy).
4. Write the merged container with `entry_count = Σ inputs` and the chosen library name
   (`--lib-name`, default = output filename stem) via `WriteLibContainer`.

The `--link` mode short-circuits the normal parse/codegen path in `main.cpp` — it never
reads `.omegasl` source, never preprocesses, never invokes dxc/metal/glslc.

### Phase 3 — Tests + docs (linker)

- CTest: compile two small `.omegasl` files → two libs → `--link` them → assert the merged
  lib's `entry_count` equals the sum and round-trips through `ReadLibContainer`.
- Negative CTest (`add_omegasl_fail_test` pattern, `CMakeLists.txt:21-25`): linking libs
  with mismatched `backend_id`, and linking libs with a duplicate shader name, both
  `WILL_FAIL`.
- Document the `--link` workflow in `gte/docs/api/Shaders.rst` and update the format spec
  in `OmegaSL-Reference.md`.

---

## 4. Feature 2 — `.omegaslh` headers and shader rejection in includes

### Model

- `.omegaslh` (new recommended extension) = a **header**: shared declarations only —
  `struct`s, resource declarations (`buffer<T>`, `texture2d`, `sampler2d`, …), plain
  helper `func` definitions, and constants. Everything in `blinn_phong.omegasl:2-30`
  *except* the `vertex`/`fragment` blocks is header-legal.
- `.omegasl` = a **translation unit** that owns shader entry points.
- A header is `#include`d into one or more translation units; each unit compiles to its
  own `.omegasllib`; the Feature 1 linker merges them.

A shader entry point in a header would be inlined into every including unit (§1.4),
duplicating it. So the preprocessor must forbid it.

### Phase 4 — `.omegaslh` convention + extension advisory

- Treat `.omegaslh` as the recommended header extension.
- When an `#include` path does **not** end in `.omegaslh`, emit an advisory **warning**
  (not an error) nudging toward the convention — e.g. including a `.omegasl` as a header.
  Warn-only keeps existing include sites working. See **Open Decision D4**.

### Phase 5 — Reject shader functions in `#include`d files

In `Preprocessor::processInternal`, inside the `#include` branch (`Preprocessor.cpp:331`):

1. Recursively process the included file as today, capturing the result in a local string
   (instead of streaming it straight into `out`).
2. Run a **stage-keyword scan** over that processed string by feeding it to the existing
   `Lexer` and looking for any `TOK_KW` token whose text is one of
   `{vertex, fragment, compute, hull, domain, mesh}` (§1.5). Reusing the Lexer gives
   correct comment/string handling and a line number for free.
3. If found: emit a precise error — included path + line number + which stage keyword —
   set `hasErrors_ = true`, and **do not** append the content. Mirrors the existing loud
   include-rejection style at `Preprocessor.cpp:340-345`.
4. If clean: append the processed content as before.

Properties:
- Only **`#include`d** files are restricted; the root `.omegasl` translation unit may
  declare shaders freely (the scan runs only on included content).
- Nested includes are covered: a header that includes another header containing a shader
  is caught at the inner include, before the outer one appends anything.
- This is a pure frontend change (preprocessor only), so it is **uniform across all three
  backends by construction** — there is no per-backend code path to diverge.

This phase is small (well under the ~300-line threshold in AGENTS.md), so it is a single
focused change rather than a sub-phased subsystem. Couples `Preprocessor` → `Lexer`; see
**Open Decision D5** for the self-contained-scan alternative.

### Phase 6 — Tests + docs (headers)

- Negative CTest (`add_omegasl_fail_test`): a `.omegasl` that `#include`s an `.omegaslh`
  containing `fragment float4 f(...) {...}` ⇒ `WILL_FAIL`, with the error message
  asserted.
- Positive CTest: a `.omegasl` that `#include`s an `.omegaslh` containing only a `struct`,
  a `buffer<>` resource decl, and a helper `func` ⇒ compiles clean, and the helper is
  usable from the including unit's shader.
- A comment/string false-positive guard test: an `.omegaslh` whose only occurrence of
  `// fragment ...` is in a comment or a string literal ⇒ compiles clean (proves the
  scan is token-based, not substring-based).
- Document the header model and the rule in `gte/docs/api/Shaders.rst`.

---

## 5. Open decisions (developer owns these)

- **D1 — Add format versioning now, or ship the linker without it?**
  *Recommendation: add it (Phase 0).* Without a backend tag the linker cannot detect a
  DXIL+SPIR-V merge and will emit a silently-corrupt archive. Fallback if deferred:
  require an explicit `--backend hlsl|metal|glsl` flag on `--link` and trust the user —
  weaker, and still cannot validate the inputs actually match.

- **D2 — `--link` subcommand of `omegaslc`, or a separate `omegasllink` executable?**
  *Recommendation: `--link` mode in `omegaslc`* — it already links the serializer TU and
  owns the format; one tool, less build wiring. A separate executable gives cleaner
  separation of concerns if preferred (the shared `LibContainer` module supports either).

- **D3 — Duplicate shader name policy when merging.**
  *Recommendation: hard error by default* (loud, deterministic). Optional `--allow-override`
  (last input wins) could be added later if a real use case appears.

- **D4 — Non-`.omegaslh` include extension: warn or hard-error?**
  *Recommendation: warn only.* A hard error would break any project that includes a
  `.omegasl` of pure declarations. The shader-content rejection (Phase 5) is the real
  guard; the extension is a convention.

- **D5 — Header scan: reuse the `Lexer`, or a self-contained scan in the preprocessor?**
  *Recommendation: reuse the Lexer* — single source of truth for "what is a stage
  keyword," correct comment/string handling, free line numbers. The cost is a
  `Preprocessor → Lexer` include dependency (today the preprocessor depends only on
  `omegasl.h`). A self-contained mini-scanner avoids the coupling but re-implements
  comment/string skipping — a place for drift.

---

## 6. Suggested sequencing

1. **Phase 5 + 6 (headers)** first — smallest, fully frontend, no format change, immediately
   testable on Vulkan, and it makes the multi-translation-unit workflow *safe* before the
   linker makes it *possible*.
2. **Phase 0** (format versioning) — the foundation the linker's safety rests on.
3. **Phase 1** (shared serializer) — removes the lockstep hazard; gives the linker a tested
   read/write core.
4. **Phase 2 + 3** (the link command + its tests).

Each phase is independently reviewable and verifiable. Phases 0/1 change `GE.cpp`, which
compiles for all three backends but is only run-verifiable on Vulkan here — D3D12/Metal
build-and-load confirmation must come from the user on those platforms.

---

## 7. Verification surface summary

| Phase | Verifiable here (Vulkan/Linux) | Needs user confirmation (D3D12 / Metal) |
|-------|--------------------------------|------------------------------------------|
| 0 Format header | writer+reader round-trip on GLSL libs | D3D12/Metal `GE.cpp` reader compile + load |
| 1 Serializer    | byte-identical round-trip golden test  | D3D12/Metal loader path |
| 2 Link command  | merge two GLSL libs, entry-count + round-trip | merge libs for D3D12/Metal backends |
| 3 Linker tests  | CTest (merge, mismatch, dup-name)      | — (host-agnostic container reads) |
| 4 Extension warn| CTest on warning emission              | — (frontend only) |
| 5 Shader reject | CTest negative + positive + comment-guard | — (frontend only, uniform) |
| 6 Header tests  | CTest                                  | — |
