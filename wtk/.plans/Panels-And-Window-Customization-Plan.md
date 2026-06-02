# Detached Panels & Window Customization Plan

This document proposes two related additions to the WTK UI layer:

- **Part A — Detached Panels (`NativePanel` / `AppPanel`):** a way to render compositor content in a *second native surface outside the `AppWindow`* — floating tool palettes, tear-off inspectors, and overlays (popovers, tooltips, menus) that need to escape the window's bounds.
- **Part B — Fully customizable `AppWindow` chrome:** app-drawn ("client-side decoration") title bars, menus, and minimize/maximize/close controls, uniform across all three platforms, with macOS able to keep its native traffic-light buttons (repositioned) for convenience.

Related documents:

- [UI-Engine-Roadmap.md](UI-Engine-Roadmap.md) — high-level engine roadmap
- [Native-API-Completion-Proposal.md](Native-API-Completion-Proposal.md) — Native abstraction gaps (§2.2 `NativeWindow`, §2.9 `NativeScreen`)
- [Native-View-Architecture-Plan.md](done/Native-View-Architecture-Plan.md) — the one-NativeItem-per-window virtual view model this plan builds on
- [Widget-Type-Catalog-Proposal.md](Widget-Type-Catalog-Proposal.md) / [Widget-Stub-Implementation-Plan.md](Widget-Stub-Implementation-Plan.md) — overlay widgets (`Popover`, `Tooltip`, `ContextMenu`, `Modal`, `PopupMenu`) that Part A can host

> **Architecture note (virtual view model):** Under the current design there is **exactly one `NativeItem` per window** — the root surface owned by the platform `NativeWindow` implementation — and the virtual `View`/`Widget` tree composites into it through `Composition::Layer`s. The full hosting chain is:
>
> ```
> AppWindow ── owns ──► Native::NWH (NativeWindow)         // platform surface
>     │                      └─ getRootView() ─► NativeItemPtr
>     ├─ ViewRenderTarget(native item)                     // GPU surface binding + DPI
>     ├─ WidgetTreeHost ── owns ──► Composition::Compositor // per-tree compositor
>     ├─ FrameBuilder                                       // window-scoped paint driver
>     └─ CompositorSurface                                  // lock-free frame mailbox
> ```
>
> A **panel is a second instance of this entire chain** bound to a *different* native surface. That is the whole point: it is a separate render target, not a layer inside the existing window. **This is categorically different from the `VideoView` "overlay,"** which is not a native overlay at all — `VideoView::queueFrame` simply appends a `DrawOp::Bitmap` into the *host window's* shared `DisplayList` during the paint pass, so it is always clipped to the window and shares the window's single compositor surface. A panel has its own `CompositorSurface`/`ViewRenderTarget` and can be positioned anywhere on screen.

---

## Completion status

| Section | Description | Status |
|---------|-------------|--------|
| **A0** | `NativeSurface` extraction (shared base for `NativeWindow` + `NativePanel`) | Planned |
| **A1** | `NativePanel` native impls + `AppPanel` UI wrapper + compositor hosting | Planned |
| **A2** | Panel positioning, parent association, non-activating panels | Planned |
| **A3** | Route overlay widgets (`Popover`/`Tooltip`/`ContextMenu`/`Modal`) through `AppPanel` | Planned |
| **B0** | `WindowChrome` mode enum + borderless plumbing (generalize `setEnableWindowHeader`) | Planned |
| **B1** | Draggable caption + resize regions (cross-platform native hit-test API) | Planned |
| **B2** | Cross-platform window controls; macOS native traffic-light repositioning | Planned |
| **B3** | App-drawn `MenuBar` + `PopupMenu` flyouts (consumes Part A) | Planned |
| **C1** | Shared global `GECommandQueue` on the Compositor; backends pull instead of create | Planned |
| **C2** | Batched per-tick `commitToGPU` + fence-isolated per-window present | Planned |

---

## 1. Current State

