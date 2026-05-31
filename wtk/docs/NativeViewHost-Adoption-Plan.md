# NativeViewHost Adoption Plan: VideoView & OmegaGTEView

## Context

Phase 5 of the Native View Architecture Plan introduced `NativeViewHost` —
the only escape hatch for embedding hardware-accelerated native content
inside OmegaWTK's virtual widget tree. Two existing View types are
candidates for adoption:

1. **VideoView** — currently a `View` subclass that software-decodes video
   frames into `BitmapImage` and blits them to a `Canvas`. This works but
   misses hardware-accelerated video presentation (AVPlayerLayer on macOS,
   Media Foundation EVR/DXVA on Windows, VA-API/DRI3 overlay on Linux).

2. **OmegaGTEView** (proposed) — a `View` subclass for 3D rendering that
   owns a GTE texture render target and blits its output through the
   Compositor. The proposal has it rendering to an off-screen texture and
   compositing through the 2D path.

Both views need direct GPU surface access that the virtual compositor path
cannot provide efficiently. Converting them to use `NativeViewHost` gives
each a real native surface (`CAMetalLayer`, DXGI swap chain, X11 child
Window) for zero-copy presentation.

---

## Where the widgets live

The two consumers ship in different libraries, because they have different
dependency surfaces.

| Widget            | Library                                   | Headers under                               |
|-------------------|-------------------------------------------|---------------------------------------------|
| `VideoViewWidget` | **default `libOmegaWTK`** (always built)  | `omegaWTK/Widgets/`                         |
| `GTEViewWidget`   | **`OmegaWTKComponent_GTEView`** (opt-in)  | `omegaWTK/Components/GTEView/`              |

VideoView is a standard part of WTK — every app already pays for
OmegaVA (playback/capture sessions) and the per-platform video surface APIs
are first-party OS APIs. Folding the widget into core has zero new dependency
cost.

GTEView is different. It pulls libOmegaGTE's full pipeline / compute /
mesh-shader machinery into the link. Most WTK apps do not need a 3D
viewport. The component pattern (under `wtk/components/`, see
`wtk/components/README.md`) lets apps that *do* need it opt in
explicitly without forcing every consumer of OmegaWTK to link the engine.

### What this means for the core/component boundary

Three things have to split cleanly so the core never depends on GTE:

1. **`Native::VisualTree` factory API.** `createVideoContentNode` is a
   member of the core `Native::VisualTree` — VideoViewWidget (in the
   default lib) calls it directly. There is no `createGPUContentNode`
   member: that factory lives in the gteview component as a free function
   that takes a `Native::VisualTree&` and constructs a GPU content node
   against the platform tree. The core thus stays free of any "what is
   a GPU surface" knowledge.

2. **`NativeContentNode` subclasses.** The core ships
   `MTLNativeContentNode` / `DCNativeContentNode` / `VKNativeContentNode`
   with **only the video-surface accessors** (`displayLayer()` /
   `videoSwapChain()` / `videoWindow()`). The gteview component ships
   parallel `MTLGPUContentNode` / `DCGPUContentNode` / `VKGPUContentNode`
   subclasses with **only the GPU-surface accessors** (`metalLayer()` /
   `renderSwapChain()` / `surfaceWindow()`). The two never need to coexist
   in one class, and the core header set never names `CAMetalLayer` or a
   D3D12 render swap chain.

3. **Per-platform tree manipulation.** Inserting / removing / restacking
   a GPU content node in the platform tree is platform-specific glue
   (`CALayer addSublayer:`, `IDCompositionVisual2::AddVisual`,
   `XMapWindow`). The core's per-platform `Native::VisualTree` subclass
   exposes a small `attachContentNode(NativeContentNode&)` /
   `detachContentNode(NativeContentNode&)` /
   `restackContentNode(NativeContentNode&, int zOrder)` interface that
   both the core's video-node factory and the component's GPU-node
   factory call. The interface doesn't know which node kind it's
   moving — both subclasses just expose the platform handle the tree
   already knows how to attach.

### Component build wiring

`wtk/components/` ships an umbrella `CMakeLists.txt` gated by the
`OMEGAWTK_BUILD_COMPONENTS` option (default OFF) that auto-discovers any
subdirectory with its own `CMakeLists.txt`. Each component declares its
own per-component option flag (e.g. `OMEGAWTK_COMPONENT_GTEVIEW`, default
OFF) and builds a STATIC library named `OmegaWTKComponent_<Name>` with
`POSITION_INDEPENDENT_CODE ON`.

Consumers link components after the `OmegaWTKApp(...)` call:

```cmake
OmegaWTKApp(NAME GTEViewSampleApp BUNDLE_ID "..." SOURCES main.cpp)
target_link_libraries(GTEViewSampleApp PRIVATE OmegaWTKComponent_GTEView)
```

`OmegaWTKApp` does not auto-link any component — selection is per-consumer
and explicit. If the component is not configured in, the link fails fast
at configure time. See `wtk/components/README.md` for the full contract.

---

## Architecture: visual-tree attachment, with the tree in Native

`NativeViewHost` does **not** wrap a heavyweight OS widget (NSView, HWND,
GtkWidget). It attaches a node into a `Native::VisualTree` — the per-window
compositor tree that the platform's own compositor walks (DirectComposition
on Windows, CoreAnimation on macOS, the X server's child-window stacking
on Linux). The native surface participates in OS compositing alongside the
WTK swap chain.

The visual tree lives in **the Native layer**, owned by `AppWindow`. This
is the structural choice that lets `NativeViewHost` track virtual layout
without frame lag (see "How layout reaches the native surface" below).

