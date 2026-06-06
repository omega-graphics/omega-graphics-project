# OmegaCommon Binary Split Plan

## Overview

Split the single `OmegaCommon` shared library into three sibling
binaries with disjoint third-party dependency footprints:

| Binary | Contents | External deps |
|--------|----------|---------------|
| **OmegaCommon.Core** | utils, fs, crt, json, cli, format, regex, crypto, multithread, net, unicode, assets | ICU, OpenSSL, PCRE2, libcurl/WinHTTP |
| **OmegaCommon.Img** | image codec (BitmapImage, loadFromFile/Buffer/URL) | libpng, libjpeg-turbo, libtiff, zlib (+ Core) |
| **OmegaCommon.ASIO** | async I/O reactor + WebSocket (see Async plan) | Boost (+ Core, + OpenSSL via Core) |

The split is **binary-only**. Public header layout under
`include/omega-common/*.h` does **not** change; the `OmegaCommon::`
namespace does **not** change. Existing consumer code keeps compiling
unchanged — only the link line in consumer `CMakeLists.txt` files needs
to be updated.

This plan is a prerequisite for the OmegaCommon-Async-WebSocket plan:
the ASIO binary is the target the async work lands in.

## Why split

- **Smaller link surface for consumers that don't need it.** wtk uses
  the image codec; gte uses both; aqua uses neither. Pulling all of
  libpng/jpeg/tiff/zlib into a Core-only consumer is dead weight.
- **Cleaner dependency story.** Today, OmegaCommon links 10+
  third-party libs. Splitting along the natural domain seams makes
  ownership and link-time errors much easier to read.
- **Pre-flight for ASIO.** Boost is ~140 MB of headers. Confining it
  to a single binary keeps Core's build time unaffected for callers
  that never touch async.

## Decisions (defaults; override in Open Items)

| # | Decision | Default | Reasoning |
|---|----------|---------|-----------|
| 1 | Target name format | `OmegaCommon.Core`, `OmegaCommon.Img`, `OmegaCommon.ASIO` (literal dot in CMake target name) | Honors the user's stated naming. CMake supports dots in target names; resulting DLL/dylib filenames are `OmegaCommon.Core.dll` etc. |
| 2 | Public header layout | Unchanged — img.h / io.h / websocket.h stay under `include/omega-common/` | Avoids consumer #include churn. The split is binary, not API. |
| 3 | Namespace | Unchanged — everything stays in `OmegaCommon::` | Same as above. |
| 4 | Export macros | One per binary: `OMEGACOMMON_CORE_EXPORT`, `OMEGACOMMON_IMG_EXPORT`, `OMEGACOMMON_ASIO_EXPORT` | The existing `OMEGACOMMON_EXPORT` macro stays as a **compat alias** for `OMEGACOMMON_CORE_EXPORT` so non-Core headers can be retargeted without a flag-day rename. |
| 5 | Dependency direction | Img → Core; ASIO → Core; Core depends on neither | Only physically possible direction — Core has no img / async knowledge. |
| 6 | Tools (omega-ebin, omega-wrapgen, omega-assetc, parse-test) | Link `OmegaCommon.Core` only | None of them touch image codec or async I/O. |
| 7 | Versioning | Single `OMEGACOMMON_VERSION` covers all three | Avoids three independently-drifting version numbers for what is logically one project. |

## Dependency graph

```
                +----------------------+
                |   OmegaCommon.Core   |
                |  ICU, OpenSSL,       |
                |  PCRE2, curl/WinHTTP |
                +----------+-----------+
                           ^
              +------------+------------+
              |                         |
  +-----------+----------+   +----------+-----------+
  |   OmegaCommon.Img    |   |   OmegaCommon.ASIO   |
  |  libpng, libjpeg,    |   |  Boost (header-only) |
  |  libtiff, zlib       |   |  (+ OpenSSL via Core)|
  +----------------------+   +----------------------+
```

Beast's `ssl_stream` uses OpenSSL. ASIO does **not** re-link OpenSSL —
it picks up the existing static `ssl` / `crypto` imported targets
through Core's transitive `PUBLIC` link (see CMake section below).

## Source migration

### `OmegaCommon.Core` sources

Everything currently in OmegaCommon **except** the image codec and
the async I/O TUs:

```
src/utils.cpp
src/fs.cpp
src/crt.c
src/json.cpp
src/cli.cpp
src/format.cpp
src/regex.cpp
src/crypto.cpp
src/assets.cpp
src/unicode/*.cpp
src/posix/net-curl.cpp        (non-Windows non-Apple)
src/win/fs-win.cpp            (Windows)
src/win/net-win.cpp           (Windows)
src/win/multithread-win.cpp   (Windows)
src/unix/fs-unixother.cpp     (Linux)
src/unix/multithread-unix.cpp (Linux)
src/macos/fs-cocoa.mm         (macOS)
```

