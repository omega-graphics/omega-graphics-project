# KRT Naming + Submodule Split — Implementation Plan

## Context

Two rename axes are landing together because doing them separately would
churn the same files twice:

1. **Type-prefix rename.** Every public type in the `Kreate` namespace gets
   a `KRT` prefix (`App` → `KRTApp`, `Object` → `KRTObject`, etc.).
   Static methods on those types do **not** get prefixed (the type
   already carries it).
2. **Library split.** The single `KREATE` shared library is broken into
   per-submodule libraries with the `KRT_<Submodule>` naming convention
   (`KRT_Core`, `KRT_Renderer`, `KRT_Pipeline`, `KRT_Mesh`, `KRT_Platform`,
   plus a foundational `KRT_Math`).

Locked-in decisions (see chat for derivation):

| # | Decision | Choice |
|---|---|---|
| 1 | Binary split scope | **Split** into per-submodule libraries |
| 2 | Library name separator | `KRT_Renderer` (underscore, NOT literal `.`) |
| 3 | Free functions (`Kreate::CreateApp`) | **No** prefix |
| 4 | Macro / header-guard rename (`KREATE_EXPORT`, `KREATE__BUILD__`, `KREATE_*_H`) | **Keep as-is** |
| 5 | Namespace + include path | `Kreate` namespace and `<kreate/...>` path **stay** |

---

## Decision still surfaced

The split forces **new** per-submodule export macros (each library is its
own DLL boundary on Windows). This is additive, not a rename of
`KREATE_EXPORT`, but it does mean public headers stop saying
`KREATE_EXPORT` and start saying `KRT_CORE_EXPORT` / `KRT_RENDERER_EXPORT`
/ etc. Decision 4 was framed as "don't rename for visual consistency"; the
split makes this structural, not cosmetic.

**Proposal:** introduce `KRT_<MOD>_EXPORT` + `KRT_<MOD>__BUILD__` per
library, defined together in `Base.h`. Each public header switches its
annotation to its owning submodule's macro. `KREATE_EXPORT` is retired
once no header references it. If you want `KREATE_EXPORT` kept as an
umbrella alias, say so before Phase 2 starts.

---

## Target architecture

```
                  KRT_Math   (header-clean of GTE; .cpp uses GTEMath)
                    ▲   ▲   ▲   ▲
                    │   │   │   │
       ┌────────────┼───┘   │   └────────────┐
       │            │       │                │
   KRT_Mesh   KRT_Pipeline  KRT_Renderer   KRT_Platform
       ▲            ▲       ▲                ▲
       └────────────┴───┬───┴────────────────┘
                        │
                     KRT_Core   (App, Scene, Object)
                        ▲
                  Game (BasicGame, user games)
```

All five engine libraries link `OmegaGTE`. `KRT_Core` is the only one that
games link directly — it transitively pulls the rest. `KRT_Math` is the
sink so the leaves stay parallel rather than chaining through `Core`.

### Per-library contents

| Library | Public headers | Sources |
|---|---|---|
| `KRT_Math` | `Math.h`, `Base.h` | `src/Math.cpp` |
| `KRT_Mesh` | `Mesh.h` | `src/mesh/Mesh.cpp`, `src/mesh/MeshFactory.h` (private) |
| `KRT_Pipeline` | `Pipeline.h` | `src/pipeline/Pipeline.cpp`, `src/pipeline/PipelineFactory.h` (private) |
| `KRT_Renderer` | (none public — `src/renderer/Renderer.h` becomes a cross-submodule internal header) | `src/renderer/Renderer.cpp` |
| `KRT_Platform` | `Window.h` | `src/platform/<os>/*Window.{cpp,mm}` (one per OS picker) |
| `KRT_Core` | `App.h`, `Scene.h`, `Object.h` | `src/App.cpp`, `src/Scene.cpp`, `src/Object.cpp` |

`Renderer.h` stays in `src/renderer/` but `KRT_Core` adds it as a private
include directory — that's enough for `App` to own a `Renderer` via
pimpl without promoting it to public API.

---

## New / moved files

No new source files — this is a rename + CMake restructure. The plan
adds:

