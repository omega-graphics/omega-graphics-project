# OmegaGTEView: 3D Graphics View Proposal

## Goal

Add a `View` subclass purpose-built for rendering complex 3D graphics within the OmegaWTK widget tree. The view owns its own GPU command queue, render pipeline state, and depth/stencil resources — giving widgets direct access to the full OmegaGTE render and compute pipeline without going through the 2D Compositor's `Canvas`/`CanvasFrame` path.

This is the 3D counterpart to what `CanvasView` is for 2D drawing and what `VideoView` is for media playback: a specialized View that owns the GPU resources appropriate to its domain.

## Motivation

The current rendering path in OmegaWTK is:

```
Widget → View → LayerTree → Canvas → CanvasFrame → Compositor → BackendRenderTargetContext
                                                                    ↓
                                                        GENativeRenderTarget (2D composition)
```

This path is optimized for 2D UI composition: tessellation of shapes/text, 2D transforms, layered effects. It uses `OmegaTriangulationEngineContext` to tessellate 2D primitives into triangles and submits them through a shared composition pipeline.

For 3D graphics — scene rendering, mesh display, CAD views, game viewports, data visualization — this path is the wrong abstraction:

1. **No depth buffer.** The 2D composition path doesn't allocate a depth/stencil attachment. 3D rendering needs depth testing.
2. **No direct pipeline control.** Widgets can't set their own vertex/fragment shaders, bind arbitrary buffers/textures, or control rasterizer state (cull mode, fill mode, front-face winding).
3. **No per-frame command encoding.** The Compositor owns command submission timing. 3D content needs a render loop with per-frame camera/transform updates.
4. **No compute pass access.** GPU compute (particle systems, physics, post-processing) can't be encoded through the Canvas API.
5. **No raytracing access.** The `GEAccelerationStruct` and `dispatchRays` APIs exist in GTE but have no path into WTK.

`OmegaGTEView` solves this by giving the widget a direct `GERenderTarget::CommandBuffer` per frame, with the full GTE command encoding API.

## Architecture

GTEView renders directly to a native GPU surface owned by a
`NativeViewHost` widget (Phase 5, Native View Architecture Plan). The
3D content presents through its own native layer (CAMetalLayer, DXGI
swap chain, VkSurface) — no off-screen texture, no Compositor blit, no
fence-synced copy. The native surface renders on top of virtual content
(the "airspace" constraint — acceptable for opaque 3D viewports).

```
GTEViewWidget : Widget
  ├── NativeViewHost
  │     └── platform native GPU surface:
  │           macOS:   CocoaItem with CAMetalLayer
  │           Windows: HWNDItem with DXGI SwapChain
  │           Linux:   GTKItem with VkSurfaceKHR
  │
  ├── GTEView (owns render resources)
  │     ├── Owns: GECommandQueue (dedicated or shared, configurable)
  │     ├── Owns: Depth/stencil GETexture
  │     │
  │     └── Present path:
  │           → acquires drawable from NativeViewHost's native GPU surface
  │           → renders to drawable's color attachment directly
  │           → presents drawable (no Compositor involvement)
  │
  ├── GTEViewDelegate (user implements)
  │     ├── onSetup(GTEViewContext &)       — one-time: create pipelines, buffers, textures
  │     ├── onFrame(GTEViewContext &)       — per-frame: encode render/compute passes
  │     ├── onResize(GTEViewContext &, w, h) — rebuild depth/stencil, update projection
  │     └── onTeardown(GTEViewContext &)    — cleanup
  │
  └── GTEViewContext (passed to delegate)
        ├── commandBuffer()          → GERenderTarget::CommandBuffer
        ├── viewport()               → GEViewport
        ├── scissorRect()            → GEScissorRect
        ├── engine()                 → OmegaGraphicsEngine &
        ├── tessellationContext()    → OmegaTriangulationEngineContext &
        ├── colorTexture()           → SharedHandle<GETexture>
        ├── depthStencilTexture()    → SharedHandle<GETexture>
        ├── backingWidth() / backingHeight()
        ├── renderScale()
        ├── frameIndex()             → uint64_t
        └── deltaTime()              → float (seconds since last frame)
```

### Frame Lifecycle

