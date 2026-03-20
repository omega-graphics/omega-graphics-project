# AUTOM Module Completion Plan

## Module Status

| Module | Status |
|--------|--------|
| `addon.autom` | **Done** — builds `.aext` shared libs |
| `fs.autom` + `fs/Module.cpp` | **Done** — native ext with glob, exists, abspath, mkdir, symlink |
| `apple.autom` | **Done** — `AppleFramework`, `AppleApp` with bundle structure and codesigning |
| `android.autom` | **Done** — `AndroidLib`, `AndroidApp` wrapping JarLib/JarExe with Maven repos |
| `linux.autom` | **Done** — `PkgConfigCflags`, `PkgConfigLibs`, `LinuxDesktopEntry` |
| `windows.autom` | **Done** — `WindowsResources`, `WindowsApp`, `WindowsDll` |
| `external_project.autom` | **Done** — `ExternalProject` fetch/extract/configure/build pipeline |

## Completed: Layer 1 — Engine fixes

### Files changed

- `src/engine/Builtins.def` — added `BUILTIN_JAR_LIB`, `BUILTIN_JAR_EXE` macro definitions
- `src/engine/Builtins.cpp` — three changes:

### Bug fix: `bf_JarLib` and `bf_JarExe` missing `addTarget`

Both functions created `JavaTarget` objects and returned `TargetWrapper` but never called
`ctxt.eval->addTarget(t)`. Targets were invisible to dependency resolution and generation.
Fixed by adding the `addTarget` call before the return in both functions.

### New builtin implementations

Implemented `bf_Copy`, `bf_Symlink`, `bf_Mkdir` backed by existing `FSTarget` factory
methods in `Target.h` that previously had no builtin wiring:

- `Copy(name, sources, dest)` — resolves sources relative to current eval dir, creates `FS_COPY` target
- `Symlink(name, source, dest)` — creates `FS_SYMLINK` target
- `Mkdir(name, dest)` — creates `FS_MKDIR` target

### Builtin registrations

All five functions registered via `BUILTIN_FUNC` at end of `tryInvokeBuiltinFunc`:

```
BUILTIN_FUNC(BUILTIN_JAR_LIB,bf_JarLib,{"name",Object::String},{"source_dir",Object::String});
BUILTIN_FUNC(BUILTIN_JAR_EXE,bf_JarExe,{"name",Object::String},{"source_dir",Object::String});
BUILTIN_FUNC(BUILTIN_FS_COPY,bf_Copy,{"name",Object::String},{"sources",Object::Array},{"dest",Object::String});
BUILTIN_FUNC(BUILTIN_FS_SYMLINK,bf_Symlink,{"name",Object::String},{"source",Object::String},{"dest",Object::String});
BUILTIN_FUNC(BUILTIN_FS_MKDIR,bf_Mkdir,{"name",Object::String},{"dest",Object::String});
```

## Completed: Layer 2 — Module implementations

### `apple.autom`

- Fixed `targets:` to `deps:` bug in `GroupTarget` calls
- Wired up codesigning via `Script` targets using existing `codesign.py`
- Creates `.framework` bundle structure: `Mkdir` for bundle root and `Headers/` subdirectory
- Creates `.app` bundle structure: `Mkdir` for bundle root and `Contents/MacOS/` subdirectory
- Dependency chain: compile -> mkdir bundle dirs -> group as pre_signed -> codesign script

### `android.autom`

- Locates `gradle` via `find_program`
- `AndroidLib(name, source_dir)` wraps `JarLib` with `google()` and `mavenCentral()` Maven repos
- `AndroidApp(name, source_dir)` wraps `JarExe` with same Maven repos

### `linux.autom`

- Locates `pkg-config` via `find_program`
- `PkgConfigCflags(package)` — Script target running `pkg-config --cflags`
- `PkgConfigLibs(package)` — Script target running `pkg-config --libs`
- `LinuxDesktopEntry(name, exec_name, icon, categories)` — configures `.desktop` file from template and installs to `share/applications/`

### `windows.autom`

- Locates `rc` (resource compiler) via `find_program`
- `WindowsResources(name, rc_file)` — Script target invoking `rc /fo` to compile `.rc` to `.res`
- `WindowsApp(name, sources)` — thin wrapper around `Executable`

### `external_project.autom`

- `ExternalProject(name, url, configure_cmd, build_cmd, outputs)` — four chained Script targets:
  1. `_fetch` — `curl -L -o` to download tarball
  2. `_extract` — `tar xzf` into named directory
  3. `_configure` — runs caller-provided configure command
  4. `_build` — runs caller-provided build command
- Final `GroupTarget` wraps the chain under a single dependency name

## Design Notes

The modules are deliberately thin — they compose existing builtins rather than introducing
new native extensions. The `fs` native module is the precedent for when native code is
needed; these modules only need the scripting layer.