### The carve-out contract

The compositor produces no visible pixels in the rect covered by a
NativeViewHost. Enforced by three rules:

1. **The NativeViewHost widget emits only `DrawOp::NativeContent`** for
   its rect — no fills, borders, descendants, or any other paint op
   inside the carve-out boundary.

2. **The carve-out rect resolves to alpha=0** in the WTK swap chain so
   the native surface beneath shows through. Required setup:
   - **DComp**: swap chain created with `DXGI_ALPHA_MODE_PREMULTIPLIED`.
   - **macOS**: `NSWindow.opaque = NO` so the WTK `CAMetalLayer` is
     alpha-blended over the sibling NativeContent layer.
   - **X11**: the child Window physically clips the parent's pixels in
     the rect — the clear is unnecessary but harmless.

3. **AABB cull, safety net.** Sibling/descendant draw ops whose AABB is
   fully inside an active carve-out are dropped before tessellation.
   Catches misuse; rare in practice. Partial overlap is not culled —
   the alpha clear handles it correctly.

A true scissor-at-tile path (skipping entire slice tiles that fall
inside a carve-out) is a follow-up optimization once profiling justifies
it. The contract above is what the first cut commits to.

### How layout reaches the native surface

`NativeViewHost::onLayoutResolved(rectPx)` calls
`visualTree->reconfigureContentNode(hostId, rectPx, zOrderHint)` directly
on the main thread, synchronously, during the same layout pass that
repositions every other virtual widget. The native surface moves in
lockstep with the virtual content it composites against — no frame lag,
no compositor-cadence dependency.

The compositor is not in this loop. It renders into the WTK swap chain
(itself one visual in the same tree) at its own cadence; the arrangement
of other visuals in the tree is established by FrameBuilder before any
frame is composed. Going through the compositor for repositioning would
have gated native-surface movement on compositor scheduling — exactly
the frame-lag that motivates this architecture.

The `DrawOp::NativeContent` op in the NativeViewHost's display list is
re-emitted when the host's layout resolves, but it does **not** carry
positioning data into a queue. Its only roles are alpha-clear (rule 2)
and AABB-cull (rule 3); positioning has already happened by the time
the compositor sees the op.

### Why the visual tree lives in Native

Lifting the visual tree into the Native layer is what makes the
synchronous-layout path above possible. With the tree in Composition,
FrameBuilder would have to reach across layers (or queue work onto the
compositor thread) to reposition a native surface — that path is
exactly what produced frame lag in the earlier carve-out drain model.

In Native, the tree is per-window state owned by `AppWindow`,
constructed alongside `NativeWindow` and (on Linux) `X11SurfaceHost`.
FrameBuilder talks to it as it does to any other Native resource —
directly, on the main thread, no compositor mediation. See §2.14 of the
Native API Completion Proposal for the subsystem definition.

Decoupling required one structural refactor: the `Visual` struct used
to own a `BackendRenderTargetContext` (the compositor's heavyweight
per-render-target wrapper). Under the move, `Visual` becomes a pure
Native primitive — position, size, platform handle, nothing more. The
compositor maintains a separate `Visual* → BackendRenderTargetContext`
side map (same idiom as the existing `PreCreatedResourceRegistry`
keyed-lookup pattern), which it consults at render time. The
compositor's render machinery is otherwise unchanged.

### The z-stack

Bottom → top, across every platform:

1. **WTK swap chain** — virtual widget content rendered by the
   compositor (everything that isn't a NativeViewHost or an overlay).
2. **NativeContent surface(s)** — one per `NativeViewHost::attach()`,
   ordered by `zOrderHint` so multiple hosts stack predictably.
3. **Overlay surface(s)** — one per `NativeViewHost` with
   `wantsOverlay()` true, always immediately above its own content.

The OS compositor (DComp / CoreAnimation / X server) does the actual
stacking. The visual tree publishes the order; each platform's tree
implementation maps it onto the platform-native ordering primitive
during `reconfigureContentNode`.

### Carve-out machinery: what stays, what goes

Some scaffolding from earlier carve-out exploration is in the codebase
today (`DrawOp::NativeContent`, `BackendNativeContentRegion`,
`BackendVisualTree::applyNativeContentCarveouts`, the
`pendingNativeContent_` recording in `BackendRenderTargetContext`).
The Native VisualTree migration changes what survives:

**Kept** — `DrawOp::NativeContent` stays in the display list. The
NativeViewHost widget emits one per attached content node on layout
resolve. The compositor's slice loop reads it for two purposes:
1. Rasterize it as alpha=0 into the WTK swap chain (rule 2).
2. AABB-cull sibling draw ops whose bounds fall inside it (rule 3).

**Removed** — `BackendNativeContentRegion`,
`BackendRenderTargetContext::pendingNativeContent_` (the recording slot),
`BackendVisualTree::applyNativeContentCarveouts` (the drain hook), and
the per-tree drain stubs in `DCVisualTree` / `MTLCALayerTree` /
`VKVisualTree`. None of this is needed once FrameBuilder reconfigures
the native surface directly. Net code volume drops — the drain
machinery was several hundred lines of plumbing across the backend.

---

## Native::VisualTree

The per-window compositor tree, lifted into the Native layer. Owned by
`AppWindow`; consulted by `FrameBuilder` (directly) and by the compositor
(read-only at render time, via a `Visual* → BackendRenderTargetContext`
side map).

```cpp
namespace OmegaWTK::Native {

class VisualTree {
public:
    // Per-window content-node registry.
    void registerContentNode(NativeContentNodePtr node);
    void unregisterContentNode(std::uint64_t hostId);

    /// Called from NativeViewHost::onLayoutResolved on the main thread.
    /// Translates `rectPixels` and `zOrderHint` into the platform's
    /// native ordering primitive synchronously — no compositor in the
    /// loop, no per-frame drain.
    void reconfigureContentNode(std::uint64_t hostId,
                                 const Composition::Rect & rectPixels,
                                 int zOrderHint);

    /// Compositor-side root present surface (WTK swap chain). Read-only
    /// from outside the compositor — created by AppWindow, handed to
    /// the compositor at render-target setup.
    Visual * rootVisual() const;

    virtual ~VisualTree() = default;
};

struct Visual {
    std::uint64_t hostId = 0;
    Composition::Rect rectPixels;
    int zOrderHint = 0;
    // Platform handle — IDCompositionVisual2* / CALayer* / Window XID —
    // exposed by the per-platform subclass. No compositor types here.
};

}
```

Per-platform implementations live in their existing per-OS Native
directories:

- `wtk/src/Native/macos/MTLVisualTree.{h,mm}` — wraps a `CALayer` tree
  rooted in the window's content view's layer; child content nodes are
  sibling/sub `CALayer`s (one per `MTLNativeContentNode`).
- `wtk/src/Native/win/DCVisualTree.{h,cpp}` — wraps an
  `IDCompositionTarget` + `IDCompositionVisual2` tree rooted on the
  window's HWND; child content nodes are sub-visuals.
- `wtk/src/Native/gtk/VKVisualTree.{h,cpp}` — wraps the toplevel `Window`
  + its child Windows allocated via `X11SurfaceHost`; "reconfigure" is
  `XMoveResizeWindow` + `XRestackWindows`.

The compositor's `BackendRenderTargetContext` (still in
`wtk/src/Composition/backend/`) binds to the tree's `rootVisual()` for
the WTK swap chain. Per-`Visual` render contexts the compositor needs
live in a side map owned by the compositor, keyed by `Visual*` — Native
types stay free of compositor dependencies.

---

## NativeContentNode

`NativeContentNode` is the platform-abstract handle attached via
`NativeViewHost::attach`. Per-platform subclasses expose the consumer's
drawable handle through type-safe downcast.

The base class is in the core. The per-platform subclasses split along
the library boundary: the core ships *video-surface* variants (used by
`VideoViewWidget` in the default lib); the gteview component ships
parallel *GPU-surface* variants. The two sets never coexist in one
class, and the core header set never names `CAMetalLayer` or a D3D12
render swap chain.

```cpp
// Core base — wtk/include/omegaWTK/Native/NativeContentNode.h
namespace OmegaWTK::Native {

class NativeContentNode {
public:
    std::uint64_t hostId() const noexcept;

    /// True after the node has been registered with a visual tree and
    /// has an underlying platform surface allocated.
    virtual bool isReady() const noexcept = 0;

    virtual ~NativeContentNode() = default;
};
using NativeContentNodePtr = SharedHandle<NativeContentNode>;

// Core video subclasses ─────────────────────────────────────────────────
// Used by VideoViewWidget (default lib). Expose only the video-surface
// accessor.

// macOS — wtk/src/Native/macos/MTLContentNode.h
class MTLNativeContentNode : public NativeContentNode {
public:
    AVSampleBufferDisplayLayer * displayLayer() const;
};

// Windows — wtk/src/Native/win/DCContentNode.h
class DCNativeContentNode : public NativeContentNode {
public:
    IDCompositionVisual2 * visual()         const;
    IDXGISwapChain1 *      videoSwapChain() const;     // bound via SetContent
};

// Linux X11 — wtk/src/Native/gtk/VKContentNode.h
class VKNativeContentNode : public NativeContentNode {
public:
    Display * display()     const;
    Window    videoWindow() const;     // child Window of the toplevel
};

}
```

```cpp
// Component GPU subclasses — wtk/components/gteview/src/Native/<platform>/
// Used by GTEViewWidget. Expose only the GPU-surface accessor. Inherit
// from Native::NativeContentNode so the visual tree's attach/detach
// machinery treats them uniformly with the core video subclasses.

namespace OmegaWTK::Components::GTEView {

// macOS — wtk/components/gteview/src/Native/macos/MTLGPUContentNode.h
class MTLGPUContentNode : public Native::NativeContentNode {
public:
    CAMetalLayer * metalLayer() const;
};

// Windows — wtk/components/gteview/src/Native/win/DCGPUContentNode.h
class DCGPUContentNode : public Native::NativeContentNode {
public:
    IDCompositionVisual2 * visual()          const;
    IDXGISwapChain1 *      renderSwapChain() const;    // D3D12 render swap chain
};

// Linux X11 — wtk/components/gteview/src/Native/gtk/VKGPUContentNode.h
class VKGPUContentNode : public Native::NativeContentNode {
public:
    Display * display()       const;
    Window    surfaceWindow() const;     // X11 child Window for VkSurfaceKHR
};

}
```

Factories split along the library boundary. The core's video factory
lives on `Native::VisualTree` itself — creation and registration are
atomic; the tree is the registry. The GPU factory lives in the gteview
component as a free function that operates on the same tree, so the core
header never names a GPU surface type.