```
1. Display-link callback or invalidateFrame() fires
2. GTEView acquires a drawable from the NativeViewHost's native GPU surface
   macOS:   [metalLayer nextDrawable]
   Windows: swapChain->GetBuffer()
   Linux:   vkAcquireNextImageKHR()
3. Builds GTEViewContext with the drawable as color attachment
4. Calls delegate->onFrame(context)
   4a. Delegate encodes render passes (3D scene)
   4b. Delegate encodes compute passes (particles, post-FX)
   4c. Delegate ends encoding
5. GTEView presents the drawable directly (no Compositor involvement)
```

The Compositor is not involved — GTEView manages its own present timing.
The NativeViewHost handles bounds synchronization with the virtual widget
tree. The native GPU surface renders on top of virtual content (the
"airspace" constraint — acceptable for opaque 3D viewports).

## Public API

### GTEView.h

```cpp
#ifndef OMEGAWTK_UI_GTEVIEW_H
#define OMEGAWTK_UI_GTEVIEW_H

#include "View.h"
#include <omegaGTE/GE.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GETexture.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/TE.h>

#include <cstdint>

namespace OmegaWTK {

class GTEViewDelegate;

/// Configuration for creating a GTEView.
struct OMEGAWTK_EXPORT GTEViewDescriptor {
    /// Pixel format for the color attachment.
    OmegaGTE::PixelFormat colorFormat = OmegaGTE::PixelFormat::RGBA8Unorm;

    /// Enable depth testing. Allocates a depth texture.
    bool enableDepth = true;

    /// Enable stencil testing. Combined with depth into a single texture
    /// when the backend supports packed depth/stencil formats.
    bool enableStencil = false;

    /// Multisample count. 1 = no MSAA.
    unsigned sampleCount = 1;

    /// Maximum command buffers the view's queue can hold.
    unsigned maxCommandBuffers = 3;

    /// If true, the view creates its own GECommandQueue.
    /// If false, it shares the Compositor's queue (lower overhead,
    /// but 3D work competes with UI composition for GPU time).
    bool dedicatedQueue = true;

    /// Target frames per second for the render loop.
    /// 0 = render only on invalidation (manual mode).
    /// 60/120 = continuous render loop driven by display link.
    unsigned targetFPS = 0;

    /// Clear color for the color attachment each frame.
    struct { float r, g, b, a; } clearColor = {0.f, 0.f, 0.f, 1.f};

    /// Depth clear value.
    float clearDepth = 1.f;

    /// Stencil clear value.
    unsigned clearStencil = 0;
};

/// Provides the delegate with all the GPU resources it needs
/// to encode a frame. Passed by reference to every delegate callback.
///
/// The context is only valid for the duration of the callback.
/// Do not store pointers to it.
class OMEGAWTK_EXPORT GTEViewContext {
    struct Impl;
    Core::UniquePtr<Impl> impl_;
    friend class GTEView;
    explicit GTEViewContext(Impl * impl);
public:
    /// The command buffer for encoding render, compute, and blit passes.
    OmegaGTE::GERenderTarget::CommandBuffer & commandBuffer();

    /// The graphics engine instance. Use for creating pipelines,
    /// buffers, textures, samplers, etc.
    OmegaGTE::OmegaGraphicsEngine & engine();

    /// The triangulation engine context. Use for tessellating 3D
    /// primitives (prisms, cylinders, cones, ellipsoids, paths).
    OmegaGTE::OmegaTriangulationEngineContext & tessellationContext();

    /// A viewport matching the view's current backing size.
    OmegaGTE::GEViewport viewport() const;

    /// A scissor rect matching the view's current backing size.
    OmegaGTE::GEScissorRect scissorRect() const;

    /// The color texture that the render pass writes to.
    SharedHandle<OmegaGTE::GETexture> colorTexture();

    /// The depth/stencil texture (nullptr if depth is disabled).
    SharedHandle<OmegaGTE::GETexture> depthStencilTexture();

    /// Backing pixel dimensions (logical size * render scale).
    unsigned backingWidth() const;
    unsigned backingHeight() const;

    /// The current display render scale (e.g. 2.0 on Retina).
    float renderScale() const;

    /// Monotonically increasing frame index.
    std::uint64_t frameIndex() const;

    /// Seconds elapsed since the last frame. 0 on the first frame.
    float deltaTime() const;

    /// The descriptor used to create this view.
    const GTEViewDescriptor & descriptor() const;

    ~GTEViewContext();
};

/// User-implemented delegate that receives frame callbacks.
class OMEGAWTK_EXPORT GTEViewDelegate {
    friend class GTEView;
protected:
    GTEView *gteView = nullptr;
public:
    /// Called once after the view's GPU resources are created.
    /// Create render pipeline states, vertex buffers, textures here.
    virtual void onSetup(GTEViewContext & ctx) {}

    /// Called every frame. Encode render and compute passes here.
    /// The command buffer has already begun; a render pass targeting
    /// the color (and optionally depth) attachment is ready to start.
    virtual void onFrame(GTEViewContext & ctx) = 0;

    /// Called when the view resizes. Depth/stencil textures are
    /// rebuilt before this is called. Update projection matrices here.
    virtual void onResize(GTEViewContext & ctx,
                          unsigned newWidth,
                          unsigned newHeight) {}

    /// Called before the view's GPU resources are destroyed.
    /// Release any resources created in onSetup here.
    virtual void onTeardown(GTEViewContext & ctx) {}

    virtual ~GTEViewDelegate() = default;
};

/// A View subclass for rendering 3D graphics using OmegaGTE.
///
/// Renders directly to a native GPU surface owned by a NativeViewHost —
/// no off-screen texture or Compositor blit. The delegate receives a
/// per-frame callback with a command buffer ready for encoding render,
/// compute, and blit passes.
class OMEGAWTK_EXPORT GTEView : public View {
    struct Impl;
    Core::UniquePtr<Impl> impl_;

    void rebuildRenderTargets();
    void executeFrame();

    friend class Widget;
    friend class GTEViewContext;
public:
    OMEGACOMMON_CLASS("OmegaWTK.GTEView")

    explicit GTEView(const Composition::Rect & rect,
                     const GTEViewDescriptor & desc,
                     ViewPtr parent = nullptr);
    ~GTEView() override;

    /// Set the delegate that will receive frame callbacks.
    void setDelegate(GTEViewDelegate *delegate);

    /// Get the current descriptor (read-only after construction).
    const GTEViewDescriptor & descriptor() const;

    /// Request a frame redraw. In manual mode (targetFPS=0),
    /// this is the only way to trigger a frame.
    /// In continuous mode, the frame loop runs automatically
    /// and this call is a no-op.
    void invalidateFrame();

    /// Start the continuous render loop (if targetFPS > 0).
    /// Called automatically on mount; exposed for pause/resume.
    void startRenderLoop();

    /// Pause the continuous render loop.
    void stopRenderLoop();

    /// Returns true if the render loop is currently running.
    bool isRenderLoopActive() const;

    /// Override: resize rebuilds depth/stencil and notifies delegate.
    void resize(Composition::Rect newRect) override;
};

}

#endif // OMEGAWTK_UI_GTEVIEW_H
```