### `OmegaCommon.Img` sources

```
src/img/*.cpp                  (everything currently matched by COMMON_IMG_SRCS)
```

### `OmegaCommon.ASIO` sources

```
src/io.cpp                     (new — from Async plan Phase 2)
src/websocket.cpp              (new — from Async plan Phases 3–4)
```

Phase 3 of this plan creates the empty ASIO binary so the Async plan
has somewhere to land.

## Public header changes

Per Decision #4, three new export macros are introduced. The existing
`OMEGACOMMON_EXPORT` keeps working as a compat alias for the Core
macro so we never need a flag-day rename.

### `include/omega-common/utils.h` (Core's home for the macro pattern)

```cpp
// Per-binary export macros. Each split binary defines its own
// OMEGACOMMON_<BIN>__BUILD__ at compile time; consumers see dllimport.
// On non-Windows targets these are all empty (default visibility).

#ifdef _WIN32
  #ifdef OMEGACOMMON_CORE__BUILD__
    #define OMEGACOMMON_CORE_EXPORT __declspec(dllexport)
  #else
    #define OMEGACOMMON_CORE_EXPORT __declspec(dllimport)
  #endif
#else
  #define OMEGACOMMON_CORE_EXPORT
#endif

// Compat: OMEGACOMMON_EXPORT continues to mean "exported from Core"
// for headers that haven't been retargeted yet (or never will be).
#define OMEGACOMMON_EXPORT OMEGACOMMON_CORE_EXPORT
```

### `include/omega-common/img.h`

Add at top:

```cpp
#ifdef _WIN32
  #ifdef OMEGACOMMON_IMG__BUILD__
    #define OMEGACOMMON_IMG_EXPORT __declspec(dllexport)
  #else
    #define OMEGACOMMON_IMG_EXPORT __declspec(dllimport)
  #endif
#else
  #define OMEGACOMMON_IMG_EXPORT
#endif
```

Then rewrite the existing `OMEGACOMMON_EXPORT` uses in img.h to
`OMEGACOMMON_IMG_EXPORT`. (Currently 6 occurrences: `PixelStorage`,
`BitmapImage`, and the four `loadFrom*` free functions.)

### `include/omega-common/io.h` and `include/omega-common/websocket.h`

Use `OMEGACOMMON_ASIO_EXPORT`, mirroring the same pattern. Defined
inline at the top of each header (no separate visibility.h —
the existing per-header pattern in OmegaCommon stays).

## CMake changes (`common/CMakeLists.txt`)

### Replace the single `add_omega_graphics_module("OmegaCommon" SHARED ...)` call

with three:

```cmake
# --- OmegaCommon.Core ---
file(GLOB COMMON_CORE_SRCS         CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB COMMON_UNICODE_SRCS      CONFIGURE_DEPENDS "src/unicode/*.cpp")
list(REMOVE_ITEM COMMON_CORE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/io.cpp"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/src/websocket.cpp")
list(APPEND COMMON_CORE_SRCS ${COMMON_UNICODE_SRCS})

add_omega_graphics_module("OmegaCommon.Core" SHARED
    SOURCES ${COMMON_CORE_SRCS}
    HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
    EMBEDDED_LIBS ${ICU_RUNTIME})

target_include_directories("OmegaCommon.Core" PRIVATE "${OMEGACOMMON_RAPIDJSON_INCLUDE_DIR}")
target_link_libraries("OmegaCommon.Core" PRIVATE pcre2-8 ssl crypto icuuc icudata icui18n)
target_compile_definitions("OmegaCommon.Core" PRIVATE OMEGACOMMON_CORE__BUILD__)
add_dependencies("OmegaCommon.Core" icu)

# Platform sources + system libs for Core (curl/WinHTTP, etc.) — same
# per-platform target_sources blocks as today, just retargeted to
# "OmegaCommon.Core" instead of "OmegaCommon".

# --- OmegaCommon.Img ---
file(GLOB COMMON_IMG_SRCS CONFIGURE_DEPENDS "src/img/*.cpp")

add_omega_graphics_module("OmegaCommon.Img" SHARED
    SOURCES ${COMMON_IMG_SRCS}
    HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries("OmegaCommon.Img"
    PUBLIC  "OmegaCommon.Core"
    PRIVATE png turbojpeg tiff z)
target_compile_definitions("OmegaCommon.Img" PRIVATE OMEGACOMMON_IMG__BUILD__)
add_dependencies("OmegaCommon.Img" libpng libjpeg-turbo libtiff zlib)

# --- OmegaCommon.ASIO ---
# Created empty in this plan's Phase 3. The Async plan (Phase 2/3/4)
# fills io.cpp and websocket.cpp.
add_omega_graphics_module("OmegaCommon.ASIO" SHARED
    SOURCES ""    # filled by Async plan
    HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries("OmegaCommon.ASIO" PUBLIC "OmegaCommon.Core")
target_compile_definitions("OmegaCommon.ASIO" PRIVATE OMEGACOMMON_ASIO__BUILD__)
# Boost include path + defines applied by the Async plan, not here.
```