```cpp
// Core — wtk/include/omegaWTK/Native/NativeVisualTree.h
class VisualTree {
    // ... existing ...

    /// Allocate a content node sized to `rect` and register it with this
    /// tree. The returned node's platform handle (AVSampleBufferDisplayLayer
    /// / DXGI video swap chain / X11 child Window) is created on the main
    /// thread. On Linux the node may be returned in a non-ready state if
    /// the X11 toplevel is not yet realized; resolution is deferred via
    /// X11SurfaceHost.
    NativeContentNodePtr createVideoContentNode(Composition::Rect rect);

    /// Lower-level entry the component layer uses to register a custom
    /// content node (e.g. the gteview component's GPU content node) into
    /// this tree. The node's platform handle has already been allocated
    /// by the caller; this hooks it into the platform tree and ordering.
    void attachContentNode(NativeContentNodePtr node, int zOrderHint);
    void detachContentNode(std::uint64_t hostId);
};
```

```cpp
// Component — wtk/components/gteview/include/omegaWTK/Components/GTEView/GPUContentNode.h
namespace OmegaWTK::Components::GTEView {

/// Allocate a GPU content node sized to `rect` and register it with the
/// given visual tree. Per-platform behind the scenes:
///   macOS:   constructs MTLGPUContentNode wrapping a CAMetalLayer
///   Windows: constructs DCGPUContentNode wrapping IDCompositionVisual2 +
///            IDXGISwapChain1 (D3D12 render swap chain)
///   Linux:   constructs VKGPUContentNode wrapping a child X11 Window
///            allocated via X11SurfaceHost
Native::NativeContentNodePtr createGPUContentNode(Native::VisualTree & tree,
                                                   Composition::Rect rect);

}
```

Consumer code reaches the tree through `AppWindow::visualTree()`.
`VideoViewWidget` calls `tree.createVideoContentNode(...)` directly.
`GTEViewWidget` calls
`OmegaWTK::Components::GTEView::createGPUContentNode(tree, rect)`. Both
hand the returned node to `NativeViewHost::attach`.

---

## NativeViewHost API

```cpp
class OMEGAWTK_EXPORT NativeViewHost : public Widget {
public:
    explicit NativeViewHost(Composition::Rect rect);
    ~NativeViewHost() override;

    /// Attach a native content node to this host. The host emits
    /// `DrawOp::NativeContent` for its rect on the next display-list
    /// rebuild; layout-resolved updates flow directly to the tree.
    void attach(Native::NativeContentNodePtr node);
    void detach();
    bool hasAttachedItem() const;

    /// Allocate (true) or release (false) the secondary surface used to
    /// render an overlay widget subtree above the native content. The
    /// surface lifetime is decoupled from `setOverlayWidget` — callers
    /// can pre-allocate during a quiet moment and assign / reassign the
    /// widget without re-allocating, or release the surface while
    /// keeping the widget reference for later. Default: false.
    void setWantsOverlay(bool wants);
    bool wantsOverlay() const;

    /// Attach a virtual widget subtree to render above the native
    /// content. The host owns the widget; mount/unmount tracks the
    /// host's lifecycle. The widget renders only when `wantsOverlay()`
    /// is also true; otherwise the reference is held but no surface is
    /// allocated. Setting nullptr unmounts the current widget without
    /// freeing the surface (controlled separately by setWantsOverlay).
    void setOverlayWidget(SharedHandle<Widget> overlay);
    Widget * overlayWidget() const;

protected:
    void onMount() override;
    void onLayoutResolved(const Composition::Rect & finalRectPx) override;
    //  ^^^^^^^^^^^^^^^^ calls visualTree->reconfigureContentNode synchronously.
};
```

### Flag and widget are orthogonal

| `wantsOverlay()` | `overlayWidget()` | Behavior                                         |
|------------------|-------------------|--------------------------------------------------|
| false            | nullptr           | No overlay surface, nothing to render.           |
| false            | non-null          | Widget is held but does not render.              |
| true             | nullptr           | Surface allocated but blank (clear-to-alpha=0).  |
| true             | non-null          | Widget renders into the overlay surface.        |

Toggling `setWantsOverlay(false)` while a widget is assigned does not
unmount the widget; it just stops rendering. Flipping back to true
resumes with the same widget. This lets a consumer pre-allocate the
surface during a quiet moment, swap widgets in/out without surface
churn, or release the platform resource without losing the assignment.

### Hit-testing

The overlay subtree is a child Widget rendered above the host. Pointer
events in the host's rect traverse the overlay subtree first via the
existing virtual hit-test traversal; unconsumed events fall through to
the host's own handlers, which the consumer (VideoViewWidget /
GTEViewWidget) can forward to its delegate.

### Layout

The overlay widget is mounted at the host's rect. Sub-region positioning
(e.g. a controls bar across the bottom 60px) is composed inside the
overlay subtree (`VStack { Spacer; ControlsBar }`). No per-overlay
sub-rect parameter — the API stays narrow.

---

## X11SurfaceHost (Linux only)

On Linux there is no GtkSocket / GtkOffscreenWindow / GtkDrawingArea
path — the NativeViewHost mechanism is X11-direct, and `VKVisualTree`
talks to the X server with `Xlib`/`xcb` directly. A small class, owned
by `AppWindow`, manages the toplevel `Window` XID and the lifetime of
every child Window allocated for NativeContent or overlay surfaces.

