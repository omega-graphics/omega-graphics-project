# Android Native Host Integration Plan

Status: **Plan** — not implemented. KREATE on Android currently ships a
compile-only stub at `kreate/src/platform/android/AndroidWindow.cpp` and
`kreate/target/android/main.cpp` so the cross-compile toolchain pipeline can
be validated end-to-end. This document describes how to replace the stub
with a real on-device host integration that drives `App::onFrame()` from
the platform's event loop.

The structure here mirrors the iOS host integration that already shipped
in `kreate/src/platform/ios/UIKitWindow.mm` and `kreate/target/ios/main.mm`.
Read those first if you want a concrete reference point — the shape of
the solution is similar even though the OS API surface differs.

## Background and constraints

KREATE's public API on desktop is:

```cpp
auto app = Kreate::CreateApp();
app->run(); // owns the loop: pollEvents + onFrame until shouldClose()
```

Android does not let an app own its loop. The system delivers events
(window created, resize, input, lifecycle transitions) through callbacks
on a `NativeActivity`, and the app's "main" is conceptually a state
machine that drains a queue. A blocking `app->run()` call is structurally
incompatible.

The chosen iOS solution was: don't change `App`'s public API; on iOS,
`target/ios/main.mm` *is* the entry point, it never calls `app->run()`,
and a `UIApplicationDelegate` drives `onFrame()` from a `CADisplayLink`.
The `Window` adopts the system-provided view rather than creating one.

We apply the same shape on Android:
- The Android target's entry point is `android_main`, not the user's
  `int main(...)`.
- A small event loop inside `android_main` drains the `ALooper` and
  invokes `onInit()` / `onFrame()` at the right moments.
- `Window::create` on Android adopts the `ANativeWindow*` provided by the
  system instead of creating one. `desc.title` is dropped (no titles on
  Android), `desc.width`/`desc.height` are advisory at best (the system
  decides the surface size).

## Constraints worth calling out before code lands

1. **Surface lifetime is not app lifetime.** On Android, the
   `ANativeWindow*` is created when the activity becomes visible
   (`APP_CMD_INIT_WINDOW`) and destroyed when it goes away
   (`APP_CMD_TERM_WINDOW`). This can happen multiple times during a
   single app session — orientation changes, foreground/background, etc.
   The Vulkan swapchain (and the GTE `GENativeRenderTarget` bound to it)
   must be torn down on `APP_CMD_TERM_WINDOW` and rebuilt on
   `APP_CMD_INIT_WINDOW`. **This is the most failure-prone part of the
   integration.** Get this wrong and the app crashes on rotate.
2. **`onFrame()` runs on the ALooper main thread.** No threading needed
   for the basic loop — the same model the iOS `CADisplayLink` uses.
3. **No automatic v-sync hook.** Android will not throttle our loop for
   us the way `CADisplayLink` does. We rely on Vulkan's swapchain
   present mode (`VK_PRESENT_MODE_FIFO_KHR`) to pace frames at refresh.
   `Choreographer` (the Android equivalent of `CADisplayLink`) is
   available but harder to wire from native code; treat it as
   follow-on work if we need decoupled simulation/render rates.
4. **Library, not executable.** Android apps are APKs containing one or
   more `.so` files plus a Java/Kotlin shell. The KREATE build already
   produces `lib<NAME>.so` for the Android target; `android_main` is the
   entry the runtime calls into via `android_native_app_glue`.
5. **Shaders are precompiled host-side.** `omegaslc` is a host-only
   tool (per the host-tools superbuild added in
   `cmake/OmegaGraphicsSuite.cmake`). Android cannot compile shaders on
   device; the `lib<NAME>.so` ships with `.spv` blobs as embedded assets
   or files alongside the APK.

## Architecture

```
+---------------------------------------------------------------+
| APK shell (auto-generated AndroidManifest.xml currently        |
| points android.app.NativeActivity at lib<APPNAME>.so)          |
+---------------------------------------------------------------+
                            |
                            v
+---------------------------------------------------------------+
|  android_main(android_app *)   <-- target/android/main.cpp     |
|  - sets app->onAppCmd / onInputEvent                          |
|  - polls ALooper; drives the App lifecycle                    |
+---------------------------------------------------------------+
       |                          |                       |
       v                          v                       v
  Kreate::CreateApp()       AppCmd::INIT_WINDOW        AppCmd::TERM_WINDOW
       |                  -> Window::create           -> destroy native RT
       |                  -> rebuild GTE native RT
       v
  Kreate::App ctor (+ onInit() once the surface exists)
       |
       v
  per-loop iteration:
    ALooper_pollAll(timeout)
    if (animating) onFrame()
```

