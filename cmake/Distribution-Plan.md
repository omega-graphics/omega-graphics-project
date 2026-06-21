# Distribution & Packaging Plan (`cmake/OmegaDist.cmake`)

## Purpose

Add CMake-layer distribution/packing helpers for **two distinct outputs**:

1. **App distribution** — turn a single `OmegaWTKApp()` into an end-user
   installer/disk image (`.pkg`/`.dmg`, `.deb`, `.msi`/`.exe`). Parallels the
   AUTOM `dist.autom` module (`autom/.plans/Distribution-Module-Plan.md`), but
   built on CMake primitives instead of AUTOM `Script` targets, and consuming
   the per-target artifacts `OmegaWTKApp()` already produces.
2. **Suite SDK distribution** — pack the **whole Omega Graphics suite** (this
   repo) into a redistributable developer kit. Mechanism: `cmake --install` into
   a throwaway staging prefix, **prune** it down to only the files a consumer
   needs (headers, libs/frameworks, tools), then archive the result
   (`.tar.gz`/`.zip`).

These are different kinds of artifact — #1 is an *application* a user installs,
#2 is a *dev kit* a downstream project builds against — so they get separate
entry points and separate helper scripts, sharing only the staging/archive
plumbing.

> **Plan location.** Placed under `cmake/` per the explicit request. `cmake/` is
> shared infrastructure, not a module with a `.plans/` lifecycle folder, so this
> doc lives here rather than under `<module>/.plans/`. If `cmake/` later grows a
> `.plans/` folder, move it there.

---

## 1. Current State (grounding)

### What `cmake --install` emits today

Install rules live in `cmake/OmegaGraphicsSuite.cmake` and a few module
`CMakeLists.txt`. Destinations are all **relative to `CMAKE_INSTALL_PREFIX`**:

| Dir | Contents | Source rule |
|-----|----------|-------------|
| `bin/` | tool exes (`autom`, `autom-install`, `omegaslc`, `omega-assetc`, `omega-ebin`, `omega-wrapgen`, …); on Win32 the module **runtime DLLs** (`OmegaCommon`, `OmegaWTK`, `OmegaGTE` + their third-party DLLs); `glslc` (Vulkan); `AssetTypes.json` | `add_omega_graphics_tool` (`install(TARGETS … RUNTIME DESTINATION bin)`), `add_omega_graphics_module` Win32 branch, `gte/CMakeLists.txt:112`, `common/CMakeLists.txt:554` |
| `lib/` | static archives + Win32 import libs; Linux `.so`; macOS `.framework` dirs; `autom-lib` | `add_omega_graphics_module` (`ARCHIVE/LIBRARY DESTINATION lib`, `install(DIRECTORY … .framework DESTINATION lib)`) |
| `include/` | each module's `HEADER_DIR` | `add_omega_graphics_module` (`install(DIRECTORY ${_ARG_HEADER_DIR} DESTINATION "include")`) |
| `modules/` | AUTOM stdlib `.autom` + compiled stdlib modules | `autom/CMakeLists.txt:73,81` |

