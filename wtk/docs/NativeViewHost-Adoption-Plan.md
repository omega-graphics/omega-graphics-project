# NativeViewHost Adoption Plan: VideoView & OmegaGTEView

## Context

Phase 5 of the Native View Architecture Plan introduced `NativeViewHost` —
the only escape hatch for embedding a real native view inside OmegaWTK's
virtual widget tree. Two existing View types are candidates for adoption:

1. **VideoView** — currently a `View` subclass that software-decodes video
   frames into `BitmapImage` and blits them to a `Canvas`. This works but
   misses hardware-accelerated video presentation (AVPlayerLayer on macOS,
   Media Foundation EVR/DXVA on Windows, VA-API overlay on Linux).

2. **OmegaGTEView** (proposed) — a `View` subclass for 3D rendering that
   owns a GTE texture render target and blits its output through the
   Compositor. The proposal has it rendering to an off-screen texture and
   compositing through the 2D path.

Both views need direct GPU surface access that the virtual compositor path
cannot provide efficiently. Converting them to use `NativeViewHost` gives
each a real native layer (CAMetalLayer, DXGI swap chain, Vulkan surface)
for zero-copy presentation.

---

## Part 1: VideoView → NativeViewHost

### Current architecture

```
VideoView : View, VideoFrameSink
  → decode frame (software or HW-assisted) → BitmapImage
  → videoCanvas->drawImage(bitmap, scaledRect)
  → videoCanvas->sendFrame()
  → Compositor blits the Canvas layer into the window surface
```

Every frame goes through: decode → CPU bitmap → Canvas drawImage
(tessellation + texture upload) → Compositor blit → present. Even when
the decoder produces GPU-resident frames (hardware decode), the
`BitmapImage` path forces a GPU→CPU→GPU round-trip.

### Target architecture

```
VideoViewWidget : Widget
  ├── NativeViewHost (provides the native layer)
  │     └── platform native video surface:
  │           macOS:   AVSampleBufferDisplayLayer (or AVPlayerLayer)
  │           Windows: DXGI SwapChain child HWND (or EVR presenter)
  │           Linux:   VA-API overlay / GStreamer video sink widget
  │
  ├── VideoView (retained as internal controller, no longer a View)
  │     → manages playback/capture sessions
  │     → pushes decoded frames to the native surface
  │     → handles scale mode, delegate callbacks
  │
  └── overlay Canvas (optional, for subtitles/controls drawn on top)
```

### Design decisions

**VideoView becomes a non-View controller class.** The View inheritance
is replaced by ownership of a NativeViewHost. VideoView keeps all its
media logic (playback sessions, capture sessions, frame sink protocol)
but delegates visual presentation to the platform's native video surface
rather than Canvas blitting.

**VideoFrameSink stays.** The `pushFrame` / `presentCurrentFrame` /
`flush` protocol is retained. Instead of drawing to a Canvas, the sink
implementation pushes frames to the native video layer:
- macOS: `[displayLayer enqueueSampleBuffer:sampleBuffer]`
- Windows: Present via DXGI swap chain or MF video presenter
- Linux: Push buffer to GStreamer video sink

**Scale mode moves to the native layer.** `AspectFit`/`AspectFill`/
`Stretch` are implemented via the native layer's gravity/transform
rather than computing `destRect` in software.

### Phases

#### Phase V1: VideoViewWidget with NativeViewHost shell

Create `VideoViewWidget` as a `Widget` subclass that owns a
`NativeViewHost`. The NativeViewHost holds a platform-specific native
item created via a new `make_native_video_item()` factory that returns:
- macOS: A `CocoaItem` wrapping an NSView with an
  `AVSampleBufferDisplayLayer`
- Windows: An `HWNDItem` wrapping a child HWND configured for DXGI
  video presentation
- Linux: A `GTKItem` wrapping a GStreamer video sink widget

```
Files:
  wtk/include/omegaWTK/Widgets/VideoViewWidget.h     — public API
  wtk/src/Widgets/VideoViewWidget.cpp                 — implementation
  wtk/include/omegaWTK/Native/NativeVideoItem.h       — factory interface
  wtk/src/Native/macos/CocoaVideoItem.mm              — macOS impl
  wtk/src/Native/win/HWNDVideoItem.cpp                — Windows impl
  wtk/src/Native/gtk/GTKVideoItem.cpp                 — Linux impl
```

