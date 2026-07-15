# KRT Naming + Submodule Split + API Redesign — Implementation Plan

## Context

Three changes are landing together because they churn the same files and
only make sense as one coherent reshaping of kREATE's surface:

1. **Type-prefix rename.** Every public type in the `Kreate` namespace gets
   a `KRT` prefix (`App` → `KRTApp`, `Object` → `KRTObject`, etc.).
   Static methods on those types do **not** get prefixed (the type
   already carries it).
2. **Library split.** The single `KREATE` shared library is broken into
   **three** per-submodule libraries — `KRTBase`, `KRTPipeline`,
   `KRTScene` (no underscore, no separator).
3. **API redesign.** The public surface is corrected in three ways that the
   original rename-only plan would otherwise have frozen in place:
   - **Pipelines leave the public API.** `Pipeline` / `PipelineDesc` /
     `CullMode` / `FillMode` become internal to `KRTPipeline`. What users
     customize instead is **material, vertex, and mesh shaders**, with a
     **default mesh renderer** (base vertex shader + basic material shader)
     for the common case.
   - **`App` stops owning the GTE and stops making meshes.** It shrinks to
     "own the window + drive the run loop." Assets are loaded and it is the
     **`Scene` + `Renderer`** that decide how they are placed and rendered.
   - **`Scene` owns the `Renderer`** (which owns the GTE device / queue /
     native render target). `Scene::render()` no longer takes an `App`.

Locked-in decisions (this session):

| # | Decision | Choice |
|---|---|---|
| 1 | Binary split scope | **Split** into 3 per-submodule libraries |
| 2 | Library naming | `KRTBase` / `KRTPipeline` / `KRTScene` — **no** underscore, no `.` |
| 3 | Free functions (`Kreate::CreateApp`) | **No** prefix |
| 4 | Legacy macros / guards (`KREATE_EXPORT`, `KREATE__BUILD__`, `KREATE_*_H`) | **Keep as-is**; add per-lib `KRT_<MOD>_EXPORT` alongside |
| 5 | Namespace + include path | `Kreate` namespace and `<kreate/...>` path **stay** |
| 6 | `Pipeline` in the public API | **Removed** — internal to `KRTPipeline`; users customize material/vertex/mesh shaders + a default mesh renderer |
| 7 | GTE / `Renderer` ownership | Moves **off `App`** onto `Scene` (Scene owns the Renderer; Renderer owns the GTE stack) |
| 8 | Mesh / asset creation | **Off `App`** — Scene/Renderer own placement + draw. CPU-data mesh creation moves onto `Scene`; file-based asset import stays deferred to Engine-Roadmap Phase 3 |

The scope decision this session was **"fold it all into this plan"** — the
rename, the 3-way split, *and* the API redesign land here as sequenced
phases, one coherent reshaping rather than a rename now / redesign later.

---

## Decisions still surfaced

These are the sub-decisions the redesign forces that are genuinely open.
Each has a recommended default so the plan is buildable, but call them
before the phase that depends on them.

- **D1 — `KRTMaterial` scope.** How much material API lands now vs. defers
  to the real material/lighting system (Roadmap Phase 4). **Recommend
  minimal:** a shader-set (optional custom vertex / material / mesh shader,
  each an OmegaSL path + entry point) plus render state (cull / fill /
  depth). Parameter binding, lighting, and PBR stay in Roadmap Phase 4.
- **D2 — Default-shader packaging.** Where the default mesh renderer's base
  vertex + basic material `.omegasl` live and how they are resolved at
  runtime. **Recommend** shipping them as engine assets alongside
  `KRTPipeline`, resolved through the same shader-resolution path
  `PipelineFactory` already uses for user shaders.
- **D3 — Scene mesh/asset entry API.** `scene->createMesh(desc, data…)`
  (CPU-vertex parity with today) vs. `scene->loadModel(path)` (needs the
  Roadmap Phase 3 importer). **Recommend** landing `createMesh` on `Scene`
  now for parity; file import stays a Roadmap Phase 3 follow-on that drops
  into the same Scene/Renderer ownership boundary.
- **D4 — `CullMode` / `FillMode` home.** Become public render-state fields
  on `KRTMaterial` (as `KRTCullMode` / `KRTFillMode`) vs. stay fully
  internal. **Recommend public on `KRTMaterial`** — users need cull/fill
  control, and it is the natural replacement for the old `PipelineDesc`
  render state.
