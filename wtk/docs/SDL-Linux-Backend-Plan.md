# SDL-based Linux Backend Plan

This document describes the plan to retire the GTK backend on Linux and
replace it with an SDL-based backend that owns the toplevel window
directly. Background and decision context live in
`Native-API-Completion-Proposal.md` §3.

## 1. Motivation

The current GTK backend cannot present Vulkan-rendered content to the
screen. GTK does not allow direct Vulkan rendering into a child
`GdkWindow` — its GSK/Cairo render cycle owns the X11 windows GTK
created and overdraws/clips them on its own schedule. The two officially
supported integration paths are:

1. Render Vulkan into an offscreen image and upload it to a `GdkTexture`
   each frame (per-frame GPU↔CPU readback).
2. `GdkVulkanContext` (GTK 4 only — the codebase builds against GTK 3).

Rather than ship the offscreen-readback adapter as a long-term solution,
we own the Linux toplevel ourselves. SDL provides the platform window
abstraction and Vulkan surface creation. The result is symmetric across
platforms:

| Platform | Toplevel | Vulkan surface |
|----------|----------|----------------|
| macOS    | `NSWindow` + `CAMetalLayer` | `vkCreateMetalSurfaceEXT` |
| Win32    | `HWND` | `vkCreateWin32SurfaceKHR` |
| Linux    | `SDL_Window` | `SDL_Vulkan_CreateSurface` |

## 2. Scope

**In scope:**
- New Linux backend in `wtk/src/Native/sdl/`, mirroring the existing
  `macos/` and `win/` directory layout.
- GTE Vulkan: optional SDL-based surface creation path in
  `gte/src/vulkan/` for `GENativeRenderTarget` on Linux.
- GTE Vulkan: SDL-supplied instance extensions surfaced into
  `GEVulkanEngine`'s instance creation.
- CMake: `find_package(SDL3)` with SDL2 fallback; `TARGET_SDL` flag
  replaces `TARGET_GTK` on Linux.

**Out of scope:**
- macOS: continues to use `NSWindow` + `CAMetalLayer` directly. SDL is
  not introduced on macOS.
- Win32: continues to use `HWND` + Vulkan swapchain on `HWND`. SDL is
  not introduced on Win32.
- GTK code: retired wholesale at Phase L7 once the SDL backend reaches
  feature parity.
- Mobile (`TARGET_MOBILE`): unaffected.

## 3. Why SDL specifically

- **Mature Vulkan integration.** `SDL_Vulkan_CreateSurface` returns a
  `VkSurfaceKHR` from any backing platform. `SDL_Vulkan_GetInstanceExtensions`
  returns the platform-required instance extensions that
  `vkCreateInstance` needs.
- **Single library covers X11 + Wayland** (and BSDs). One dependency,
  one event loop.
- **Proven plumbing** for input, clipboard, drag-and-drop, IME, gamepads,
  multi-monitor, fractional scale. We pick what we need.
- **No UI toolkit baggage.** SDL doesn't render widgets — we own all the
  rendering through OmegaGTE.
- **Packaged on every distro.** `libsdl3-dev` / `libsdl2-dev` available in
  Debian, Ubuntu, Fedora, Arch.

## 4. Architecture

### 4.1 GTE side

`OmegaGTE::NativeRenderTargetDescriptor` (in `gte/include/omegaGTE/GE.h`)
gains a new field:

```cpp
struct NativeRenderTargetDescriptor {
    // ...existing fields (wl_surface, wl_display, x_window, x_display, hwnd, ...)...

    // SDL Vulkan surface creation. When set, GEVulkanEngine prefers the
    // SDL path over xlib/wayland direct surface creation. The SDL_Window
    // must be created with SDL_WINDOW_VULKAN.
    void *sdl_window = nullptr;
};
```

In `GEVulkanEngine::makeNativeRenderTarget` (`gte/src/vulkan/GEVulkan.cpp`):

