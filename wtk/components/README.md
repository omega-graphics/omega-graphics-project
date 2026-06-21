# OmegaWTK Components

`wtk/components/` houses opt-in widget/view libraries that ship as part of
the OmegaWTK SDK rather than inside the core library. Each component
compiles to its own shared library named `OmegaWTK_<name>` and is bundled
alongside `OmegaWTK` so consumer apps see the whole SDK as one product —
they never name a component on their own link line.

The default OmegaWTK SDK contains every widget that every WTK application
is expected to need: layout containers, buttons, text, images, video.
Anything that pulls in a substantial extra dependency, expands the surface
beyond what a typical app uses, or wraps a third-party engine that would
otherwise force itself on every consumer, lives here instead.

## Why a component is not a normal widget

A component pays for itself in two cases:

1. **Heavyweight dependency.** GTEView pulls OmegaGTE's full pipeline /
   compute / mesh-shader machinery into the link. Apps that just want to
   lay out buttons and play a video do not pay for it.

2. **Specialist surface.** PDF viewers, web views, map tiles, chart layers
   — feature surfaces that a small subset of apps actually use. The
   component pattern keeps the default WTK link small.

Everything else stays in `wtk/src/Widgets/` and `wtk/src/UI/`.

## Where each component lives

All components except `gteview` are **external repos fetched via
AUTOMDEPS** (named `omega-graphics/wtk-<name>` on the source host) into
`wtk/components/<name>/`. The folder is empty in a fresh tree until
AUTOMDEPS clones the external repo into it.

`gteview` is the only INTERNAL component — it lives in this repo because
GTE's own tests exercise it directly. Treat it as the canonical reference
when authoring a new external component.

Current roster:

| Component  | Origin                                | Status                       |
|------------|---------------------------------------|------------------------------|
| `gteview`  | this repo (internal)                  | scaffold (build wiring only) |
| `webview`  | `omega-graphics/wtk-webview`          | fetched via AUTOMDEPS        |

Folders for components that do not yet exist as repos (`chartview`,
`livegraphview`, `mapview`, `pdfview`) are reserved name slots — the
umbrella skips any subdir without a `CMakeLists.txt`, so empty folders
cost nothing at configure time.

## How a component is packaged

| Platform | Where the shared lib lands                                              |
|----------|-------------------------------------------------------------------------|
| macOS    | Embedded inside `OmegaWTK.framework/Versions/<V>/Libraries/`            |
| Windows  | Sibling of `OmegaWTK.dll` in `${CMAKE_BINARY_DIR}/bin/`                 |
| Linux    | Sibling of `libOmegaWTK.so` in `${CMAKE_BINARY_DIR}/lib/`               |

On macOS the framework's `add_omega_graphics_module(... FRAMEWORK ...)`
call lists every configured-in component in its `EMBEDDED_LIBS`, so the
framework bundle build copies each component dylib into the framework's
Libraries/ directory and code-signs the whole bundle as a unit.

On Windows/Linux the component sets its own output directory to match
the core library's. `omega_stage_runtime_dlls` (Win32) and the executable's
`$ORIGIN` rpath (Linux) already cover anything that lives next to the core
library, so no extra staging glue is required.

## Layout of a component

```
wtk/components/<name>/
├── CMakeLists.txt
├── include/
│   └── omegaWTK/
│       └── Components/
│           └── <Name>/
│               └── *.h          # public headers
└── src/
    ├── *.cpp                    # cross-platform implementation
    └── Native/                  # optional, per-platform GPU/native glue
        ├── macos/*.mm
        ├── win/*.cpp
        └── gtk/*.cpp
```

Public headers live under `omegaWTK/Components/<Name>/` so consumers write:

```cpp
#include <omegaWTK/Components/GTEView/GTEViewWidget.h>
```

That keeps the namespace obvious at the use site and stops component
headers from polluting the core `omegaWTK/Widgets/` include tree.

## Build wiring contract

A component CMakeLists.txt must:

1. Declare its opt-in flag (default OFF) and early-return when unset:
   ```cmake
   option(OMEGAWTK_COMPONENT_<NAME> "Build OmegaWTK_<name> (...)" OFF)
   if(NOT OMEGAWTK_COMPONENT_<NAME>)
       return()
   endif()
   ```

2. Build a SHARED target named **`OmegaWTK<name>`** (CMake target name
   == output base name).

3. Set per-platform output directory on Windows/Linux to put the binary
   alongside the core library (macOS uses the framework's `EMBEDDED_LIBS`
   path instead — see gteview for the exact pattern).

4. Define `OMEGAWTK_APP` plus the appropriate `TARGET_*` macros when
   compiling on Win32. The component is a consumer of `OmegaWTK`'s
   exported symbols, so `OMEGAWTK_EXPORT` must resolve to `dllimport` in
   its TUs.

5. Register itself with the parent build:
   ```cmake
   set_property(GLOBAL APPEND PROPERTY OMEGAWTK_COMPONENT_TARGETS OmegaWTK_<name>)
   ```

The component does **not** link `OmegaWTK` from its own CMakeLists —
components are configured before the framework target exists. The parent
`wtk/CMakeLists.txt` adds the framework as a link dependency in a second
sweep after the framework is created.

The umbrella `wtk/components/CMakeLists.txt` auto-discovers any subdir
with a `CMakeLists.txt`, so adding a new component is purely additive —
no central edit required.

Everything is gated by `OMEGAWTK_BUILD_COMPONENTS=ON` at the top-level
WTK configure. With that on, each component is then individually
controlled by its own `OMEGAWTK_COMPONENT_<NAME>` flag.

## Consuming a component from a test or app

`OmegaWTKApp()` auto-links every configured-in component into every app
it builds. Apps that link `OmegaWTK` get everything that was configured
in at WTK build time, with zero per-app boilerplate:

```cmake
OmegaWTKApp(
    NAME      MyApp
    BUNDLE_ID "org.example.MyApp"
    SOURCES   main.cpp)
# No need to mention OmegaWTK_GTEView, OmegaWTK_WebView, etc.
# If they were configured in, the app links them.
```

This is what the SDK's "no external dependency linkage on the App"
contract means in CMake terms: component selection happens once, at WTK
configure time, via the per-component `OMEGAWTK_COMPONENT_<NAME>` flags.
Apps express what they use through `#include` and code, not through link
lines.

## Component-side exports (Phase G1+, not in scaffold)

When a component grows its own exported types, it ships an
`Export.h` header under `include/omegaWTK/Components/<Name>/` mirroring
`wtk/include/OmegaWTKExport.h`:

- Defines `OMEGAWTK_<NAME>_EXPORT` as `dllexport` when the component is
  being built (gated on `OMEGAWTK_<NAME>_BUILDING` set by the component
  CMakeLists), `dllimport` otherwise, empty on non-Win32 platforms.
- Component public headers mark their exported classes / functions with
  `OMEGAWTK_<NAME>_EXPORT`.
- The component's CMakeLists adds
  `target_compile_definitions(OmegaWTK_<name> PRIVATE OMEGAWTK_<NAME>_BUILDING)`
  to flip the macro to dllexport during its own compilation.

No component has reached this point yet; the gteview scaffold omits the
header until Phase G1 brings in classes that need to be exported.