| Area | Today | Notes |
|------|-------|-------|
| `AppWindow` | UI wrapper (pimpl) over `Native::NWH` | `wtk/include/omegaWTK/UI/AppWindow.h` |
| `NativeWindow` | Abstract interface; `make_native_window(rect, emitter)` | `wtk/include/omegaWTK/Native/NativeWindow.h`; impls in `src/Native/{macos,win,gtk}/` (`CocoaAppWindow`, Win32, `GTKAppWindow`) |
| Secondary surfaces | **None** | No way to render a widget tree outside the `AppWindow`; `VideoView` rides the host DisplayList, `NativeViewHost` embeds foreign native views *inside* the window |
| Custom chrome | `AppWindow::setEnableWindowHeader(bool)` (non-mobile) | Win32 extends the frame into the client area via `DwmExtendFrameIntoClientArea` |
| Custom controls | `getExitButton()` / `getMaxmizeButton()` / `getMinimizeButton()` | **`#ifdef TARGET_WIN32` only** — returns `View`s; no macOS/GTK equivalent |
| Window controls (logic) | `minimize()` / `maximize()` / `restore()` / `close()` / `toggleFullscreen()` | Already public and cross-platform on `AppWindow` |
| Native menu | `AppWindow::setMenu(SharedHandle<Menu>)` | macOS binds to `NSApp.mainMenu` (app-global), Win32/GTK are per-window |

### Key gaps

1. **No detached render surface.** Every widget tree is bound to exactly one `AppWindow`. Overlays that must exceed window bounds (a dropdown near the window edge, a floating palette, a screen-anchored tooltip) cannot be expressed.
2. **Custom chrome is non-uniform and incomplete.** The only custom-control hooks are Win32-only `View` accessors. There is no borderless mode contract, no draggable-caption hit-testing, and no cross-platform control story. This violates the front-end-uniformity rule (a construct must be expressible on all backends or be rejected, not silently diverge).
3. **No client-side-decoration (CSD) drag/resize.** A borderless window currently cannot be moved or resized by the user, because the native layer has no way to ask the virtual layer "is this point a caption / resize border?"

---

## Part A — Detached Panels

### A.1 Concept

An `AppPanel` is a lightweight, top-level **secondary window** that hosts its own virtual widget tree and its own compositor binding. It is borderless and floating by default, can be associated with a parent `AppWindow` (for z-ordering and follow-on-move), and can optionally be **non-activating** (it never steals key-window focus — essential for tooltips, popovers, and tool palettes).

`AppPanel : NativePanel` mirrors `AppWindow : NativeWindow` exactly. Everything the panel needs to render — `WidgetTreeHost`, `FrameBuilder`, `ViewRenderTarget`, `CompositorSurface`, the window `LayerTree`, and `Compositor::registerWindowSurface` — is the **same machinery `AppWindow` already owns**, just bound to a different native surface.

> **Reuse callout:** The compositor-hosting plumbing in `AppWindow` (render-target propagation, frame-flush callback registration, surface registration) should be factored into a shared base/helper so `AppWindow` and `AppPanel` share one implementation rather than diverging. See A0.

### A.0 `NativeSurface` extraction (recommended)

Both `NativeWindow` and `NativePanel` need: a root `NativeItemPtr`, `getRect`/`setRect`, `scaleFactor`, `setOpacity`, `setFrameFlushCallback`/`requestFrameFlush`, `enable`/`disable`/`close`, and key-window state. Extract these into:

```cpp
// wtk/include/omegaWTK/Native/NativeSurface.h
namespace OmegaWTK::Native {
    INTERFACE NativeSurface {
        INTERFACE_METHOD NativeItemPtr getRootView() ABSTRACT;
        virtual Composition::Rect getRect() const = 0;
        virtual void setRect(const Composition::Rect & rect) = 0;
        virtual float scaleFactor() const = 0;
        virtual void setOpacity(float alpha) = 0;
        virtual void enable() = 0;
        virtual void disable() = 0;
        virtual void close() = 0;
        void setFrameFlushCallback(std::function<void()> cb);
        virtual void requestFrameFlush();
        virtual ~NativeSurface() = default;
    };
}
```

`NativeWindow` keeps its window-only surface (menu, header, min/max/fullscreen, resizable, min/max size); `NativePanel` adds panel-only configuration. The UI-side compositor host (`WindowCompositingHost`, also extracted) drives any `NativeSurface`.

