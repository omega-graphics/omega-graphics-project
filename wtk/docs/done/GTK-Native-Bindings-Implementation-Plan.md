# GTK Native Bindings Implementation Plan

## Goals
1. Align Linux startup and lifecycle behavior with existing macOS and Windows target launch flow.
2. Establish a stable GTK application binding that can run the OmegaWTK event loop correctly.
3. Define and stage the remaining work for full GTK native window/menu/item parity with Cocoa.

## Baseline (Current State)
1. macOS startup is complete and thin in `wtk/target/macos/main.mm`:
   - creates `OmegaWTK::AppInst`
   - calls `omegaWTKMain`
   - deletes app instance
2. Linux startup is currently stubbed in `wtk/target/gtk/main.cpp`:
   - no `AppInst` creation
   - no `omegaWTKMain` handoff
3. GTK native app binding exists in `wtk/src/Native/gtk/GTKApp.cpp` but is incomplete:
   - `terminate()` unrefs application instead of quitting application loop
   - `runEventLoop()` ignores launch argc/argv
4. Linux app target wiring is incomplete in `wtk/cmake/OmegaWTKApp.cmake`:
   - no automatic inclusion of `wtk/target/gtk/main.cpp`
   - Linux executable target is not explicitly linked to `OmegaWTK`
5. Full GTK native window binding (`make_native_window`) is not implemented yet.

## Target Architecture (Linux)
1. Launcher parity:
   - `wtk/target/gtk/main.cpp` mirrors macOS/Windows pattern.
2. Launch args contract:
   - pass argc/argv through `AppInst(void *data)` into GTK native app binding.
3. App lifecycle:
   - `runEventLoop()` uses `g_application_run(...)`
   - `terminate()` uses `g_application_quit(...)`
   - native `GtkApplication` cleanup in destructor
4. Build wiring:
   - Linux app targets automatically receive `target/gtk/main.cpp`
   - Linux app targets link against `OmegaWTK`

## Implementation Phases

### Phase 1 (Simple Implementation - This change set)
1. Add a cross-platform launch-args struct in `NativeApp.h`.
2. Replace GTK target stub main with a real launcher.
3. Update GTK native app binding to:
   - parse launch args
   - run with argc/argv
   - quit application cleanly
4. Update `OmegaWTKApp.cmake` Linux branch to:
   - add `target/gtk/main.cpp`
   - link `OmegaWTK`
   - define Linux compile definitions for target app (`TARGET_GTK`, `TARGET_VULKAN`)

### Phase 2 (Native Window Binding)
1. Add `GTKAppWindow` implementing `NativeWindow`.
2. Implement `make_native_window(...)` for Linux.
3. Map GTK signals to OmegaWTK native window events:
   - close
   - resize/live resize complete
4. Root view attachment model:
   - create root native item/widget container
   - implement `addNativeItem` and initial display behavior

### Phase 3 (Menu and Item Completion)
1. Fix GTK item visibility semantics (`enable/disable` currently inverted).
2. Complete scroll bar toggle APIs in GTK item implementation.
3. Harden GTK menu behavior:
   - guard submenu assignment
   - ensure delegate callbacks and ownership are stable

### Phase 4 (Backend Integration and Validation)
1. Add/complete Vulkan backend visual tree creation path for Linux.
2. Validate Wayland and X11 descriptor plumbing end-to-end.
3. Execute cross-platform smoke tests:
   - window create/show/close
   - resize event propagation
   - basic render submission

## Risks and Constraints
1. GTK native window and compositor backend are coupled; Phase 1 intentionally limits scope to startup/lifecycle plumbing.
2. Linux runtime backend selection (Wayland vs X11) must match OmegaGTE Vulkan surface support.
3. Without Phase 2 and Phase 4, Linux may compile but not be functionally complete for full window/render behavior.

## Acceptance Criteria (Phase 1)
1. Linux launcher creates `AppInst`, calls `omegaWTKMain`, and returns its status.
2. GTK native app event loop runs through `g_application_run`.
3. `AppInst::terminate()` path exits GTK application loop via `g_application_quit`.
4. Linux app targets created by `OmegaWTKApp(...)` include GTK launcher and link against `OmegaWTK`.
