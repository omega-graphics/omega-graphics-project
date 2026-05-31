# OmegaWTK Components

`wtk/components/` houses opt-in widget/view libraries that ship **alongside**
OmegaWTK rather than inside it. Each component is its own statically-linked
library that a consumer opts into per app.

The default OmegaWTK SDK contains every widget that every WTK application
is expected to need: layout containers, buttons, text, images, video. Anything
that pulls in a substantial extra dependency, expands the surface beyond what
a typical app uses, or wraps a third-party engine that would otherwise force
itself on every consumer, lives here instead.

## Why a component is not a normal widget

A component pays for itself in two cases:

1. **Heavyweight dependency.** GTEView pulls libOmegaGTE's full pipeline /
   compute / mesh-shader machinery into the link. Apps that just want to lay
   out buttons and play a video do not pay for it.

2. **Specialist surface.** PDF viewers, web views, map tiles, chart layers —
   feature surfaces that a small subset of apps actually use. The component
   pattern keeps the default WTK link small.

Everything else stays in `wtk/src/Widgets/` and `wtk/src/UI/`.

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

That keeps the namespace obvious at the use site and stops component headers
from polluting the core `omegaWTK/Widgets/` include tree.

## Build wiring

A component opts itself in via a CMake option that defaults to OFF:

```cmake
option(OMEGAWTK_COMPONENT_<NAME> "Build the <Name> component" OFF)
if(NOT OMEGAWTK_COMPONENT_<NAME>)
    return()
endif()
```

The library target is named `OmegaWTKComponent_<Name>`, STATIC, with
`POSITION_INDEPENDENT_CODE ON`. It links the core `OmegaWTK` framework /
shared lib publicly and whatever extra deps the component itself needs
(privately, where the headers don't expose them).

The umbrella `wtk/components/CMakeLists.txt` auto-discovers any subdir with a
`CMakeLists.txt`, so adding a new component is a single `add_subdirectory`
side effect — no central edit required.

Components are only reached when `OMEGAWTK_BUILD_COMPONENTS=ON` at the
top-level WTK configure (gated from `wtk/CMakeLists.txt`). With that on,
each component is then individually controlled by its own
`OMEGAWTK_COMPONENT_<NAME>` flag.

## Consuming a component from a test or app

`OmegaWTKApp()` does NOT auto-link any component. That is intentional —
component selection is per-consumer. After the helper call, link the
components your app actually needs:

```cmake
OmegaWTKApp(
    NAME          MyApp
    BUNDLE_ID     "org.example.MyApp"
    SOURCES       main.cpp)
target_link_libraries(MyApp PRIVATE OmegaWTKComponent_GTEView)
```

If the component is off (or `OMEGAWTK_BUILD_COMPONENTS=OFF`), the target
does not exist and the link fails fast at configure time — the consumer
finds out immediately rather than discovering missing symbols at runtime.

## Component compile-time flags consumers should know

Tests/apps that link a component should keep the same per-platform flags
`OmegaWTKApp` already sets (`OMEGAWTK_APP`, `TARGET_*`). Components
themselves are built once, not per-app, so they compile in the
library-build mode (no `OMEGAWTK_APP` defined).

When a component grows enough to ship as its own shared library (DLL on
Windows) instead of a static archive, that component will need its own
`OMEGAWTK_COMPONENT_<NAME>_EXPORT` macro mirroring the
`OMEGAWTK_EXPORT`/`OMEGAWTK_APP` flip in `OmegaWTKExport.h`. The current
static-link contract avoids that complexity until a real consumer needs it.

## What components exist today

| Component | Status | Plan |
|-----------|--------|------|
| `gteview/` | scaffold (build wiring only) | `wtk/docs/NativeViewHost-Adoption-Plan.md` Part 2 |
| `chartview/` | empty | — |
| `livegraphview/` | empty | — |
| `mapview/` | empty | — |
| `pdfview/` | empty | — |
| `webview/` | empty | — |

Components with no `CMakeLists.txt` are skipped by the umbrella — empty
folders cost nothing at configure time.
