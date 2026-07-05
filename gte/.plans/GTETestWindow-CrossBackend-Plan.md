# GTETestWindow Cross-Backend Plan

> Status: **Phase 1 landed & verified; Phase 2 landed & verified on native macOS/Metal; Phase 3 landed & verified on native Wayland; Phase 4 landed (GPUTessTest verified on native Metal, the rest written pending Windows/Linux verification).** Phase 1 (API + Win32 + DX 2DTest) is shipped and visually confirmed. Phase 2 (Cocoa backend + Metal 2DTest) is now built and run on a native macOS host (see Phase 4 below for what that session verified and fixed). Phase 3 (GTK-4 + Vulkan 2DTest) is verified on native Wayland. Open Decisions #1/#4/#5 (Phase 1) and #2 (precompiled, Phase 2) are resolved; #3 (D3D12 ComputeTest hidden window) is still open — out of Phase 4's scope (ComputeTest never migrates; see Scope). Phases 5–6 are still proposals.

## Goal

Stand up a single, backend-neutral `GTETestWindow` API that hides the per-platform GUI toolkit (Win32 / Cocoa / GTK / Wayland / Android) behind the same surface every GTE GUI test already needs: *"open one window, hand me back a `NativeRenderTargetDescriptor`, pump the run loop until I exit."*

Today every GTE test that opens a window writes its own toolkit boilerplate three times. The same render body — tessellate a rect, write the vertex buffer, encode a render pass, present — exists side-by-side in `directx/2DTest/main.cpp`, `metal/2DTest/main.mm`, and `vulkan/2DTest/main.cpp`, and the three copies have drifted (texture sampling and UV winding are only in DX; Metal uses runtime shader compilation, DX uses a precompiled `.omegasllib`; the GTK build also rolls in a separate GTK-realize-then-render dance). Net effect: bug fixes land in one backend and not the others, and adding a new GUI test costs 3x the work.

The plan replaces that with one shared per-test `main.cpp` plus a per-backend implementation of `GTETestWindow` selected at CMake time, and rewires every GUI test in `gte/Tests/{directx,metal,vulkan}/` through it.

## Current State

### Per-backend GUI surface today

| Backend | Window-opening test files | Toolkit | Entry point | Native handle the backend reads |
|---|---|---|---|---|
| D3D12 | `directx/2DTest/main.cpp`, `directx/GPUTessTest/main.cpp`, `directx/ComputeTest/main.cpp` (opens a hidden window), `directx/MeshAndRaytracingTest/main.cpp` | Win32 (`RegisterClassEx`, `CreateWindow`, `PeekMessage` loop) | `int APIENTRY WinMain(...)` | `HWND` |
| Metal | `metal/2DTest/main.mm`, `metal/GPUTessTest/main.mm` | Cocoa (`NSApplication`, `NSWindowController`, `NSView` + `CAMetalLayer`) | `int main(int, char**)` + `[NSApp run]` | `CAMetalLayer *` |
| Vulkan/X11 | `vulkan/2DTest/main.cpp`, `vulkan/GPUTessTest/main.cpp`, `vulkan/CPUTessTest/main.cpp`, `vulkan/BlitTest/main.cpp` | GTK3 (`gtk_application_new`, `GtkDrawingArea`, `gdk_window_get_xid`) | `int main(int, char**)` + `g_application_run` | `Display *` + `Window` |
| Vulkan/Wayland | *(none yet)* | — | — | `wl_display *` + `wl_surface *` |
| Vulkan/Android | *(none yet)* | — | — | `ANativeWindow *` |

The backend-portable surface GTE actually consumes is already discriminated:

```cpp
// gte/include/omegaGTE/GE.h:497
struct OMEGAGTE_EXPORT NativeRenderTargetDescriptor {
    bool allowDepthStencilTesting = false;
    PixelFormat pixelFormat = PixelFormat::BGRA8Unorm;
#ifdef TARGET_DIRECTX
    bool isHwnd;
    HWND hwnd;
    unsigned width;
    unsigned height;
#endif
#if defined(TARGET_METAL) && defined(__OBJC__)
    CAMetalLayer *metalLayer;
#endif
#if defined(TARGET_VULKAN)
#  ifdef VULKAN_TARGET_X11
    Window x_window;
    Display *x_display;
#  endif
#  ifdef VULKAN_TARGET_WAYLAND
    wl_surface *wl_surface = nullptr;
    wl_display *wl_display = nullptr;
    unsigned width;
    unsigned height;
#  endif
#  ifdef VULKAN_TARGET_ANDROID
    ANativeWindow *window;
#  endif
#endif
};
```

So GTE itself is *already* portable from the swap-chain in; what is not portable is **everything sitting on top of the descriptor**: window creation, run-loop pumping, entrypoint shape, and the resource-teardown order required before `OmegaGTE::Close(gte)`.

### CMake wiring today

