# OmegaCommon Binary Split Plan

## Overview

Split the single `OmegaCommon` shared library into three sibling
binaries with disjoint third-party dependency footprints:

| Binary | Contents | External deps |
|--------|----------|---------------|
| **OmegaCommonCore** | utils, fs, crt, json, cli, format, regex, crypto, multithread, net, unicode, assets | ICU, OpenSSL, PCRE2, libcurl/WinHTTP |
| **OmegaCommonImg** | image codec (BitmapImage, loadFromFile/Buffer/URL) | libpng, libjpeg-turbo, libtiff, zlib (+ Core) |
| **OmegaCommonASIO** | async I/O reactor + WebSocket (see Async plan) | Boost (+ Core, + OpenSSL via Core) |

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
| 1 | Target name format | `OmegaCommonCore`, `OmegaCommonImg`, `OmegaCommonASIO` (no dot) | Honors the user's stated naming. Plain concatenated names avoid any loader / install-tooling quirks around dotted DLL filenames; resulting DLL/dylib filenames are `OmegaCommonCore.dll` etc. |
| 2 | Public header layout | Unchanged — img.h / io.h / websocket.h stay under `include/omega-common/` | Avoids consumer #include churn. The split is binary, not API. |
| 3 | Namespace | Unchanged — everything stays in `OmegaCommon::` | Same as above. |
| 4 | Export macro | A single `OMEGACOMMON_EXPORT` shared by all three binaries (the existing macro, unchanged) | The split is binary-only; all public headers keep the one macro they use today, so there is zero header churn and nothing to alias or retarget. Each binary defines `OMEGACOMMON__BUILD__` while compiling its own TUs. |
| 5 | Dependency direction | Img → Core; ASIO → Core; Core depends on neither | Only physically possible direction — Core has no img / async knowledge. |
| 6 | Tools (omega-ebin, omega-wrapgen, omega-assetc, parse-test) | Link `OmegaCommonCore` only | None of them touch image codec or async I/O. |
| 7 | Versioning | Single `OMEGACOMMON_VERSION` covers all three | Avoids three independently-drifting version numbers for what is logically one project. |

## Dependency graph

```
                +----------------------+
                |   OmegaCommonCore    |
                |  ICU, OpenSSL,       |
                |  PCRE2, curl/WinHTTP |
                +----------+-----------+
                           ^
              +------------+------------+
              |                         |
  +-----------+----------+   +----------+-----------+
  |   OmegaCommonImg     |   |   OmegaCommonASIO    |
  |  libpng, libjpeg,    |   |  Boost (header-only) |
  |  libtiff, zlib       |   |  (+ OpenSSL via Core)|
  +----------------------+   +----------------------+
```

Beast's `ssl_stream` uses OpenSSL. ASIO does **not** re-link OpenSSL —
it picks up the existing static `ssl` / `crypto` imported targets
through Core's transitive `PUBLIC` link (see CMake section below).

## Source migration

### `OmegaCommonCore` sources

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

### `OmegaCommonImg` sources

```
src/img/*.cpp                  (everything currently matched by COMMON_IMG_SRCS)
```

### `OmegaCommonASIO` sources

```
src/asio/io.cpp                     (new — from Async plan Phase 2)
src/asio/websocket.cpp              (new — from Async plan Phases 3–4)
```

Phase 3 of this plan creates the empty ASIO binary so the Async plan
has somewhere to land.

## Public header changes

Per Decision #4, there is a single export macro shared by all three
binaries: the existing `OMEGACOMMON_EXPORT`. No new macros are
introduced, no public header's `OMEGACOMMON_EXPORT` uses change, and
there is nothing to alias. The only edit is to the macro's own
definition in `utils.h`.

### `include/omega-common/utils.h` (home of the export macro)

```cpp
// Single export macro shared by every OmegaCommon binary. Whichever
// binary is currently compiling defines OMEGACOMMON__BUILD__, so its
// own TUs emit dllexport; consumers see dllimport. On non-Windows
// targets it is empty (default visibility).

#ifdef _WIN32
  #ifdef OMEGACOMMON__BUILD__
    #define OMEGACOMMON_EXPORT __declspec(dllexport)
  #else
    #define OMEGACOMMON_EXPORT __declspec(dllimport)
  #endif
#else
  #define OMEGACOMMON_EXPORT
#endif
```

### `include/omega-common/img.h`, `io.h`, and `websocket.h`

No change. These headers keep using `OMEGACOMMON_EXPORT` exactly as
they do today (img.h's 6 uses — `PixelStorage`, `BitmapImage`, and the
four `loadFrom*` free functions — stay as-is). They inherit the macro
from `utils.h`, so there is no per-header macro block and no separate
visibility.h.