> **Alternative (lower effort):** make `NativePanel : public NativeWindow` and leave window-only methods as no-ops on panels. Cheaper now, but saddles panels with a menu/header API that does not apply and keeps two copies of the hosting plumbing. The extraction is preferred on uniformity/reuse grounds — flagged in Open Questions.

### A.1.1 `NativePanel` (Native layer)

```cpp
// wtk/include/omegaWTK/Native/NativePanel.h
namespace OmegaWTK::Native {
    enum class PanelLevel : uint8_t { Normal, Floating, ModalPanel, PopupMenu, Tooltip };

    struct PanelConfig {
        PanelLevel level = PanelLevel::Floating;
        bool activating = false;   // false => never becomes key window
        bool shadow     = true;
        bool resizable  = false;
    };

    INTERFACE NativePanel /* : public NativeSurface */ {
        INTERFACE_METHOD NativeItemPtr getRootView() ABSTRACT;
        virtual void setParentSurface(NativeSurface * parent) = 0; // ownership + follow
        virtual void orderFront() = 0;
        virtual void orderBack() = 0;
        // ... NativeSurface members (rect, scale, opacity, frame flush) ...
        virtual ~NativePanel() = default;
    };
    typedef SharedHandle<NativePanel> NPP;
    NPP make_native_panel(Composition::Rect & rect, const PanelConfig & cfg, NativeEventEmitter * emitter);
}
```

**Platform mapping:**

| Platform | Detached surface |
|----------|------------------|
| macOS | `NSPanel` (or borderless `NSWindow`) with `NSWindowStyleMaskNonactivatingPanel` for non-activating; floating `NSWindowLevel`; parented via `-[NSWindow addChildWindow:ordered:]` |
| Win32 | `WS_POPUP` HWND, `WS_EX_TOOLWINDOW` (+ `WS_EX_NOACTIVATE` for non-activating); owned via owner HWND; DirectComposition/swap chain on the panel HWND |
| Linux/GTK | `GtkWindow` with utility/popup type hint + `gtk_window_set_transient_for`; under Wayland an `xdg-popup`/subsurface for anchored popups |

### A.1.2 `AppPanel` (UI layer)

```cpp
// wtk/include/omegaWTK/UI/AppPanel.h
namespace OmegaWTK {
    class OMEGAWTK_EXPORT AppPanel : public Native::NativeEventEmitter {
        struct Impl;
        Core::UniquePtr<Impl> impl_;
    public:
        OMEGACOMMON_CLASS("OmegaWTK.AppPanel")
        explicit AppPanel(Composition::Rect rect, const Native::PanelConfig & cfg = {});

        void setRootWidget(WidgetPtr widget);

        void present();                          // order front + display
        void dismiss();                          // hide (retain tree) or close

        void setParentWindow(AppWindow * parent);
        void setScreenPosition(Composition::Point2D screenOrigin);
        void anchorToWidget(Widget * anchor, /* edge + offset */);  // A2

        void setRect(const Composition::Rect & rect);
        Composition::Rect getRect() const;
        void setOpacity(float alpha);

        void requestFrame();
        void flushFrame();
        ~AppPanel() override;
    };
}
```

`AppPanel` registers with `AppWindowManager` (or a parallel panel registry) so the platform run loop drives its coalesced frame flush exactly like an `AppWindow`.

### A.2 Positioning, parenting, activation

- **Absolute placement:** `setScreenPosition` in screen coordinates. Correct multi-monitor placement depends on **`NativeScreen` (§2.9 of the Native API proposal, *Not started*)** — until then, panels anchor to the parent window's screen. Note this dependency.
- **Parent association:** `setParentWindow` makes the panel a child surface — it follows the parent on move/minimize and orders above it.
- **Non-activating panels:** `PanelConfig::activating = false` keeps keyboard focus on the owner; key events still route through the parent's virtual focus manager.

### A.3 Overlay widgets on panels (consumes Part A)

The overlay widgets in [Widget-Stub-Implementation-Plan.md](Widget-Stub-Implementation-Plan.md) Phase 6 (`Popover`, `Tooltip`, `ContextMenu`, `Modal`, `PopupMenu`) currently assume an in-window `ZStack` host, which clips them to the window. With `AppPanel` available, each overlay gains an optional **panel-hosted mode**: present its content in an `AppPanel` so it can extend past the window edge. The in-window `ZStack` path stays as the default (no extra native surface); panel-hosting is opt-in per overlay (e.g. a long menu near the screen edge).