* `gte/Tests/CMakeLists.txt` dispatches to one of `directx/`, `metal/`, `vulkan/` based on `TARGET_*`.
* `gte/Tests/directx/CMakeLists.txt` uses `add_d3d12_test(...)` and forces `/SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup`.
* `gte/Tests/metal/CMakeLists.txt` has `add_metal_test` (windowed, bundled) and `add_metal_cli_test` (CLI).
* `gte/Tests/vulkan/CMakeLists.txt` has `add_vulkan_test`, links against `gtk+-3.0`.
* CLI tests (`sampler_bind_test`, `matrix_ops_test`, `bitfield_ops_test`, `int_vector_io_test`, `mesh_shader_test`, `push_constant_test`, `sampler_validation_test`, `std140_layout_test`) live as **single shared sources** under `gte/Tests/` already and are pulled into each backend's CMake as `../<name>.cpp`. That precedent is exactly the layout the GUI tests should converge on.

### What we will *not* touch

* The CLI / headless tests above. They already share a single source. `GTE_TEST_ENTRY_POINT` in `gte/Tests/GTETestEntryPoint.h` keeps working for them unchanged.
* The shape of `NativeRenderTargetDescriptor` itself — backends already accept it. GTETestWindow only fills it.
* OmegaSL → `.omegasllib` compilation; tests keep loading shaders the same way (`gte.graphicsEngine->loadShaderLibrary(...)` for precompiled, `loadShaderLibraryRuntime(...)` for inline).

## Design

### `GTETestWindow.h` — the cross-backend surface

A new header at `gte/Tests/GTETestWindow.h` is the only thing test bodies include:

```cpp
#ifndef OMEGAGTE_TESTS_GTETESTWINDOW_H
#define OMEGAGTE_TESTS_GTETESTWINDOW_H

#include <omegaGTE/GE.h>
#include <functional>
#include <string>

namespace OmegaGTETests {

    /// Backend-neutral window spec. Every field maps 1:1 to something
    /// every toolkit (Win32, Cocoa, GTK, Wayland, Android) can honor.
    struct GTETestWindowDescriptor {
        const char  *title  = "GTE Test";
        unsigned     width  = 500;
        unsigned     height = 500;
        OmegaGTE::PixelFormat pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
        bool         allowDepthStencilTesting = false;

        /// Optional: capture the first rendered frame to PNG and exit.
        /// Empty (default) = interactive window. Hookup deferred to
        /// Phase 5 — Phase 1 ignores this field but reserves the slot.
        std::string  captureFramePath;
    };

    /// Lifecycle callbacks. The runtime guarantees all three fire on
    /// the GUI thread, in this order:
    ///   onReady  — once, after the window is realized and the
    ///              NativeRenderTargetDescriptor is fully populated for
    ///              the active backend. Test code creates the render
    ///              target inside onReady and holds it.
    ///   onFrame  — every redraw event after onReady. Optional. Tests
    ///              that render once and never animate can leave it
    ///              unset.
    ///   onClose  — once, just before the platform run loop returns to
    ///              RunGTETestWindow's caller. Test code resets every
    ///              GE SharedHandle here, in dependency order, then
    ///              calls OmegaGTE::Close(gte).
    struct GTETestWindowDelegate {
        std::function<void(const OmegaGTE::NativeRenderTargetDescriptor &)> onReady;
        std::function<void()> onFrame;
        std::function<void()> onClose;
    };

    /// Open the window per `desc`, run the platform main loop until
    /// the user closes it, and return the process exit code. Must be
    /// called from the thread that will become the GUI main thread
    /// (i.e. from int main / WinMain; not from a worker).
    int RunGTETestWindow(int argc,
                         const char *argv[],
                         const GTETestWindowDescriptor &desc,
                         const GTETestWindowDelegate &delegate);

} // namespace OmegaGTETests
#endif
```

The header is **pure C++**. No `#import`, no `<windows.h>`, no GTK pull-ins — those live behind the impl files. Test bodies stay platform-independent.

### Per-backend implementations

Three small files, selected by CMake on the active backend macro:

* `gte/Tests/directx/GTETestWindow_Win32.cpp` — `RegisterClassExA`, `CreateWindowExA`, a wndproc that calls `delegate.onFrame` from `WM_PAINT`, a `PeekMessage`/`DispatchMessage` loop, DPI scaling via `GetDpiFromDpiAwarenessContext` (lifted directly from the current `directx/2DTest/main.cpp:202`). Fills `desc.hwnd`, `desc.isHwnd = true`, `desc.width`, `desc.height` before invoking `onReady`. Calls `onClose` on `WM_DESTROY` before posting `WM_QUIT`.
* `gte/Tests/metal/GTETestWindow_Cocoa.mm` — declares its own private `AppDelegate` + `WindowController` mirroring the structure in `metal/2DTest/main.mm:157`, opens an `NSWindow` with an `NSView` whose `layer` is a `CAMetalLayer`, dispatches `delegate.onReady` from `-applicationDidFinishLaunching:` with the layer wired into the descriptor, calls `delegate.onClose` from `-applicationWillTerminate:`, then `[app run]`.
* `gte/Tests/vulkan/GTETestWindow_GTK.cpp` (X11), `GTETestWindow_Wayland.cpp` (when wired), `GTETestWindow_Android.cpp` (when wired) — for X11 today, the GTK realize-then-fill path from `vulkan/2DTest/main.cpp:141`: `gtk_application_new`, `gtk_widget_realize`, then `GDK_WINDOW_XDISPLAY` / `GDK_WINDOW_XID` into the descriptor, then dispatch `onReady`. The Wayland and Android variants follow the same shape (realize the surface, fill the matching descriptor fields, dispatch `onReady`), gated by `VULKAN_TARGET_WAYLAND` / `VULKAN_TARGET_ANDROID` — the same macros that already gate the descriptor fields in `GE.h`.

