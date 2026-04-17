# AUTOM — Build System Generator Completion & CMake Migration Plan

Status: Proposal — 2026-04-16
Scope: finish AUTOM as a full-feature build system generator (CMake peer), then migrate
OmegaGraphics from CMake to AUTOM across the entire repository.

---

## 1. Why This Plan Exists

AUTOM today is a serviceable generator for the common case: declare targets, glob
sources, emit Ninja/Xcode/VS/Gradle. The module layer (`apple`, `android`, `linux`,
`windows`, `external_project`, `dist`) pushes it further into platform territory.

But AUTOM is not yet a drop-in replacement for CMake in this repo. Reading
`cmake/OmegaGraphicsSuite.cmake` against `autom/docs/Syntax.rst` reveals real,
load-bearing CMake features that AUTOM has no equivalent for:

- usage-requirement propagation (`PUBLIC`/`PRIVATE`/`INTERFACE` on includes, libs,
  defines, compile/link flags)
- cache variables / user-facing `option(...)` knobs
- generator expressions (`$<TARGET_FILE:...>`, `$<CONFIG:Debug>`, …)
- imported targets for prebuilt / vendored libraries
- a testing layer (`enable_testing`, `add_test`, a CTest-like runner)
- configurable build types (Debug/Release/RelWithDebInfo) and multi-config
  generator awareness
- package config export / discovery (`find_package`, CMake config files)
- feature detection (`try_compile`, `check_symbol_exists`, compiler/feature tests)
- Objective-C++ / ASM / Swift language enablement
- precompiled headers
- `install(DIRECTORY ...)`, install components, CPack-style stages
- RPATH and `install_name_tool` orchestration on Darwin
- toolchain files for cross-compilation
- response-file-driven very-long link lines (already partial in `TargetNinja.cpp`)

This plan proposes the AUTOM extension work required to close that gap, then a
staged migration of every `CMakeLists.txt` in the tree to `AUTOM.build`.

---

## 2. Gap Analysis: CMake → AUTOM

Mapped against actual usage in this repository.

### 2.1 Usage requirement propagation

CMake has `target_link_libraries(foo PUBLIC bar)` with automatic include/define/flag
propagation across the DAG. AUTOM has flat `libs` and `include_dirs` properties —
no propagation, no visibility.

**Needed:** visibility-qualified target property APIs.

```autom
foo.public_libs       = ["bar"]
foo.private_libs      = ["gte-impl"]
foo.interface_libs    = []
foo.public_include_dirs  = ["./include"]
foo.private_include_dirs = ["./src/internal"]
foo.public_defines    = ["OG_PUBLIC=1"]
foo.private_defines   = ["OG_BUILDING_FOO"]
```

Engine change: the target graph must walk `public_*`/`interface_*` edges during
generation and merge them into dependents' effective compile/link lines.

### 2.2 Cache variables / options

Essential for `CODE_SIGNATURE`, `CROSS_COMPILE`, `BUILD_SHARED_LIBS`, and anything
else a user sets at configure time.

```autom
option(name:"OG_WITH_VULKAN", desc:"Build Vulkan backend", default:true)
option(name:"CODE_SIGNATURE", desc:"Apple Developer Team ID", default:"")
option(name:"CROSS_COMPILE",  desc:"Cross-compile mode", default:false)

if(autom.options.OG_WITH_VULKAN) { ... }
```

Needs:

- persistent cache file in the build directory (`AUTOMCACHE` JSON)
- `-D NAME=VALUE` CLI overrides on the `autom` driver
- typed options (bool / string / path / enum)
- `--show-options` introspection

### 2.3 Generator expressions

Required by `add_custom_command` chains, codesign pipelines, bundle staging. Today
CMake uses `$<TARGET_FILE:foo>` to reference a target's output path without the
build file needing to know build type or output dir.

**Needed:** a small expression sublanguage evaluated at generation time:

| Form | Meaning |
|------|---------|
| `$(target:foo.output)` | absolute path of foo's primary build output |
| `$(target:foo.output_dir)` | directory of foo's primary build output |
| `$(target:foo.interface_includes)` | exported include dirs |
| `$(config:Debug?-g:-O2)` | per-config ternary |
| `$(platform:darwin?-framework Cocoa)` | per-platform selection |