- **D5 — GPU `KRTMesh` handle visibility.** Public opaque handle vs. fully
  internal. **Recommend public handle:** the header (`Mesh.h`) stays in
  `KRTBase`'s public include dir, but its GTE-touching `.cpp` compiles into
  `KRTPipeline`. Header location and compilation unit are decoupled, so no
  link cycle results (see "The header-vs-compilation-unit rule" below).

---

## Target architecture

```
        OmegaGTE
           ▲
           │
        KRTBase      Math, Window (+platform), Object, Mesh (descriptors
           ▲         + opaque handle decl), App, Base.h
           │
       KRTPipeline   Renderer (owns GTE stack) · Pipeline* · Material ·
           ▲         Mesh GPU impl · pipeline/mesh factories   (*internal)
           │
        KRTScene     Scene — owns a Renderer, drives placement + draw,
           ▲         hosts the default mesh renderer
           │
         Game        BasicGame, user games — link KRTScene only
```

The chain is essentially linear. A game links **`KRTScene`**, whose `PUBLIC`
dependency edges transitively expose `KRTPipeline` and `KRTBase` (so `App`,
`Window`, `Object`, math types, `KRTMaterial`, and `Scene` are all
type-visible). Every library links `OmegaGTE`.

### The header-vs-compilation-unit rule (load-bearing)

The split's cleanliness rests on one existing pattern in this codebase:
`Object.h` already forward-declares `class Pipeline;` / `class Mesh;` and
holds `std::shared_ptr<>` members of them without completing the types.
`std::shared_ptr<Incomplete>` is legal to declare, hold, copy, and destroy
— the deleter is type-erased into the control block at construction time, so
the *destroying* translation unit never needs the complete type.

That lets us put a type's **public header in `KRTBase`** while compiling its
**GTE-touching `.cpp` in `KRTPipeline`**, with no link cycle, as long as no
`KRTBase` translation unit ever *completes* the type:

- `Object` (KRTBase) forward-declares `KRTMaterial` + `KRTMesh` and only
  stores/returns `shared_ptr<>`s of them → `KRTBase` needs no link edge to
  `KRTPipeline`.
- `Mesh.h`'s opaque handle is declared in `KRTBase`'s include dir but
  `src/mesh/Mesh.cpp` (which includes `GEMesh`) compiles into `KRTPipeline`.
- `KRTScene` and `KRTPipeline` *do* complete these types and link the
  library that defines them.

Any deviation (a `KRTBase` `.cpp` constructing/dereferencing `KRTMaterial`
or `KRTMesh`) reintroduces the cycle — this is the invariant to protect in
review.

### Per-library contents

| Library | Public headers (`include/kreate/`) | Sources compiled here | Depends |
|---|---|---|---|
| `KRTBase` | `Base.h`, `Math.h`, `Window.h`, `App.h`, `Object.h`, `Mesh.h` | `src/Math.cpp`, `src/App.cpp`, `src/Object.cpp`, `src/platform/<os>/*Window.{cpp,mm}` | `OmegaGTE` |
| `KRTPipeline` | `Material.h` | `src/renderer/Renderer.{h,cpp}` (internal), `src/pipeline/Pipeline.cpp` + `src/pipeline/Pipeline.h` + `PipelineFactory.h` (internal), `src/material/Material.cpp` (new), `src/mesh/Mesh.cpp` + `src/mesh/MeshFactory.h` (internal) | `KRTBase`, `OmegaGTE` |
| `KRTScene` | `Scene.h` | `src/Scene.cpp` | `KRTPipeline`, `KRTBase`, `OmegaGTE` |

`Renderer.h` stays at `src/renderer/Renderer.h` as a cross-submodule
internal header: `KRTScene` adds `src/renderer/` (and the internal
`src/material/` / `src/pipeline/` headers it needs) as **private** include
dirs and links `KRTPipeline`, so `Scene` can own a `unique_ptr<Renderer>`
and hand it materials/meshes without any of that reaching the public API.

---

## New / moved / deleted files