### Entrypoint shape

The existing `gte/Tests/GTETestEntryPoint.h` already solves the WinMain-vs-main split for CLI tests. **We reuse it as-is.** Every GUI test body becomes:

```cpp
#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"
// ... static GE state ...

GTE_TEST_ENTRY_POINT {
    gte = OmegaGTE::InitWithDefaultDevice();
    // shader library + pipeline setup ...

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title  = "GTE 2DTest";
    desc.width  = 500;
    desc.height = 500;

    OmegaGTETests::GTETestWindowDelegate del;
    del.onReady = [](const auto &nrt) { /* makeNativeRenderTarget + first draw */ };
    del.onClose = []                  { /* teardown */ };

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
}
```

`GTE_TEST_ENTRY_POINT` expands to `int main(int argc, const char *argv[])` everywhere except D3D12, where it expands to `WinMain` that forwards `__argc`/`__argv` into a shadow `gteTestMain` — see `gte/Tests/GTETestEntryPoint.h:19`. The user never writes a platform-specific entrypoint.

### Shared per-test sources

Each GUI test collapses three sibling sources into one shared body, and **every runtime asset — images, meshes, shaders — moves into the existing shared `gte/Tests/assets/` tree** (the precedent the FBX racket already established: see `gte/Tests/assets/orange_tennis_racket/orange_tennis_racket.fbx`, referenced by the DX MeshAndRaytracingTest at `gte/Tests/directx/CMakeLists.txt:81`). The per-backend subfolders survive **only** for genuinely backend-specific *target-template* files — Win32 executable manifests, Cocoa bundle resources — that have no meaning to another platform and are *not* loaded as runtime data.

```
gte/Tests/
├── 2DTest/main.cpp                       # NEW — shared, platform-independent
├── GPUTessTest/main.cpp                  # NEW — shared
├── ComputeTest/main.cpp                  # already shared (headless; see Scope)
├── MeshAndRaytracingTest/main.cpp        # NEW — shared
├── BlitTest/main.cpp                     # NEW — shared (Vulkan-only today)
├── CPUTessTest/main.cpp                  # NEW — shared (Vulkan-only today)
├── GTETestWindow.h                       # NEW
├── GTETestEntryPoint.h                   # existing
├── assets/                               # SHARED runtime data, every backend
│   ├── orange_tennis_racket/             # existing
│   │   └── orange_tennis_racket.fbx
│   ├── 2DTest/                           # NEW
│   │   ├── test.png                      # MOVED from directx/2DTest/
│   │   └── shaders.omegasl               # MOVED + DEDUPED (DX vs Metal copies)
│   └── MeshAndRaytracingTest/            # NEW
│       └── meshAndRaytracing.omegasl     # MOVED from directx/MeshAndRaytracingTest/
├── directx/
│   ├── CMakeLists.txt                    # updated
│   ├── GTETestWindow_Win32.cpp           # NEW
│   └── 2DTest/                           # KEEPS Win32-only template files:
│       ├── manifest.rc.in                #   Win32 manifest source
│       └── app.exe.manifest              #   Win32 manifest payload
├── metal/
│   ├── CMakeLists.txt                    # updated
│   ├── GTETestWindow_Cocoa.mm            # NEW
│   └── 2DTest/                           # KEEPS Cocoa bundle template items:
│       ├── Info.plist
│       ├── MainMenu.nib
│       └── English.lproj/
└── vulkan/
    ├── CMakeLists.txt                    # updated
    └── GTETestWindow_GTK.cpp             # NEW
```

The rule that follows from this layout — and that should govern every test added after this plan lands:

* **`gte/Tests/assets/<TestName>/`** holds anything a test loads at runtime: textures, meshes, fonts, `.omegasl` source (or precompiled `.omegasllib`), and any other data the test body opens with a file path. One copy, one path on disk, every backend stages it the same way.
* **`gte/Tests/<backend>/<TestName>/`** holds *only* build-time, OS-specific template items: Win32 executable manifests, Cocoa `Info.plist` / `.nib` / `.lproj` bundle resources, Android `AndroidManifest.xml`, Wayland desktop entries — files whose schema is owned by the platform, not by the test. If a test has no such files for a backend (Vulkan tests typically don't), the per-backend `<TestName>/` subfolder simply doesn't exist.

`shaders.omegasl` is treated as runtime data even though it is **compiled** at build time — it lives next to the rest of that test's assets in `assets/<TestName>/shaders.omegasl`, and each backend's CMakeLists points `add_omegasl_lib` at that single canonical path instead of three near-identical sibling copies. The current DX vs Metal divergence (DX precompiles a `.omegasllib`; Metal uses inline string compilation) collapses onto one source as part of Open Decision #2 below.

### Lifetime + teardown contract

The D3D12 2DTest documents at `directx/2DTest/main.cpp:302-333` a precise teardown order: `commitToGPUAndWait` → reset every GE `SharedHandle` in dependency order → `OmegaGTE::Close(gte)` → release COM. Get this wrong and D3D12MA's allocator destructor asserts. **The runtime calls `delegate.onClose` strictly before the platform run loop returns**, so the test body owns its own reset sequence inside `onClose` and the static-storage destructors at process exit see nulled handles. This contract is identical on all three backends — Metal needs the matching reset order before `OmegaGTE::Close` to avoid the framework's release-after-close traps, and Vulkan likewise.

## Scope — which tests migrate, which don't

A test migrates to `GTETestWindow` iff it currently opens a swap-chain-backed window (calls `makeNativeRenderTarget`). By that test, the migration covers exactly:

* **2DTest** — D3D12 ✅, Metal ✅, Vulkan ✅
* **GPUTessTest** — D3D12 ✅, Metal ✅, Vulkan ✅
* **MeshAndRaytracingTest** — D3D12 ✅ (currently the only backend; portable shared body is ready for Metal/Vk when they wire it)
* **BlitTest** — Vulkan ✅ (single-backend today; portable shared body)
* **CPUTessTest** — Vulkan ✅ (single-backend today; portable shared body)

Tests that **don't** migrate (already headless / CLI, no swap chain):

* `ComputeTest` on Metal+Vulkan (CLI; opens no window)
* `AssetTest`, `SamplerBindTest`, `MatrixOpsTest`, `BitfieldOpsTest`, `IntVectorIOTest`, `MeshShaderTest`, `PushConstantTest`, `sampler_validation_test`, `std140_layout_test` — already share one source under `gte/Tests/`, no GUI surface.

**Decision point — D3D12 ComputeTest:** the current D3D12 ComputeTest opens a hidden Win32 window (`directx/ComputeTest/main.cpp:30`) even though it doesn't render to a swap chain. This is almost certainly vestigial — the Metal and Vulkan ComputeTest siblings are headless CLI. Recommend collapsing the D3D12 case to headless too (use `GTE_TEST_ENTRY_POINT` only, drop the WinMain window), but flagging it under Open Decisions because you may know a reason that window has to exist.

## Phasing

### Phase 1 — Land the API and the Win32 backend ✅ Done

> Landed and visually confirmed: the DX 2DTest renders the textured rect through
> `RunGTETestWindow` (single 300×300 window — the old 500×500 parent + 300×300
> child shell collapsed; render content is identical). Resolved Open Decisions by
> recommendation: #1 single shared body (`gte/Tests/2DTest/main.cpp`), #4 teardown
> stays in the test body's `onClose`, #5 `Init`/`Close` stay in the test body. One
> backend-specific island remains in the shared body: the WIC/COM PNG upload is
> still `#ifdef TARGET_DIRECTX` (Metal/Vulkan render a flat rect today) — the
> portable image path is reconciled in Phases 2–3 (Open Decision #2). Assets
> (`test.png`, `shaders.omegasl`) still live under `directx/2DTest/`; they relocate
> to `assets/2DTest/` in Phase 4.

1. Write `gte/Tests/GTETestWindow.h` per the Design.
2. Write `gte/Tests/directx/GTETestWindow_Win32.cpp`.
3. Add a tiny `omegagte_testwindow` `OBJECT` library to each backend's `CMakeLists.txt` that wraps the impl file and re-exports `OmegaGTE`'s include dirs / link requirements. (Object library, not static, so we don't add another link target the test exes have to thread through.)
4. Update `add_d3d12_test` to inject `$<TARGET_OBJECTS:omegagte_testwindow>` into every windowed test.
5. Pick the smallest test (2DTest is fine) and rewrite **its DirectX build only** through the new API as a smoke test. Other backends still build from their current copies; nothing else moves yet.
6. Run the DX 2DTest, screenshot via user (per AGENTS.md Visual Debugging), confirm the rect renders identically to today.

### Phase 2 — Metal backend ✅ Done (pending Mac verification)

> Implemented (not yet built/screenshotted — this is a Windows host; Metal needs
> a macOS build to verify). What landed:
> - `gte/Tests/metal/GTETestWindow_Cocoa.mm` — Cocoa backend (NSWindow + NSView +
>   CAMetalLayer, NSApplication run loop). onReady fires from
>   `-applicationDidFinishLaunching`, onClose from `-applicationWillTerminate`.
>   MRR (no ARC), matching the rest of the Metal backend. Layer forced to 1:1
>   contentsScale at 300×300 so the drawable matches the shared body's 300×300
>   viewport (retina-crisp viewport tracking left as a follow-up).
> - **Open Decision #2 resolved → precompiled.** Metal 2DTest dropped the inline
>   runtime-string shader + `loadShaderLibraryRuntime`; both backends now
>   `loadShaderLibrary("./shaders.omegasllib")`.
> - **Texture convergence (your call): GETextureAsset for both backends.** The
>   shared body's WIC/COM `#ifdef TARGET_DIRECTX` island is gone — texture load is
>   now portable `GETextureAsset` (DirectXTex on D3D12, MTKTextureLoader on Metal).
>   cwd setup unified onto `OmegaCommon::FS::changeCWD(getExecutableDir())`. The
>   shared body is now fully platform-independent (no #ifdef islands).
> - **2DTest assets relocated to `gte/Tests/assets/2DTest/`** (test.png +
>   shaders.omegasl, deduped onto the DX-canonical shader) — pulled Phase 4's
>   relocation forward for this test because Phase 2 makes both backends consume
>   them. Both backends' `<TestName>/` now hold only OS template files.
> - **`add_omegasl_lib` gained an optional `INCLUDE_DIRS` keyword** (forwards
>   `-I <dir>` to omegaslc); backward compatible, used by both backends' 2DTest
>   shader compile. `add_metal_test` gained `ASSETS`/`SHADERS` (mirrors
>   `add_d3d12_test`) + a `omegagte_testwindow_metal` OBJECT lib injection;
>   assets/shader lib stage into the bundle's `Contents/MacOS`.
> - **DX re-verify needed:** the DX texture path changed (WIC → GETextureAsset),
>   so the DX 2DTest screenshot should be re-confirmed alongside the Metal one.
> - **Risk flagged:** the shared body is a `.cpp` (no `__OBJC__`), so its view of
>   `NativeRenderTargetDescriptor` omits the `metalLayer` field (which is
>   `#if defined(TARGET_METAL) && defined(__OBJC__)`). Safe because the body only
>   passes the descriptor by reference and never touches `metalLayer`; if the Mac
>   build complains, compile `2DTest/main.cpp` as OBJCXX on Metal.

1. Write `gte/Tests/metal/GTETestWindow_Cocoa.mm`.
2. Mirror Phase 1's CMake plumbing (`add_metal_test` injects the object lib).
3. Rewrite Metal 2DTest through the new API. The Metal sibling still uses runtime shader compilation today (`compile(... fromString(shaders))` at `metal/2DTest/main.mm:237`); the shared body must accept either a runtime source or a precompiled `.omegasllib` based on which is present next to the executable. (Cleanest: shared body checks for `./shaders.omegasllib`; if absent, falls back to an inline string. Or — preferred — converge all three on the precompiled path. Flagged under Open Decisions.)
4. Run, screenshot, confirm visual parity with today.

### Phase 3 — Vulkan/X11/Wayland backend ✅ Done (verified on native Wayland)

> Implemented, built, and visually confirmed on the native Linux/Wayland host
> (NVIDIA RTX 5080, Vulkan). The 2DTest window renders the shared body's green
> clear + the sampled `test.png` (pear) rect, upright — UV winding / Y-flip
> parity with DX/Metal holds. No Vulkan validation errors. What landed:
> - `gte/tests/vulkan/GTETestWindow_GTK.cpp` — GTK-4 bare-`GdkSurface` backend.
>   Carries the developer-directed bare-toplevel architecture from the old
>   `vulkan/2DTest/main.cpp` (no `GtkWindow` → no GSK renderer competing with the
>   Vulkan swap chain). `onReady` fires from the `layout` signal — the race-free
>   point the compositor first reports a real logical size, required because
>   Wayland WSI has no surface extent. `onClose` fires after the loop, strictly
>   before the `GdkSurface`/`wl_surface` is destroyed, so the test body tears
>   down the Vulkan swap chain while its backing surface is still alive. Runtime
>   X11/Wayland dispatch gated by the same `VULKAN_TARGET_*` macros as `GE.h`.
>   `onFrame` is intentionally NOT pumped (the bare toplevel has no repaint
>   clock we can hook without reintroducing GSK, and the swap-chain present
>   persists the one-shot frame; an animating test would drive it off the
>   surface's `GdkFrameClock`).
> - **CMake:** added the GTK-4-gated `omegagte_testwindow_vulkan` OBJECT lib and
>   extended `add_vulkan_test` with the object-lib injection + `ASSETS`/`SHADERS`
>   staging (mirrors `add_d3d12_test`). The windowed (`GTK4`) branch links GTK 4,
>   which resolves the object lib's GDK/GTK symbols; the test body pulls no GTK.
>   The non-`GTK4` branch (still-GTK3 tests, Phase 4) is unchanged.
> - **Vulkan 2DTest now builds from the shared `../2DTest/main.cpp`** with the
>   relocated `../assets/2DTest/` copies — precompiled `shaders.omegasllib` +
>   portable `GETextureAsset` (OmegaCommon::Img PNG decode on Vulkan). No
>   `#ifdef` islands; same code path as DX/Metal. The old
>   `vulkan/2DTest/main.cpp` (runtime-string shader, flat-red rect, no texture)
>   is deleted.
> - **Runtime parity fix:** at display scale 2 the descriptor first carried the
>   pixel extent (600×600), which mismatched the shared body's fixed 300×300
>   viewport (content landed in one quadrant). The Wayland descriptor now
>   carries the LOGICAL extent so swap chain == viewport == descriptor size —
>   the same 1:1 invariant Win32 (client rect) and Cocoa (`contentsScale = 1`)
>   hold. HiDPI-crisp rendering (pixel-sized swap chain + `wl_surface_set_-
>   buffer_scale` + pixel-sized viewport) is the same retina follow-up left open
>   on Metal; deferred by developer call.

1. Write `gte/Tests/vulkan/GTETestWindow_GTK.cpp`. Carry the GTK-realize → fill X11/Wayland descriptor path from `vulkan/2DTest/main.cpp:141`.
2. CMake: `add_vulkan_test` injects the object lib; the test exe no longer has to pull GTK directly — the object lib does.
3. Rewrite Vulkan 2DTest through the new API.
4. Run, screenshot, confirm.

### Phase 4 — Migrate the remaining GUI tests ✅ Done (GPUTessTest verified on native Metal; rest written pending Windows/Linux verification)

> Implemented on a native macOS host, which — for the first time in this
> plan's history — let Phase 2's Cocoa backend actually be built and run
> rather than only written. That surfaced two real, pre-existing bugs
> unrelated to this migration, both fixed here because they blocked
> verifying the migration itself:
>
> - **`GEMetalCommandQueue::commitToGPUAndWait()` was UB on an empty queue**
>   (`gte/src/metal/GEMetalCommandQueue.mm`). It unconditionally read
>   `commandBuffers.back()`; once a prior fire-and-forget `commitToGPU()` had
>   already cleared that vector (which it always does), `.back()` on an empty
>   vector read garbage and `__bridge`-cast it to `id<MTLCommandBuffer>` —
>   which crashed the *first* time it ever ran on real hardware
>   (`-[__NSCFNumber addCompletedHandler:]: unrecognized selector`), hit via
>   2DTest's `onClose` (`commandQueue->commitToGPUAndWait()` after an earlier
>   `onReady`-time `commitToGPU()`). Fixed by inserting a lightweight barrier
>   command buffer on the raw `MTLCommandQueue` and waiting on *that* when
>   `commandBuffers` is empty — Metal command buffers on one queue complete in
>   submission order, so the barrier completing proves everything submitted
>   before it has too.
> - **Metal's GPU tessellation of a rect lands in the wrong quadrant vs. the
>   CPU path** — surfaced by the new shared `GPUTessTest` actually running on
>   Metal for the first time (it used to be a headless CLI test with no run
>   loop at all; see below). Left unfixed — it's a bug in the triangulation
>   engine's Metal compute path, not in this window-migration plan — but
>   flagged as a separate follow-up rather than silently absorbed into a
>   passing test run.
>
> The migration itself: `GPUTessTest` now has one shared body
> (`gte/Tests/GPUTessTest/main.cpp`) across all three backends. It renders
> nothing — it only needs a `NativeRenderTargetDescriptor` to build a TE
> context from, so it runs its CPU-vs-GPU comparison synchronously in
> `onReady` and self-closes with the pass/fail exit code. That needed an API
> addition not anticipated by the original design: **`RequestGTETestWindow-
> Close(int exitCode)`**, added to `GTETestWindow.h` and implemented per
> backend:
> - **Win32**: `PostQuitMessage(exitCode)` — safe whether called before or
>   after the message loop starts, since it queues on the calling thread.
> - **Cocoa**: the hard one. `[NSApp terminate:]` (the old close path) calls
>   `exit()` from inside AppKit and never returns from `-run`, so
>   `RunGTETestWindow` could not hand back a caller-chosen exit code. Switched
>   to `[NSApp stop:]` + a posted dummy event (Apple's documented workaround
>   for `-stop:` needing another event to be noticed, which matters here
>   because a self-closing test calls it from inside
>   `applicationDidFinishLaunching:`), with `onClose` now firing in
>   `RunGTETestWindow` right after `-run` returns instead of from
>   `-applicationWillTerminate:`. Also added `-applicationShouldTerminate:`
>   (returns `NSTerminateCancel` and redirects through the same path) — AppKit
>   registers its own Apple-Event handler for "quit" (Dock menu, Cmd+Q, an
>   AppleScript `tell app to quit`) independent of any menu, and its default
>   path bypasses `onClose` entirely; verified via `osascript ... to quit`
>   against 2DTest (AppleScript reports a cosmetic "User canceled" for the
>   `NSTerminateCancel`, but the process exits cleanly with `onClose` having
>   run — confirmed from the log).
> - **GTK**: `g_main_loop_quit(g_loop)` — safe because `onReady` (`"layout"`)
>   already fires from inside `g_main_loop_run`'s own dispatch, so `g_loop` is
>   always valid and running by the time a test can call this.
>
> `MeshAndRaytracingTest` (D3D12-only), `BlitTest` and `CPUTessTest`
> (Vulkan-only) got the same shared-body treatment. `BlitTest` additionally
> resolves Open Decision #2 for itself: its inline runtime-compiled `R"(...)"`
> shader string is now the canonical precompiled `assets/BlitTest/shaders.
> omegasl`, loaded via `loadShaderLibrary` like every other migrated test.
> `meshAndRaytracing.omegasl` moved to `assets/MeshAndRaytracingTest/`
> alongside the FBX it already shared. Metal's GPUTessTest changed from a CLI
> test (a bare, windowless `CAMetalLayer` with no `NSApplication` run loop at
> all — never actually exercised the Cocoa windowing path) to a real
> `add_metal_test` bundle with a minimal `metal/GPUTessTest/Info.plist` (no
> nib — the window is built programmatically).
>
> **Per developer direction, the source-of-truth per-test file lists moved
> out of the per-backend CMakeLists.txt entirely**: `gte/Tests/CMakeLists.txt`
> now declares `GTE_TEST_<NAME>_{SOURCES,ASSETS,SHADERS}` once per test (for
> every migrated test, including 2DTest, applied retroactively for
> consistency).
>
> **Follow-up consolidation (developer direction): one global
> `add_gte_window_test`.** The three per-backend window-registration helpers —
> `add_d3d12_test`, `add_metal_test`, and the `GTK4` branch of
> `add_vulkan_test` — are gone. In their place, `gte/Tests/CMakeLists.txt`
> defines a single cross-platform `add_gte_window_test(NAME … SOURCES …
> [ASSETS …] [SHADERS …] [LIBS …] [BACKENDS …] [PLIST …] [RESOURCES …]
> [FRAMEWORKS …])` that branches on the active `TARGET_*` internally and folds
> in one `omegagte_testwindow` OBJECT library (its source is the active
> backend's `RunGTETestWindow` impl — `GTETestWindow_Win32.cpp` /
> `_Cocoa.mm` / `_GTK.cpp`). Every windowed test is now registered exactly
> ONCE, cross-platform, at the `gte/Tests` level — including the single-backend
> ones, which pass `BACKENDS d3d12` / `BACKENDS vulkan` to skip cleanly on
> backends they don't target (the `PLIST`/`RESOURCES` macOS-bundle items are
> passed unconditionally and ignored off Metal). Metal-bundle mechanics
> (framework resolution + embedding, rpaths, Info.plist) and the asset/shader
> staging (compile `.omegasl` → `.omegasllib`, copy next to the exe /
> `Contents/MacOS`) all live inside the one function. Because the registration
> moved up a directory, the `GTE_TEST_*` paths dropped their leading `../`
> (they're now relative to `gte/Tests`, not `gte/Tests/<backend>`).
>
> The per-backend `CMakeLists.txt` files now carry ONLY the non-windowed CLI /
> headless tests, via `add_<backend>_cli_test` helpers — these genuinely differ
> per backend (D3D12 WinMain subsystem + DLL staging, Metal framework rpaths,
> Vulkan plain/GTK links) and there is no window entry left in any of them.
> `add_metal_cli_test` already existed; `add_d3d12_test` → `add_d3d12_cli_test`
> (object-lib injection + asset/shader staging stripped, since CLI tests open
> no swap chain), and `add_vulkan_test` → `add_vulkan_cli_test` (its GTK-4
> window branch removed, leaving the plain-exe path). The single `omegasllib`
> object library replaces the three former `omegagte_testwindow_{d3d12,metal,
> vulkan}` libs. `_GTE_TESTWINDOW_OK` gates windowed registration so a host
> missing a hard dep (Vulkan without GTK 4) skips the windowed tests cleanly
> instead of erroring.
>
> **Verified on native macOS**: `GPUTessTest` builds and runs correctly —
> opens, computes the CPU/GPU comparison, self-closes via
> `RequestGTETestWindowClose`, tears down GPU resources in dependency order,
> and returns the right process exit code (repeatable across runs).
> `2DTest`'s user-close and Apple-Event-quit paths were re-verified against
> the Cocoa refactor and both now complete cleanly (previously crashed, see
> above). D3D12 (`MeshAndRaytracingTest`) needs a Windows host; Vulkan/GTK-4
> (`BlitTest`, `CPUTessTest`, and the Vulkan/DX sides of `GPUTessTest`) need a
> Linux host with GTK4 — both written to the same pattern as the verified
> Metal/Win32 code but unbuilt this session.

For each of `GPUTessTest`, `MeshAndRaytracingTest`, `BlitTest`, `CPUTessTest`:

1. Create the shared `gte/Tests/<TestName>/main.cpp` by merging the existing sibling(s), reusing the same `GTE_TEST_ENTRY_POINT` + `RunGTETestWindow` skeleton.
2. **Relocate runtime data into `gte/Tests/assets/<TestName>/`.** Images, meshes, and `shaders.omegasl` all move there. After this step there is exactly one copy of each asset on disk, regardless of how many backends consume it. Where the DX and Metal `shaders.omegasl` copies diverged (different inline shader strings between backends today), reconcile them onto one canonical source as part of the same step.
3. **Leave OS-specific template files in `directx|metal|vulkan/<TestName>/`** — Win32 manifests stay under `directx/`, Cocoa `Info.plist` / `.nib` / `lproj` stay under `metal/`. Nothing moves into `assets/` that the platform owns the schema for.
4. Update each backend CMakeLists to: source the shared `../<TestName>/main.cpp`; point its `ASSETS` / `RESOURCES` / `add_omegasl_lib` calls at the new `../assets/<TestName>/...` paths (the DX MeshAndRaytracingTest's `../assets/orange_tennis_racket/orange_tennis_racket.fbx` reference at `gte/Tests/directx/CMakeLists.txt:81` is the template); keep referencing the local `<TestName>/` subfolder only for the backend-specific template items called out in step 3.
5. Delete the sibling sources and any duplicate asset copies once parity is confirmed under screenshot.

Each test is its own commit; if one fails to reach parity, only that test rolls back.

### Phase 5 — Headless capture mode (deferred)

`GTETestWindowDescriptor::captureFramePath`, populated from `argv` (e.g. `--capture <path>`), drives the runtime to render one frame, call `renderTarget->captureToFile(...)`, post a quit, and exit 0. This lets CI run a windowed test in a single-frame mode for visual regression diffing without anyone watching the screen. Specced now so the API doesn't change later; **implementation lands as a separate plan** after Phases 1–4 are green.

### Phase 6 — Wayland + Android backends (when needed)

When a `VULKAN_TARGET_WAYLAND` or `VULKAN_TARGET_ANDROID` test build is wired, add the matching `GTETestWindow_Wayland.cpp` / `GTETestWindow_Android.cpp` next to the X11 one. The descriptor fields and the `#ifdef` gating already exist in `GE.h:524-532`; no other test touches anything but the implementation file.

## CMake delta — concrete edits

`gte/Tests/directx/CMakeLists.txt`:

```cmake
add_library(omegagte_testwindow_d3d12 OBJECT GTETestWindow_Win32.cpp)
target_include_directories(omegagte_testwindow_d3d12 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../common/include")
target_compile_definitions(omegagte_testwindow_d3d12 PRIVATE ${PUBLIC_DEFS})

function(add_d3d12_test)
    cmake_parse_arguments(...)
    add_executable(${_ARG_NAME} WIN32
        ${_ARG_SOURCES}
        $<TARGET_OBJECTS:omegagte_testwindow_d3d12>)
    target_link_libraries(${_ARG_NAME} PRIVATE "OmegaGTE" Pathcch ${_ARG_LIBS})
    # ... rest unchanged ...
endfunction()
```

`gte/Tests/metal/CMakeLists.txt`: same shape, ObjC-compiled object lib (`GTETestWindow_Cocoa.mm`) linked into both `add_metal_test` and (only if the test opens a window) `add_metal_cli_test`. Cocoa / QuartzCore / Metal frameworks move from per-test specifications into the object lib's `target_link_libraries` so the per-test list shrinks.

`gte/Tests/vulkan/CMakeLists.txt`: object lib `GTETestWindow_GTK.cpp` carries the GTK include dirs / link dirs / `gtk+-3.0` libraries; `add_vulkan_test` injects it and the per-test target stops pulling GTK directly.

## Open Decisions

These are calls only you can make — surfacing them now rather than committing them silently:

1. **Single shared `main.cpp` per test, or three backend copies that share only `GTETestWindow`?** I recommend single-shared (kills the drift problem at its root). The alternative — keep three sibling copies, just have them all call `RunGTETestWindow` — wastes 80% of the value of this work, but you may want it if you have downstream reasons for per-backend test bodies (e.g. WSL/Windows handoff per AGENTS.md building rules, where a Mac-side agent can't compile the DX body anyway).
2. **Inline-string OmegaSL vs precompiled `.omegasllib` in the shared body.** Today: DX is precompiled, Metal+Vulkan are inline strings. The shared body wants one path. Recommend converging on precompiled (matches the production usage pattern; CMake already wires `add_omegasl_lib` on every backend). The downside: Metal's CLI tests currently use inline compilation as a smoke test of `omegaSlCompiler` — that test value would have to live in a dedicated CLI test, not in the windowed test.
3. **D3D12 ComputeTest's hidden window** — keep or drop? Recommendation in Scope above is to drop, but flagging because I can't see why it was added.
4. **Resource teardown ownership.** Recommend keeping it in the test body's `onClose` (because every test has its own statics and only the test knows their dependency order). The alternative — a generic teardown helper inside `GTETestWindow` — looks tempting but won't generalize: the D3D12 2DTest's COM-vs-Close ordering (`directx/2DTest/main.cpp:336-351`) is genuinely test-specific.
5. **Should `RunGTETestWindow` own `OmegaGTE::InitWithDefaultDevice` / `OmegaGTE::Close`?** Currently the design has the test body own these around the `RunGTETestWindow` call. Pulling them inside the runtime would shrink every test by ~6 lines, but couples the test window helper to the device lifecycle in a way that hurts the multi-device tests later (e.g. if a future test wants two devices side-by-side). Recommend leaving them in the test body.

## Verification

* Per-phase visual check via the screenshot workflow in AGENTS.md (user-captured until `omega-debugviz` is signed off — see CLAUDE.md / AGENTS.md `Visual Debugging` note that the tool is not currently trusted for verification).
* After each migrated test: diff the screenshot against the pre-migration baseline. Pixel-identical is the goal; near-identical with a documented reason (e.g. converging Metal+Vulkan onto the textured DX path adds a sampled image where there used to be a flat color) is acceptable if you sign off.
* `ctest` exit codes remain green on every backend (the CLI tests we don't touch should be unaffected — they don't depend on `GTETestWindow.h`).
* Windows build is driven by the user per AGENTS.md "Building" — do not assume a green DX build until they paste the toolchain output.

## Out of scope

* WTK reuse. WTK depends on GTE; the GTE test suite must not reverse that layering. `GTETestWindow` is a purpose-built test helper, not a slimmed-down WTK.
* General-purpose application framework. The API exposes exactly what a render-once-and-present GTE test needs. Anything more (multi-window, custom event routing, input handling beyond close) is deliberately omitted; if a test wants that, it falls back to writing the toolkit code directly.
* Replacing `GTETestEntryPoint.h`. The CLI tests already use it; this plan extends the GUI tests onto the same macro, not the other way around.