Implementation: a string-interpolation pass in `TargetDumper` / each generator,
expanded before emitting build rules. Unlike CMake's genexes, these stay textual
and legible.

### 2.4 Imported / external targets

Needed for:

- system frameworks (`Cocoa.framework`, `Metal.framework`, …)
- prebuilt vendored libs from `autom-deps` exports (OpenSSL, PCRE2, ICU, …)
- `ExternalProject` outputs treated as first-class dependencies

```autom
var openssl = ImportedLibrary(
    name:"openssl",
    kind:"shared",
    location:AutomDepsExport(name:"openssl-src.lib"),
    interface_include_dirs:[AutomDepsExport(name:"openssl-src.include")]
)
myExe.libs = ["openssl"]
```

Engine change: new target kind `IMPORTED_TARGET` in `Targets.def`, ignored by
compile steps but consulted for link flags and include propagation.

### 2.5 Interface targets (header-only libs)

```autom
var headers = InterfaceTarget(name:"common-headers")
headers.interface_include_dirs = ["./include"]
headers.interface_defines = ["OG_HEADER_ONLY"]
```

Needed for the common/ include tree and the numerous ABI-only headers in gte/.

### 2.6 Testing

CMake uses `enable_testing()` + `add_test()` + `ctest`. The repo has
`add_omega_graphics_test` helpers and dedicated `tests/CMakeLists.txt` files in
`wtk/`, `gte/`, `common/assetc/`, `autom/`.

**Needed:** first-class `Test` target and an `autom test` subcommand.

```autom
var t = Test(name:"gte-basic", target:"gte-basic-test", args:["--gtest_color=yes"])
t.working_dir = "./tests/data"
t.timeout = 60
t.env = {"OG_DISABLE_GPU":"1"}
t.labels = ["fast", "gpu-optional"]
```

Generator side: emit a manifest (`AUTOMTESTS.json`) readable by
`autom test --filter ... --parallel N` and by CI adapters (JUnit XML output).

### 2.7 Build types / configurations

Today `AUTOM.build` can branch on `autom.toolchain` but has no notion of
Debug/Release. Multi-config generators (Xcode, MSBuild) require per-config flag
arrays.

```autom
autom.config_flags.Debug.cxx   = ["-g","-O0","-DOG_DEBUG=1"]
autom.config_flags.Release.cxx = ["-O2","-DNDEBUG"]
main_lib.config_defines.Debug = ["OG_ASSERTS=1"]
```

Driver change: `autom gen ... --config Debug` for single-config generators;
Xcode/VS generators already know their own multi-config matrix.

### 2.8 Package config export / discovery

This is the hardest CMake replacement. Two halves:

**Producer half** — after `autom install`, emit `lib/autom/<pkg>/<pkg>-config.autom`:

```autom
# Generated by autom install
project(name:"OmegaGTE", version:"0.9")

var gte = ImportedLibrary(name:"OmegaGTE", kind:"shared",
    location:"$(prefix)/lib/libOmegaGTE.dylib",
    interface_include_dirs:["$(prefix)/include/OmegaGTE"])
```

**Consumer half** — `find_package(name:"OmegaGTE", version:">=0.9")` that
searches a configurable path list (`AUTOM_PACKAGE_PATH`, system dirs) and
imports the named targets.

Also need a CMake-interop direction: `autom gen cmake-package` producing a
`<pkg>Config.cmake` so projects that stay on CMake can still link against
Omega libraries.

### 2.9 Feature detection

Used indirectly today via hand-rolled `find_program` probes in
`autom/tools/default_toolchains.json`. CMake's `try_compile` / `check_*`
functions are absent.

```autom
if(try_compile(src:"int main(){return __builtin_expect(0,0);}")) {
    autom.cxx_flags += ["-DOG_HAVE_BUILTIN_EXPECT"]
}

if(check_include(header:"arm_neon.h")) { ... }
if(check_symbol(symbol:"pthread_setname_np", header:"pthread.h")) { ... }
```

Implementation: invoke the active toolchain's compiler, cache results under
`AUTOMCACHE`, expose via `autom.features`.

### 2.10 Additional language enablement

Needed for this repo specifically:

- **Objective-C++** — currently enabled via `enable_language(OBJCXX)` on Apple;
  wtk/gte Metal paths need it.