## CMake changes (`common/CMakeLists.txt`)

### Replace the single `add_omega_graphics_module("OmegaCommon" SHARED ...)` call

with three:

```cmake
# --- OmegaCommonCore ---
file(GLOB COMMON_CORE_SRCS         CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB COMMON_UNICODE_SRCS      CONFIGURE_DEPENDS "src/unicode/*.cpp")
list(REMOVE_ITEM COMMON_CORE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/asio/io.cpp"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/src/asio/websocket.cpp")
list(APPEND COMMON_CORE_SRCS ${COMMON_UNICODE_SRCS})

add_omega_graphics_module("OmegaCommonCore" SHARED
    SOURCES ${COMMON_CORE_SRCS}
    HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
    EMBEDDED_LIBS ${ICU_RUNTIME})

target_include_directories("OmegaCommonCore" PRIVATE "${OMEGACOMMON_RAPIDJSON_INCLUDE_DIR}")
target_link_libraries("OmegaCommonCore" PRIVATE pcre2-8 ssl crypto icuuc icudata icui18n)
target_compile_definitions("OmegaCommonCore" PRIVATE OMEGACOMMON__BUILD__)
add_dependencies("OmegaCommonCore" icu)

# Platform sources + system libs for Core (curl/WinHTTP, etc.) — same
# per-platform target_sources blocks as today, just retargeted to
# "OmegaCommonCore" instead of "OmegaCommon".

# --- OmegaCommonImg ---
file(GLOB COMMON_IMG_SRCS CONFIGURE_DEPENDS "src/img/*.cpp")

add_omega_graphics_module("OmegaCommonImg" SHARED
    SOURCES ${COMMON_IMG_SRCS}
    HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries("OmegaCommonImg"
    PUBLIC  "OmegaCommonCore"
    PRIVATE png turbojpeg tiff z)
target_compile_definitions("OmegaCommonImg" PRIVATE OMEGACOMMON__BUILD__)
add_dependencies("OmegaCommonImg" libpng libjpeg-turbo libtiff zlib)

# --- OmegaCommonASIO ---
# Created empty in this plan's Phase 3. The Async plan (Phase 2/3/4)
# fills io.cpp and websocket.cpp.
add_omega_graphics_module("OmegaCommonASIO" SHARED
    SOURCES ""    # filled by Async plan
    HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries("OmegaCommonASIO" PUBLIC "OmegaCommonCore")
target_compile_definitions("OmegaCommonASIO" PRIVATE OMEGACOMMON__BUILD__)
# Boost include path + defines applied by the Async plan, not here.
```

### Tools — retarget to Core

```cmake
add_omega_graphics_tool("omega-wrapgen" LIBS "OmegaCommonCore" ...)
add_omega_graphics_tool("omega-assetc"  LIBS "OmegaCommonCore" ...)
add_omega_graphics_tool("omega-ebin"    LIBS "OmegaCommonCore" ...)
add_omega_graphics_tool("parse-test"    LIBS "OmegaCommonCore" ...)
```

None of the tools touch image codec or async I/O today.

### macOS install-name fixups