```cpp
if(desc.sdl_window != nullptr){
    SDL_Window *window = static_cast<SDL_Window *>(desc.sdl_window);
    if(SDL_Vulkan_CreateSurface(window, instance, /*allocator*/nullptr, &surfaceKhr) != 0){
        std::cerr << "SDL_Vulkan_CreateSurface failed: " << SDL_GetError() << std::endl;
        surfaceKhr = VK_NULL_HANDLE;
    }
}
// Fall through to existing xlib / wayland / win32 paths if sdl_window is unset.
```

The existing X11 / Wayland paths stay in place; users not using SDL can
still drive them directly. WTK on Linux always uses the SDL path.

`GEVulkanEngine` instance creation (in `GEVulkanEngine::initialize`)
must merge the SDL-required instance extension list before calling
`vkCreateInstance`. SDL3 returns this via
`SDL_Vulkan_GetInstanceExtensions(&count)` (a `const char *const *`).
The merge is union-by-name; duplicates are dropped.

A small SDL bootstrap step — `SDL_Init(SDL_INIT_VIDEO)` and
`SDL_Vulkan_LoadLibrary(nullptr)` — must happen before instance
creation. The `SDLApp` ctor handles this; GTE Vulkan init can assume it
has run.

### 4.2 WTK side

New directory `wtk/src/Native/sdl/`, mirroring `macos/` / `win/`:

```
wtk/src/Native/sdl/
├── SDLApp.cpp           # NativeApp impl: SDL_Init, event loop, dispatch
├── SDLApp.h
├── SDLAppWindow.cpp     # NativeWindow impl: SDL_Window + Vulkan surface
├── SDLAppWindow.h
├── SDLItem.cpp          # NativeItem impl: minimal — single root item
├── SDLEvent.cpp         # SDL_Event → NativeEvent translation
└── SDLDialog.cpp        # NativeDialog impl via xdg-desktop-portal (L4)
```

Key correspondences with the existing GTK backend:

| GTK file | SDL replacement | Notes |
|----------|-----------------|-------|
| `GTKApp.cpp` | `SDLApp.cpp` | Owns `SDL_Init`, event pump |
| `GTKAppWindow.cpp` | `SDLAppWindow.cpp` | SDL_Window-backed |
| `GTKItem.cpp` | `SDLItem.cpp` | One NativeItem per window (the root); virtual views live in WTK UI layer |
| `GTKMenu.cpp` | (deleted) | Virtualized — see Phase L3 |
| `GTKNote.cpp` | `SDLNotification.cpp` (L4) | DBus `org.freedesktop.Notifications` (no SDL involvement) |

`SDLAppWindow` constructs the `SDL_Window` with `SDL_WINDOW_VULKAN`
(plus `SDL_WINDOW_RESIZABLE`, `SDL_WINDOW_HIDDEN` until `enable()`,
`SDL_WINDOW_HIGH_PIXEL_DENSITY` for HiDPI on macOS-style displays).
The Vulkan surface is created via `SDL_Vulkan_CreateSurface` and handed
to GTE through `NativeRenderTargetDescriptor::sdl_window`.

The `NativeApp` event loop pumps SDL events and dispatches them through
`NativeEventEmitter` to `WidgetTreeHost` for hit testing — same pipeline
the macOS / Win32 backends already use.

### 4.3 Event translation (SDLEvent.cpp)

| SDL event | NativeEvent type |
|-----------|------------------|
| `SDL_EVENT_QUIT` | `WindowWillClose` |
| `SDL_EVENT_WINDOW_RESIZED` | `WindowWillResize` |
| `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` | `WindowWillResize` (for HiDPI) |
| `SDL_EVENT_WINDOW_DISPLAY_CHANGED` | `WindowScaleFactorChanged` (when scale differs between displays) |
| `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` | `WindowScaleFactorChanged` |
| `SDL_EVENT_MOUSE_MOTION` | `CursorMove` |
| `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` (left/right) | `LMouseDown`/`LMouseUp`, `RMouseDown`/`RMouseUp` |
| `SDL_EVENT_MOUSE_WHEEL` | `ScrollWheel` |
| `SDL_EVENT_KEY_DOWN`/`SDL_EVENT_KEY_UP` | `KeyDown`/`KeyUp` (with SDL keycode → `KeyCode` mapping) |
| `SDL_EVENT_WINDOW_FOCUS_GAINED`/`LOST` | `AppActivate`/`AppDeactivate` |
| `SDL_EVENT_WINDOW_MOUSE_ENTER`/`LEAVE` | `CursorEnter`/`CursorExit` |