| File | Change | Purpose |
|---|---|---|
| `kreate/include/kreate/Pipeline.h` | **Deleted** (moved) | Public pipeline header retired; contents relocate to `src/pipeline/Pipeline.h` (internal). |
| `kreate/src/pipeline/Pipeline.h` | **New** (from the old public header) | Internal `Pipeline` / `PipelineDesc` + (if D4 lands public) the render-state enums that stay internal. |
| `kreate/include/kreate/Material.h` | **New** | `KRTMaterial` — the public shader-customization surface that replaces public `Pipeline`. Owned by `KRTPipeline`. |
| `kreate/src/material/Material.cpp` | **New** | `KRTMaterial` impl + the material→internal-pipeline build path (absorbs `PipelineFactory` use). |
| `kreate/assets/shaders/DefaultMesh.omegasl` (or similar) | **New** (D2) | Base vertex + basic material shader backing the default mesh renderer. Location per D2. |
| `kreate/cmake/KrtSubmodule.cmake` | **Proposed** | Optional helper wrapping `add_omega_graphics_module` for the three libraries — only if the repeated CMake boilerplate looks ugly; skip if not. |

No new *platform* sources — the OS-picker Window sources are unchanged, they
just move library.

## Modified files (overview)

- **Public headers** in `kreate/include/kreate/*.h` — type renames
  (Phase 1) + `App`/`Object`/`Scene`/`Window`/`Mesh` surface changes
  (Phase 2) + per-submodule export macro swap (Phase 4).