### Exit criteria (Part A)

- An `AppPanel` renders an arbitrary widget tree in a window separate from any `AppWindow`, with its own DPI scale.
- A panel positioned partly outside the parent window's bounds renders fully (proving it is a distinct surface, not a clipped overlay).
- A non-activating panel does not steal key-window focus from its parent.
- Closing the parent `AppWindow` tears down its child panels.

---

## Part B — Fully Customizable `AppWindow`

### B.1 Goal

Let an application draw its **entire** window chrome — title bar, menu bar, and window controls — as widgets, uniformly on all platforms. macOS keeps its native traffic-light buttons (for convenience and platform-correct behavior) but can **reposition** them into an app-specified rect.

> **Uniformity principle:** The existing `#ifdef TARGET_WIN32` control accessors are replaced by a single cross-platform API. Platform differences (macOS native traffic lights vs. app-drawn buttons) are expressed through a **chrome-mode enum**, never through platform-only methods. A construct that cannot be expressed on all three backends is rejected at the API, not left to diverge per-platform.

### B.0 Chrome mode

```cpp
// AppWindow.h
enum class WindowChrome : uint8_t {
    Native,                 // OS-drawn title bar + controls (current default)
    Custom,                 // borderless; app draws title bar, menus, and controls
    CustomWithNativeControls // app draws title bar; OS controls repositioned into appRect (macOS traffic lights)
};

void setWindowChrome(WindowChrome chrome);
```

`Custom`/`CustomWithNativeControls` generalize today's `setEnableWindowHeader(false)`: borderless plumbing already exists on Win32 (`DwmExtendFrameIntoClientArea`) and must be filled in for macOS (`NSWindowStyleMaskFullSizeContentView` + transparent titlebar) and GTK (`gtk_window_set_decorated(false)` or `gtk_window_set_titlebar`).

### B.1 Draggable caption + resize regions (the hard part)

When the OS no longer draws the title bar, the native layer must ask the virtual layer which points are draggable/resizable. New cross-platform `NativeWindow` API:

```cpp
struct ClientDecorationRegions {
    Composition::Rect caption;                       // drag-to-move region
    OmegaCommon::Vector<Composition::Rect> excluded; // buttons/menus inside caption that should NOT drag
    float resizeBorderThickness = 6.f;               // hit zone for edge/corner resize
};
virtual void setClientDecorationRegions(const ClientDecorationRegions & regions) = 0; // NativeWindow
```

The app recomputes and pushes these whenever its title-bar widgets relayout.

| Platform | Implementation |
|----------|----------------|
| Win32 | Handle `WM_NCHITTEST` → `HTCAPTION` in the caption rect, `HTLEFT/HTTOP/HTTOPLEFT/...` in resize borders, `HTCLIENT` in excluded rects |
| macOS | `NSWindow.movableByWindowBackground` for simple cases, or `-[NSView mouseDownCanMoveWindow]` / `-[NSWindow performWindowDragWithEvent:]` keyed off the caption rect; resize via `NSWindowStyleMaskResizable` |
| GTK | `gtk_window_begin_move_drag` / `gtk_window_begin_resize_drag` on button-press inside the caption/border regions; or full CSD via `gtk_window_set_titlebar` |

### B.2 Window controls

Two complementary paths:

1. **App-drawn buttons (primary, fully uniform):** the app builds ordinary `Button` widgets and wires them to the **already-public** `AppWindow::minimize()` / `maximize()` / `restore()` / `close()` / `toggleFullscreen()`. No new native objects required. The three Win32-only `getExitButton()`/`getMaxmizeButton()`/`getMinimizeButton()` accessors are **deprecated and removed** in favor of this path.
2. **Native control repositioning (`CustomWithNativeControls`, macOS-centric):** keep the OS-drawn controls but move them into an app-specified rect. On macOS this maps to `-[NSWindow standardWindowButton:]` frame adjustment (the "traffic lights can be moved around" case). On Win32/GTK this mode falls back to app-drawn buttons (there is no equivalent movable native caption-button object), expressed through the enum rather than a platform `#ifdef`.