## Implementation Plan

### Phase 1: Core View and Direct Rendering

**Goal:** GTEView creates a command queue and depth/stencil resources. Delegate can encode a single render pass per frame, rendering directly to the native GPU surface.

#### 1A. GTEView Construction

```
GTEView(rect, desc, parent)
  → engine().makeCommandQueue(desc.maxCommandBuffers)
  → engine().makeTexture(depth, backing W × H, RenderTargetAndDepthStencil usage)  [if enableDepth]
```

Store all of these in `GTEView::Impl`. The color attachment comes from
the native GPU surface drawable each frame — no owned color texture.

#### 1B. Frame Execution

```
executeFrame()
  → acquire drawable from NativeViewHost's native GPU surface
  → build GTEViewContext with drawable as color attachment
  → delegate->onFrame(context)
  → present drawable
```

#### 1C. Resize

```
resize(newRect)
  → View::resize(newRect)
  → compute new backingWidth/backingHeight from rect * renderScale
  → release old depth texture
  → create new depth texture at new size
  → delegate->onResize(context, newWidth, newHeight)
```

#### Source Files

- `wtk/include/omegaWTK/UI/GTEView.h` — public header (as above)
- `wtk/src/UI/GTEView.cpp` — implementation
- `wtk/src/UI/GTEViewImpl.h` — private `Impl` struct

#### Verification

- GTEView renders a colored triangle to the native GPU surface.
- The triangle appears in the widget tree at the correct position and size.
- Resizing the view updates the depth texture dimensions.

---

### Phase 2: Render Loop and Timing