- **Sources** across `kreate/src/**` — type renames at use sites, plus the
  ownership rewiring (App loses GTE/mesh/pipeline; Scene gains the Renderer;
  Object's pipeline→material; factories move behind the Scene/Renderer).
- `kreate/CMakeLists.txt` — three `add_omega_graphics_module` calls instead
  of one; per-library `KRT_<MOD>__BUILD__` defines and dependency edges.
- `kreate/cmake/KreateGame.cmake` — link `KRTScene` instead of `KREATE`
  (lines 40, 63, 91, 112, 133); the macOS `DEPS KREATE OmegaGTE.framework`
  (line 86) expands to embed all three KRT libraries; Windows
  `omega_stage_runtime_dlls` already fans out `bin/` so it should pick up
  the new DLL set automatically — **verify**.
- Root `CMakeLists.txt:124` — `omega_graphics_add_subdir(KREATE kreate)`
  still passes `kreate/` as the subdir, but the umbrella `KREATE` target is
  gone; `kreate/CMakeLists.txt` now defines three targets directly. Keep the
  call signature working with whatever `omega_graphics_add_subdir` expects
  (resolve when Phase 5 lands).
- `kreate/tests/BasicGame.cpp` — rewritten onto the new API (Phase 2):
  Scene owns the Renderer, no `App::createPipeline/createMesh`, Object
  carries a `KRTMaterial`. Still links only the game's single library
  (`KRTScene`).
- `kreate/AGENTS.md`, `kreate/README.md`, `KreateGame.cmake` header comment
  — name + surface references.
- Codedb area map `utils/omega-codedb/OMEGA-Project.json` — existing areas
  already map to the right folders; area `name` labels stay descriptive.
  **Open question — leave area labels alone.**

---

## Phases

The refactor is dominated by mechanical renames, but the API redesign and
the binary split are real architectural changes. The phases below keep each
landing reviewable on its own, and — critically — **each phase ends with the
Linux Vulkan build green and `BasicGame` still drawing the spinning cube.**

**Phase 0 runs first** (it predates the rename/split): it swaps the cube over
to a real primitive API and the shader over to a precompiled pak, so every
later phase inherits the finished asset/primitive flow instead of the
hand-authored 36-vertex cube and runtime-compiled loose `.omegasl`.

### Phase 0 — Precompiled base-shader pak + 3D primitive API

Two changes land together because they reshape *how the cube exists* — where
its shader comes from and where its geometry comes from — and the test that
proves both is the same spinning cube. This is the plan's **first** executable
work, ahead of the rename (Phase 1).

Nothing here needs new engine machinery: `add_omegasl_lib` (precompile),
`omega-assetc` (pak), `OmegaCommon::AssetBundle` +
`loadShaderLibraryFromInputStream` (runtime load), and
`GESpace::addPrimitive` / `meshOf` (primitives) all exist and are proven — WTK
ships its compositor shader library through exactly this pak path
(`wtk/cmake/OmegaWTKApp.cmake`, `wtk/src/Composition/backend/Pipeline.cpp`),
and GESpace Phase 4 shipped `addPrimitive`. Phase 0 is wiring, not invention.

**0.1 — Base shaders move to `kreate/src/shaders/` and precompile.**
`tests/shaders/Phase1Basic.omegasl` moves to `kreate/src/shaders/` — the home
for every engine ("main Kreate") shader — and is compiled at build time by
`add_omegasl_lib(KRTBaseShaderLib kreate/src/shaders/*.omegasl)` (the helper
from `gte/OmegaGTE.cmake` that WTK uses for `OmegaWTKCompositorShaderLib`),
producing a host-native `.omegasllib`. `add_dependencies(... omegaslc)` gates
it on the compiler, as WTK does. *(Optional: rename `Phase1Basic` →
`KRTBaseMesh` since it is becoming the engine base shader, not a test shader —
see decision P-A.)*

**0.2 — Bundle into `krt-base.pak` via `omega-assetc`.** A CMake recipe
modeled on `OmegaWTKApp`'s `default.pak` build: a generated manifest lists the
`.omegasllib` as `type=shader`, `--strip-prefix` trims it to a clean logical
entry name, `--no-encrypt` for now, output `krt-base.pak`. This is the engine's
**base** asset bundle — the container for *all* main Kreate shaders, seeded
with the base mesh shader and extended as the default mesh renderer (KRT plan
Phase 3) and future engine shaders land. `KreateGame.cmake` stages it next to
the executable (and into `Contents/Resources/` on macOS) the same way it
stages loose shader sources today — replacing that loose-source staging.

**0.3 — Load the base shader library from the pak at runtime.** At engine
init the Renderer opens `krt-base.pak` (`AssetBundle::open`, exe-relative),
streams the base-shader entry, and builds the library with
`loadShaderLibraryFromInputStream` — the compositor path WTK already runs. The
base/default pipeline resolves its shaders from that library instead of
runtime-compiling a loose `.omegasl`. Runtime compile-from-source stays for
*user* custom shaders (`createPipeline`), but the engine's own shaders ship
precompiled. **This resolves existing decision D2** (default-shader
packaging): the default mesh renderer's shaders live in `krt-base.pak`.

**0.4 — Expose basic 3D primitives (TEParams under the hood).** Kreate gains a
primitive API for the seven solids GESpace Phase 4 accepts — RectangularPrism,
Pyramid, Cylinder, Cone, Torus, Sphere, Capsule — each a thin wrapper over the
matching `TETriangulationParams` factory. The `Scene` already owns the
`Renderer` (GTE stack) and a `GESpace`, so it mints a TE context from its
render target and drives `GESpace::addPrimitive` → `meshOf(engine)` under the
hood. **`BasicGame` builds its cube from `RectangularPrism`** (unit dims),
deleting `makeCubeVertices()` and the 36-vertex hand-authored buffer — the
concrete proof the primitive path renders. See decision P-C for the exact
surface and the primitive-vs-`Object`-vs-GESpace-slot reconciliation.

**Exit criteria:** `BasicGame` still draws the spinning cube — now (a) built
from a `RectangularPrism` primitive and (b) drawn with the base shader loaded
from `krt-base.pak` — with the Linux Vulkan build green. No hand-authored cube
vertices and no loose runtime-compiled base `.omegasl` remain.

**Decisions surfaced (Phase 0) — all resolved:**

- **P-C ✅ RESOLVED → `scene->createPrimitive(type, dims)` returns an
  `Object`** whose GESpace slot *is* the `addPrimitive` object (one
  registration; geometry + transform together; `meshOf(engine)` builds the
  drawn mesh lazily). This means Kreate's `Object`/`Scene` registration model
  (Phase 5) must let an `Object` be created **already bound** to a GESpace slot
  rather than only registering on `Scene::add` — a primitive object is
  pre-placed in the space by `createPrimitive`.
- **P-D ✅ RESOLVED → push-constant tint.** The base mesh shader takes a single
  `float4` color via the push constant alongside the MVP (block grows 64→80B;
  `Renderer::draw` gains a tint arg, `Object` gains a color, `Scene` passes the
  object's color). The primitive cube is single-shaded; per-vertex face colors
  do not survive (real per-object/material color = Roadmap Phase 4).
- **P-A — base-shader naming/home.** Move to `kreate/src/shaders/` (agreed).
  Rename `Phase1Basic.omegasl` → a base name (`KRTBaseMesh.omegasl`) now, or
  keep `Phase1Basic` to minimize churn? **Recommend rename** — it is the engine
  base shader, not a Phase-1 test artifact. Compilation unit that *loads* the
  pak lives in the render layer (Renderer → GTE), so despite the `krt-base`
  name it compiles into `KRTPipeline` post-split, honoring the
  header-vs-compilation-unit rule (`KRTBase` never touches GTE loading).
- **P-B — pak ownership + lifetime.** `krt-base.pak` is engine-built (one pak,
  shipped with the engine, holding the main Kreate shaders) and opened once at
  Renderer init, its `AssetBundle` held for the process. **Recommend** this over
  a per-app pak; per-app *game* assets can be a separate `app.pak` later
  (parallels WTK's per-app `default.pak`, but here the base pak is engine-owned).
- **P-C — primitive API surface + GESpace reconciliation.** A primitive is
  already a GESpace object (`addPrimitive` registers it with a transform), but
  today Kreate's `Object` also registers itself in the Scene's GESpace as a
  plain `addObject`. Two options: **(rec)** `scene->createPrimitive(type,
  dims)` returns a fully-formed `Object` whose GESpace slot *is* the
  `addPrimitive` object (one registration, geometry + transform together), and
  `meshOf(engine)` lazily builds the drawn `KRTMesh`; or a lower-level
  `KRTMesh`-only factory that leaves placement to a separate `Object`
  (two registrations, geometry decoupled from transform). Recommend the former
  — it matches "a primitive is a placeable thing," avoids double-registration,
  and keeps `Object`'s transform authority in the space. Confirm before 0.4.
- **P-D — draw path for a primitive's `KRTMesh`.** `meshOf(engine)` builds a
  `GEMesh`; Kreate's `Mesh` already wraps a `GEMesh` (`MeshFactory::geMesh`),
  so a primitive's `Mesh` is that GEMesh adopted into a `KRTMesh` handle. The
  base shader consumes Position-only vertices (per
  [[project-gespace-primitives-position-only]] — attachment data is blank;
  material shaders own the rest), so the base mesh shader must be
  Position-only-compatible. **Confirmed risk (read the shader):**
  `Phase1Basic.omegasl`'s `VertexIn` is `{float3 pos; float4 color}` (32-byte
  std430 stride) and the fragment stage returns the per-vertex color. A
  primitive supplies a **Position-only** buffer (16-byte stride, no color), so
  feeding it to this shader mismatches the stride — the Finding-E failure that
  shreds geometry — and reads garbage color. So Phase 0 **must** rework the base
  mesh shader to a Position-only input, and get its color from somewhere other
  than the vertex (options: a constant/push-constant tint, a normal-based shade,
  or a flat debug color). This is the one substantive code change in Phase 0
  beyond wiring; pick the color source when 0.4 lands. (BasicGame's six distinct
  face colors do not survive this — a primitive cube is single-shaded until the
  material system, Roadmap Phase 4, carries real per-object color.)

### Phase 1 — Public type renames (no surface or binary changes)

Add the `KRT` prefix to every public type that **remains** public. This is a
pure, greppable, class-name-only rename; member signatures and ownership are
untouched. The build still produces a single `KREATE` library.

**Renames:**

| Old | New |
|---|---|
| `Kreate::App` | `Kreate::KRTApp` |
| `Kreate::AppDesc` | `Kreate::KRTAppDesc` |
| `Kreate::Window` | `Kreate::KRTWindow` |
| `Kreate::WindowDesc` | `Kreate::KRTWindowDesc` |
| `Kreate::Scene` | `Kreate::KRTScene` |
| `Kreate::Object` | `Kreate::KRTObject` |
| `Kreate::Mesh` | `Kreate::KRTMesh` |
| `Kreate::MeshDesc` | `Kreate::KRTMeshDesc` |
| `Kreate::MeshTopology` | `Kreate::KRTMeshTopology` |
| `Kreate::IndexFormat` | `Kreate::KRTIndexFormat` |
| `Kreate::Vec3` | `Kreate::KRTVec3` |
| `Kreate::Vec4` | `Kreate::KRTVec4` |
| `Kreate::Color` | `Kreate::KRTColor` |
| `Kreate::Mat4` | `Kreate::KRTMat4` |

**Deliberately NOT renamed here:**

- `Pipeline`, `PipelineDesc`, `CullMode`, `FillMode` — these are **leaving**
  the public API in Phase 2. Prefixing them now would be churn we undo two
  phases later, so they keep their names this phase (they are still public
  and still used by `BasicGame` until Phase 2).
- `Kreate::CreateApp()` — free function (decision 3).
- All static methods (`KRTObject::create`, `KRTScene::create`,
  `KRTMat4::perspective`, `KRTMat4::lookAt`, `KRTMat4::rotation`, …).
- The `Kreate::VertexAttribute` namespace (namespaces aren't types).
- `Renderer` (internal).
- Macros / guards (`KREATE_EXPORT`, `KREATE__BUILD__`, `KREATE_*_H`).

**Exit criteria:** Vulkan native build green on Linux, `BasicGame` runs and
shows the spinning cube. Single `KREATE` library still produced.

### Phase 2 — API redesign (still one library)

The core reshaping — done inside the single `KREATE` library so the split
(Phase 5) later carves along boundaries that are already correct.

**2.1 — Pipeline goes internal.** Delete `include/kreate/Pipeline.h`; move
its `Pipeline` / `PipelineDesc` (and, per D4, the internal render-state
enums) into `src/pipeline/Pipeline.h`. `PipelineFactory` already lives in
`src/pipeline/` and already hides GTE — it stays, now invoked from the
material path instead of `App`.

**2.2 — Introduce `KRTMaterial`** (new public `include/kreate/Material.h`,
scope per D1/D4). Holds an optional custom shader set (vertex / material /
mesh, each OmegaSL path + entry point) and render state (cull / fill /
depth). A default-constructed `KRTMaterial` denotes "use the default mesh
renderer." The internal `Pipeline` is built lazily from a `KRTMaterial` by
the Renderer / material path and cached.

**2.3 — Slim `App`.** Remove `createPipeline`, `createPipelineFromLibrary`,
`createMesh`, `internalRenderer()`, and `friend class Scene`. `App::Impl`
stops constructing a `Renderer` — it owns only the `Window`. `App` keeps
`window()`, `onInit`/`onFrame`, and `run()`. `App.h` drops its `Pipeline.h`
and `Mesh.h` includes.

**2.4 — `Scene` owns the `Renderer`.** `KRTScene::create(KRTWindow &)`
builds and owns the `Renderer` (which owns the GTE stack + the window's
native render target). `Scene::render()` loses its `App &` parameter and
drives its own Renderer. Scene gains the mesh entry point per D3
(`scene->createMesh(desc, data…)`), routing to `MeshFactory` with its
Renderer's `gte()`.

**2.5 — `Object` carries a material, not a pipeline.**
`setPipeline`/`pipeline()` → `setMaterial`/`material()` over
`shared_ptr<KRTMaterial>`; `Object::create(material = nullptr,
mesh = nullptr)`. A null material means "default mesh renderer."

**2.6 — Rewrite `BasicGame`** onto the new flow:

```cpp
class BasicGame : public Kreate::KRTApp {
    std::shared_ptr<Kreate::KRTScene>  scene;
    std::shared_ptr<Kreate::KRTObject> cube;
    void onInit() override {
        scene = Kreate::KRTScene::create(window());
        auto mesh = scene->createMesh(mdesc, verts.data(), …);
        // Phase 2 uses a CUSTOM material wrapping the already-working
        // Phase1Basic shaders, so the runnable exit criterion never
        // depends on the not-yet-written default shaders (those arrive
        // in Phase 3).
        auto mat  = Kreate::KRTMaterial::fromShaders("Phase1Basic.omegasl", …);
        cube = Kreate::KRTObject::create(mat, mesh);
        scene->add(cube);
        scene->setProjectionMatrix(…);
        scene->setViewMatrix(…);
    }
    void onFrame() override { cube->setTransform(…); scene->render(); }
};
```

**Exit criteria:** single `KREATE` library; `BasicGame` draws the spinning
cube through the new API (Scene-owned Renderer, `KRTMaterial`, no
`App::create*`).

### Phase 3 — Default mesh renderer

Ship the base vertex shader + basic material shader (D2) and wire a
default-constructed `KRTMaterial` to them, so an object added with **no**
material still renders. Kept separate from Phase 2 so Phase 2's runnable
exit criterion doesn't depend on brand-new shader assets.

**Work:** author the default `.omegasl` (D2 location), make the material
path fall back to it when a `KRTMaterial` names no custom shaders, and add a
demo (or a `BasicGame` variant) that draws with a default material to prove
the path.

**Exit criteria:** an object with a null/default `KRTMaterial` renders via
the default mesh renderer; still one `KREATE` library.

### Phase 4 — Per-submodule export macros

Additive; no split yet. Define `KRT_<MOD>_EXPORT` / `KRT_<MOD>__BUILD__` for
the three libraries in `Base.h`, and switch each public header to its owning
submodule's macro. Until Phase 5 turns on the split, all three collapse to
`KREATE_EXPORT`, so the single library still works.

```cpp
// Pre-split (Phase 4): every KRT_<MOD>_EXPORT collapses to KREATE_EXPORT.
#define KRT_BASE_EXPORT     KREATE_EXPORT
#define KRT_PIPELINE_EXPORT KREATE_EXPORT
#define KRT_SCENE_EXPORT    KREATE_EXPORT
```

**Header migration:** `App.h` / `Window.h` / `Object.h` / `Math.h` /
`Mesh.h` → `KRT_BASE_EXPORT`; `Material.h` → `KRT_PIPELINE_EXPORT`;
`Scene.h` → `KRT_SCENE_EXPORT`.

**Exit criteria:** build still produces a single `KREATE` library; no
behavioral change.

### Phase 5 — Library split

The binary change. `kreate/CMakeLists.txt` is rewritten to define three
libraries with their own `KRT_<MOD>__BUILD__` define and dependency edges.

**5.1 — `KRTBase`.** Foundation. Sources `src/Math.cpp`, `src/App.cpp`,
`src/Object.cpp`, and the OS-picker `platform/<os>/*Window.*`. Public
headers `Base.h`, `Math.h`, `Window.h`, `App.h`, `Object.h`, `Mesh.h`.
Links `OmegaGTE`. (Contains no translation unit that *completes*
`KRTMaterial`/`KRTMesh` — see the header-vs-compilation-unit rule.)

**5.2 — `KRTPipeline`.** Rendering backend. Sources `src/renderer/*.cpp`,
`src/pipeline/*.cpp`, `src/material/*.cpp`, `src/mesh/Mesh.cpp`. Internal
headers (`Renderer.h`, `Pipeline.h`, `PipelineFactory.h`, `MeshFactory.h`)
stay in `src/`. Public header `Material.h`. `PUBLIC`-links `KRTBase` +
`OmegaGTE` (so `Scene` sees GTE + base types through it). This is where the
default-shader assets (D2) are staged.

**5.3 — `KRTScene`.** Top of the graph. Source `src/Scene.cpp`, public
header `Scene.h`. Adds `src/renderer/` + the internal `src/pipeline/`,
`src/material/` headers as **private** include dirs. `PUBLIC`-links
`KRTPipeline` (→ `KRTBase`, `OmegaGTE` transitively).
`target_compile_definitions(KRTScene PRIVATE KRT_SCENE__BUILD__)`.

**5.4 — Flip `Base.h`.** Each `KRT_<MOD>_EXPORT` now expands from its own
`KRT_<MOD>__BUILD__` gate instead of the umbrella alias. `KREATE_EXPORT` /
`KREATE__BUILD__` stay defined (decision 4) but are no longer referenced by
public headers.

**Platform linkage** (frameworks / X11 / android-log) moves to the library
that owns the platform Window sources — `KRTBase`.

**Exit criteria:** three libraries built; `BasicGame` links only `KRTScene`
and still runs; the Windows DLL set in `${CMAKE_BINARY_DIR}/bin/` is
`KRTBase.dll`, `KRTPipeline.dll`, `KRTScene.dll`.

### Phase 6 — KreateGame.cmake + docs + codedb + plan move

**6.1 — `KreateGame.cmake`.** Replace every `PRIVATE KREATE` with
`PRIVATE KRTScene` (lines 40, 63, 91, 112, 133). The macOS branch's
`add_app_bundle DEPS KREATE OmegaGTE.framework …` (line 86) expands to embed
all three KRT libraries. Windows `omega_stage_runtime_dlls` already fans out
`bin/`, so it should pick up the new DLL set — **verify**. Android's
`PRIVATE KREATE android log` → `PRIVATE KRTScene android log` (android/log
now pulled `PUBLIC` from `KRTBase`'s Android branch — confirm the
PUBLIC/PRIVATE split when 5.1 lands).

**6.2 — Docs.** Update `kreate/AGENTS.md`, `kreate/README.md`, and the
`KreateGame.cmake` header block for the three libraries, the `KRT`-prefixed
types, the removed public `Pipeline`, and the `KRTMaterial` /
default-mesh-renderer surface. Leave the namespace explanation unchanged.

**6.3 — Codedb.** Run
`python3 utils/omega-codedb/codedb.py index --rebuild`. Spot-check that the
renamed types resolve (`find KRTApp`, `find KRTMaterial`) and that
`Pipeline` no longer shows in the public surface.

**6.4 — Plan move.** Once Phase 6 ships, move this plan to
`kreate/.plans/done/`.

---

## Risk + sequencing notes

- **The runnable slice must survive every phase.** There is a *working*
  spinning cube today. Each phase's exit criterion is "Linux Vulkan build +
  `BasicGame` still draw the cube." Phase 2 in particular rewrites
  `BasicGame` onto a custom `KRTMaterial` that wraps the **already-working**
  Phase1Basic shaders, precisely so the runnable check never blocks on the
  not-yet-authored default shaders (which are Phase 3's job).
- **The header-vs-compilation-unit invariant.** The 3-library graph is
  acyclic only because no `KRTBase` translation unit completes
  `KRTMaterial`/`KRTMesh`. Guard this in review — a stray `#include
  "material/Material.h"` (or dereference) inside a `KRTBase` `.cpp`
  reintroduces the `KRTBase → KRTPipeline` cycle.
- **CRLF churn (project memory).** The tree shows as modified due to CRLF;
  each phase commits only the files it actually touches, and reviews use
  `git diff --ignore-cr-at-eol`.
- **Common identifier names.** `App`, `Window`, `Scene`, `Mesh`, `Object`
  are very common. Strictly scope every rename to `kreate/` (plus the one
  root `CMakeLists.txt:124` line in Phase 5) so no other module is clipped.
- **Windows-only build paths.** Phases 4–5 introduce per-library DLL
  boundaries — the only place `__declspec(dllexport/dllimport)` actually
  matters. Linux/macOS visibility is a no-op in `Base.h`, so the Linux
  Vulkan build passes long before the Windows side is verified. **Per
  AGENTS.md "Building"**: every phase that touches CMake or export macros is
  handed to the user for a Windows build verification before being declared
  done.
- **Single landing per phase.** None of the six phases should land as an
  intermediate broken build.

---

## Out of scope

- Renaming the `Kreate` namespace, the `<kreate/...>` include path, or the
  `KREATE_EXPORT` / `KREATE__BUILD__` / `KREATE_*_H` macros/guards
  (decisions 4 + 5).
- Renaming `Kreate::CreateApp` (decision 3) or the static methods on the
  prefixed types.
- The **full material & lighting system** — parameter binding, PBR,
  light types, forward-vs-deferred. That is Engine-Roadmap Phase 4; this
  plan lands only the minimal `KRTMaterial` surface (D1) needed to remove
  public `Pipeline`.
- The **file-based asset importer** (glTF/OBJ/USD via `GEMeshAsset`). That
  is Engine-Roadmap Phase 3; this plan only moves the mesh-creation
  *ownership* onto the Scene/Renderer so the importer drops in later without
  further surface change.
- The **entity-component model** (Roadmap Phase 6). `Object` stays an
  object node; `KRTScene`, `KRTPipeline`, `KRTBase` co-exist with whatever
  entity model Phase 6 chooses.
- Promoting `Renderer.h` to public API — it stays a cross-submodule
  internal header, now owned by `KRTScene` via a private include dir.