```cpp
void setNativeControlRect(const Composition::Rect & rect); // honored in CustomWithNativeControls mode
```

### B.3 Custom menus

macOS menus are app-global (`NSApp.mainMenu`), so "custom menus" means **app-drawn**, not native-repositioned:

- A new `MenuBar` widget hosts horizontal menu titles inside the custom title bar; clicking one opens a `PopupMenu` / `ContextMenu` (the in-view overlay widgets from the catalog). For menus that overflow the window edge, the flyout is hosted in an **`AppPanel`** (Part A).
- `AppWindow::setMenu(SharedHandle<Menu>)` (native menu) remains valid and is the default under `WindowChrome::Native`. The two are mutually exclusive per window.

### Exit criteria (Part B)

- A borderless `AppWindow` can be moved by dragging an app-defined caption region and resized from its edges, on all three platforms.
- App-drawn min/max/close buttons drive the corresponding window operations.
- On macOS, `CustomWithNativeControls` repositions the traffic lights into the app's rect and they remain functional.
- An app-drawn `MenuBar` opens `PopupMenu` flyouts; an edge-overflowing menu renders fully via `AppPanel`.

---

## Part C — Shared Global `GECommandQueue`

> **Terminology reconciliation:** The request was phrased as "the Compositor(s) from each Window." In the current code there is **one** global `Compositor` — a static singleton, `globalCompositor()` in `wtk/src/UI/WidgetTreeHost.cpp` — shared by every `WidgetTreeHost`/`AppWindow`, with a single `CompositorFrameWorker` thread. What is actually **per-window** is the `GECommandQueue`: each window's `BackendRenderTargetContext` owns its own queue, created in the backend visual-tree builder via `gte.graphicsEngine->makeCommandQueue(64)`. This section is therefore about collapsing those **N per-window queues into one shared queue**, owned centrally. (If the longer-term intent is *also* to move to one `Compositor` per window, the design below still holds — the queue is owned above the Compositor layer, so it is shared either way.)

### C.1 Current state

- **Compositors:** 1 global (`globalCompositor()`); one `CompositorFrameWorker` thread.
- **Queues:** N — one per window. Created in `backend/vk/VKLayerTree.cpp`, `backend/dx/DCVisualTree.cpp` (and the Metal builder) with `gte.graphicsEngine->makeCommandQueue(64)`, stored in `BackendRenderTargetContext::commandQueue_` (`wtk/src/Composition/backend/RenderTarget.h`), and bound to the swapchain via `makeNativeRenderTarget(desc, presentQueue)`.
- **Submission:** the single frame-worker thread drains all windows (`Compositor::drainWindowSurfaces` → `renderCompositeFrame` → `BackendRenderTargetContext::commit()` → `commandQueue_->commitToGPU()`). **Exactly one thread submits to every queue**, with no locking.

### C.2 Motivation

1. **No parallelism is being bought.** N queues exist but a single frame-worker thread feeds them serially, so the extra queues add driver/memory overhead without any concurrency. Detached panels (Part A) make this worse — every panel would otherwise spin up yet another GPU queue.
2. **Idiomatic backend usage.** Metal, D3D12, and Vulkan all favour a **small, fixed number of queues** with many swapchains/render targets attached. One shared queue is the normal pattern; one-queue-per-surface is the unusual one.
3. **Frame batching.** A single submission point lets all windows' command buffers commit in **one `commitToGPU()` per worker tick** instead of N, improving GPU scheduling and multi-window frame coherency.
4. **Already single-threaded.** Because submission is already confined to one frame-worker thread, sharing a queue needs **no new locking** on the hot path — the hard part (thread affinity) is already satisfied.

### C.3 Design

