# Vertical Slice: Window + GTE Render Target

First vertical slice for AQUA — open a native window and render into it via OmegaGTE.

## Goal

A minimal executable that:
1. Opens a platform-native window (HWND / NSWindow / GTK)
2. Initializes OmegaGTE and creates a `GENativeRenderTarget` backed by that window
3. Runs a render loop that clears to a solid color and presents each frame
4. Shuts down cleanly on window close

## Architecture

```
┌──────────────────────────────────────────┐
│  main()                                  │
│    AquaWindow::create(...)               │
│    OmegaGTE::InitWithDefaultDevice()     │
│    engine->makeNativeRenderTarget(desc)  │
│    run loop                              │
│    OmegaGTE::Close()                     │
└──────────────────────────────────────────┘

┌──────────────────────────────────────────┐
│  AquaWindow  (lightweight platform shim) │
│    - create / destroy                    │
│    - pollEvents / shouldClose            │
│    - nativeHandle()                      │
│    - width / height                      │
├──────────────────────────────────────────┤
│  Win32       │  Cocoa       │  GTK       │
│  HWND        │  NSWindow +  │  GtkWindow │
│              │  CAMetalLayer│            │
└──────────────┴──────────────┴────────────┘
```

### AquaWindow

A minimal cross-platform window abstraction. Not a toolkit — just the thinnest layer needed to hand a native surface to GTE.

**Public interface** (single header, `include/aqua/Window.h`):

```cpp
namespace Aqua {

struct WindowDesc {
    const char *title;
    unsigned width;
    unsigned height;
};

class Window {
public:
    static std::unique_ptr<Window> create(const WindowDesc &desc);
    
    bool shouldClose() const;
    void pollEvents();
    
    unsigned width() const;
    unsigned height() const;

    // Platform-specific handle for NativeRenderTargetDescriptor.
    // - Win32:  HWND
    // - macOS:  CAMetalLayer*
    // - Linux:  X11 Window + Display*, or wl_surface + wl_display
    void *nativeHandle() const;

#if defined(TARGET_VULKAN) && defined(VULKAN_TARGET_X11)
    Display *x11Display() const;
#endif

    ~Window();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Aqua
```

**Platform implementations** (one .cpp/.mm per platform in `src/platform/`):

| Platform | File | Window API | Surface for GTE |
|----------|------|------------|-----------------|
| Windows | `src/platform/Win32Window.cpp` | Win32 `CreateWindowExW` | `HWND` → `NativeRenderTargetDescriptor::hwnd` |
| macOS | `src/platform/CocoaWindow.mm` | `NSWindow` + `NSView` | `CAMetalLayer` on the view → `NativeRenderTargetDescriptor::metalLayer` |
| Linux | `src/platform/X11Window.cpp` | Xlib `XCreateSimpleWindow` | `Window` + `Display*` → `NativeRenderTargetDescriptor::x_window/x_display` |

### GTE Integration

Wire `AquaWindow::nativeHandle()` into a `NativeRenderTargetDescriptor`:

```cpp
// Pseudocode — platform ifdefs omitted for clarity
auto gte = OmegaGTE::InitWithDefaultDevice();

OmegaGTE::NativeRenderTargetDescriptor desc {};
#if defined(TARGET_DIRECTX)
    desc.hwnd   = (HWND)window->nativeHandle();
    desc.width  = window->width();
    desc.height = window->height();
#elif defined(TARGET_METAL)
    desc.metalLayer = (CAMetalLayer *)window->nativeHandle();
#elif defined(TARGET_VULKAN) && defined(VULKAN_TARGET_X11)
    desc.x_window  = (::Window)(uintptr_t)window->nativeHandle();
    desc.x_display = window->x11Display();
#endif

auto renderTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
```

### Render Loop

```cpp
while (!window->shouldClose()) {
    window->pollEvents();

    auto cmdBuf = renderTarget->commandBuffer();

    GERenderTarget::RenderPassDesc::ColorAttachment color(
        {0.1f, 0.1f, 0.1f, 1.0f},   // dark gray clear color
        GERenderTarget::RenderPassDesc::ColorAttachment::Clear
    );
    GERenderTarget::RenderPassDesc passDesc {};
    passDesc.colorAttachment = &color;

    cmdBuf->startRenderPass(passDesc);
    cmdBuf->endRenderPass();

    renderTarget->submitCommandBuffer(cmdBuf);
    renderTarget->commitAndPresent();
}

OmegaGTE::Close(gte);
```

## File Layout

```
aqua/
  include/aqua/
    Window.h
  src/
    main.cpp                    # Entry point: create window, init GTE, run loop
    platform/
      Win32Window.cpp
      CocoaWindow.mm
      X11Window.cpp
```

## Build (CMakeLists.txt changes)

Uncomment and adapt the existing CMake scaffolding:

```cmake
omega_graphics_project(AQUA VERSION 0.1 LANGUAGES CXX OBJCXX)

set(PLATFORM_SRCS)
if(WIN32)
    list(APPEND PLATFORM_SRCS src/platform/Win32Window.cpp)
elseif(APPLE)
    list(APPEND PLATFORM_SRCS src/platform/CocoaWindow.mm)
elseif(UNIX)
    list(APPEND PLATFORM_SRCS src/platform/X11Window.cpp)
endif()

add_executable(AQUA
    src/main.cpp
    ${PLATFORM_SRCS}
)
target_include_directories(AQUA PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AQUA PRIVATE OmegaGTE)
```

## Steps

1. **Window.h** — write the platform-agnostic header
2. **CocoaWindow.mm** — macOS implementation (current dev platform)
3. **main.cpp** — init GTE, create render target, run clear-color loop
4. **Build & verify** — window opens, clears to dark gray, closes cleanly
5. **Win32Window.cpp** — Windows implementation
6. **X11Window.cpp** — Linux implementation

Start with macOS since that's what we can test on immediately. Windows and Linux follow once the interface is proven.

## Success Criteria

- A window appears with the correct title and dimensions
- The window interior is a solid dark gray (GTE clear color)
- Closing the window exits the process cleanly (no leaks, GTE shutdown)
- No WTK dependency