```cpp
namespace OmegaWTK::Native {

class X11SurfaceHost {
public:
    explicit X11SurfaceHost(Display * dpy);
    ~X11SurfaceHost();

    /// Called by AppWindow on `realize`. Resolves any deferred actions
    /// queued while the toplevel was unrealized.
    void onToplevelRealized(::Window toplevel);

    bool      isRealized() const;
    Display * display()    const;
    ::Window  toplevel()   const;

    ::Window createChildWindow(const Composition::Rect & rect);
    void destroyChildWindow(::Window child);
    void reconfigureChildWindow(::Window child,
                                 const Composition::Rect & rect,
                                 int zOrder);

    /// Defer a callback until the toplevel is realized. Runs immediately
    /// if already realized.
    void runOnRealize(std::function<void()> action);
};

}
```

Lifetime: `AppWindow` constructs the `X11SurfaceHost` in its ctor (display
handle in hand), passes it to the `VKVisualTree` at tree-construction
time, and tears it down in its dtor *after* the visual tree (which itself
tears down all `NativeContentNode`s). Child Windows are destroyed before
the toplevel.

Realization timing: a `createVideoContentNode` call (core) or a
`Components::GTEView::createGPUContentNode` call (component) before the
toplevel is realized returns a node in the non-ready state; the actual
child Window allocation is queued via `runOnRealize` and fires when the
realize signal arrives. This eliminates the "first frame at wrong DPI
then rebuild" sequence at startup.

§2.13 of the Native-API Completion Proposal commits to the toplevel
`Window` being owned by `AppWindow` directly (no `GtkDrawingArea`
indirection); the surface host is the lifetime gate that lets child
surfaces survive realize-timing edges.

---

## Shared prerequisite: Native VisualTree migration

Before any consumer phases (V1, G1) start, the visual tree moves out of
Composition and into Native. This is the structural change the rest of
the plan rests on.

### Steps

1. **File moves.**
   - `wtk/src/Composition/backend/VisualTree.h` → `wtk/include/omegaWTK/Native/NativeVisualTree.h` (abstract base in public header).
   - `wtk/src/Composition/backend/dx/DCVisualTree.{h,cpp}` → `wtk/src/Native/win/DCVisualTree.{h,cpp}`.
   - `wtk/src/Composition/backend/mtl/CALayerTree.{h,mm}` → `wtk/src/Native/macos/MTLVisualTree.{h,mm}` (renamed for symmetry — class becomes `MTLVisualTree`).
   - `wtk/src/Composition/backend/vk/VKLayerTree.cpp` → `wtk/src/Native/gtk/VKVisualTree.cpp` (the existing `VKVisualTree` rename from earlier lands here).

2. **Namespace shift.** `OmegaWTK::Composition::BackendVisualTree` → `OmegaWTK::Native::VisualTree`. Per-platform classes lose the `Backend` prefix where they had one. `NativeContentNode` is in `OmegaWTK::Native` (was tentatively `OmegaWTK::Composition` in earlier drafts).

3. **Decouple `Visual` from `BackendRenderTargetContext`.** The struct loses its `std::unique_ptr<BackendRenderTargetContext> renderTarget` field. The compositor adds a `Visual* → BackendRenderTargetContext` side map (in `Compositor` or `BackendResourceFactory`) and resolves the render context at use sites.

4. **Ownership shift.** `BackendResourceFactory::createVisualTreeForView`, `VisualTreeBundle`, `PreCreatedVisualTreeData`, `PreCreatedResourceRegistry` — all removed. `AppWindow` constructs `Native::VisualTree` in its ctor (alongside `NativeWindow` and, on Linux, `X11SurfaceHost`). Compositor reaches the tree through `AppWindow::visualTree()`.

5. **Remove the drain machinery.** `BackendNativeContentRegion`, `BackendRenderTargetContext::pendingNativeContent_`, `BackendVisualTree::applyNativeContentCarveouts`, and the per-tree drain overrides — all deleted. The slice-loop `case DrawOp::NativeContent:` branch shrinks to alpha-clear + AABB-cull bookkeeping; no recording.

6. **FrameBuilder integration.** `NativeViewHost::onLayoutResolved` calls `appWindow_->visualTree()->reconfigureContentNode(hostId, rectPx, zOrderHint)` synchronously. No queue, no deferred work.

### Verification

- WTK swap chain still presents normally on all three platforms (root visual path is unchanged structurally — the compositor still acquires `rootVisual()` and renders into it).
- No `applyNativeContentCarveouts` symbol anywhere in the tree (`grep` returns zero).
- Compositor builds against `Native::VisualTree*` (read-only), not against `BackendVisualTree`-owned render contexts.

This phase is a pure refactor — no new functionality lands. It exists so
V1 and G1 can build on a tree the rest of the system can talk to without
the compositor in the loop.

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
  ├── NativeViewHost (owns the carve-out + secondary overlay surface)
  │     └── NativeContentNode (per-platform):
  │           macOS:   MTLNativeContentNode wrapping AVSampleBufferDisplayLayer
  │           Windows: DCNativeContentNode wrapping IDCompositionVisual2 +
  │                    IDXGISwapChain1 (DXGI video swap chain)
  │           Linux:   VKNativeContentNode wrapping a child X11 Window
  │                    (consumer presents via VA-API / DRI3)
  │
  ├── VideoView (retained as internal controller, no longer a View)
  │     → manages playback/capture sessions
  │     → pushes decoded frames to the content node's surface
  │     → handles scale mode, delegate callbacks
  │
  └── overlay widget (optional, set via setOverlayWidget — subtitles,
      transport controls, scrubber, etc.)