**Not installed:** tests (`add_omega_graphics_test` — comment: *"NOT INSTALLED
UNLIKE TOOLS"*), and the `OmegaWTKApp()` apps themselves.

> Implication for System 2: the install prefix is *already close* to a dev kit —
> the prune step is mostly about dropping bulky/irrelevant install fallout
> (e.g. third-party CMake config dirs, static deps a consumer never links,
> `glslc` if not redistributable) rather than hand-picking files.

### What `OmegaWTKApp()` produces per target

From `wtk/cmake/OmegaWTKApp.cmake` — there is **no single uniform artifact**;
each target lands somewhere different, and System 1 must know each location:

| Target | Artifact(s) | Location |
|--------|-------------|----------|
| macOS | signed `.app` bundle (exe + embedded `OmegaWTK`/`OmegaGTE` frameworks + embedded libs + `default.pak` in `Contents/Resources`) | `${CMAKE_BINARY_DIR}/Apps/<NAME>.app` (`APP_BUNDLE_OUTPUT_DIR`) |
| Win32 | `<NAME>.exe` + staged runtime DLLs (`omega_stage_runtime_dlls`) + `default.pak` | `$<TARGET_FILE_DIR:<NAME>>` |
| Linux | `<NAME>` ELF + `default.pak` (rpath `$ORIGIN`) | `$<TARGET_FILE_DIR:<NAME>>` |

macOS is already a self-contained, signed bundle — packaging is "wrap the
existing `.app`". Win32/Linux are loose files in a build dir — packaging must
first **stage** the exe + its sibling DLLs/`.pak` into a clean tree.

### Existing precedents to reuse (do not reinvent)

- **`PYEXEC`** (`py` on Windows, `python3` elsewhere) — resolved once in
  `OmegaGraphicsSuite.cmake`; every script helper is invoked through it.
- **Python helpers driven by `add_custom_command`** — `codesign.py`,
  `AppleFramework.py`, and the `-P`-mode `OmegaCopyDlls.cmake`. This is the
  repo's idiom for "do a packaging/file-shuffling step from CMake."
- **`add_script_target(NAME SCRIPT OUTPUTS;DEPS;ARGS)`** macro — wraps a Python
  helper as an `add_custom_target` + `add_custom_command` pair. The natural
  vehicle for every packaging step below.
- **`code_sign_bundle` / `CODESIGN_SCRIPT` / `CODE_SIGNATURE`** — macOS signing
  is already solved; App distribution reuses it for the `.app` and for signing
  the final `.pkg`/`.dmg`.

---

## 2. Key Decision: custom Python helpers vs. CPack

**Recommendation: custom Python helpers (matching `codesign.py` + the AUTOM
`dist` plan), not CPack.**

| | Custom Python helpers (recommended) | CPack |
|---|---|---|
| Fits repo conventions | ✅ identical to `codesign.py`, `OmegaCopyDlls.cmake`, AUTOM `dist` | ❌ zero CPack usage in-tree today |
| Reuses existing signing | ✅ calls `codesign.py` directly | ⚠️ awkward to thread `codesign.py` into CPack's macOS generators |
| Suite "install→prune→archive" (System 2, your stated mechanism) | ✅ direct: drive `cmake --install` + prune script + `tar` | ⚠️ CPack owns the install step; prune becomes a `CPACK_INSTALL_SCRIPT` hook |
| Win32 `.msi`/NSIS | ✅ generate `.wxs`/`.nsi` (mirrors AUTOM `dist_windows.py`) | ✅ WIX/NSIS generators built in |
| Cross-tool consistency with AUTOM | ✅ the two build systems package the same way | ❌ divergent |
| Lines of code | more (we own the templates) | fewer (generators built in) |

CPack's only real win is the built-in DEB/WIX/NSIS/productbuild generators. But
this repo already commits to *owning* its packaging tools (the AGENTS.md
dependency philosophy: "control how third party deps are shipped"), the AUTOM
sibling plan already specifies the Python-helper approach, and System 2's
mechanism (install to temp → prune → pack) is something you described as a
script flow, not a CPack run. Mirroring AUTOM keeps **one mental model across
both build systems**.

> CPack stays a viable fallback if maintaining the WIX/NSIS templates by hand
> becomes a burden; the `OmegaDist.cmake` API below is generator-agnostic enough
> that a CPack backend could be swapped under it later.

---

## 3. Design Principles

1. **No engine/source changes.** Everything is built from existing primitives:
   `add_custom_command`/`add_custom_target`, `PYEXEC`, `add_script_target`,
   `install()`, `code_sign_bundle`. (Same posture as the AUTOM plan's "No Engine
   Changes Required.")
2. **Manifest is the contract.** Each helper script takes a generated JSON
   manifest (via `configure_file` / `file(WRITE)`) describing name, version,
   arch, file list, and metadata — never a sprawl of positional CLI args. Avoids
   shell-quoting pain and matches the AUTOM `.dist.json` design.
3. **Platform dispatch at the CMake layer**, format generation + tool
   invocation in Python. CMake decides *which* packager via the existing
   `TARGET_MACOS`/`TARGET_WIN32`/`TARGET_LINUX` flags (same flags `OmegaWTKApp`
   branches on); Python does the per-format work.
4. **Stage, then pack.** Every packager copies into a clean per-package staging
   dir first, so the source build tree is never the thing handed to the
   packaging tool. (System 2 stages via `cmake --install`; System 1 stages the
   app's loose files / wraps the `.app`.)
5. **Namespaced intermediate targets.** `<name>_dist_stage`, `<name>_dist_pkg`,
   etc., so multiple packages in one configure don't collide (the same hazard
   `OmegaWTKApp` already guards against with per-app `Info.plist` names).

---

## 4. System 1 — App Distribution

### 4.1 CMake API (`cmake/OmegaDist.cmake`)

```cmake
# Top-level: dispatch to the right packager for the current target OS.
OmegaWTKAppDist(
    APP          <name>              # the target created by OmegaWTKApp()
    VERSION      1.2.0
    IDENTITY     com.acme.myapp      # reverse-DNS; → pkg identifier / MSI upgrade
    DISPLAY_NAME "My App"
    MAINTAINER   "dev@acme.com"      # deb/Authenticode metadata
    EXTRA_FILES  README.md LICENSE   # staged alongside the app payload
    [FORMAT      pkg|dmg|deb|msi|nsis] # optional override of the per-OS default
)
```

Defaults by OS: macOS → `dmg`, Linux → `deb`, Win32 → `msi`. `FORMAT` overrides.
Per-format functions (`OmegaAppPkg`, `OmegaAppDmg`, `OmegaAppDeb`,
`OmegaAppMsi`, `OmegaAppNsis`) are public for callers who want a specific format
without the dispatch.

### 4.2 Staging — the per-target divergence

The one real complication: the payload location differs per target (§1).
`OmegaWTKAppDist` resolves the payload before handing off to the packager:

- **macOS** — payload is the existing bundle `${APP_BUNDLE_OUTPUT_DIR}/<APP>.app`.
  Depend on the `<APP>.app` aggregate target (which already chains codesign) so
  packaging waits for a signed bundle. No re-staging of internals.
- **Win32 / Linux** — payload is `$<TARGET_FILE:<APP>>` + its sibling
  `default.pak` + (Win32) the DLLs `omega_stage_runtime_dlls` placed in the
  target dir. Stage these into `${CMAKE_BINARY_DIR}/_dist/<APP>/` via
  `add_custom_command` copies that depend on the `<APP>` target, so the packager
  sees a clean tree, not the build dir.

### 4.3 Helper scripts (`cmake/dist/`)

Mirror the AUTOM `dist_*.py` trio; each reads a JSON manifest:

```
cmake/dist/
  dist_macos.py     # --format {pkg|dmg}: pkgbuild+productbuild / hdiutil
  dist_debian.py    # generate DEBIAN/control, map arch (x86_64→amd64,
                    #   aarch64→arm64), run dpkg-deb --build
  dist_windows.py   # --format {msi|nsis}: generate .wxs → candle+light,
                    #   or .nsi → makensis
```

`dist_macos.py --format pkg` signs via the same `CODE_SIGNATURE` identity
already used by `codesign.py` (pass it through the manifest); `--format dmg`
runs `hdiutil create -format UDZO` over the staged `.app`.

### 4.4 Target chain (per package)

```
<APP>            (built by OmegaWTKApp — already exists)
  └─ <APP>_dist_stage     (Win/Linux: copy exe+pak+dlls into _dist/<APP>/;
  │                         macOS: no-op, payload is the .app)
       └─ <APP>_dist_manifest   (file(WRITE)/configure_file → <APP>.dist.json)
            └─ <APP>_dist_pkg    (add_script_target: PYEXEC dist_<os>.py
                                  --manifest <APP>.dist.json)
                 └─ <APP>_dist   (aggregate target the user builds)
```

Output: `${CMAKE_BINARY_DIR}/_dist/<name>-<version>.<ext>`.

> **Capability lowering is out of scope here.** `wtk/.plans/App-Building-Plan.md`
> owns identity/capabilities/icons (`IDENTITY`, `CAPABILITIES`, `.entitlements`,
> AUMID+shortcut, `.desktop`). This plan consumes whatever that work produces —
> e.g. once the Win32 Start-Menu shortcut exists, `dist_windows.py` includes it
> in the MSI. The two plans meet at the staged payload; `OmegaWTKAppDist` does
> not itself generate manifests/entitlements. `IDENTITY` here is only the
> packaging identifier until App-Building lands the unified one.

---

## 5. System 2 — Suite SDK Distribution

### 5.1 CMake API

```cmake
OmegaSuiteDist(
    VERSION   0.1.0                    # SDK version (no project(VERSION) today — see Open Q)
    [FORMAT   tgz|zip]                 # default: zip on Win32, tgz elsewhere
    [COMPONENTS runtime devel tools]   # default: all
)
```

Produces one aggregate target `omega-suite-dist`.

### 5.2 Mechanism (your stated flow: install → temp → prune → pack)

```
omega-suite-dist
  └─ stage:   cmake --install ${CMAKE_BINARY_DIR}
  │             --prefix ${CMAKE_BINARY_DIR}/_sdk_stage/OmegaGraphics-<ver>
  │           (driven by add_custom_command; depends on the install-relevant
  │            targets so a fresh build is staged)
  └─ prune:   PYEXEC cmake/dist/sdk_prune.py
  │             --root  _sdk_stage/OmegaGraphics-<ver>
  │             --rules cmake/dist/sdk_keep.txt
  └─ pack:    cmake -E tar czf OmegaGraphics-<ver>-<os>-<arch>.tar.gz
                (or `… cf … --format=zip` for zip)
```

Notes on each step:

- **Stage.** Use `cmake --install <build> --prefix <staging>` (a fresh prefix,
  not the system prefix). This re-runs the existing `install()` rules into the
  throwaway tree, exactly as described. Wrap in `add_custom_command` whose
  `DEPENDS` cover the module/tool targets so the staged tree reflects a current
  build. The macOS `.framework` dirs install correctly via the existing
  `install(DIRECTORY … .framework)` rule.
- **Prune.** `sdk_prune.py` walks the staged prefix and **keeps only what a
  consumer needs**, deleting the rest. Driven by a small keep/drop rules file
  (`sdk_keep.txt`) rather than hardcoded paths, so the rules are reviewable and
  live next to the script. Candidate drops, to be confirmed against an actual
  staged tree (Open Q): bundled third-party CMake `lib/cmake/*` config dirs,
  static deps a consumer never links directly, redistribution-restricted tools
  (e.g. `glslc` if its license forbids bundling), `__pycache__`, and any
  test/sample fallout. Candidate keeps: `include/`, `lib/` (Omega libs +
  frameworks), `bin/` Omega tools, `modules/`.
- **Pack.** `cmake -E tar` is portable across all hosts (no external `tar`/`zip`
  dependency), honoring AGENTS.md's "control how things ship" posture.

### 5.3 Components (optional split)

`COMPONENTS` lets the SDK be sliced so a runtime-only consumer doesn't pull
headers/tools:

| Component | Contents |
|-----------|----------|
| `runtime` | `lib/` shared libs / frameworks, Win32 `bin/` DLLs |
| `devel`   | `include/`, static archives + import libs |
| `tools`   | `bin/` exes, `modules/` |

Implementation: the prune rules file is component-tagged; selecting a subset
restricts what survives the prune. Default = all three (one full SDK archive).
This is a P2 refinement — the P1 path ships a single all-in archive.

---

## 6. Files to Create

| File | Purpose |
|------|---------|
| `cmake/OmegaDist.cmake` | `OmegaWTKAppDist`, per-format app funcs, `OmegaSuiteDist`; `include()`d from `OmegaGraphicsSuite.cmake` (so `PYEXEC`/`CODE_SIGNATURE` are already set) |
| `cmake/dist/dist_macos.py` | `.pkg` (pkgbuild+productbuild) and `.dmg` (hdiutil) |
| `cmake/dist/dist_debian.py` | `DEBIAN/control` + `dpkg-deb --build`; arch mapping |
| `cmake/dist/dist_windows.py` | `.wxs`→candle+light (MSI), `.nsi`→makensis (NSIS) |
| `cmake/dist/sdk_prune.py` | prune a staged install prefix per a keep/drop rules file |
| `cmake/dist/sdk_keep.txt` | component-tagged keep/drop rules for the suite SDK |

No new install rules and no source/engine changes.

---

## 7. Implementation Phases

Per AGENTS.md multi-phase authoring (production CMake/scripts, so phased):

- **Phase 1 — `OmegaDist.cmake` skeleton + manifest plumbing.** Add the module,
  `include()` it from `OmegaGraphicsSuite.cmake`, implement `OmegaWTKAppDist`
  dispatch + staging (the per-target payload resolution in §4.2) + JSON manifest
  generation. No packagers yet — stage + manifest only, verifiable by inspecting
  the staged tree and `.dist.json`. (~under 300 lines; small-feature note rather
  than sub-phasing.)
- **Phase 2 — Suite SDK path.** `OmegaSuiteDist` + `sdk_prune.py` + `sdk_keep.txt`
  + `cmake -E tar` packing. Highest value, lowest external-tool dependency
  (only Python + CMake). **Recommended first shippable** because it needs no
  platform installer toolchain. Verify by unpacking the archive and diffing
  against an expected file list.
- **Phase 3 — macOS app packagers.** `dist_macos.py` (`dmg` then `pkg`); reuse
  `CODE_SIGNATURE`. Verifiable on the Linux/macOS host directly.
- **Phase 4 — Linux `.deb`.** `dist_debian.py`. Verifiable on the native Linux
  host (`dpkg-deb`, `lintian`).
- **Phase 5 — Windows `.msi`/NSIS.** `dist_windows.py`. **Windows build is
  driven by the user via WSL handoff** (AGENTS.md) — author the templates, then
  iterate on `candle`/`light`/`makensis` output the user pastes back. Gated on
  WIX/NSIS being available (AUTOMDEPS may need to fetch them — Open Q).

---

## 8. Open Questions

1. **Suite version source.** `project(OmegaGraphics C CXX)` declares no
   `VERSION`. Where does `OmegaSuiteDist(VERSION …)` get its default — add a
   `VERSION` to the top-level `project()`, a `OMEGA_SUITE_VERSION` cache var, or
   require the caller to pass it? (Recommend: add `VERSION` to `project()` and
   default from `${PROJECT_VERSION}`.) Y

   YES 
2. **Prune rules — ground against a real staged tree.** The §5.2 keep/drop lists
   are candidates. Before writing `sdk_keep.txt`, run `cmake --install` to a
   scratch prefix and enumerate what actually lands (esp. third-party
   `lib/cmake/*`, static deps, `glslc`) so the rules match reality, not guesses.
3. **WIX / NSIS / dpkg-deb provisioning.** Are these expected on the dev machine,
   or fetched via AUTOMDEPS (like Strawberry Perl / VulkanSDK already are)? If
   AUTOMDEPS, add the tool entries there; if assumed-present, `dist_windows.py`
   should fail with a clear "install WIX/NSIS" message.
4. **`glslc` redistribution.** It's installed to `bin/` (`gte/CMakeLists.txt:112`)
   from the Vulkan SDK. Confirm its license permits bundling in the suite SDK; if
   not, the prune step drops it (drives an entry in `sdk_keep.txt`).
5. **App `EXTRA_FILES` install location.** For `.deb`/`.msi`, do docs/licenses go
   to a docs dir (`/usr/share/doc/<app>`, `Program Files\<app>`) or beside the
   exe? (Recommend per-platform convention, encoded in the helper.)
6. **Overlap with `App-Building-Plan.md`.** That plan adds `IDENTITY`/
   `CAPABILITIES`/icons/signing knobs to `OmegaWTKApp()`. Confirm the handoff
   boundary: App-Building produces the *payload + identity*, this plan *packs*
   it. If App-Building lands first, `OmegaWTKAppDist` should read `IDENTITY`/
   `VERSION` from the app's generated manifest instead of re-taking them as args.
```