- **ASM** — required for PCRE2, possibly a few gte fast paths.
- **Swift** — not used today, but desirable for AQUA.
- **CUDA** — out of scope for now.

Extension: `autom.languages = ["c","cxx","objcxx","asm"]` drives per-language
rule generation in every backend.

### 2.11 Precompiled headers

Gte and wtk both have large common headers that would benefit. CMake provides
`target_precompile_headers`. AUTOM needs:

```autom
main_lib.pch = "src/internal/pch.h"
```

Emitted in Ninja as the usual `-include-pch` flow and in Xcode/VS as native PCH
settings.

### 2.12 Install features

`install_targets` / `install_files` exist. Missing:

- `install_directory(path, dest, patterns:["*.h"])` — headers
- install components (`runtime`, `devel`, `docs`) matched to `autom install --component`
- permissions on installed files (`0755` for scripts)
- `install_symlink(link, target, dest)` — needed for dylib versioning

### 2.13 RPATH / install_name_tool orchestration

`OmegaGraphicsSuite.cmake` has non-trivial Darwin rewrite logic
(`set_library_install_name`, `add_library_rpath`, `reset_library_dependent_name`)
serialized per-library. AUTOM needs a high-level equivalent:

```autom
import "apple"

apple_fix_install_names(
    target:"OmegaWTK",
    install_name:"@rpath/OmegaWTK.framework/Versions/A/OmegaWTK",
    rpaths:["@executable_path/../Frameworks"],
    rewrites:[
        {from:"/usr/local/opt/openssl/lib/libssl.3.dylib",
         to:  "@rpath/../Libraries/libssl.3.dylib"}
    ]
)
```

Under the hood: emits ordered `Script` targets mirroring the CMake macros.

### 2.14 Toolchain files (cross-compile)

The existing `autom/tools/default_toolchains.json` is static and host-picked.
For `CROSS_COMPILE=TRUE` builds we need explicit toolchain selection:

```sh
autom gen ninja --toolchain=./toolchains/aarch64-linux-gnu.autom
```

Toolchain file shape:

```autom
toolchain(
    name:"aarch64-linux-gnu",
    c_compiler:"aarch64-linux-gnu-gcc",
    cxx_compiler:"aarch64-linux-gnu-g++",
    linker:"aarch64-linux-gnu-ld",
    sysroot:"/opt/sysroots/aarch64-linux-gnu",
    target_arch:"aarch64",
    target_os:"linux"
)
```

### 2.15 Response-file / long link line handling

`TargetNinja.cpp` already understands response files for compile lines; link
lines on Windows and the gte Metal-shim linker can easily exceed OS argv limits.
Extend response-file use to every generator's link step.

### 2.16 `autom build` subcommand (driver polish)

CMake's value is partly the `cmake --build ...` abstraction. AUTOM should have:

| Command | Behavior |
|---------|----------|
| `autom configure` | evaluate build files, write cache + chosen generator output |
| `autom build [target]` | drive the backing tool (ninja/msbuild/xcodebuild) |
| `autom test` | run tests declared by `Test(...)` |
| `autom install [--component]` | execute the `AUTOMINSTALL` manifest |
| `autom package` | invoke `dist.autom` producers |
| `autom clean` | wipe build tree, keep cache |
| `autom reconfigure` | re-run configure with current cache |

### 2.17 Diagnostics & IDE support

Already have `CompileCommands.cpp`. Need:

- `compile_commands.json` produced for all generators (currently Ninja-only?)
- `autom query targets --format json` for IDE/tooling consumption
- LSP-style source location in `Diagnostic.cpp` errors (file:line:col)

### 2.18 Summary matrix