**Goal:** Continuous render loop driven by display link or timer, with delta time tracking.

#### 2A. Display Link Integration

On macOS, use `CVDisplayLink` (or `CADisplayLink` on iOS). On Windows, use a `WaitableTimer` or `IDXGIOutput::WaitForVBlank`. On Linux/Vulkan, use a timer thread synced to the swap interval.

The render loop calls `executeFrame()` at the target FPS.

#### 2B. Frame Pacing

```
renderLoopTick()
  → compute deltaTime from last frame timestamp
  → executeFrame()
  → track frame timing for diagnostics
```

#### 2C. Manual vs Continuous Mode

- `targetFPS = 0`: No render loop. `invalidateFrame()` triggers a single `executeFrame()` on the next Compositor tick.
- `targetFPS > 0`: Render loop runs continuously. `invalidateFrame()` is a no-op. `stopRenderLoop()` / `startRenderLoop()` for pause/resume.

#### Verification

- Rotating cube at 60 FPS with smooth delta-time-based rotation.
- `stopRenderLoop()` freezes the last frame; `startRenderLoop()` resumes.
- Manual mode: `invalidateFrame()` triggers exactly one frame.

---

### Phase 3: Depth, Stencil, and MSAA

**Goal:** Full depth/stencil support and optional multisample anti-aliasing.

#### 3A. Depth/Stencil Texture

When `enableDepth = true`:
- Create a depth texture with `RenderTargetAndDepthStencil` usage.
- The render pass descriptor's `DepthStencilAttachment` references this texture with `Clear` load action and `clearDepth`/`clearStencil` values.

When `enableStencil = true`:
- Use a packed depth/stencil format (D24S8 or D32S8, platform-dependent).
- Expose stencil ref through the command buffer's `setStencilRef()`.

#### 3B. MSAA

When `sampleCount > 1`:
- Color and depth textures are created with `sampleCount` in the `TextureDescriptor`.
- A resolve texture is created at 1x sample count.
- The render pass uses `multisampleResolve = true` with the resolve texture as the destination.
- The resolve texture is presented to the native GPU surface (not the MSAA texture).

#### Verification

- Overlapping 3D objects render with correct depth ordering.
- Stencil masking works (e.g. portal effect).
- MSAA 4x produces visibly smoother edges than 1x.

---

### Phase 4: Compute and Raytracing

**Goal:** Expose GTE compute and raytracing passes through the GTEView context.

#### 4A. Compute Passes

The `GERenderTarget::CommandBuffer` already exposes:
- `startComputePass(pipelineState)`
- `bindResourceAtComputeShader(buffer/texture, id)`
- `dispatchThreadgroups(x, y, z)` / `dispatchThreads(x, y, z)`
- `endComputePass()`

These are all available through `GTEViewContext::commandBuffer()`. No additional API needed — the delegate can encode compute passes between or after render passes in `onFrame`.

#### 4B. Raytracing (Conditional)

When `OMEGAGTE_RAYTRACING_SUPPORTED` is defined:
- `GTEViewContext` exposes `supportsRaytracing()` → bool.
- The delegate can use `engine().allocateAccelerationStructure()` in `onSetup`.
- In `onFrame`, encode accel struct build passes and `dispatchRays()` through the command buffer.

#### Verification

- GPU particle system using compute: particles update + render in a single frame.
- (If raytracing hardware available) Simple ray-traced shadow pass.

---

### Phase 5: GTEViewWidget (NativeViewHost integration)

**Goal:** Widget wrapper that uses `NativeViewHost` (Phase 5, Native
View Architecture Plan) to embed a real native GPU surface for
direct-present rendering.

**Dependency:** `NativeViewHost` (already implemented).

#### 5A. Native GPU surface item

Create a `make_native_gpu_item()` factory that returns a NativeItem
wrapping a platform-specific GPU surface:

- **macOS:** `CocoaItem` + `CAMetalLayer` (the `metalLayer_` member
  already exists in `CocoaItem` — reuse and configure for direct
  rendering rather than compositor-managed presentation)
- **Windows:** `HWNDItem` + DXGI SwapChain
- **Linux:** `GTKItem` + `VkSurfaceKHR` (or `GtkGLArea`)