| File | Purpose |
|---|---|
| `kreate/cmake/KrtSubmodule.cmake` (proposed) | Tiny helper that wraps `add_omega_graphics_module` for the five submodules — keeps `kreate/CMakeLists.txt` flat. Only if the repeated boilerplate looks ugly; skip if not. |

## Modified files (overview)

- All 8 public headers in `kreate/include/kreate/*.h` — type renames + per-submodule export macro swap.
- All sources in `kreate/src/**` (toplevel + `mesh/`, `pipeline/`, `renderer/`, `platform/<os>/`) — type renames at use sites.
- `kreate/CMakeLists.txt` — five `add_omega_graphics_module` calls instead of one; per-library `target_compile_definitions(... KRT_<MOD>__BUILD__)`.
- `kreate/cmake/KreateGame.cmake` — link `KRT_Core` instead of `KREATE`; stage all six runtime DLLs on Windows.
- Root `CMakeLists.txt:116` — `omega_graphics_add_subdir(KREATE kreate)` → still passes `kreate/` as the subdir but the umbrella name is gone; the kreate `CMakeLists.txt` now defines five targets directly. Keep the call signature working with whatever `omega_graphics_add_subdir` expects (TBD when Phase 3 lands).
- `kreate/tests/BasicGame.cpp` — type renames at use sites; still links `KRT_Core` only.
- `kreate/AGENTS.md`, `kreate/README.md`, `kreate/cmake/KreateGame.cmake` header comments — name references.
- Codedb area map `utils/omega-codedb/OMEGA-Project.json` — the existing areas already map to the right folders; the area `name` fields stay descriptive ("Omega kREATE Renderer" etc.), no change needed unless we want the area names to reflect the new library names. **Open question — leave area labels alone.**

---

## Phases

The whole refactor is **roughly 800–1200 lines of diff** dominated by
mechanical renames, but the binary split is a real architectural change.
Splitting the work into four phases keeps each landing reviewable on its
own.

### Phase 1 — Public type renames (no binary changes)

Rename every public type in the `Kreate` namespace to add the `KRT`
prefix. Static methods and the `Kreate::CreateApp` free function are
**not** renamed. Headers, sources, and the `BasicGame` test all land
together — the build still produces a single `KREATE` library at the
end of this phase.

**Renames:**

| Old | New |
|---|---|
| `Kreate::App` | `Kreate::KRTApp` |
| `Kreate::AppDesc` | `Kreate::KRTAppDesc` |
| `Kreate::Window` | `Kreate::KRTWindow` |
| `Kreate::WindowDesc` | `Kreate::KRTWindowDesc` |
| `Kreate::Scene` | `Kreate::KRTScene` |
| `Kreate::Object` | `Kreate::KRTObject` |
| `Kreate::Pipeline` | `Kreate::KRTPipeline` |
| `Kreate::PipelineDesc` | `Kreate::KRTPipelineDesc` |
| `Kreate::Mesh` | `Kreate::KRTMesh` |
| `Kreate::MeshDesc` | `Kreate::KRTMeshDesc` |
| `Kreate::Vec3` | `Kreate::KRTVec3` |
| `Kreate::Vec4` | `Kreate::KRTVec4` |
| `Kreate::Color` | `Kreate::KRTColor` |
| `Kreate::Mat4` | `Kreate::KRTMat4` |
| `Kreate::MeshTopology` | `Kreate::KRTMeshTopology` |
| `Kreate::IndexFormat` | `Kreate::KRTIndexFormat` |
| `Kreate::CullMode` | `Kreate::KRTCullMode` |
| `Kreate::FillMode` | `Kreate::KRTFillMode` |

**Not renamed:**