| Area | CMake | AUTOM today | Gap |
|------|-------|-------------|-----|
| Targets (exe/static/shared) | ✅ | ✅ | — |
| Usage requirements | ✅ PUBLIC/PRIVATE/INTERFACE | ❌ flat props | **§2.1** |
| Cache / options | ✅ | ❌ | **§2.2** |
| Generator expressions | ✅ | ❌ | **§2.3** |
| Imported targets | ✅ | ❌ | **§2.4** |
| Interface targets | ✅ | ❌ | **§2.5** |
| Testing | ✅ CTest | ❌ | **§2.6** |
| Build types | ✅ | ⚠️ toolchain only | **§2.7** |
| Package config | ✅ find_package | ❌ | **§2.8** |
| Feature detection | ✅ | ❌ | **§2.9** |
| OBJCXX / ASM / Swift | ✅ | ⚠️ C/C++ | **§2.10** |
| PCH | ✅ | ❌ | **§2.11** |
| Install polish | ✅ | ⚠️ | **§2.12** |
| Darwin install_name/rpath | handwritten | ❌ | **§2.13** |
| Toolchain files | ✅ | ⚠️ static | **§2.14** |
| Response files | ✅ | ⚠️ partial | **§2.15** |
| Driver UX | ✅ cmake --build | ⚠️ gen only | **§2.16** |
| compile_commands.json | ✅ | ⚠️ | **§2.17** |

---

## 3. Proposed Extension: **AUTOM v1.0 — Build System Generator Complete**

The goal of AUTOM v1.0 is "feature parity with the CMake surface area this repo
actually uses, plus the peer-level amenities (testing, options, package export)
that make a build system generator usable for other projects."

### 3.1 Layered deliverables

**Layer A — Target Graph Semantics**
Introduces usage requirements and target kinds required for everything else.

- A1: `public_*` / `private_*` / `interface_*` target properties
- A2: `InterfaceTarget`
- A3: `ImportedLibrary` (static/shared/framework)
- A4: transitive requirement propagation in `TargetDumper`
- A5: generator-expression string resolver

**Layer B — Configuration Surface**
Everything the user controls from the command line.

- B1: `option(...)` builtin + `AUTOMCACHE` persistence
- B2: `-D NAME=VALUE` driver flag
- B3: typed options (bool / path / string / enum)
- B4: `autom configure --show-options`
- B5: build-type matrix (`Debug`/`Release`/…)
- B6: toolchain-file loader (`--toolchain`)

**Layer C — Testing & Packaging**
Turns AUTOM into an end-to-end workflow tool, not just a generator.

- C1: `Test(...)` target type, `AUTOMTESTS.json`
- C2: `autom test` driver subcommand with filtering, parallelism, JUnit output
- C3: `autom build` / `autom install` / `autom package` / `autom clean`
  subcommands (thin wrappers over the active generator)
- C4: install components, permissions, `install_directory`, `install_symlink`

**Layer D — Discovery & Interop**
Closes the "third-party integration" gap.

- D1: `find_package(name, version, required?)` — searches
  `AUTOM_PACKAGE_PATH`, consumes generated `<pkg>-config.autom`
- D2: producer side: `autom install` emits `lib/autom/<pkg>/<pkg>-config.autom`
- D3: CMake-interop: `autom gen cmake-package` emitting `<pkg>Config.cmake`
- D4: feature detection builtins (`try_compile`, `check_include`,
  `check_symbol`), cached in `AUTOMCACHE`
- D5: `autom-deps` ↔ AUTOM bridge: `AutomDepsExport(...)` builtin consuming
  `.automdeps/exports.json` (already called out in the autom-deps plan §15)

**Layer E — Platform / Language Depth**
Completes language and platform coverage needed by this repo.

- E1: Objective-C++ language enablement across all generators
- E2: ASM language support
- E3: Swift language support (deferred; AQUA future)
- E4: PCH property and generator implementation
- E5: `apple_fix_install_names` helper in `apple.autom`
- E6: response files everywhere (Windows link, Xcode, MSBuild)

**Layer F — Diagnostics & Tooling**
Developer UX parity with modern CMake.

- F1: `compile_commands.json` from every generator
- F2: `autom query targets --format json`
- F3: structured diagnostics with file:line:col + caret (already partially in
  `Diagnostic.h`)
- F4: build-graph visualization (`autom graph --format dot`)

### 3.2 Engine changes (C++)