The existing `add_dependencies("OmegaCommon" libicuuc.dylib_install_name ...)`
block retargets to `OmegaCommonCore` (it's the binary that embeds ICU).

## Consumer impact (other modules)

Audit from `grep`-of-record (already done — see implementation):

| Consumer | Current link | Post-split link |
|----------|--------------|-----------------|
| `gte` (OmegaGTE) | `OmegaCommon` PUBLIC | `OmegaCommonCore` PUBLIC + `OmegaCommonImg` PUBLIC (uses image loading for textures — confirmed by `OmegaCommon::Img` reference in gte/CMakeLists.txt comment) |
| `wtk` (OmegaWTK_Core / _Native / _Composition) | `OmegaCommon` PUBLIC | `OmegaCommonCore` PUBLIC + `OmegaCommonImg` PUBLIC (uses image loading for asset thumbnails / bitmap surfaces) |
| `aqua` | `OmegaCommon` PUBLIC | `OmegaCommonCore` PUBLIC (no image / async usage) |
| `kreate` | not currently linking OmegaCommon directly — confirm during Phase 4 | TBD per audit |
| `ide` | not currently linking OmegaCommon directly — confirm during Phase 4 | TBD per audit |
| `autom` | not a consumer (it sits below common in the build) | unchanged |

Consumer updates land in Phase 4 of this plan, gated on the Core and
Img binaries existing and tested in isolation.

## Implementation phases

### Phase 1 — Confirm the single export macro + build flag (~10 LOC)

With one shared `OMEGACOMMON_EXPORT` (Decision #4) there are no new
macros to introduce — this phase just confirms the existing pattern is
what the split relies on.

- `1a` Confirm `utils.h` defines `OMEGACOMMON_EXPORT` gated on
  `OMEGACOMMON__BUILD__` (the single-macro form shown above). No new
  macro, no alias.
- `1b` `img.h` — no change; it already uses `OMEGACOMMON_EXPORT`.
- `1c` `io.h` / `websocket.h` — created by the Async plan and use
  `OMEGACOMMON_EXPORT` from the start; nothing to pre-stage.
- `1d` CMake: confirm the existing single `OmegaCommon` target defines
  `OMEGACOMMON__BUILD__` (no rename — every split binary will define the
  same flag). Still a single binary, still called `OmegaCommon`. **No
  consumer change yet.**
- Verifies: clean build on Linux + Windows. Symbol table of the lone
  DLL unchanged.

Small-feature exception applies (<300 LOC). No sub-bullets needed.

### Phase 2 — Split out `OmegaCommonImg` (~120 LOC of CMake)

- `2a` Add a second `add_omega_graphics_module("OmegaCommonImg" SHARED ...)`
  call. Drop `src/img/*.cpp` from Core's source glob.
- `2b` Drop `png turbojpeg tiff z` from Core's `target_link_libraries`,
  add them to Img.
- `2c` Drop `libpng libjpeg-turbo libtiff zlib` from Core's
  `add_dependencies`, add them to Img.
- `2d` Img `target_link_libraries(... PUBLIC OmegaCommonCore)`.
- `2e` **Rename the Core target from `OmegaCommon` → `OmegaCommonCore`.**
  This is the flag-day. Two consumer updates needed in the same commit:
  - `gte/CMakeLists.txt`: `OmegaCommon` → `OmegaCommonCore` + add
    `OmegaCommonImg` to PUBLIC link.
  - `wtk/CMakeLists.txt`: `OmegaCommon` → `OmegaCommonCore` + add
    `OmegaCommonImg` to PUBLIC link (three `target_link_libraries`
    lines).
  - `aqua/CMakeLists.txt`: `OmegaCommon` → `OmegaCommonCore` (no Img).
- `2f` Audit kreate/ide for any indirect references; update if found.
- Verifies: full top-level CMake build on Linux + Windows. wtk
  `OmegaWTK` framework still links and image loads still work.

### Phase 3 — Reserve empty `OmegaCommonASIO` (~30 LOC)

- `3a` Add a third `add_omega_graphics_module("OmegaCommonASIO" SHARED ...)`
  with a single stub TU `src/asio/asio_stub.cpp` that defines nothing
  (so the linker has something to produce a DLL from).
- `3b` `target_link_libraries(OmegaCommonASIO PUBLIC OmegaCommonCore)`.
- `3c` `target_compile_definitions(OmegaCommonASIO PRIVATE OMEGACOMMON__BUILD__)`.
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
  - On Windows, confirm `libOmegaCommonImg.dll` ends up next to the
    consumer at install time (the `EMBEDDED_LIBS` /
    `add_omega_graphics_module` infrastructure already handles co-located
    DLLs; double-check by examining the build/bin/ tree).
  - On Linux/macOS, the dependency graph at runtime (`ldd` / `otool -L`)
    must show OmegaCommonImg loaded by wtk binaries.

## Risks

| Risk | Mitigation |
|------|------------|
| Phase 2 flag-day touches three top-level modules in one commit | Land Phase 2 with the consumer updates in a single PR. Don't try to half-migrate. |
| Single `OMEGACOMMON_EXPORT` + single `OMEGACOMMON__BUILD__`: when an Img/ASIO TU includes a Core header on Windows, `OMEGACOMMON__BUILD__` is defined for that compile, so Core's symbols are marked `dllexport` instead of `dllimport` (risk of LNK4217 / accidental re-export from the wrong DLL). | Non-Windows is unaffected — the macro is empty there. For Windows, the clean fix is to keep the one macro as a no-op and let CMake auto-export via per-target `WINDOWS_EXPORT_ALL_SYMBOLS`; otherwise confirm at the Phase 2 Windows hand-off whether the mismatch actually surfaces (pure consumers, which never define the flag, are always correct). |
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

1. **Target name format** — RESOLVED: no-dot names `OmegaCommonCore` /
   `OmegaCommonImg` / `OmegaCommonASIO`, per the user's instruction.
   Decision #1 and the rest of the plan reflect this.
2. **Audit scope for Phase 4** — should kreate/ide/va be included even
   though grep shows no direct `OmegaCommon` reference today, or is the
   transitive-via-OmegaGTE path good enough? Default: include them in
   the audit, fix only if direct refs are found.
