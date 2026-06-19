# OmegaSL Linker + `.omegaslh` Headers — Implementation Plan

**Status:** Fully implemented 2026-06-19. Feature 2 — Phases 4, 5, 6 (extension advisory +
shader-in-header rejection + tests/docs). Feature 1 — Phases 0 (format versioning + backend
tag), 1 (shared `ShaderArchive` (de)serializer), 2 (`--link` command), 3 (linker tests +
docs). All verified on Vulkan/Linux; D3D12/Metal load/link confirmation is the only
outstanding off-platform item (see the per-phase cross-backend callouts).
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

### Phase 0 — Format versioning + backend tag (foundation) — **DONE (2026-06-19)**

> Implemented. The 12-byte prefix (magic `OSLL`, uint32 version=1, uint8 backend_id,
> 3 reserved) is emitted by `CodeGen::linkShaderObjects` and validated by
> `GE.cpp::loadShaderLibraryFromInputStream`. To kill the writer/reader drift hazard
> (§1.3.3) the magic + version constants live in **one** place — `gte/include/omegasl.h`
> (`OMEGASLLIB_MAGIC` / `OMEGASLLIB_FORMAT_VERSION` + an `omegasl_backend_id` enum whose
> values match `Target::Kind`, so the writer casts `target->kind()` directly). The reader
> rejects bad magic and unknown `format_version` loudly; it reads-and-skips `backend_id`
> (the engine only loads its own backend's lib — the cross-backend check is the future link
> tool's job). **D1 resolved: versioning added now.**
>
> **Verification (this Vulkan/Linux host):** byte-checked a generated lib (magic `OSLL`,
> version 1, backend_id 2/GLSL, reserved 0). Added a new **writer↔reader round-trip test**
> `omegagte_lib_file_roundtrip` (`gte/tests/vulkan/LibFileRoundTripTest/`): omegaslc compiles
> a fixture into a real `.omegasllib`, the engine loads it back and resolves the shader, and
> two corrupted copies (bad magic, bumped version) are both rejected — all three pass. This
> is the *only* test that exercises the file (not runtime) library path on Vulkan; every
> other GTE shader test uses the in-memory `loadShaderLibraryRuntime`, which doesn't touch
> the format. Full omegasl suite 116/116; full Vulkan suite 7/7 (run with the bundled SDK
> validation layers — `source gte/deps/vulkan_sdk/1.3.283.0/setup-env.sh` — or the system VVL
> perturbs the unrelated mesh-shader test).
>
> **Cross-backend (needs user confirmation off-platform):** the D3D12 and Metal readers
> compile the same `GE.cpp` and the writer is the single shared one, but only the GLSL/Vulkan
> round-trip is run-verified here. D3D12/Metal compile + load of a prefixed lib must be
> confirmed on those platforms.

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

### Phase 1 — Extract a shared container (de)serializer — **DONE (2026-06-19)**

> Implemented as `gte/omegasl/src/ShaderArchive.{h,cpp}` (struct `OmegaSLShaderArchive`,
> `ReadShaderArchive` / `WriteShaderArchive`), compiled into both `omegaslc` and the engine
> via the `OMEGASL_SRCS` glob (needs a `cmake` reconfigure to pick up the new file).
>
> **1a (writer):** `CodeGen::linkShaderObjects` now builds an archive (reading each object
> file's bytes into archive-owned storage, metadata aliasing CodeGen memory) and calls
> `WriteShaderArchive`. Verified **byte-identical**: generated 6 libs (vertex/fragment,
> compute, buffers/layout, vertex-params) with the pre-refactor binary and `cmp`'d against
> the refactored binary — all identical.
>
> **1b (reader):** `loadShaderLibraryFromInputStream` now `ReadShaderArchive`s then loops
> the records doing feature-gating + `_loadShaderFromDesc`; the per-stage parsing and the
> hand-rolled binary-read helpers/limits are gone from `GE.cpp`. The old per-buffer
> `.release()` **leak** is replaced by the archive itself: `GTEShaderLibrary` gained a
> type-erased `std::shared_ptr<void> _backingStore` that owns the archive, so the records'
> pointers (aliased into `GTEShader::internal`, dereferenced at pipeline creation — e.g.
> `internal.pLayout` in `GEVulkan.cpp`) stay valid for the library's lifetime, then free
> with it. Runtime-loaded libs leave `_backingStore` null (records owned by the caller).
>
> **Verification (Vulkan/Linux):** new host-only CTest `omegasl_archive_roundtrip` proves
> `Read`∘`Write` is byte-identical on a real lib (no GPU). The engine round-trip test
> (`omegagte_lib_file_roundtrip`) was strengthened to also **build a compute pipeline** from
> the file-loaded shader — exercising the post-load `internal.pLayout` read that the new
> lifetime must keep valid. omegasl suite 118/118; full Vulkan suite 7/7 (run with the
> bundled SDK validation layers — see the mesh-shader note in Phase 0).
>
> **Cross-backend (needs user confirmation off-platform):** the writer is the single shared
> one; the `GE.cpp` reader refactor compiles for all three backends but is only run-verified
> on Vulkan here. The lifetime change (`_backingStore` vs the old leak) is backend-neutral,
> but D3D12/Metal load + pipeline-build of a real lib must be confirmed on those platforms —
> their `_loadShaderFromDesc` also copies `omegasl_shader` and reads `internal.pLayout` at
> pipeline creation, so the same "archive must outlive the shaders" guarantee applies.

New module: `gte/omegasl/src/ShaderArchive.{h,cpp}` (linked into both `omegaslc` and the
engine; pure stdlib + `omegasl.h`, no GTE device dependency).

```cpp
struct OmegaSLArchive {            // owns all backing storage
    std::string name;
    uint8_t backendId = 0;
    uint32_t formatVersion = 1;
    std::vector<omegasl_shader> shaders;     // pointers into the buffers below
    // owning buffers: names, data blobs, layout arrays, vertex-param arrays
};

bool ReadShaderArchive (std::istream&, OmegaSLShaderArchive& out, std::string& err); // pure parse, no device
bool WriteShaderArchive(std::ostream&, const OmegaSLShaderArchive&,  std::string& err);
```

- `ReadShaderArchive` parses the container into owned memory and performs **no** device
  feature-gating and **no** `_loadShaderFromDesc` — it stops at raw records. This is the
  reusable core the linker needs (it must read a lib with no GPU device present).
- Refactor `CodeGen::linkShaderObjects` (`CodeGen.h:497`) to: load each compiled object
  file's bytes into a record, then call `WriteShaderArchive`.
- Refactor `GE.cpp:143` loader to: `ReadShaderArchive`, then per record run the existing
  feature-gating (`GE.cpp:349-360`) and `_loadShaderFromDesc`. The fiddly manual
  `.release()` ownership transfer in the current reader (`GE.cpp:362-369`) is replaced by
  the container owning its buffers.
- This collapses the three-copies-must-match hazard (§1.3.3) into one tested module.

**Verification:** a byte-identical round-trip golden test — `ReadShaderArchive` an existing
generated lib, `WriteShaderArchive` it back, assert the two byte streams are identical.

This phase is the largest single change and may be split into **1a** (writer extraction)
and **1b** (reader extraction) so each lands and is verified independently.

> Cross-backend callout: same as Phase 0 — the `GE.cpp` reader refactor is exercised on
> Vulkan here; D3D12/Metal compile-and-load must be user-confirmed.

### Phase 2 — The link command — **DONE (2026-06-19)**

> Implemented as a `--link` mode in `omegaslc` (`runLink` in `main.cpp`), per **D2**. A
> pre-scan in `main` returns to `runLink` before the front-end (builtins/parser/codegen)
> spins up — it reads no `.omegasl`, preprocesses nothing, invokes no toolchain, needs no
> GPU. It `ReadShaderArchive`s each input (keeping them alive so the merged records can alias
> their owned buffers), checks every input shares the first input's `backend_id` and
> `format_version` (`ReadShaderArchive` already rejected bad magic / unknown version), merges
> the entry lists with a **hard error on a duplicate shader name** (per **D3**; the
> `--allow-override` escape hatch is deferred), and `WriteShaderArchive`s the result with the
> `--lib-name` (default = output file name). Help text updated. Manually verified end to end:
> merging two 1-shader libs yields a 2-shader lib with the right prefix; duplicate-name and
> backend-mismatch inputs both exit nonzero with a precise diagnostic.

Add a `--link` mode to `omegaslc` (recommended — reuses the serializer TU already linked
into the compiler; see **Open Decision D2** for the separate-executable alternative):

```
omegaslc --link in1.omegasllib in2.omegasllib [in3...] -o out.omegasllib [--lib-name NAME]
```

Behavior:
1. `ReadShaderArchive` each input (no toolchain invoked — pure container reads).
2. Validate compatibility across inputs: identical `magic`, identical `format_version`,
   identical `backend_id`. Any mismatch ⇒ precise error naming the offending file, exit
   nonzero. (This is the payoff of Phase 0.)
3. Merge entry lists. On a **duplicate shader name**, error by default (see
   **Open Decision D3** for an override policy).
4. Write the merged container with `entry_count = Σ inputs` and the chosen library name
   (`--lib-name`, default = output filename stem) via `WriteShaderArchive`.

The `--link` mode short-circuits the normal parse/codegen path in `main.cpp` — it never
reads `.omegasl` source, never preprocesses, never invokes dxc/metal/glslc.

### Phase 3 — Tests + docs (linker) — **DONE (2026-06-19)**

> Six CTests added (all passing; omegasl suite now 124/124). A single fixture-setup step
> (`SetupLinkFixtures.cmake`) compiles `link_a.omegasl` + `link_b.omegasl` (distinct compute
> entry points), `--link`s them into `link_merged.omegasllib`, and writes a backend-bumped
> variant of A. Tests:
> - `omegasl_link_merge` — positive: the merged lib holds **2** shaders (Σ inputs) and is
>   byte-identical through `ReadShaderArchive`→`WriteShaderArchive` (the `ArchiveRoundTripTest`
>   helper gained an optional expected-count arg).
> - `omegasl_link_dup_fail` / `_dup_diag` — linking a lib with itself: nonzero exit +
>   `"duplicate shader name"` diagnostic.
> - `omegasl_link_backend_fail` / `_diag` — linking against the backend-variant: nonzero exit
>   + `"backend mismatch"` diagnostic. The variant is produced cross-platform by a new
>   `ArchiveRoundTripTest backend-variant` subcommand (bumps `backend_id` to a different
>   backend — no second graphics platform needed). Docs: `--link` workflow added to
>   `gte/docs/api/Shaders.rst`, format/`--link` note added to `OmegaSL-Reference.md` §9.

- CTest: compile two small `.omegasl` files → two libs → `--link` them → assert the merged
  lib's `entry_count` equals the sum and round-trips through `ReadShaderArchive`.
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

### Phase 4 — `.omegaslh` convention + extension advisory — **DONE (2026-06-19)**

> Implemented in `Preprocessor.cpp`: `hasOmegaslhExtension()` (anon namespace) + a warn-only
> `std::cerr` advisory in the `#include` branch, keyed on the user-written include path and
> placed past the `rejectIncludes_` guard (so the runtime path, which resolves no includes,
> never emits it). Warn-only per **D4** — does not set `hasErrors_`, the include is still
> processed. Verified zero blast radius: no existing `.omegasl`/`.omegaslh` in the tree uses
> `#include` (only the new fixtures do). Tests: `omegasl_compile_non_omegaslh_include`
> (asserts exit 0 — warn ≠ error) + `omegasl_warn_non_omegaslh_ext` (asserts the advisory
> text), fixtures `shared_decls.omegasl` (declarations only, non-`.omegaslh` on purpose) +
> `include_non_omegaslh_header.omegasl`. Documented in `Shaders.rst`.

- Treat `.omegaslh` as the recommended header extension.
- When an `#include` path does **not** end in `.omegaslh`, emit an advisory **warning**
  (not an error) nudging toward the convention — e.g. including a `.omegasl` as a header.
  Warn-only keeps existing include sites working. See **Open Decision D4**.

### Phase 5 — Reject shader functions in `#include`d files — **DONE (2026-06-19)**

> Implemented in `Preprocessor.{h,cpp}` (`includeDeclaresShader` + `isStageKeyword`,
> reusing the `Lexer` per D5) and the `#include` branch of `processInternal`.
>
> **Gap caught during implementation:** the offline driver (`main.cpp`) never checked
> `Preprocessor::hasErrors()` after `process()` — only the runtime path
> (`omegasl_runtime.cpp:104`) did. Setting `hasErrors_` alone would have silently dropped
> the offending include and let the unit compile to exit 0 (the negative test could never
> fail). Added a `hasErrors()` gate in `main.cpp` right after `process()` (Cleanup + return
> 1) mirroring the runtime path. The `omegasl_reject_shader_in_header` WILL_FAIL test exists
> specifically to guard this exit-code gate against regression.
>
> **Backend verification:** this is a pure frontend change in the preprocessor, which runs
> *before* backend selection — uniform across HLSL/MSL/GLSL by construction (no per-backend
> code path). The rejection itself is therefore fully verified here. The positive/guard test
> *shaders* were compiled to a real lib only via the GLSL/Vulkan toolchain on this host; on
> D3D12/Metal the identical preprocessor runs, so only the backend object-emission of those
> two positive fixtures is unverified off-platform (the rejection logic is not).

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

### Phase 6 — Tests + docs (headers) — **DONE (2026-06-19)**

> All four tests added to `gte/omegasl/tests/CMakeLists.txt` and passing (114/114 in the
> omegasl suite, no regressions). Fixtures: `header_with_shader.omegaslh` +
> `include_shader_in_header.omegasl` (negative), `header_decls.omegaslh` +
> `include_clean_header.omegasl` (positive), `header_comment_keywords.omegaslh` +
> `include_comment_guard.omegasl` (guard). The message assertion uses a new
> `add_omegasl_diag_test` helper (`PASS_REGULAR_EXPRESSION`) since the existing
> `add_omegasl_fail_test` only checks the exit code — both are wired so exit-code *and*
> diagnostic text are each guarded. The guard fixture additionally exercises the
> token-vs-substring distinction with an identifier (`fragment_like_helper`) that contains a
> stage keyword as a substring. Docs added to `gte/docs/api/Shaders.rst`
> ("Sharing declarations with `.omegaslh` headers"). The `--link` workflow / format-spec
> docs are deferred with Feature 1.

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

- **D1 — Add format versioning now, or ship the linker without it?** *Resolved
  (2026-06-19): added now (Phase 0).* Without a backend tag the linker cannot detect a
  DXIL+SPIR-V merge and will emit a silently-corrupt archive. Fallback if deferred:
  require an explicit `--backend hlsl|metal|glsl` flag on `--link` and trust the user —
  weaker, and still cannot validate the inputs actually match. The 12-byte magic/version/
  backend prefix is in place, so the linker (Phase 2) can validate inputs by construction.

- **D2 — `--link` subcommand of `omegaslc`, or a separate `omegasllink` executable?**
  *Resolved (2026-06-19): `--link` mode in `omegaslc`* — it already links the serializer TU
  and owns the format; one tool, less build wiring. A separate executable gives cleaner
  separation of concerns if preferred (the shared `ShaderArchive` module supports either).

- **D3 — Duplicate shader name policy when merging.** *Resolved (2026-06-19): hard error by
  default* (loud, deterministic). Optional `--allow-override` (last input wins) could be
  added later if a real use case appears — deferred (no use case yet).

- **D4 — Non-`.omegaslh` include extension: warn or hard-error?** *Resolved (2026-06-19):
  warn only.* A hard error would break any project that includes a
  `.omegasl` of pure declarations. The shader-content rejection (Phase 5) is the real
  guard; the extension is a convention. (Implemented in Phase 4.)

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
| 0 Format header | **DONE** — byte-check + `omegagte_lib_file_roundtrip` (load + reject bad-magic/bad-version) | D3D12/Metal `GE.cpp` reader compile + load |
| 1 Serializer    | **DONE** — `omegasl_archive_roundtrip` (Read∘Write byte-identical) + byte-identical writer vs pre-refactor + pipeline-build lifetime guard | D3D12/Metal loader path |
| 2 Link command  | **DONE** — merge two GLSL libs, entry-count + round-trip | merge libs for D3D12/Metal backends |
| 3 Linker tests  | **DONE** — CTest (merge, mismatch, dup-name; 6 tests) | — (host-agnostic container reads) |
| 4 Extension warn| **DONE** — CTest: compile (exit 0) + diag (warning text) | — (frontend only) |
| 5 Shader reject | **DONE** — CTest negative + diag + positive + comment-guard (114/114) | — (frontend only, uniform; positive-fixture object emission only) |
| 6 Header tests  | **DONE** — CTest + Shaders.rst docs    | — |