| File | Change |
|------|--------|
| `src/Targets.def` | add `INTERFACE_TARGET`, `IMPORTED_TARGET`, `TEST_TARGET`, per-visibility prop storage |
| `src/Target.h` | visibility-aware property accessors; `resolveInterface()` walker |
| `src/TargetDumper.{h,cpp}` | compute effective compile/link sets via walker |
| `src/engine/AST.def`, `AST.cpp`, `Lexer.cpp` | new keywords/syntax if any (`option`, `Test`, `ImportedLibrary`, `InterfaceTarget` are builtins, no grammar change) |
| `src/engine/Builtins.def`, `Builtins.cpp` | register `BUILTIN_OPTION`, `BUILTIN_TEST`, `BUILTIN_IMPORTED_LIBRARY`, `BUILTIN_INTERFACE_TARGET`, `BUILTIN_FIND_PACKAGE`, `BUILTIN_TRY_COMPILE`, `BUILTIN_CHECK_*`, `BUILTIN_AUTOM_DEPS_EXPORT` |
| `src/engine/Execution.{h,cpp}` | generator-expression pass; cache file I/O; toolchain-file evaluator |
| `src/gen/TargetNinja.cpp` | per-config matrix, PCH, OBJCXX/ASM, response files on link, compile_commands emission, test manifest |
| `src/gen/TargetXcode.cpp` | per-config native support, PCH, test scheme generation |
| `src/gen/TargetVisualStudio.cpp` | per-config native support, PCH, long link line response files |
| `src/gen/TargetGradle.cpp` | (deferred, low priority) |
| `src/gen/CompileCommands.cpp` | factor out so all generators can reuse |
| `src/driver/main.cpp` | new subcommands (`configure`/`build`/`test`/`install`/`package`/`clean`/`query`/`graph`) |
| `src/InstallFile.{h,cpp}` | components, permissions, directories, symlinks |
| `src/tools/install-main.cpp` | honor components + permissions |

New engine files:

| File | Purpose |
|------|---------|
| `src/Cache.{h,cpp}` | `AUTOMCACHE` persistence, typed options |
| `src/Toolchain.cpp` | extended: toolchain-file evaluator |
| `src/FeatureProbe.{h,cpp}` | `try_compile` / `check_*` driving the toolchain |
| `src/PackageConfig.{h,cpp}` | package config reader/writer |
| `src/GenExpr.{h,cpp}` | generator-expression lexer + evaluator |
| `src/TestManifest.{h,cpp}` | `AUTOMTESTS.json` reader/writer |

### 3.3 Module-layer additions

| Module | Addition |
|--------|----------|
| `modules/apple.autom` | `apple_fix_install_names`, framework/app + embedded lib rewrite (replaces `OmegaGraphicsSuite.cmake` Darwin section) |
| `modules/external_project.autom` | alignment with `autom-deps` exports so `ExternalProject` is a degenerate case; `ImportedLibrary` emitted automatically |
| `modules/test.autom` | `GTestTarget`, `CTestCompat` helpers |
| `modules/pkg.autom` | (distinct from `dist.autom`) `find_package` producer helpers, `autom_export_package(name, targets, headers)` |
| `modules/autom_deps.autom` (new) | `AutomDepsExport(name)` / `AutomDepsExports()` bulk |

### 3.4 Syntax additions (with docs under `autom/docs/Syntax.rst`)

```autom
# Options
option(name:"OG_WITH_VULKAN", type:"bool",   default:true,  desc:"...")
option(name:"CODE_SIGNATURE", type:"string", default:"",    desc:"...")
option(name:"OG_SANITIZER",   type:"enum",   choices:["none","asan","ubsan"], default:"none")

# Interface / Imported
var hdrs = InterfaceTarget(name:"common-headers")
hdrs.interface_include_dirs = ["./include"]

var ossl = ImportedLibrary(
    name:"openssl", kind:"shared",
    location:AutomDepsExport(name:"openssl-src.lib"),
    interface_include_dirs:[AutomDepsExport(name:"openssl-src.include")]
)

# find_package
var gte = find_package(name:"OmegaGTE", version:">=0.9", required:true)

# Usage requirements
main_lib.public_include_dirs  = ["./include"]
main_lib.private_include_dirs = ["./src/internal"]
main_lib.public_libs          = ["common-headers"]
main_lib.private_libs         = ["openssl"]
main_lib.public_defines       = ["OG_PUBLIC=1"]

# Build type / generator expressions
main_lib.config_defines.Debug   = ["OG_DEBUG=1"]
main_lib.config_defines.Release = ["NDEBUG"]
post = Script(name:"post", cmd:"python3",
    args:["tools/post.py", "$(target:main_lib.output)"],
    outputs:["post.stamp"])

# PCH
main_lib.pch = "src/internal/pch.h"

# Tests
var t = Test(name:"gte-basic", target:"gte-basic-test", labels:["fast"])

# Feature probes
if(try_compile(src:"#include <arm_neon.h>\nint main(){return 0;}")) {
    main_lib.public_defines += ["OG_HAVE_NEON"]
}
```