- `Kreate::CreateApp()` — free function.
- All static methods (`KRTObject::create`, `KRTScene::create`, `KRTMat4::perspective`, `KRTMat4::lookAt`, `KRTMat4::rotation`).
- The `Kreate::VertexAttribute` namespace (namespaces aren't types).
- `Renderer` (internal — gets touched in Phase 3 when the split happens, NOT here).
- Macros (`KREATE_EXPORT`, `KREATE__BUILD__`, `KREATE_*_H`).

**Exit criteria:** Vulkan native build green on Linux, `BasicGame`
runs and shows the spinning cube. Single `KREATE` library still
produced.

### Phase 2 — Introduce per-submodule export macros

Additive only; no library split yet. Defines the new
`KRT_<MOD>_EXPORT` / `KRT_<MOD>__BUILD__` macros in `Base.h`, and
updates each public header to use its submodule's macro instead of
`KREATE_EXPORT`. Until Phase 3 turns on the split, all five macros
expand to the same `KREATE__BUILD__`-gated definition, so the single
library still works.

**Macros defined in `Base.h`:**

```cpp
// While the single KREATE library is still in use (pre-Phase 3),
// every KRT_<MOD>_EXPORT collapses to KREATE_EXPORT.
#define KRT_CORE_EXPORT     KREATE_EXPORT
#define KRT_MATH_EXPORT     KREATE_EXPORT
#define KRT_MESH_EXPORT     KREATE_EXPORT
#define KRT_PIPELINE_EXPORT KREATE_EXPORT
#define KRT_RENDERER_EXPORT KREATE_EXPORT
#define KRT_PLATFORM_EXPORT KREATE_EXPORT
```

Phase 3 swaps each definition to use its own `KRT_<MOD>__BUILD__`
gate.

**Header migration:**

- `App.h`, `Scene.h`, `Object.h` → `KRT_CORE_EXPORT`
- `Math.h` → `KRT_MATH_EXPORT`
- `Mesh.h` → `KRT_MESH_EXPORT`
- `Pipeline.h` → `KRT_PIPELINE_EXPORT`
- `Window.h` → `KRT_PLATFORM_EXPORT`
- (No public Renderer.h.)

**Exit criteria:** build still produces single `KREATE` library; no
behavioral change.

### Phase 3 — Library split

The real architectural change. `kreate/CMakeLists.txt` is rewritten
to define five (six counting Math) libraries instead of one, each
with its own `KRT_<MOD>__BUILD__` define and its own dependency edges.

**3.1 — `KRT_Math`.** Smallest leaf. `src/Math.cpp` becomes its own
library; links `OmegaGTE` (for `GTEMath`). Public include dir
exposes only `Math.h` + `Base.h`.

**3.2 — `KRT_Mesh`, `KRT_Pipeline`.** Independent leaves. Each
takes its `src/<dir>/*.cpp`, depends on `KRT_Math` + `OmegaGTE`. The
private factory headers (`MeshFactory.h`, `PipelineFactory.h`) stay
in `src/` and are visible only to that library.

**3.3 — `KRT_Renderer`.** `src/renderer/Renderer.cpp` + `Renderer.h`.
Renderer.h stays at `src/renderer/Renderer.h`. `KRT_Core` adds
`src/renderer/` as a private include dir + declares
`target_link_libraries(KRT_Core PRIVATE KRT_Renderer)` so the App
pimpl can still own a `Renderer&`. Depends on `KRT_Math`,
`KRT_Pipeline`, `KRT_Mesh`, `OmegaGTE`.

**3.4 — `KRT_Platform`.** Same OS-picker pattern as today, but
now its own library. One source per platform
(`Win32Window.cpp` / `X11Window.cpp` / `CocoaWindow.mm` /
`UIKitWindow.mm` / `AndroidWindow.cpp`) with the corresponding
system-framework / X11 / android-log linkage moved here. Public
header is `Window.h`. Depends on `KRT_Math`, `OmegaGTE`.

**3.5 — `KRT_Core`.** Top of the graph. Sources: `src/App.cpp`,
`src/Scene.cpp`, `src/Object.cpp`. Public headers: `App.h`,
`Scene.h`, `Object.h`. `PUBLIC` link deps: `KRT_Math`, `KRT_Mesh`,
`KRT_Pipeline`, `KRT_Platform`, `OmegaGTE` (so a game linking
`KRT_Core` gets the full type-visible surface). `PRIVATE` link dep:
`KRT_Renderer`. `target_compile_definitions(... PRIVATE KRT_CORE__BUILD__)`.

**3.6 — Flip `Base.h`.** Each `KRT_<MOD>_EXPORT` macro now expands
based on its own `KRT_<MOD>__BUILD__` gate instead of the umbrella
alias. The umbrella `KREATE_EXPORT` and `KREATE__BUILD__` stay
defined (decision 4: no macro rename) but are no longer used by
public headers.

**Exit criteria:** six libraries built; `BasicGame` links only
`KRT_Core` and still runs; Windows DLL set in `${CMAKE_BINARY_DIR}/bin/`
is `KRT_Math.dll`, `KRT_Mesh.dll`, `KRT_Pipeline.dll`, `KRT_Renderer.dll`,
`KRT_Platform.dll`, `KRT_Core.dll`.

### Phase 4 — KreateGame.cmake + docs + codedb

**4.1 — `KreateGame.cmake`.** Replace every `target_link_libraries(... PRIVATE KREATE)` with
`target_link_libraries(... PRIVATE KRT_Core)`. The macOS branch's
`EMBEDDED_FRAMEWORKS OmegaGTE` and `add_app_bundle DEPS KREATE OmegaGTE.framework ...`
list expands to embed all six KRT libraries. Windows
`omega_stage_runtime_dlls` already fans out everything in `bin/`,
so it should pick up the new DLL set automatically — **verify**.

**4.2 — Docs.** Update `kreate/AGENTS.md`, `kreate/README.md`, and
the `KreateGame.cmake` header block to reference the six libraries
and the `KRT`-prefixed types. Leave the namespace explanation
("all code is under namespace `Kreate`") unchanged.

**4.3 — Codedb.** Run
`python3 utils/omega-codedb/codedb.py index --rebuild`. Spot-check
that the renamed types show up in `find <KRTApp>` and that the area
labels still resolve.

**4.4 — Plan move.** Once Phase 4 is shipped, move this plan to
`kreate/.plans/done/`.

---

## Risk + sequencing notes

- **CRLF churn (project memory).** The whole tree shows as modified
  due to CRLF; each phase should commit only the files actually
  touched, and reviews should use `git diff --ignore-cr-at-eol`.
- **Common identifier names.** `App`, `Window`, `Scene`, `Mesh`, `Object`
  are very common. Strictly scope every rename to `kreate/` (plus the
  one root `CMakeLists.txt:116` line in Phase 3) so no other module
  gets clipped.
- **Windows-only build paths.** Phases 3 and 4 introduce per-library
  DLL boundaries — the only place `__declspec(dllexport/dllimport)`
  actually matters. Linux/macOS visibility is currently a no-op in
  `Base.h`, so the Linux Vulkan build will pass cleanly long before
  the Windows side is verified. **Per AGENTS.md "Building"**: every
  phase that touches CMake or export macros must be handed off to
  the user for a Windows build verification before being declared
  done.
- **`add_kreate_game` for Android.** Android currently uses
  `target_link_libraries(... PRIVATE KREATE android log)`. After
  the split it's `target_link_libraries(... PRIVATE KRT_Core android log)`
  with `KRT_Platform` indirectly pulling android/log via PUBLIC
  link from inside `KRT_Platform`'s Android branch. Confirm the
  PUBLIC/PRIVATE split is right when Phase 3.4 lands.
- **Single landing.** None of the four phases should land as
  intermediate broken builds — each phase's exit criterion is "the
  Linux Vulkan build + BasicGame still run." Phase 2 in particular
  is a no-op at runtime but lays the groundwork for Phase 3.

---

## Out of scope

- Renaming the `Kreate` namespace, the `<kreate/...>` include path, or
  the `KREATE_EXPORT` / `KREATE__BUILD__` / `KREATE_*_H` macros
  (decisions 4 + 5).
- Renaming `Kreate::CreateApp` (decision 3).
- Renaming static methods on the prefixed types (the type carries the
  prefix; the method names stay).
- Splitting `KRT_Core` further (App/Scene/Object stay together — they
  share too much state to be worth separating today).
- Promoting `Renderer.h` to public API. It remains a cross-submodule
  internal header.