```

### Design decisions

**VideoView becomes a non-View controller class.** The View inheritance
is replaced by ownership of a `NativeViewHost`. VideoView keeps all its
media logic (playback sessions, capture sessions, frame sink protocol)
but delegates visual presentation to the platform's native video surface.

**VideoFrameSink stays.** The `pushFrame` / `presentCurrentFrame` /
`flush` protocol is retained. Instead of drawing to a Canvas, the sink
implementation pushes frames to the content node's surface:
- macOS: `[displayLayer enqueueSampleBuffer:sampleBuffer]`
- Windows: present via the DXGI swap chain (or MF video presenter)
- Linux: VA-API / DRI3 present onto the X11 child Window

**Scale mode moves to the native surface.** `AspectFit` /
`AspectFill` / `Stretch` are implemented via the native layer's
gravity/transform rather than computing `destRect` in software.

**Overlays.** Subtitles, transport controls, scrub bar — anything that
needs to render above the video — go in a widget passed to
`setOverlayWidget`. `setWantsOverlay(true)` allocates the secondary
surface up front so the first overlay paint isn't gated on surface
creation.

### Phases

#### Phase V1: VideoViewWidget with NativeViewHost shell

Create `VideoViewWidget` as a `Widget` subclass that owns a
`NativeViewHost`. `Native::VisualTree::createVideoContentNode` returns a
`NativeContentNodePtr` backed by:
- macOS: `MTLNativeContentNode` wrapping an `AVSampleBufferDisplayLayer`
- Windows: `DCNativeContentNode` wrapping an `IDCompositionVisual2` with
  an `IDXGISwapChain1` (`SetContent`-bound) configured for video
- Linux: `VKNativeContentNode` wrapping a child `Window` allocated
  via `X11SurfaceHost::createChildWindow`

```
Files:
  wtk/include/omegaWTK/Widgets/VideoViewWidget.h        — public API
  wtk/src/Widgets/VideoViewWidget.cpp                   — implementation
  wtk/include/omegaWTK/Native/NativeContentNode.h       — base class
  wtk/src/Native/macos/MTLContentNode.{h,mm}            — macOS impl
  wtk/src/Native/win/DCContentNode.{h,cpp}              — Windows impl
  wtk/src/Native/gtk/VKContentNode.{h,cpp}              — Linux X11 impl
  wtk/src/Native/gtk/X11SurfaceHost.{h,cpp}             — X11 lifetime
```

Verification: the NativeViewHost appears at the correct position in the
widget tree. The native video surface is visible (a black or
test-pattern rectangle initially). Resizing the host moves the surface
synchronously — no one-frame lag at any scale.

#### Phase V2: Wire VideoFrameSink to native surface

Replace the Canvas-based `queueFrame()` / `presentCurrentFrame()` with
platform-specific frame presentation through the content node:

```
macOS:
  VideoFrame → CMSampleBuffer → AVSampleBufferDisplayLayer
                                (via MTLNativeContentNode::layer())

Windows:
  VideoFrame → ID3D11Texture2D → IDXGISwapChain1::Present
                                  (via DCNativeContentNode::swapChain())

Linux:
  VideoFrame → VA-API surface  → DRI3 present onto the X11 child Window
                                  (via VKNativeContentNode::window())
```

Hardware-decoded frames skip the CPU round-trip entirely — the decoder
output texture goes directly to the native surface. Software-decoded
frames upload to a GPU texture once and present.

Verification: video playback displays frames at correct timing through
the native surface. AspectFit/Fill/Stretch work via layer/visual/window
gravity.

#### Phase V3: Migrate VideoView API to VideoViewWidget

- `VideoView` class becomes internal (non-View controller).
- Public API moves to `VideoViewWidget`.
- `VideoViewDelegate` callbacks remain unchanged.
- Old `VideoView` header marked deprecated, forwards to `VideoViewWidget`.

Verification: all existing VideoView usage compiles and works through
VideoViewWidget. No Canvas blit path for video frames.

#### Phase V4: Remove Canvas blit path from VideoView

Delete `videoCanvas`, `drawImage`, `sendFrame` from the video path. The
Canvas dependency is removed entirely. VideoView (now the internal
controller) only manages sessions and pushes to the content node's
surface.

Verification: `VideoView.cpp` no longer includes `Canvas.h`. Clean
compile on all platforms.

---

## Part 2: OmegaGTEView → NativeViewHost

### Current proposal architecture (from `OmegaGTEView-Proposal.md`)

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
GTEViewWidget : Widget                          ← lives in
  │                                               OmegaWTKComponent_GTEView
  │                                               (wtk/components/gteview/)
  ├── NativeViewHost (owns the carve-out + secondary overlay surface)
  │     └── NativeContentNode (per-platform GPU surface, component-side):
  │           macOS:   MTLGPUContentNode wrapping CAMetalLayer
  │           Windows: DCGPUContentNode wrapping IDCompositionVisual2 +
  │                    IDXGISwapChain1 (D3D12 render swap chain)
  │           Linux:   VKGPUContentNode wrapping a child X11 Window
  │                    (GTE creates VkSurfaceKHR via
  │                     VkXlibSurfaceCreateInfoKHR against the child)
  │
  ├── GTEView (retained, owns render resources)
  │     → GECommandQueue (dedicated or shared)
  │     → depth/stencil textures
  │     → GTEViewDelegate receives onFrame callbacks
  │     → renders directly to the content node's GPU surface
  │
  ├── GTEViewContext (unchanged API)
  │
  └── overlay widget (optional, set via setOverlayWidget — debug HUD,
      gizmos, viewport controls, etc.)
```

### Design decisions