The `android_native_app_glue` static library (shipped in the NDK at
`<NDK>/sources/android/native_app_glue/`) provides the canonical event
pump. We compile its single `.c` file into the Android target binary —
no extra system dependency.

## Phased implementation

### Phase 1 — Build wiring

1. In `kreate/CMakeLists.txt` under the Android branch, add the
   `android_native_app_glue.c` source from the NDK to KREATE's compile.
   The path is `${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/`.
2. Add `target_include_directories(... ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)`
   so consumers see `<android_native_app_glue.h>`.
3. In `kreate/cmake/KreateGame.cmake` Android branch, ensure the produced
   `.so` exports `ANativeActivity_onCreate` (it's the entry the manifest
   binds to). `android_native_app_glue` defines this for us — just keep
   the linker from stripping it: add
   `target_link_options(${_NAME} PRIVATE -u ANativeActivity_onCreate)`.
4. Move the existing stubbed `target/android/main.cpp` to
   `target/android/main.cpp.bak` or just rewrite it (Phase 2).

### Phase 2 — Real `android_main` and lifecycle

Rewrite `target/android/main.cpp`:

```cpp
#include <android_native_app_glue.h>
#include <kreate/App.h>

#include <memory>

namespace {

struct EngineState {
    std::unique_ptr<Kreate::App> app;
    bool animating = false;
    bool initialized = false;
    android_app *androidApp = nullptr;
};

void HandleCmd(android_app *app, int32_t cmd) {
    auto *state = static_cast<EngineState *>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            // Surface exists. Construct the KREATE App now (its Window
            // will adopt app->window) and run onInit() exactly once.
            // On subsequent INIT_WINDOW (after a TERM_WINDOW), only
            // rebuild the GTE native render target — keep the App.
            // ...
            state->animating = true;
            break;
        case APP_CMD_TERM_WINDOW:
            // Tear down the GTE native render target. Keep the App so
            // game state survives a transient surface loss.
            state->animating = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            state->animating = true;
            break;
        case APP_CMD_LOST_FOCUS:
            state->animating = false;
            break;
        case APP_CMD_DESTROY:
            state->app.reset();
            break;
    }
}

int32_t HandleInput(android_app *app, AInputEvent *event) {
    // Phase 4 — input dispatch. For now return 0 (not handled).
    (void)app; (void)event;
    return 0;
}

} // namespace

extern "C" void android_main(android_app *app) {
    EngineState state{};
    state.androidApp = app;
    app->userData = &state;
    app->onAppCmd = HandleCmd;
    app->onInputEvent = HandleInput;

    while (!app->destroyRequested) {
        // Block waiting for events when not animating; poll non-blocking
        // when we have frames to draw.
        int events;
        android_poll_source *source;
        int timeout = state.animating ? 0 : -1;
        while (ALooper_pollAll(timeout, nullptr, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) return;
        }

        if (state.animating && state.app) {
            state.app->onFrame();
        }
    }
}
```

This is the canonical NDK sample skeleton; nothing exotic. The two
non-trivial pieces are:
- **App construction timing.** Don't call `Kreate::CreateApp()` until the
  first `APP_CMD_INIT_WINDOW`. The KREATE `App` ctor calls
  `Window::create(desc.window)` which depends on `app->window` being
  non-null.
- **Surface rebuild on INIT_WINDOW after TERM_WINDOW.** Either re-call
  `gte.graphicsEngine->makeNativeRenderTarget(...)` and reassign
  `App::renderTarget()`, or expose a small `App::rebuildRenderTarget()`
  hook for embedders to call. The latter is cleaner — propose adding
  that to the public API in this phase.

### Phase 3 — `Window` adopts `ANativeWindow*`

Rewrite `kreate/src/platform/android/AndroidWindow.cpp`:

```cpp
struct Window::Impl {
    ANativeWindow *native = nullptr;
    unsigned w = 0;
    unsigned h = 0;
};

std::unique_ptr<Window> Window::create(const WindowDesc &desc) {
    (void)desc.title;
    // The android_main loop has parked the active ANativeWindow* in a
    // process-global the embedder sets just before constructing the App.
    // (Static seam — not pretty, but matches how iOS pulls keyWindow.)
    ANativeWindow *native = KreateAndroid::GetActiveNativeWindow();
    if (!native) return nullptr;

    auto window = std::unique_ptr<Window>(new Window());
    window->impl->native = native;
    window->impl->w = ANativeWindow_getWidth(native);
    window->impl->h = ANativeWindow_getHeight(native);
    return window;
}

void Window::fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const {
#if defined(VULKAN_TARGET_ANDROID)
    desc.window = impl->native;
#endif
}

bool Window::shouldClose() const { return false; } // owned by android_main
void Window::pollEvents() {}                       // owned by android_main
```