`SDL_EVENT_WINDOW_RESIZE` in the live phase: SDL fires it during the
drag (similar to AppKit's live resize). The existing
`AppWindowDelegate::windowWillStartResize` / `windowHasFinishedResize`
pipeline (§2.1) absorbs the begin/middle/end pattern naturally — emit
`WindowWillStartResize` on the first event of a drag, `WindowWillResize`
on each subsequent, and `WindowHasFinishedResize` after a debounce
timeout (similar to GTK's existing 120ms debounce in `GTKAppWindow.cpp`).

### 4.4 Build system

- `CMakeLists.txt` (Linux branch):
  - `find_package(SDL3 QUIET)` → if found, define `TARGET_SDL` and link
    `SDL3::SDL3`.
  - else `find_package(SDL2 REQUIRED)` → define `TARGET_SDL`,
    `SDL_USE_SDL2` and link `SDL2::SDL2`.
  - SDL2 fallback uses a thin shim in `sdl/sdl_compat.h` mapping SDL3
    function/event names to SDL2 equivalents (mostly mechanical).
- `TARGET_GTK` flag dropped at Phase L7. Until then, the build uses
  whichever flag is set; CI exercises `TARGET_SDL` once L1 lands.

## 5. Phasing

### Phase L1 — Minimal "hello window" (X11 only)

**Goal:** prove the SDL+GTE+WTK pipeline end to end. Render content
visibly on X11.

- `SDLApp` boots, calls `SDL_Init(SDL_INIT_VIDEO)` and
  `SDL_Vulkan_LoadLibrary(nullptr)`, runs an event-pump loop.
- `SDLAppWindow` creates an `SDL_Window` with `SDL_WINDOW_VULKAN`,
  hands its pointer to GTE via `NativeRenderTargetDescriptor::sdl_window`.
- GTE `makeNativeRenderTarget` calls `SDL_Vulkan_CreateSurface` and
  builds the swapchain.
- Basic event translation: window resize, close, mouse move/button,
  keyboard down/up.
- `SVGViewRenderTest` renders visibly.

**Exit criteria:** `SVGViewRenderTest` shows the SVG on screen on X11
under the SDL backend; no VVL errors; clean shutdown.

**Files added:**
- `wtk/src/Native/sdl/SDLApp.{cpp,h}`
- `wtk/src/Native/sdl/SDLAppWindow.{cpp,h}`
- `wtk/src/Native/sdl/SDLItem.{cpp,h}`
- `wtk/src/Native/sdl/SDLEvent.cpp`
- `gte/include/omegaGTE/GE.h` — `sdl_window` field on
  `NativeRenderTargetDescriptor`
- `gte/src/vulkan/GEVulkan.cpp` — SDL surface creation branch in
  `makeNativeRenderTarget`
- `gte/src/vulkan/GEVulkan.cpp` — merge SDL-required instance extensions

### Phase L2 — §2.2 NativeWindow parity

Reach feature parity with the macOS / Win32 backends on the §2.2
window-control surface:

- `minimize`/`maximize`/`restore`/`toggleFullscreen` →
  `SDL_MinimizeWindow`/`SDL_MaximizeWindow`/`SDL_RestoreWindow`/
  `SDL_SetWindowFullscreen`.
- `isMinimized`/`isMaximized`/`isFullscreen`/`isVisible` →
  `SDL_GetWindowFlags`.
- `getRect`/`setRect` → `SDL_GetWindowPosition`/`Size`,
  `SDL_SetWindowPosition`/`Size`.
- `scaleFactor` → `SDL_GetWindowDisplayScale` (SDL3) /
  `SDL_GetWindowPixelDensity`.
- `setMinSize`/`setMaxSize` → `SDL_SetWindowMinimumSize`/`MaximumSize`.
- `setResizable` → `SDL_SetWindowResizable`.
- `orderFront`/`orderBack` → `SDL_RaiseWindow` / `SDL_HideWindow`+show.
- `setOpacity`/`getOpacity` → `SDL_SetWindowOpacity`/`Get`.
- `setCursorShape` → `SDL_CreateSystemCursor` + `SDL_SetCursor`,
  with the `CursorShape` enum mapped to SDL's system cursor IDs.
- `isKeyWindow`/`becomeKeyWindow` → `SDL_GetKeyboardFocus()` /
  `SDL_RaiseWindow`.
- `WindowScaleFactorChanged` → emitted on
  `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` /
  `SDL_EVENT_WINDOW_DISPLAY_CHANGED` (when scale differs).

`NativeTimer` (§2.4) lands in the same phase via `SDL_AddTimer`.

### Phase L3 — Virtualized menus

Move menu rendering from `GTKMenu` (real GTK widgets) to the
`WidgetTreeHost` virtual layer:

- Menu bar renders as a virtual widget at the top of the AppWindow's
  content area.
- Context menus render as a `WidgetTreeHost`-managed popup widget tree,
  positioned at the cursor.
- The Win32 backend in `WinMenu.cpp` continues to use `HMENU` (a
  separate cross-platform decision; this phase only changes Linux).
- Menu shortcuts are picked up through the normal keyboard-event path
  (the focus manager from §2.3a).

DBus AppMenu publication for GNOME/Plasma global-menu integration is
deferred — virtualized in-window menus first.

### Phase L4 — Clipboard, DnD, dialogs, theme, notifications

- **§2.6 Clipboard:** `SDL_GetClipboardText` / `SDL_SetClipboardText`
  for plain text. File paths and HTML use `SDL_RegisterClipboardData`
  (SDL3) for custom MIME types.
- **§2.7 Drag-and-drop:** SDL drop events
  (`SDL_EVENT_DROP_FILE`/`DROP_TEXT`/`DROP_BEGIN`/`DROP_COMPLETE`) for
  inbound. Outbound drags via xdg-desktop-portal `FileTransfer`
  interface (no SDL primitive for outbound).
- **§2.8 Dialog:** xdg-desktop-portal `FileChooser` for FS dialogs;
  portal `Settings` notification or simple custom rendering for alerts.
  Direct dbus calls — no GTK file chooser dependency.
- **§2.5 NativeTheme on Linux:** XSettings (`Net/IconThemeName`,
  `Gtk/ColorScheme`) + portal `Settings` notification subscription for
  light/dark.
- **`NativeNote`:** DBus `org.freedesktop.Notifications` directly. The
  existing `GTKNote.cpp` code path used libnotify which talks to the
  same dbus service — port the call sites to direct `dbus-1`.

### Phase L5 — IME

- Wire `SDL_StartTextInput` / `SDL_StopTextInput` to the focused View's
  input-method context.
- Receive `SDL_EVENT_TEXT_INPUT` and `SDL_EVENT_TEXT_EDITING` events;
  route them through the focus manager (§2.3a) to the focused View.
- `SDL_SetTextInputArea` for the candidate window position — feed it
  the focused View's text caret rect in window coordinates.
- AT-SPI accessibility bridge (§2.10) lands in the same phase or later.

### Phase L6 — Wayland

SDL handles the protocol; what we need from WTK side:

- Handle `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` for fractional scale.
- Confirm `SDL_Vulkan_CreateSurface` returns a Wayland `VkSurfaceKHR`
  via the SDL-selected backend (no WTK changes needed in GTE).
- Handle Wayland clipboard / DnD differences via SDL's abstraction
  (most code from L4 carries over).

### Phase L7 — GTK retirement

- Delete `wtk/src/Native/gtk/` and all its files.
- Remove `TARGET_GTK` from CMake; remove libgtk-3-dev from
  Linux build dependencies; remove the GTK flag from
  `compile_commands.json` baseline.
- Delete `Notification.cpp`'s GTK code path.
- Update `Native-API-Completion-Proposal.md` §3 to note the
  migration is complete.

## 6. Risks and open questions

### 6.1 SDL2 vs SDL3

SDL3 is the future: cleaner Vulkan API, callback-based event handling
optional, true HDR, fractional scale events. SDL2 is more widely
packaged on conservative distros (Debian stable, RHEL).

**Plan:** target SDL3 as the default. Provide an SDL2 compatibility
shim (`sdl/sdl_compat.h`) for distros without SDL3, mapping the small
set of SDL3 symbols we use to SDL2 equivalents. Most differ only in
naming (`SDL_EVENT_QUIT` vs `SDL_QUIT`) and a few API signatures
(`SDL_Vulkan_CreateSurface` gains an allocator parameter in SDL3).
The shim is mechanical.

### 6.2 Required Vulkan instance extensions

`SDL_Vulkan_GetInstanceExtensions` must be called before
`vkCreateInstance`. GTE currently builds its extension list internally
in `GEVulkanEngine::initialize`. We need to merge SDL's list (typically
`VK_KHR_surface` + a platform-specific surface extension like
`VK_KHR_xlib_surface` or `VK_KHR_wayland_surface`) before the instance
is created. Merge is union-by-name; duplicates dropped.

The merge happens in GTE, not WTK — see §4.1.

### 6.3 Menu accessibility

Virtualized menus are not picked up by AT-SPI without explicit work.
The menu accessibility story falls under §2.10 (accessibility bridge).
Not a blocker for L3, but the bridge needs to know about menu items as
accessible elements once it lands.

### 6.4 GNOME global menu / Plasma menu publication

For full native feel on GNOME and Plasma, an app's menu model is
expected on dbus (`com.canonical.AppMenu.Registrar`). Virtualized
menus rendered in our window do not appear in the global menu bar
without this publication. Out of scope for the initial L3; revisit
once L3 lands and we have user feedback.

### 6.5 X11 DPI weakness

X11 has no formal per-monitor DPI signal — apps read `Xft.dpi` from
XSettings. SDL surfaces this through `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`
and `SDL_GetWindowDisplayScale`, but the fidelity depends on the WM /
desktop. Mitigations are in §2.2's per-monitor DPI handling — the
event flow doesn't change for SDL.

### 6.6 Compositor-side decoration

On Wayland, modern compositors expect apps to draw their own
decorations or negotiate via `xdg_decoration`. SDL handles this for
us; we set `SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR=1` if we want
client-side decorations.

## 7. Cross-references

- `Native-API-Completion-Proposal.md`
  - §2.2 — Window control surface (the §2.2 implementation that SDL
    backend must reach parity with).
  - §2.3a — Virtual focus / cursor / tooltip layer (already
    cross-platform; no change for SDL).
  - §2.6 / §2.7 / §2.8 — Clipboard / DnD / dialogs (Phase L4).
  - §2.10 — Accessibility (Phase L5+).
  - §2.12 — Menus (existing native impl on Win32; virtualized on
    Linux in Phase L3).
- `Frame-Pacing-Plan.md` — the SDL backend must integrate with the
  same frame-pacing primitives the macOS / Win32 backends use.
- SDL 3 docs: <https://wiki.libsdl.org/SDL3/CategoryVulkan>
- SDL Vulkan tutorial:
  <https://wiki.libsdl.org/SDL3/SDL_Vulkan_CreateSurface>

## 8. Decision log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-05-06 | Adopt SDL as the Linux platform-window abstraction | GTK does not allow direct Vulkan in child windows; offscreen-readback adapter rejected as long-term; SDL gives X11+Wayland in one library. |
| 2026-05-06 | Target SDL3 with SDL2 fallback shim | SDL3 has cleaner Vulkan API; SDL2 is broader distro coverage today. |
| 2026-05-06 | X11 first, Wayland follows in L6 | Lower risk; X11 well-trodden; Wayland complexity concentrated in one phase. |