**GTEView renders directly to the native layer's GPU surface.** Instead
of rendering to an off-screen texture and blitting, GTEView acquires its
drawable / back buffer directly from the content node:
- macOS: `[metalLayer nextDrawable]` → render to drawable's texture
- Windows: `swapChain->GetBuffer()` → render to back buffer
- Linux: `vkAcquireNextImageKHR()` → render to swapchain image

This eliminates the off-screen texture, the Compositor blit, and the
fence-synced copy. The 3D content presents directly.

**Off-screen mode remains available.** For cases where the 3D content
needs custom compositing the OS layer can't express (transparency, complex
blending), the off-screen texture path from the original proposal is
retained as an opt-in mode (`GTEViewDescriptor::directPresent = true`
vs `false`). Direct present is the default for full-viewport 3D.

**The WTK Compositor is not involved in direct-present mode.** GTEView
manages its own present timing (display link or manual). The Compositor
only needs to know about the NativeViewHost's bounds for layout — it
doesn't touch the GPU surface.

**Overlays.** Debug HUDs, gizmo widgets, viewport mode toggles — any
WTK-rendered UI that overlays the 3D viewport — goes in a widget passed
to `setOverlayWidget`. The overlay surface composites above the GPU
swap chain via the OS compositor on every platform (DComp sibling
visual / sibling `CAMetalLayer` / secondary X11 child Window stacked
above).

### Phases

#### Phase G1: GPU content node factory

Lands in the gteview component (`wtk/components/gteview/`).
`OmegaWTK::Components::GTEView::createGPUContentNode(tree, rect)` returns
a `Native::NativeContentNodePtr` whose underlying platform handle is:

- macOS: `MTLGPUContentNode` wrapping a `CAMetalLayer` configured for
  direct rendering and attached to the `MTLVisualTree` (via
  `Native::VisualTree::attachContentNode`) as a sibling above the WTK
  swap-chain layer.
- Windows: `DCGPUContentNode` wrapping an `IDCompositionVisual2` with
  an `IDXGISwapChain1` (D3D12 render swap chain) bound via `SetContent`,
  attached to the `DCVisualTree`.
- Linux X11: `VKGPUContentNode` wrapping a child `Window` allocated
  via `X11SurfaceHost::createChildWindow`. GTE creates a `VkSurfaceKHR`
  via `VkXlibSurfaceCreateInfoKHR` against the child Window and runs
  its own swapchain.

```
Files (under wtk/components/gteview/):
  include/omegaWTK/Components/GTEView/GPUContentNode.h    — factory entry
  src/GPUContentNode.cpp                                   — common glue
  src/Native/macos/MTLGPUContentNode.{h,mm}                — macOS impl
  src/Native/win/DCGPUContentNode.{h,cpp}                  — Windows impl
  src/Native/gtk/VKGPUContentNode.{h,cpp}                  — Linux X11 impl
```

Verification: native GPU surface appears at the correct position.
`CAMetalLayer` / swap chain / Vulkan swapchain resizes with the host's
widget rect synchronously during layout. Configured behind
`OMEGAWTK_COMPONENT_GTEVIEW=ON`.

#### Phase G2: GTEView direct rendering

Modify `GTEView::executeFrame()` to acquire a drawable from the content
node's GPU surface instead of an off-screen texture render target:

```
executeFrame()
  → acquire drawable / back buffer from native surface
  → build GTEViewContext with drawable as color attachment
  → delegate->onFrame(context)
  → present drawable (no Compositor blit)
```

The `GTEViewDescriptor` gains a `directPresent` flag (default `true`).
When `false`, the original off-screen texture path is used.

Verification: a colored triangle renders directly to the native surface.
No Compositor blit. Resize updates the surface dimensions.

#### Phase G3: GTEViewWidget

Widget wrapper following the same pattern as `VideoViewWidget`:

```cpp
class GTEViewWidget : public Widget {
    NativeViewHost *hostView_;
    GTEView *gteView_;         // internal, manages GPU resources
    GTEViewDescriptor desc_;
public:
    explicit GTEViewWidget(Composition::Rect rect,
                           const GTEViewDescriptor & desc = {});
    GTEView & gteView();
    void setDelegate(GTEViewDelegate *delegate);
    void invalidateFrame();
    void startRenderLoop();
    void stopRenderLoop();

    // Overlay forwarded to the inner NativeViewHost.
    void setWantsOverlay(bool wants);
    void setOverlayWidget(SharedHandle<Widget> overlay);
};
```

```cpp
namespace OmegaWTK::Components::GTEView {

class GTEViewWidget : public Widget {
    // ... as above ...
};

}
```

```
Files (under wtk/components/gteview/):
  include/omegaWTK/Components/GTEView/GTEViewWidget.h   — public API
  src/GTEViewWidget.cpp                                  — implementation
```

`GTEView` (the internal controller that holds the render resources and
delegate) also moves into the component — it is not part of the default
WTK link. Its header lives under
`wtk/components/gteview/include/omegaWTK/Components/GTEView/GTEView.h`,
implementation in `src/GTEView.cpp`. Consumer code reaches the widget
via the component header:

```cpp
#include <omegaWTK/Components/GTEView/GTEViewWidget.h>
```

Verification: `GTEViewWidget` in an HStack with other widgets. 3D view
resizes correctly. Multiple instances render independently. App links
`OmegaWTKComponent_GTEView` explicitly via
`target_link_libraries(<app> PRIVATE OmegaWTKComponent_GTEView)`.

#### Phase G4: Display link integration