### Tools — retarget to Core

```cmake
add_omega_graphics_tool("omega-wrapgen" LIBS "OmegaCommon.Core" ...)
add_omega_graphics_tool("omega-assetc"  LIBS "OmegaCommon.Core" ...)
add_omega_graphics_tool("omega-ebin"    LIBS "OmegaCommon.Core" ...)
add_omega_graphics_tool("parse-test"    LIBS "OmegaCommon.Core" ...)
```

None of the tools touch image codec or async I/O today.

### macOS install-name fixups

The existing `add_dependencies("OmegaCommon" libicuuc.dylib_install_name ...)`
block retargets to `OmegaCommon.Core` (it's the binary that embeds ICU).

## Consumer impact (other modules)

Audit from `grep`-of-record (already done — see implementation):

| Consumer | Current link | Post-split link |
|----------|--------------|-----------------|
| `gte` (OmegaGTE) | `OmegaCommon` PUBLIC | `OmegaCommon.Core` PUBLIC + `OmegaCommon.Img` PUBLIC (uses image loading for textures — confirmed by `OmegaCommon::Img` reference in gte/CMakeLists.txt comment) |
| `wtk` (OmegaWTK_Core / _Native / _Composition) | `OmegaCommon` PUBLIC | `OmegaCommon.Core` PUBLIC + `OmegaCommon.Img` PUBLIC (uses image loading for asset thumbnails / bitmap surfaces) |
| `aqua` | `OmegaCommon` PUBLIC | `OmegaCommon.Core` PUBLIC (no image / async usage) |
| `kreate` | not currently linking OmegaCommon directly — confirm during Phase 4 | TBD per audit |
| `ide` | not currently linking OmegaCommon directly — confirm during Phase 4 | TBD per audit |
| `autom` | not a consumer (it sits below common in the build) | unchanged |

Consumer updates land in Phase 4 of this plan, gated on the Core and
Img binaries existing and tested in isolation.

## Implementation phases

### Phase 1 — Per-binary export macros, single binary intact (~60 LOC)

- `1a` Edit `utils.h` to define the new `OMEGACOMMON_CORE_EXPORT` macro
  and alias the legacy `OMEGACOMMON_EXPORT` to it.
- `1b` Edit `img.h` to define `OMEGACOMMON_IMG_EXPORT` and rewrite its
  6 export-macro uses.
- `1c` Pre-stage `io.h` / `websocket.h` export macros (the Async plan
  Phase 1c creates the headers themselves; this phase just confirms the
  macro pattern is in place beforehand).
- `1d` CMake: rename the `OMEGACOMMON__BUILD__` compile-definition on
  the existing single `OmegaCommon` target to `OMEGACOMMON_CORE__BUILD__`.
  Still a single binary, still called `OmegaCommon`. **No consumer
  change yet.**
- Verifies: clean build on Linux + Windows. Symbol table of the lone
  DLL unchanged.

Small-feature exception applies (<300 LOC). No sub-bullets needed.

### Phase 2 — Split out `OmegaCommon.Img` (~120 LOC of CMake)

- `2a` Add a second `add_omega_graphics_module("OmegaCommon.Img" SHARED ...)`
  call. Drop `src/img/*.cpp` from Core's source glob.
- `2b` Drop `png turbojpeg tiff z` from Core's `target_link_libraries`,
  add them to Img.
- `2c` Drop `libpng libjpeg-turbo libtiff zlib` from Core's
  `add_dependencies`, add them to Img.
- `2d` Img `target_link_libraries(... PUBLIC OmegaCommon.Core)`.
- `2e` **Rename the Core target from `OmegaCommon` → `OmegaCommon.Core`.**
  This is the flag-day. Two consumer updates needed in the same commit:
  - `gte/CMakeLists.txt`: `OmegaCommon` → `OmegaCommon.Core` + add
    `OmegaCommon.Img` to PUBLIC link.
  - `wtk/CMakeLists.txt`: `OmegaCommon` → `OmegaCommon.Core` + add
    `OmegaCommon.Img` to PUBLIC link (three `target_link_libraries`
    lines).
  - `aqua/CMakeLists.txt`: `OmegaCommon` → `OmegaCommon.Core` (no Img).
- `2f` Audit kreate/ide for any indirect references; update if found.
- Verifies: full top-level CMake build on Linux + Windows. wtk
  `OmegaWTK` framework still links and image loads still work.

### Phase 3 — Reserve empty `OmegaCommon.ASIO` (~30 LOC)

- `3a` Add a third `add_omega_graphics_module("OmegaCommon.ASIO" SHARED ...)`
  with a single stub TU `src/asio_stub.cpp` that defines nothing (so
  the linker has something to produce a DLL from).
- `3b` `target_link_libraries(OmegaCommon.ASIO PUBLIC OmegaCommon.Core)`.
- `3c` `target_compile_definitions(OmegaCommon.ASIO PRIVATE OMEGACOMMON_ASIO__BUILD__)`.
- No Boost yet — that's Async plan Phase 1b.
- Verifies: empty DLL builds clean on all platforms.

### Phase 4 — Consumer migration audit (~50 LOC)

- `4a` Grep across `kreate/`, `ide/`, `va/` for any indirect
  OmegaCommon link references (e.g. via `OmegaGTE`'s transitive
  PUBLIC dependency, which automatically retargets).
- `4b` Fix any direct-link references found.
- `4c` Update `common/.plans/OmegaCommon-Completion-Plan.md` status
  table to note the binary split.
- Verifies: end-to-end build of every top-level subdir clean.

## Test strategy

- Phases 1 and 3 are CMake-only refactors; the existing test suite
  (assetc tests, wrapgen tests) is the verification.
- Phase 2 carries the real risk — `OmegaCommon::Img` symbols need to
  resolve from a different DLL than before. wtk's image-loading paths
  are the load-bearing test:
  - On Windows, confirm `libOmegaCommon.Img.dll` ends up next to the
    consumer at install time (the `EMBEDDED_LIBS` /
    `add_omega_graphics_module` infrastructure already handles co-located
    DLLs; double-check by examining the build/bin/ tree).
  - On Linux/macOS, the dependency graph at runtime (`ldd` / `otool -L`)
    must show OmegaCommon.Img loaded by wtk binaries.

## Risks

| Risk | Mitigation |
|------|------------|
| Windows DLL filenames with literal dots (`OmegaCommon.Core.dll`) confuse loader / install tooling | CMake-supported; existing `add_omega_graphics_module` SHARED path already handles arbitrary names. If a real issue surfaces during Phase 2, fall back to `OmegaCommonCore` (no dot) — `~10`-line CMake change, no header impact. |
| Phase 2 flag-day touches three top-level modules in one commit | Land Phase 2 with the consumer updates in a single PR. Don't try to half-migrate. |
| The `OMEGACOMMON_EXPORT → OMEGACOMMON_CORE_EXPORT` alias means non-Core headers using the legacy macro silently get dllimport-from-Core symbols | Phase 1b rewrites img.h's uses immediately. The Async plan headers use the new ASIO macro from the start. The alias is for **transitional** use only — by end of Phase 2, no non-Core public header uses the bare `OMEGACOMMON_EXPORT`. |
| Image consumer count higher than the audit caught | Phase 4 explicit audit pass covers this. |

## Out of scope

- Splitting Core further (e.g. separating crypto/regex/unicode into
  their own binaries). Not justified by deps overlap — they all share
  OpenSSL/PCRE2/ICU which already coexist.
- Renaming or moving public headers. Layout stays as-is.
- Changing the `OmegaCommon::` C++ namespace.
- Touching the autom `AUTOM.build` file. CMake is the source of truth
  for this project; the legacy autom build is informational.

## Open items (confirm before Phase 1 lands)

1. **Target name format** — keep the literal `.` (`OmegaCommon.Core` etc.)
   or canonicalize to `OmegaCommonCore` / `OmegaCommonImg` /
   `OmegaCommonASIO`? Default in this plan: literal dot per the user's
   wording. Easy to change later.
2. **Audit scope for Phase 4** — should kreate/ide/va be included even
   though grep shows no direct `OmegaCommon` reference today, or is the
   transitive-via-OmegaGTE path good enough? Default: include them in
   the audit, fix only if direct refs are found.