### 3.5 Backward compatibility

- Existing `include_dirs`, `libs`, `deps`, `output_*` properties keep working and
  become shorthand for `private_*`.
- Every current `AUTOM.build` in the tree (root, `common/`, `gte/`, `wtk/`,
  `aqua/`, `autom/`, `autom/tests/*`) continues to evaluate without change.
- New builtins are additive only; no grammar change.

### 3.6 Implementation phases (AUTOM side)

| Phase | Scope | Gate |
|-------|-------|------|
| **P1** Layer A — target graph | §2.1, §2.4, §2.5; engine walkers; generator updates | `autom/AUTOM.build` self-builds with new props; wtk builds using `public_include_dirs` |
| **P2** Layer B — configuration | §2.2, §2.7, §2.14 | `option()` reads from `-D`, persists across runs |
| **P3** Layer C — test & driver polish | §2.6, §2.16 | `autom test` runs existing gtests/wtk tests |
| **P4** Layer D — discovery & interop | §2.8, §2.9, autom-deps bridge | `find_package(OmegaGTE)` works after install; `try_compile` cached |
| **P5** Layer E — platform depth | §2.10–§2.13, §2.15 | Darwin bundles, PCH, OBJCXX/ASM |
| **P6** Layer F — tooling | §2.17, §3.1-F4 | `compile_commands.json`, `autom query`, `autom graph` |

Phases can overlap; the critical ordering is **A → B → C/D/E in parallel → F**.
A must land first because usage requirements underlie everything else.

---

## 4. Migration Plan: CMake → AUTOM, repository-wide

### 4.1 Current CMake footprint

From a repo scan:

```
CMakeLists.txt                              — root (entry)
autom/CMakeLists.txt                        — AUTOM itself
common/CMakeLists.txt
common/assetc/tests/CMakeLists.txt
gte/CMakeLists.txt
gte/tests/CMakeLists.txt
gte/tests/metal/CMakeLists.txt
gte/tests/vulkan/CMakeLists.txt
gte/tests/directx/CMakeLists.txt
gte/omegasl/tests/CMakeLists.txt
wtk/CMakeLists.txt
wtk/tests/CMakeLists.txt
wtk/tests/RootWidget/CMakeLists.txt
aqua/CMakeLists.txt
cmake/OmegaGraphicsSuite.cmake              — shared macros (the hard part)
```

And matching `AUTOM.build` already exists at: root, `autom/`, `autom/Fileformats/`,
`autom/tests/*`, `wtk/`, `common/`, `aqua/`, `gte/`. The AUTOM build files exist
but are not currently the source of truth.

### 4.2 Guiding principles

1. **Two build systems coexist during migration.** Every commit must leave the
   CMake build green until the final cutover for that module. No "big bang".
2. **Leaf-first.** Migrate deepest directories first (test subfolders, omegasl),
   then mid-tier (`common/`, `gte/`, `wtk/`), then `aqua/`, then root.
3. **Parity tests at each step.** A migrated module is done when:
   - `autom configure && autom build` succeeds on macOS, Linux, Windows
   - outputs binary-compare to the CMake build for the same target (or a written
     justification for the delta)
   - `autom test` passes the same test set CMake's CTest invocation ran
4. **`OmegaGraphicsSuite.cmake` is the hardest file.** It contains Darwin bundle
   + install_name orchestration, `add_third_party`, `add_omega_graphics_module`,
   and language/signing policy. Porting it requires Layers A+E to be complete.

### 4.3 Migration phases

#### **M0 — Prerequisites** (blocks on AUTOM P1)

- AUTOM P1 (Layer A: usage requirements) merged
- `autom-deps v2` Phase 4 done (exports.json consumable from AUTOM)
- CI matrix in place: macOS (Xcode + Ninja), Linux (Ninja), Windows (VS + Ninja)
  runs **both** CMake and AUTOM for every PR

#### **M1 — Port `OmegaGraphicsSuite.cmake` equivalents into AUTOM modules**

Blocks on AUTOM P1 + P5.