Wire the render loop to platform display links for frame pacing:
- macOS: `CVDisplayLink` callback → `executeFrame()`
- Windows: `IDXGIOutput::WaitForVBlank` on the swap-chain output
- Linux: timer thread synced to swap interval (or X Present extension /
  `_NET_WM_FRAME_DRAWN` where available)

Verification: rotating cube at 60 FPS with smooth delta-time rotation.
`stopRenderLoop()` / `startRenderLoop()` work correctly.

---

## Shared infrastructure

Both `VideoViewWidget` and `GTEViewWidget` go through the same machinery —
they differ only in which `NativeContentNode` subclass family supplies the
underlying platform handle (core video subclasses vs. the component's GPU
subclasses).

| Widget                              | Library                       | macOS surface              | Windows surface                  | Linux X11 surface              |
|-------------------------------------|-------------------------------|----------------------------|----------------------------------|--------------------------------|
| VideoViewWidget                     | core (`libOmegaWTK`)          | AVSampleBufferDisplayLayer | IDXGISwapChain1 (video) + DComp  | child Window (VA-API / DRI3)   |
| GTEViewWidget                       | `OmegaWTKComponent_GTEView`   | CAMetalLayer               | IDXGISwapChain1 (render) + DComp | child Window (VkXlibSurface)   |
| Overlay surface (either widget)     | core (NativeViewHost)         | CAMetalLayer (sibling)     | IDXGISwapChain1 + DComp visual   | secondary child Window         |

All native surfaces are registered with the per-window
`Native::VisualTree` through `attachContentNode` (the common entry both
the core video factory and the component GPU factory call into).
Layout-resolved updates from `FrameBuilder` reach the tree synchronously
via `reconfigureContentNode`, regardless of which subclass family the
node belongs to.

---

## Dependency graph

This plan's "Shared prerequisite" *is* §2.14 NativeVisualTree from
`Native-API-Completion-Proposal.md`. The two documents describe the
same architectural move from different angles: §2.14 specifies the
subsystem (file moves, class renames, Visual/RTC decoupling, drain
removal); this plan's prerequisite is the consumer-side view of
landing it. Nothing in Part 1 or Part 2 can start until §2.14 is in.

### Cross-document order

```
External prerequisites — Native-API-Completion-Proposal.md
─────────────────────────────────────────────────────────────
  §2.13 Linux/X11 direct surface ownership
   └── X11SurfaceHost  ─────────────────┐
                                          │  (Linux branch only)
                                          ▼
  §2.14 NativeVisualTree  ←─── needed on all three platforms
        │
        ▼
Internal — this plan
─────────────────────────────────────────────────────────────
  Phase 5 NativeViewHost shell  (already done)
        │
        ▼
  Shared prerequisite: Native VisualTree migration
        │                              (== §2.14 landing in code)
        │
        ├── Part 1: VideoView → NativeViewHost
        │     V1 → V2 → V3 → V4
        │
        └── Part 2: GTEView → NativeViewHost
              G1 → G2 → G3 → G4
```

### What blocks what

| Item                                      | Blocks                                                  |
|-------------------------------------------|---------------------------------------------------------|
| §2.13 X11SurfaceHost                      | §2.14 Linux branch (`VKVisualTree`)                     |
| §2.14 macOS + Windows                     | V1 / G1 on macOS + Windows (V1/G1 builds + verification) |
| §2.14 full (all platforms)                | Shared prerequisite → entire NativeViewHost plan        |
| Shared prerequisite                       | V1, G1                                                  |
| V1                                        | V2 → V3 → V4                                            |
| G1                                        | G2 → G3 → G4                                            |

Parts 1 and 2 are independent of each other and can proceed in parallel
once the shared prerequisite lands. Within each part, phases are
strictly sequential.

§2.14 itself can be staged per-platform: the macOS and Windows tree
moves are independent of §2.13 and can land first; the Linux move
waits on §2.13's `X11SurfaceHost`. The compositor-side work in §2.14
(Visual/RTC decoupling, drain removal, side-map addition) is uniform
across platforms and lands once.

### What does *not* block this plan

- **§2.2 NativeWindow** (DPI/state APIs) — independent. The
  `WindowScaleFactorChanged` event flows through `ViewRenderTarget` and
  doesn't touch the NativeContentNode path.
- **§2.3a Focus / cursor / tooltip** — virtual-layer concerns,
  independent.
- **§2.9 NativeScreen** — independent. Supersedes §2.13's interim
  primary-monitor anchoring but doesn't gate the NativeViewHost path.
- **§2.4 NativeApp / NativeTimer** — G4 display-link integration uses
  platform-specific APIs (`CVDisplayLink`, `WaitForVBlank`); the Linux
  swap-interval timer could leverage `NativeTimer` but isn't strictly
  required.
- **§2.6 NativeClipboard, §2.7 NativeDragDrop, §2.10 NativeAccessibility**
  — independent of the visual-tree work entirely.

---

## What stays the same

- `VideoViewDelegate` callback interface — unchanged
- `GTEViewDelegate` callback interface — unchanged
- `VideoFrameSink` protocol — implementation changes, interface stays
- `GTEViewContext` public API — unchanged
- Widget tree layout, hit testing, resize propagation — handled by the
  virtual widget tree as before; hit-testing into the overlay subtree
  uses the existing traversal order
- All other widgets — remain purely virtual, no native surfaces
- Compositor render machinery — slice loop, FrameRenderPass, tessellation,
  blur scratch, fence pool — structurally unchanged. Only the per-Visual
  render-context ownership pattern shifts (now a side map keyed by
  `Visual*` instead of `Visual::renderTarget`).