- **Ownership:** add a lazily-created `SharedHandle<OmegaGTE::GECommandQueue>` owned by the global `Compositor` (the natural home — it is already the singleton submission owner). Accessor `Compositor::sharedCommandQueue()` creates it on first use from `gte.graphicsEngine->makeCommandQueue(N)`. Lifetime is app-global, outliving every window/panel swapchain.
- **Backend builders pull, not create.** `VKLayerTree`, `DCVisualTree`, and the Metal builder stop calling `makeCommandQueue(64)`; they pass `Compositor::sharedCommandQueue()` into `makeNativeRenderTarget(desc, queue)` and `BackendRenderTargetContext`. This is a **uniform change across all three backends** — no backend keeps the per-window path.
- **Swapchain binding stays valid.** D3D12 binds the queue at swapchain creation (`CreateSwapChainForHwnd(queue, …)`); a shared, app-lifetime queue is exactly what that wants. Vulkan and Metal present multiple surfaces from one queue without issue.
- **Per-window present isolation via fences.** Per-window presentation keeps its **own** `GEFence` (the existing `submitCommandBuffer(buffer, signalFence)` / `waitForFence` surface). Sharing the *submission* queue does **not** share per-window present sync.
- **Batched commit (recommended):** record all windows' command buffers during `drainWindowSurfaces`, then issue a single `commitToGPU()` at the end of the tick instead of one commit per `BackendRenderTargetContext::commit()`. Keep `commitToGPUAndWait` off the hot path so a slow present on one window cannot head-of-line-block the others.
- **Sizing:** the shared queue's `maxBufferCount` must cover all live surfaces (≈ `perSurfaceBuffers × maxConcurrentSurfaces`, currently 64 per surface). Either size generously up front or make it configurable — flagged as a tuning parameter.

### C.4 Risks / caveats

- **Vulkan present-family support.** A shared queue must belong to a queue family that can present to **every** window/panel surface. On typical desktop GPUs the graphics family presents to all surfaces, but a device that splits graphics/present families needs a fallback (a shared queue per present family, or a dedicated present queue). Verify during the Vulkan pass.
- **Head-of-line blocking.** Any blocking wait (`commitToGPUAndWait`) on the shared queue stalls every window. The hot path must use non-blocking commit + fences only.
- **Future multi-threaded submission.** If later work submits from a second thread (e.g. async resource upload), the shared queue gains a concurrency requirement and will need locking or a separate upload queue. Document this as a forward constraint, not a present-day need.

### Exit criteria (Part C)

- All three backend builders obtain their queue from `Compositor::sharedCommandQueue()`; a `grep` shows no remaining per-window `makeCommandQueue` in the visual-tree builders.
- Two `AppWindow`s plus one `AppPanel` render concurrently through a single `GECommandQueue` with correct per-window present (no cross-window tearing or stalls).
- One `commitToGPU()` per worker tick (verified by instrumentation), down from one-per-window.

---

## File Change Summary

**New (Native):**

- `wtk/include/omegaWTK/Native/NativeSurface.h` — shared surface base (A0)
- `wtk/include/omegaWTK/Native/NativePanel.h` + `src/Native/NativePanel.cpp`
- `src/Native/macos/CocoaAppPanel.{h,mm}`, `src/Native/win/Win32AppPanel.{h,cpp}`, `src/Native/gtk/GTKAppPanel.cpp`

**New (UI):**

- `wtk/include/omegaWTK/UI/AppPanel.h` + `src/UI/AppPanel.cpp`
- `src/UI/WindowCompositingHost.{h,cpp}` — extracted compositor-hosting plumbing shared by `AppWindow` + `AppPanel`
- `MenuBar` widget — `wtk/include/omegaWTK/Widgets/Navigation.h` + `src/Widgets/Navigation.cpp` (B3)

**Modified:**

- `Native/NativeWindow.h` — derive from `NativeSurface`; add `setClientDecorationRegions`
- `UI/AppWindow.h` / `src/UI/AppWindow.cpp` — `WindowChrome` enum, `setWindowChrome`, `setNativeControlRect`, `setClientDecorationRegions` pass-through; **remove** the `#ifdef TARGET_WIN32` button accessors; reuse `WindowCompositingHost`
- `Composition/Compositor.*` — confirm `registerWindowSurface` supports >1 surface per app (multiple windows already imply this; verify for panels); add `sharedCommandQueue()` owner (Part C)
- Platform `NativeWindow` impls — borderless mode + hit-test handlers for all three platforms

**Modified (Part C — shared queue):**