The `KreateAndroid::GetActiveNativeWindow()` accessor is declared in a small
internal header (`kreate/src/platform/android/AndroidNativeWindowAccess.h`)
and set from `target/android/main.cpp` on `APP_CMD_INIT_WINDOW`. This
keeps the cross-platform `Window::create()` signature unchanged.

### Phase 4 — Input

`HandleInput` translates `AInputEvent` → KREATE's input model. KREATE does
not yet have a public input model; we'll need to design one (touch
events, key events, motion events). Defer this to a follow-up so it's
not blocking the rendering bring-up.

### Phase 5 — APK packaging

The current `kreate/cmake/KreateGame.cmake` Android branch produces
`lib<NAME>.so` and a configured `AndroidManifest.xml`. To actually run
on a device we also need:

- A minimal Gradle/AGP project that wraps the `.so` into an APK. Most
  practical option: ship a `kreate/target/android/gradle-template/` tree
  containing `build.gradle.kts` and a stub Java source, and have a CMake
  `add_custom_target` (or a small Python script) instantiate it per game.
- Sign the APK (debug keystore is fine for "test it" scope).
- `adb install` integration as a CMake target (`make install-<NAME>`).

This whole phase is its own sub-plan; a thin first cut is sufficient to
unblock device testing.

## API surface impact

Single proposed addition to `kreate/include/kreate/App.h`:

```cpp
class KREATE_EXPORT App {
public:
    // ... existing ...

    /// Rebuilds the GTE native render target against the current
    /// platform surface. Called by the platform integration when the
    /// underlying surface is destroyed and recreated (Android
    /// orientation changes, surface loss). No-op when the surface has
    /// not changed. Default implementation in App.cpp; platform
    /// integrations (target/android/main.cpp) call it on
    /// APP_CMD_INIT_WINDOW after the first one.
    void rebuildRenderTarget();
};
```

This is also useful for iOS down the line if we add scene reconfiguration
support, so the API isn't Android-specific.

## Testing plan

1. **Build only**: `cmake --build` an APK-less variant produces
   `lib<NAME>.so`. Validates Phase 1 + 2 compile and link on the NDK.
2. **APK install**: a minimal Gradle wrapper from Phase 5 produces an
   APK, `adb install`, manually launch.
3. **Lifecycle stress**: rotate the device; background and foreground;
   confirm no crash and rendering resumes. This is where Phase 2's
   render target rebuild gets exercised.
4. **`BasicGame` on Android**: once Phase 5 lands, drop the
   `if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")` guard in
   `kreate/CMakeLists.txt` so `BasicGame` builds and runs on Android too.

## Open questions

1. **`Choreographer` vs. plain `ALooper` loop?** Plain loop is simpler
   and matches the NDK sample. Choreographer gives v-sync-aligned
   `onFrame()` callbacks decoupled from event polling. Pick plain for
   bring-up; revisit if frame pacing is visibly bad.
2. **Where do `.spv` shader assets live?** APK `assets/` directory,
   accessed via `AAssetManager`. Plumbing this through GTE's pipeline
   loader is its own discussion.
3. **Do we want a Kotlin/Java shell, or pure NativeActivity?**
   Pure NativeActivity is friction-free but locks out platform features
   that need JNI (notifications, Play Services, etc.). For an engine,
   pure NativeActivity is the right starting point.
4. **Multi-window and split-screen?** Out of scope for the bring-up.
5. **`android_native_app_glue` is C** — fine to compile into KREATE, but
   if KREATE grows another C compile-unit we should keep this in mind for
   the `target_compile_definitions` story.

## Non-goals for this plan

- Editor / tooling on Android.
- Audio, networking, sensors.
- Custom Activity subclasses (we use the stock NativeActivity).
- Asset pipeline beyond loading shader blobs.

When this is implemented, file as `Android-Host-Integration.md` in
`kreate/docs/done/` (mirroring the existing pattern in this directory) and
update `kreate/CMakeLists.txt` to drop the Android-specific BasicGame skip.