| CMake macro / function | AUTOM replacement |
|------------------------|-------------------|
| `add_script_target` | already covered by builtin `Script(...)` |
| `add_framework_bundle` | `apple.autom::AppleFramework` (+ gaps: EMBEDDED_FRAMEWORKS, EMBEDDED_LIBS, resource copy) |
| `add_app_bundle` | `apple.autom::AppleApp` (+ gaps: EMBEDDED_FRAMEWORKS, EMBEDDED_LIBS, resources, PLIST) |
| `code_sign_bundle` | `apple.autom::AppleCodesign` (already implemented) |
| `set_library_install_name` / `add_library_rpath` / `reset_library_dependent_name` | `apple.autom::apple_fix_install_names` (**new**, §2.13) |
| `add_third_party` (`ExternalProject_Add`) | `external_project.autom::ExternalProject` + `ImportedLibrary` wiring via autom-deps exports |
| `add_omega_graphics_tool` | wrapper in new `omega.autom` module — `OmegaTool(name, sources, libs)` |
| `add_omega_graphics_test` | `Test(...)` + `OmegaTest(...)` wrapper |
| `add_omega_graphics_module` | `OmegaModule(name, sources, header_dir, static?, shared?, framework?, embedded_libs?, version, info_plist)` — composes `Archive`/`Shared`/`AppleFramework` and installs headers |
| `target_link_system_frameworks`, `target_link_frameworks` | property shorthand: `mytgt.frameworks = [...]` (already partial in `apple.autom`) |
| `omega_graphics_add_subdir` | use existing `subdir(path:...)` |

Deliverable: `autom/modules/omega.autom` centralizing all project-level
conventions, so downstream `AUTOM.build` files remain short and uniform.

#### **M2 — Test subtrees**

Blocks on M1 + AUTOM P3 (Layer C testing).

- `autom/tests/*` — already have `AUTOM.build`; wire into `autom test`
- `common/assetc/tests/` — port
- `gte/tests/`, `gte/tests/metal/`, `gte/tests/vulkan/`, `gte/tests/directx/`,
  `gte/omegasl/tests/` — port; each backend dir stays conditional on feature
  detection (§2.9)
- `wtk/tests/`, `wtk/tests/RootWidget/` — port

Each leaf migrates independently. Parity check: same set of tests run + pass
under `autom test` as under `ctest`.

#### **M3 — `autom/` self-hosting**

Blocks on AUTOM P1–P5.

The AUTOM source tree builds itself today via both `autom/CMakeLists.txt` and
`autom/AUTOM.build`. Cutover step:

- make `autom/AUTOM.build` the canonical build recipe for AUTOM
- `autom/bootstrap.py` retains responsibility for the very first build from
  source (when no `autom` binary is yet on `$PATH`)