```
Files:
  wtk/include/omegaWTK/Native/NativeGPUItem.h    — factory interface
  wtk/src/Native/macos/CocoaGPUItem.mm            — macOS impl
  wtk/src/Native/win/HWNDGPUItem.cpp              — Windows impl
  wtk/src/Native/gtk/GTKGPUItem.cpp               — Linux impl
```

#### 5B. GTEViewWidget

```cpp
class GTEViewWidget : public Widget {
    NativeViewHost *hostView_;
    GTEView *gteView_;         // internal, manages GPU resources
    GTEViewDescriptor desc_;
public:
    explicit GTEViewWidget(Composition::Rect rect,
                           const GTEViewDescriptor & desc = {});

    GTEView & gteView();
    const GTEView & gteView() const;

    void setDelegate(GTEViewDelegate *delegate);

    void invalidateFrame();
    void startRenderLoop();
    void stopRenderLoop();
};
```

The constructor:
1. Creates a `NativeViewHost` as a child widget
2. Creates a native GPU item via `make_native_gpu_item()`
3. Calls `hostView_->attach(gpuItem)` to embed it
4. Creates `GTEView` configured to render to the native surface

#### Source Files

- `wtk/include/omegaWTK/Widgets/MediaWidgets.h` — add `GTEViewWidget`
- `wtk/src/Widgets/MediaWidgets.cpp` — implementation

#### Verification

- `GTEViewWidget` embedded in an `HStack` with other UI widgets.
- 3D view resizes correctly when the stack layout changes.
- Multiple `GTEViewWidget` instances in the same window render independently.
- No Compositor blit — GPU surface presents directly.

---

## Platform Considerations

### Metal (macOS/iOS)

- `GTEView` acquires drawables directly from `CAMetalLayer` via `NativeViewHost`.
- Depth texture uses Metal textures internally.
- Display link: `CVDisplayLink` on macOS, `CADisplayLink` on iOS.
- Depth format: `MTLPixelFormatDepth32Float` or `MTLPixelFormatDepth32Float_Stencil8`.

### Direct3D 12 (Windows)

- `GTEView` acquires back buffers from a DXGI SwapChain owned by the `NativeViewHost`.
- GTE creates D3D12 depth textures.
- Display link: `IDXGIOutput::WaitForVBlank` or `WaitableTimer`.
- Depth format: `DXGI_FORMAT_D32_FLOAT` or `DXGI_FORMAT_D24_UNORM_S8_UINT`.

### Vulkan (Linux/Android)

- `GTEView` acquires swapchain images from the `VkSurfaceKHR` owned by the `NativeViewHost`.
- GTE creates Vulkan depth images.
- Display link: Timer thread (no native display link on Vulkan).
- Depth format: `VK_FORMAT_D32_SFLOAT` or `VK_FORMAT_D24_UNORM_S8_UINT`.

## Resource Lifecycle

| Resource | Created | Rebuilt on Resize | Destroyed |
|----------|---------|-------------------|-----------|
| `GECommandQueue` | Construction | No | Destruction |
| Depth/stencil texture | Construction | Yes | Destruction |
| User pipelines, buffers | `onSetup` | User decides | `onTeardown` |

## Open Questions

1. **Shared vs dedicated queue:** Should the default be a dedicated queue, or should we default to sharing the Compositor's queue to reduce GPU context-switching overhead? On macOS Metal, multiple queues are cheap. On D3D12, they're heavier.

2. ~~**Texture blit path:**~~ **Resolved:** GTEView renders directly to the NativeViewHost's native GPU surface — no Compositor blit, no off-screen texture.

3. **Swap chain buffering:** The native GPU surface manages its own back buffer count (typically double or triple buffered by the platform). Should `GTEViewDescriptor` expose a preferred buffer count, or leave it to platform defaults?

4. **Render scale:** Should the GTE view respect the system render scale (2x on Retina), or should this be configurable independently? 3D content may prefer a lower render scale for performance.

5. **Integration with TE:** The `OmegaTriangulationEngineContext` is exposed through the context for convenience, but 3D applications often bring their own mesh data. Should TE integration be optional/lazy?

6. **Async readback:** Should `GTEViewContext` expose a readback path for screen capture / screenshot? This would require a staging buffer and a fence-synced copy.

7. **Multiple render passes per frame:** The proposal allows the delegate to encode multiple render passes (e.g. shadow map pass → scene pass → post-processing compute). Should the context provide helpers for managing multiple render targets, or is the raw command buffer sufficient?