Verification: NativeViewHost appears at the correct position in the
widget tree. The native video surface is visible (even if just a black
rectangle initially).

#### Phase V2: Wire VideoFrameSink to native surface

Replace the Canvas-based `queueFrame()` / `presentCurrentFrame()` with
platform-specific frame presentation:

```
macOS:
  VideoFrame → CMSampleBuffer → AVSampleBufferDisplayLayer

Windows:
  VideoFrame → ID3D11Texture2D → IDXGISwapChain::Present

Linux:
  VideoFrame → GstBuffer → GStreamer video sink
```

Hardware-decoded frames skip the CPU round-trip entirely — the decoder
output texture goes directly to the native surface.

Software-decoded frames (`BitmapImage`) are uploaded to a GPU texture
once and presented to the native surface.

Verification: Video playback displays frames at correct timing through
the native surface. AspectFit/Fill/Stretch work via layer gravity.

#### Phase V3: Migrate VideoView API to VideoViewWidget

- `VideoView` class becomes internal (non-View controller)
- Public API moves to `VideoViewWidget`
- `VideoViewDelegate` callbacks remain unchanged
- Old `VideoView` header marked deprecated, forwards to
  `VideoViewWidget`

Verification: All existing VideoView usage compiles and works through
VideoViewWidget. No Canvas blit path for video frames.

#### Phase V4: Remove Canvas blit path from VideoView

Delete `videoCanvas`, `drawImage`, `sendFrame` code path. The Canvas
dependency is removed entirely. VideoView (now the internal controller)
only manages sessions and pushes to the native surface.

Verification: VideoView.cpp no longer includes Canvas.h. Clean compile
on all platforms.

---

## Part 2: OmegaGTEView → NativeViewHost

### Current proposal architecture (from OmegaGTEView-Proposal.md)

```
GTEView : View
  → owns GETextureRenderTarget (off-screen, depth/stencil)
  → owns GECommandQueue
  → delegate encodes render/compute passes per frame
  → Compositor blits color texture → window's native layer
```

The off-screen texture → Compositor blit is an extra copy. For a 3D
viewport (which may consume most of the window), this copy is expensive
and adds a frame of latency.

### Target architecture

```
GTEViewWidget : Widget
  ├── NativeViewHost (provides the native layer with GPU surface)
  │     └── platform native GPU surface:
  │           macOS:   CocoaItem with CAMetalLayer
  │           Windows: HWNDItem with DXGI SwapChain
  │           Linux:   GTKItem with VkSurfaceKHR
  │
  ├── GTEView (retained, owns render resources)
  │     → GECommandQueue (dedicated or shared)
  │     → depth/stencil textures
  │     → GTEViewDelegate receives onFrame callbacks
  │     → renders directly to the NativeViewHost's GPU surface
  │
  └── GTEViewContext (unchanged API)
```

### Design decisions

**GTEView renders directly to the native layer's GPU surface.** Instead
of rendering to an off-screen texture and blitting, GTEView acquires its
drawable/back buffer directly from the NativeViewHost's native layer:
- macOS: `[metalLayer nextDrawable]` → render to drawable's texture
- Windows: `swapChain->GetBuffer()` → render to back buffer
- Linux: `vkAcquireNextImageKHR()` → render to swapchain image

This eliminates the off-screen texture, the Compositor blit, and the
fence-synced copy. The 3D content presents directly.

**Off-screen mode remains available.** For cases where the 3D content
needs to composite with 2D UI (transparency, overlays), the off-screen
texture path from the original proposal is retained as an opt-in mode
(`GTEViewDescriptor::directPresent = true` vs `false`). Direct present
is the default for full-viewport 3D. Off-screen is used when the 3D
view needs to layer under 2D widgets.

**The Compositor is not involved in direct-present mode.** The GTEView
manages its own present timing (display link or manual). The Compositor
only needs to know about the NativeViewHost's bounds for layout — it
doesn't touch the GPU surface.

**The NativeViewHost handles airspace.** The native GPU surface renders
on top of virtual content (the airspace problem). This is acceptable for
3D viewports, which are typically opaque and occupy a defined region.

### Phases

#### Phase G1: Native GPU surface item

Create a `make_native_gpu_item()` factory that returns a NativeItem
wrapping a platform-specific GPU surface:

```
macOS:   CocoaItem + CAMetalLayer (already exists in CocoaItem — 
         the metalLayer_ member. Reuse and expose.)
Windows: HWNDItem + DXGI SwapChain
Linux:   GTKItem + VkSurfaceKHR (or GtkGLArea)
```

The macOS path is nearly free — `CocoaItem` already manages a
`CAMetalLayer`. The factory configures it for direct rendering rather
than compositor-managed presentation.

```
Files:
  wtk/include/omegaWTK/Native/NativeGPUItem.h    — factory interface
  wtk/src/Native/macos/CocoaGPUItem.mm            — macOS impl
  wtk/src/Native/win/HWNDGPU Item.cpp             — Windows impl
  wtk/src/Native/gtk/GTKGPU Item.cpp              — Linux impl
```

Verification: Native GPU surface appears at correct position.
`CAMetalLayer` / swap chain created and resized with widget.

#### Phase G2: GTEView direct rendering

Modify `GTEView::executeFrame()` to acquire a drawable from the native
GPU surface instead of the off-screen texture render target:

```
executeFrame()
  → acquire drawable/back buffer from native surface
  → build GTEViewContext with drawable as color attachment
  → delegate->onFrame(context)
  → present drawable (no Compositor blit)
```

The `GTEViewDescriptor` gains a `directPresent` flag (default `true`).
When `false`, the original off-screen texture path is used.

Verification: Colored triangle renders directly to the native surface.
No Compositor blit. Resize updates the surface dimensions.

#### Phase G3: GTEViewWidget

Widget wrapper following the same pattern as VideoViewWidget:

```cpp
class GTEViewWidget : public Widget {
    NativeViewHost *hostView_;
    GTEView *gteView_;         // internal, manages GPU resources
    GTEViewDescriptor desc_;
public:
    explicit GTEViewWidget(Core::Rect rect,
                           const GTEViewDescriptor & desc = {});
    GTEView & gteView();
    void setDelegate(GTEViewDelegate *delegate);
    void invalidateFrame();
    void startRenderLoop();
    void stopRenderLoop();
};
```

```
Files:
  wtk/include/omegaWTK/Widgets/MediaWidgets.h     — add GTEViewWidget
  wtk/src/Widgets/MediaWidgets.cpp                 — implementation
```

Verification: GTEViewWidget in an HStack with other widgets. 3D view
resizes correctly. Multiple instances render independently.

#### Phase G4: Display link integration

Wire the render loop to platform display links for frame pacing:
- macOS: `CVDisplayLink` callback → `executeFrame()`
- Windows: `WaitableTimer` or `IDXGIOutput::WaitForVBlank`
- Linux: Timer thread synced to swap interval

Verification: Rotating cube at 60 FPS with smooth delta-time rotation.
`stopRenderLoop()` / `startRenderLoop()` work correctly.

---

## Shared infrastructure

Both VideoViewWidget and GTEViewWidget share the NativeViewHost
embedding mechanism. The per-platform native items differ:

| Widget          | macOS native item            | Windows native item     | Linux native item        |
|-----------------|------------------------------|-------------------------|--------------------------|
| VideoViewWidget | AVSampleBufferDisplayLayer   | DXGI video swap chain   | GStreamer video sink      |
| GTEViewWidget   | CAMetalLayer                 | DXGI render swap chain  | VkSurfaceKHR / GtkGLArea |

Both use `NativeViewHost::attach()` to embed and
`NativeViewHost::syncBounds()` (via `onLayoutResolved`) to track
position.

---

## Dependency graph

```
Phase 5 (NativeViewHost) ← already done
    │
    ├── Part 1: VideoView → NativeViewHost
    │     V1: VideoViewWidget shell + NativeVideoItem
    │     V2: Wire frame sink to native surface
    │     V3: Migrate public API
    │     V4: Remove Canvas blit path
    │
    └── Part 2: GTEView → NativeViewHost
          G1: Native GPU surface item
          G2: Direct rendering path
          G3: GTEViewWidget wrapper
          G4: Display link integration

Parts 1 and 2 are independent and can proceed in parallel.
Within each part, phases are sequential.
```

---

## What stays the same

- `VideoViewDelegate` callback interface — unchanged
- `GTEViewDelegate` callback interface — unchanged
- `VideoFrameSink` protocol — implementation changes, interface stays
- `GTEViewContext` public API — unchanged
- Widget tree layout, hit testing, resize propagation — handled by
  NativeViewHost + virtual widget tree
- All other widgets — remain purely virtual, no native views