- delete `autom/CMakeLists.txt` (and any CMake references in autom/docs/*)

#### **M4 — `common/`**

Blocks on M1, M3, and autom-deps Phase 4 (for PCRE2/OpenSSL/ICU exports).

Steps:

1. Expand `common/AUTOM.build` to cover everything `common/CMakeLists.txt` does:
   library targets, header install, `assetc` tool, tests
2. Consume `autom-deps` exports for PCRE2/ICU/OpenSSL instead of
   `add_third_party` blocks
3. Dual-build CI for two weeks; resolve binary diffs
4. Delete `common/CMakeLists.txt`

#### **M5 — `gte/`**

Blocks on M4 + AUTOM P5 (OBJCXX for Metal, ASM optional).

Same template as M4. Additional work:

- Metal shader compilation pipeline ported to `Script` + `GroupTarget`
- DX/Vulkan/Metal backends gated on feature-probe `option(...)` + `try_compile`
- `omegasl` tool and tests

#### **M6 — `wtk/`**

Blocks on M4, M5.

- Darwin framework bundle via `OmegaModule(..., framework:true, embedded_libs:...)`
- Windows DLL + manifest via `windows.autom::WindowsDll`
- Linux .so via plain `Shared`

#### **M7 — `aqua/`**

Blocks on M4–M6. Guarded by `option(CROSS_COMPILE)` to match current behavior.

#### **M8 — Root cutover**

Blocks on M3–M7.

- Delete `CMakeLists.txt`, `cmake/OmegaGraphicsSuite.cmake`
- Move any remaining CMake helpers (codesign.py is already referenced from
  autom) into `autom/modules/...`
- Update `README.md`, `AGENTS.md`, onboarding docs to reference `autom configure`
- Remove CI CMake jobs
- Tag release: **OmegaGraphics v1.0 — AUTOM-built**

### 4.4 Dual-build CI contract

While migration is in flight, CI runs per PR on each platform:

| Job | Command |
|-----|---------|
| `cmake-legacy` | `cmake -S . -B build-cmake -G Ninja && cmake --build build-cmake && ctest --test-dir build-cmake` |
| `autom-new` | `autom configure -G ninja -B build-autom && autom build -B build-autom && autom test -B build-autom` |
| `diff` | compare exported libs/binaries by symbol table, not byte-for-byte |

A module's migration PR is allowed to **delete its `CMakeLists.txt`** only when:

- `autom-new` has been green on all three platforms for two consecutive weeks
- `diff` shows no unexpected regressions for that module
- the autom-deps exports that module depends on are checksummed in `state.json`

### 4.5 Risks and mitigations

| Risk | Mitigation |
|------|------------|
| Darwin bundle rewrite logic (`install_name_tool` chains) is order-sensitive — bugs here break shipping apps | Port via `apple_fix_install_names` helper that mirrors CMake's serialization with explicit `AFTER:` ordering; validate with `otool -L` diff |
| Third-party CMake integration (OpenSSL, PCRE2) relies on the third-party's own CMake | Keep `ExternalProject` invoking their native CMake; only AUTOM-ify the *consumption* side via `ImportedLibrary` from autom-deps exports |
| No `find_package` for system libs (X11, Vulkan SDK, etc.) | Ship Find*-style helpers in `modules/find_*.autom`; for the SDK cases, combine feature probes with `autom-deps` tool exports |
| Xcode / Visual Studio generator regressions | Lock Xcode/VS projects under the AUTOM generator by golden-file tests in `autom/tests/gen-xcode/`, `gen-sln/` |
| Contributors trained on CMake lose muscle memory | `autom configure` flag aliases (`-S`, `-B`, `-G`) match CMake; migration guide under `autom/docs/Migrating-From-CMake.md` |
| Long migration tail leaves the repo with two build systems | Hard deadline: root cutover (M8) must land within one release cycle after M4. If slipping, halt new AUTOM features and finish the migration |

### 4.6 Out of scope

- Replacing `ctest` in downstream CMake consumers of Omega libraries (we ship a
  CMake config file for them via §2.8 D3)
- Bootstrap on systems without Python 3 (same constraint CMake build had)
- A GUI (`autom-gui`) — can come later
- Gradle generator polish (Android flow already has `android.autom` + JarLib)

---

## 5. Recommended Execution Order

If only one thing ships next, ship **AUTOM P1 (Layer A — target graph
semantics)**. Nothing else in this plan is usable without usage-requirement
propagation, and the migration is impossible without it.

Immediate next-quarter slice:

1. AUTOM P1 (Layer A) — usage requirements, imported/interface targets, genexes
2. AUTOM P2 (Layer B) — `option()` and cache, unblocks `CODE_SIGNATURE` etc.
3. autom-deps Phase 4 (already a separate plan) — unblocks third-party consumption
4. Build the `omega.autom` module (M1) — compress `OmegaGraphicsSuite.cmake`
5. Dual-build CI
6. Migrate `autom/` itself (M3) as the first real proof point

Everything else unblocks from that sequence.

---

## 6. Open Questions

These need a decision from the Omega team before work starts:

1. **CMake co-existence deadline.** Two release cycles? One? Open-ended?
2. **Package config format.** Is AUTOM-native `<pkg>-config.autom` enough, or is
   CMake `<pkg>Config.cmake` a required export for external consumers?
3. **`autom test` filter/report format.** JUnit XML as the primary CI contract,
   or do we need TAP / gtest-native output too?
4. **Generator expression syntax.** `$(target:foo.output)` (proposed here, to
   stay lexically distinct from the shell-style `$(var)` variable substitution
   used by `autom-deps`) vs. CMake's `$<...>` (more familiar to migrators).
5. **Version floor of the autom binary that downstream Omega modules require
   after migration.** Commit to a single `autom >= 1.0` baseline repo-wide?

Answers drive the Phase 1 ticket list.