- `wtk/src/Composition/Compositor.{h,cpp}` — own + lazily create `sharedCommandQueue()`; optional batched per-tick `commitToGPU`
- `wtk/src/Composition/backend/RenderTarget.h` — `BackendRenderTargetContext` receives the shared queue (no behavioural change to its own `commit()` beyond the source of the queue)
- `wtk/src/Composition/backend/vk/VKLayerTree.cpp`, `backend/dx/DCVisualTree.cpp`, and the Metal visual-tree builder — pull `Compositor::sharedCommandQueue()` instead of `gte.graphicsEngine->makeCommandQueue(64)` (uniform across all three backends)

**Build:** sources are globbed per directory in `wtk/CMakeLists.txt` (`src/Native/*.cpp`, `src/Native/{win,macos,gtk}/*`, `src/UI/*.cpp`, `src/Widgets/*.cpp`); new files are picked up automatically.

---

## Platform verification status

> **Unverified backends:** This plan is authored on **Linux (Clang)**. The **GTK/Linux** path is the only one locally compile/run-verifiable. The **macOS (Cocoa/`NSPanel`/traffic-light)** and **Win32 (`WS_POPUP`/`WM_NCHITTEST`/DWM)** paths are **compile- and run-unverified off-platform** and must be validated on their host OSes before the corresponding phases are marked done. Platform-specific API names (e.g. `NSWindowStyleMaskNonactivatingPanel`, `performWindowDragWithEvent:`, `standardWindowButton:`) are proposed targets, not verified call sites.
>
> The Part C shared-queue change maps onto the GPU backends the same way: the **Vulkan** path (`VKLayerTree`) is locally verifiable on Linux; the **D3D12** (`DCVisualTree`, `CreateSwapChainForHwnd` queue binding) and **Metal** builders are **unverified off-platform**.

---

## Open Questions

1. **`NativeSurface` extraction vs. `NativePanel : NativeWindow`.** Recommended: extract `NativeSurface` (uniformity + single hosting implementation). Cheaper alternative: inherit `NativeWindow` and no-op the window-only methods. Which do we commit to before A1?
2. **Panel registry.** Do panels live in `AppWindowManager` alongside windows, or in a separate `AppPanelManager`? Lifetime is tied to a parent window in the common case but standalone panels (no parent) also need an owner for the run loop.
3. **`NativeScreen` dependency.** Absolute/multi-monitor panel placement (A2) really wants `NativeScreen` (§2.9, not started). Do we land panels with parent-screen-only placement first and layer multi-monitor on later, or pull `NativeScreen` forward as a prerequisite?
4. **Wayland anchored popups.** `xdg-popup` has strict anchoring/grab semantics (a popup must be anchored to a parent surface and is dismissed by the compositor). Does the GTK panel backend expose enough control, or do popups need a distinct "anchored" code path from free-floating palettes?
5. **macOS menu ownership.** Since the native menu is app-global, should `WindowChrome::Native` + multiple windows continue to share one `NSApp.mainMenu` (current behavior), and does the app-drawn `MenuBar` fully replace it under `Custom`, or coexist?
6. **Caption hit-testing source of truth.** Should `setClientDecorationRegions` be a push API (app recomputes on relayout) or a pull callback (`std::function<HitResult(Point)>` the native layer invokes per `WM_NCHITTEST`)? Push is simpler and avoids cross-thread virtual-tree access from the native message pump; pull is more precise for irregular regions.
7. **Shared-queue ownership.** Does the shared `GECommandQueue` live on the `Compositor` (recommended — it is the single submission owner) or beside the global `gte` handle in `Core/GTEHandle.h`? The former keeps queue lifetime tied to the compositor; the latter survives compositor teardown.
8. **Vulkan present-family fallback.** What is the fallback when one device exposes separate graphics/present queue families and a single shared queue cannot present to all surfaces — one shared queue per present family, or a dedicated present queue handed to `makeNativeRenderTarget`?
9. **Commit granularity.** Batched per-tick `commitToGPU` (C2) changes when GPU work is flushed relative to per-window present. Do any current present-timing/frame-pacing assumptions (see [Frame-Pacing-Plan.md](Frame-Pacing-Plan.md)) depend on per-window commit ordering?
