# Native API Completion Proposal

This document proposes the API additions and changes needed to bring the WTK Native abstraction layer to a production-complete state. It is organized by subsystem, starting with the highest-impact gaps.

> **Architecture note (virtual view model):** Views are now purely virtual — the per-View NativeItem was removed in Phase 3. There is **exactly one NativeItem per window**: the root surface owned by the platform `AppWindow` (`CocoaAppWindow`/`Win32AppWindow`/`GTKAppWindow`), which the virtual view tree composites into via `Composition::Layer`s. Every "per-view" OS feature must therefore be re-thought:
>
> - **OS-level features that map to "the window"** (cursor sink, window opacity, key-window state) belong on `NativeWindow` / `AppWindow`. There is no per-NativeItem version because there is no per-View NativeItem.
> - **Per-view features** (which view is focused for keyboard input, which view's cursor shape is active, which widget's tooltip is showing) live entirely in the virtual layer — `View` / `Widget` / `WidgetTreeHost` — and a dispatcher commits the *currently active* value to the single root NativeItem.
> - Hit-testing is already covered by `View::containsPoint`. The hover dispatcher that drives `CursorEnter`/`CursorExit` virtually is the same machinery that will drive cursor-shape and tooltip activation.
>
> The original §2.3 (NativeItem additions for focus / cursor / tooltip / opacity / hit-test) is therefore obsolete and has been re-routed below.

---

## Completion status

| Section | Description | Status |
|---------|-------------|--------|
| **2.1 NativeEvent** | Complete event model (ModifierFlags, mouse/key params, KeyCode, new event types, multi-receiver emitter) | **Done** |
| **2.5 NativeTheme** | ThemeAppearance, populated ThemeDesc (colors, typography), `queryCurrentTheme()` on macOS & Win32 | **Done** |
| **2.11 NativeNote / NotificationCenter** | Permissions, scheduling, callbacks, removal, categories — macOS UN, Win32 ToastNotificationManager, GTK libnotify | **Done** |
| **2.12 NativeMenu / Menu** | Shortcuts, check/radio items, contextual menus, dynamic updates, validation delegate — macOS NSMenu, Win32 HMENU, GTK GtkMenu | **Done** (icons deferred) |
| **2.13 Linux/X11 direct surface ownership** | WTK owns its X11 surfaces directly. Root NativeItem falls through to the GTKAppWindow's toplevel `Window`; NativeViewHost child surfaces are X11 child Windows managed by `X11SurfaceHost` (no `GtkDrawingArea`, no `GtkSocket`, no `GTKItem` in the embedding path). | **Source-complete, compile-verified.** Run-verified: BasicAppTest comes up, `ResizeSession Completed w=600 h=500` flows through configure-event, no warnings. - **Done (Verified by Developer seperately)**|
| 2.14 NativeVisualTree | Per-window compositor tree moves from Composition → Native. Owned by `AppWindow`, called directly by `FrameBuilder` during layout (no compositor in the loop). Decouples `Visual` from `BackendRenderTargetContext`. Removes the carve-out drain machinery. Includes the `VKFallbackVisualTree` → `VKVisualTree` rename, `MTLCALayerTree` → `MTLVisualTree` rename, and the `CALayerTree`/`DCVisualTree`/`VKLayerTree.cpp` file moves into `wtk/src/Native/{macos,win,gtk}`. | Not started |
| 2.2 NativeWindow | Full window control (minimize/maximize/fullscreen, scaleFactor, opacity, cursor sink, DPI scale change events) | **Mostly done.** Header surface complete (`NativeWindow.h`), `WindowScaleFactorChanged` event type + params in `NativeEvent.h`, all three backends (`CocoaAppWindow.mm`, `WinAppWindow.cpp`, `GTKAppWindow.cpp`) implement every method and emit the scale-change event on the right native trigger. **Remaining (owned by [UIView-Render-Redesign-Plan Phase F](UIView-Render-Redesign-Plan.md#phase-f-follow-up--window-resize-always-relayouts--repaints-no-resize-opt-in)):** the `AppWindow` consumer that subscribes to `WindowScaleFactorChanged`, runs the three-step coupling (renderTarget resize → Compositor scale propagation → full-tree repaint that re-rasterizes every element, not just text), and shares its implementation with the resize handler. §2.2 ships the native-side surface and emit; Phase F owns the cross-platform consumer. Plus the `ViewRenderTarget::scaleChanged()` rebuild trigger (cross-plan to `DPI-Aware-Text-Plan.md`'s per-monitor-DPI section), which Phase F's handler will call. |
| ~~2.3 NativeItem~~ | **Obsolete under virtual view model — no new NativeItem APIs.** See §2.3 below. | Removed |
| 2.3a View / Widget / TreeHost focus + cursor + tooltip | Virtual focus manager, declarative cursor shape, virtual tooltip popups | Not started |
| 2.4 NativeApp | Delegate, command-line args, timers | Not started |
| 2.6 NativeClipboard | New subsystem | Not started |
| 2.7 NativeDragDrop | New subsystem | Not started |
| **2.8 NativeDialog** | Alert dialog (Result), file filters + multi-select, GTK backend | **Done** |
| **2.9 NativeScreen** | **Cross-platform screen-info API — geometry, DPI, refresh rate / vsync source, screen-targeting for `AppWindow`.** Owns the per-screen properties that other sections consume (`§2.2 NativeWindow::scaleFactor` forwards here; Phase F's DPI consumer reads `currentScreen().scaleFactor`; Phase H's `FramePacer` binds to `displayLinkForScreen(currentScreen())`). | Not started — **lands first** (prerequisite for §2.2's DPI consumer, Phase F, Phase H). |
| 2.10 NativeAccessibility | New subsystem (stub) | Not started |

---

## 1. Current State Summary

| Header | Purpose | Platform coverage |
|--------|---------|-------------------|
| `NativeApp.h` | App lifecycle, event loop | macOS, Win32, GTK |
| `NativeWindow.h` | Window: display, menu, title, close | macOS, Win32, GTK |
| `NativeItem.h` | View/HWND tree: add/remove children, events, scroll | macOS, Win32, GTK |
| `NativeEvent.h` | Event types and emitter/processor | macOS, Win32, GTK |
| `NativeMenu.h` | Menu and menu item abstraction | macOS, Win32, GTK |
| `NativeDialog.h` | File dialog, note dialog | macOS, Win32; GTK missing |
| `NativeNote.h` | System notification (NoteCenter) | macOS, Win32, GTK |
| `NativeTheme.h` | Theme query and observer | macOS, Win32, GTK |

> **Note (macOS):** `NativeMenu` on macOS is bound to the application (`NSApp.mainMenu`), not to individual windows. On Win32 and GTK, menus are per-window. The current `AppWindow::setMenu` API hides this distinction. A future revision should consider moving menu ownership to `NativeApp` for macOS targets so that menu changes when switching windows are handled correctly.

### Key Gaps

1. **NativeWindow** — No minimize/maximize/fullscreen API; `rect` and `eventEmitter` are behind `#ifdef TARGET_MACOS`; no DPI/scale query; no `isVisible`; no window-level opacity; no per-window cursor sink.
2. **Virtual view tree (View / Widget / WidgetTreeHost)** — No focus manager (so keyboard events have nowhere to route), no declarative cursor shape on View, no tooltip on Widget. These can only exist in the virtual layer because there is no per-view NativeItem.
3. **NativeApp** — No command-line argument access, no open-URL/open-file handler, no delegate.
4. **NativeDialog** — No confirmation dialog (OK/Cancel/Yes/No); `NativeNoteDialog` returns no result; no file-type filters.
5. **Missing subsystems** — Clipboard, drag-and-drop, timers, accessibility stubs, screen/display info.
6. **GTK platform** — Missing Dialog, Theme, and full Event implementations; clipboard and DnD also absent.

---

## 2. Proposed Extensions

### 2.1 NativeEvent — Complete Event Model ✅ Done (one delta pending)

Implemented. See `NativeEvent.h`: `ModifierFlags`, `MouseEventParams`, `CursorMoveParams`, `KeyCode`, `KeyDownParams`/`KeyUpParams`, all new event types (`CursorMove`, `DragBegin`/`Move`/`End`, `FocusGained`/`FocusLost`, `GesturePinch`/`Pan`/`Rotate`, `AppActivate`/`AppDeactivate`, full window resize sequence), multi-receiver `NativeEventEmitter`.

**Delta — `WindowScaleFactorChanged`:** one new event type lands as part of §2.2 (DPI scale change handling). The params struct + enum case below extend `NativeEvent.h` but the dispatch wiring is platform-side and is owned by §2.2. Adding the type itself is non-breaking.

---

### 2.2 NativeWindow — Full Window Control + OS Sinks

**Status: mostly done.** What's landed and what isn't, point-by-point against the spec below:

| Item                                                                                                       | Status |
|------------------------------------------------------------------------------------------------------------|--------|
| `NativeWindow.h` header surface (`minimize` / `maximize` / `restore` / `toggleFullscreen` / `isMinimized` / `isMaximized` / `isFullscreen` / `isVisible` / `getRect` / `setRect` / `scaleFactor` / `setMinSize` / `setMaxSize` / `setResizable` / `orderFront` / `orderBack` / `setOpacity` / `getOpacity` / `setCursorShape` / `isKeyWindow` / `becomeKeyWindow`) + `CursorShape` enum | ✅ Done — see `wtk/include/omegaWTK/Native/NativeWindow.h:138–186` |
| `#ifdef TARGET_MACOS` guards around `rect` / `eventEmitter` removed                                         | ✅ Done — both are unguarded on the base interface |
| `WindowScaleFactorChanged` event type + `WindowScaleFactorChangedParams` struct in `NativeEvent.h`         | ✅ Done — `wtk/include/omegaWTK/Native/NativeEvent.h:97` (params), `:142` (enum case) |
| **macOS** `CocoaAppWindow` — all methods + scale-change emit on `-windowDidChangeBackingProperties:`       | ✅ Done — `wtk/src/Native/macos/CocoaAppWindow.mm:179–284` |
| **Win32** `WinAppWindow` — all methods + scale-change emit on `WM_DPICHANGED` with `suggestedRect`         | ✅ Done — `wtk/src/Native/win/WinAppWindow.cpp:266–544` |
| **GTK** `GTKAppWindow` — all methods + scale-change emit on `notify::scale-factor`                          | ✅ Done — `wtk/src/Native/gtk/GTKAppWindow.cpp:268–1127`. Wayland `wp_fractional_scale_v1::preferred_scale` + X11 `Xft.dpi` listeners are still future work (integer scale covers the common case). |
| `AppWindow` consumer — subscribe to `WindowScaleFactorChanged`, run the three-step coupling, force a full-tree repaint that re-rasterizes **every** element (not just text) | ❌ **Not done.** `wtk/src/UI/AppWindow.cpp:178` only seeds `renderScale` once at construction. There is no subscription anywhere in `AppWindow.cpp`. The native side emits the event; nobody listens. Ownership re-homed to [UIView-Render-Redesign-Plan Phase F](UIView-Render-Redesign-Plan.md#phase-f-follow-up--window-resize-always-relayouts--repaints-no-resize-opt-in): the consumer shares its implementation with the resize handler (same `dispatchResize*ToHosts` `ScopedFrame`, same step ordering, same "every element re-rasterizes" semantic). |
| `ViewRenderTarget::scaleChanged()` rebuild trigger                                                          | ❌ **Not done.** Cross-plan item owned by `DPI-Aware-Text-Plan.md` "Per-monitor DPI updates"; not in this proposal's scope to implement but the AppWindow handler can't fully ship without it. Phase F's handler is the caller. |

**Net:** the platform surface is wired and emits the right events; the cross-platform consumer that turns those events into crisp glyphs on the new monitor is the one piece left. An app today can call any of the §2.2 APIs (`minimize`/`maximize`/`setOpacity`/`setCursorShape`/etc.) and they work; an app that's moved between monitors will get the event fired but no automatic re-render at the new scale until the AppWindow handler lands.

---

**Goal:** Platform-uniform window management. Because the window owns the only NativeItem, all OS-level "per-view" concerns that aren't actually per-view (cursor, opacity, key-window state) live here.

```cpp
INTERFACE NativeWindow {
    // ... existing ...

    // -- Window state --
    virtual void minimize() = 0;
    virtual void maximize() = 0;
    virtual void restore() = 0;
    virtual void toggleFullscreen() = 0;
    virtual bool isMinimized() const = 0;
    virtual bool isMaximized() const = 0;
    virtual bool isFullscreen() const = 0;
    virtual bool isVisible() const = 0;

    virtual Composition::Rect getRect() const = 0;
    virtual void setRect(const Composition::Rect & rect) = 0;

    virtual float scaleFactor() const = 0;  // DPI / backing scale

    virtual void setMinSize(float w, float h) = 0;
    virtual void setMaxSize(float w, float h) = 0;

    virtual void setResizable(bool resizable) = 0;

    virtual void orderFront() = 0;
    virtual void orderBack() = 0;

    // -- OS sinks (single per-window) --
    // Window-wide alpha. macOS: NSWindow.alphaValue. Win32: SetLayeredWindowAttributes.
    // GTK: gtk_widget_set_opacity on the toplevel.
    virtual void setOpacity(float alpha) = 0;
    virtual float getOpacity() const = 0;

    // Cursor sink. The view tree's hover dispatcher commits the active virtual
    // view's CursorShape here. macOS: [NSCursor set]. Win32: SetCursor.
    // GTK: gdk_window_set_cursor on the GtkWindow's GdkWindow.
    virtual void setCursorShape(CursorShape shape) = 0;

    // Key/main window state — drives where keyboard events land before the
    // virtual focus manager picks a target.
    virtual bool isKeyWindow() const = 0;
    virtual void becomeKeyWindow() = 0;

    // Remove #ifdef TARGET_MACOS guards around rect and eventEmitter
    NativeEventEmitter * eventEmitter() const;
};

// Defined here (or in NativeCursor.h) because NativeWindow is the sink:
enum class CursorShape : int {
    Arrow,
    IBeam,
    Crosshair,
    PointingHand,
    ResizeLeftRight,
    ResizeUpDown,
    ResizeAll,
    NotAllowed,
    Wait,
    Custom
};
```

`AppWindow` exposes thin pass-throughs (`setOpacity`, `isKeyWindow`, `becomeKeyWindow`) for app-level code; cursor is *not* exposed on `AppWindow` directly because the dispatcher should be the only writer.

> **Once [§2.9 NativeScreen](#29-nativescreen-new--prerequisite-for-22s-dpi-consumer-phase-f-and-phase-h) lands, `scaleFactor()` becomes a forwarder.** It returns `currentScreen().scaleFactor` — the canonical source. Today the per-backend implementations query their native DPI APIs directly (already shipped, see the §2.2 status block), which is correct but duplicates work. After §2.9 each backend keeps its emit machinery (`WindowScaleFactorChanged` fires on the same native trigger) but `scaleFactor()` resolves through `NativeScreen` so there is one source of truth. Same for refresh rate: code paths that need the current refresh rate query `currentScreen().refreshHz` (Phase H's `FramePacer` does exactly this).

#### DPI scale change handling

`scaleFactor()` returns the **current** value. It can change at runtime when:

- Win32: the user drags the window between monitors of different DPIs (`WM_DPICHANGED`), or changes the system scaling.
- macOS: the window moves between displays of different `backingScaleFactor` (`-windowDidChangeBackingProperties:`).
- Linux/Wayland: the compositor sends a new `wp_fractional_scale_v1::preferred_scale`, or the integer `wl_output` scale changes; X11: `Xft.dpi` change via XSettings.

`NativeWindow` emits `WindowScaleFactorChanged` through its `eventEmitter()` whenever this happens. The event carries enough information for the receiver to react without re-querying:

```cpp
struct OMEGAWTK_EXPORT WindowScaleFactorChangedParams {
    float oldScale;
    float newScale;
    /// Win32 WM_DPICHANGED suggests a new window rect that preserves
    /// physical size on the new monitor. macOS/Linux leave this empty
    /// (the window does not resize on backing-scale change). Receivers
    /// that want to honor the suggestion call setRect(suggestedRect).
    Core::Optional<Composition::Rect> suggestedRect;
};

// NativeEvent.h:
typedef enum : OPT_PARAM {
    /* ...existing... */
    WindowScaleFactorChanged
} EventType;
```

**Default AppWindow behavior — automatic propagation.** Apps must not be required to subscribe explicitly to keep their UI crisp after a DPI scale change. The platform `AppWindow` subscribes to its own `NativeWindow`'s emitter and runs a handler that re-rasterizes **every** element on the new monitor, not just text. The handler — and its three-step coupling (renderTarget resize → Compositor scale propagation → full-tree repaint) — is owned by **[UIView-Render-Redesign-Plan Phase F](UIView-Render-Redesign-Plan.md#phase-f-follow-up--window-resize-always-relayouts--repaints-no-resize-opt-in)**, sharing implementation with the resize handler (same `dispatchResize*ToHosts` `ScopedFrame`, same step ordering, same full-tree-repaint semantic). On Win32 the handler additionally calls `nativeWindow->setRect(*params.suggestedRect)` before the three steps so the OS-suggested physical-size-preserving rect lands.

App-level code that needs to react beyond the default (e.g. a custom asset cache keyed on scale) attaches its own listener to the same emitter — `NativeEventEmitter` is multi-receiver per §2.1.

#### Connection to every rendered element

The DPI scale change re-rasterizes **every `DrawOp` in the `DisplayList`** at the new pixel density. The text pipeline (originally called out here as the headline consumer) is one of several:

- **Shapes — `Shape::Rect` / `Shape::RoundedRect` / `Shape::Ellipse` (SDF path).** Re-author the 6-vertex quad at the new pixel rect; SDF sampling reads `renderScale` for supersampling tolerance.
- **`Shape::Path` (vector path).** Re-triangulate at the new size (`triangulateSync` reads the new tolerance derived from `renderScale`); Phase G's tessellation cache absorbs the cost for unchanged paths.
- **Gradient brushes.** Recompute texture coordinates against the new pixel rect; the gradient texture itself is scale-invariant.
- **Drop shadows.** Re-rasterize the blur kernel against the new radius-in-pixels; the offscreen tile is sized in pixels and rebuilt.
- **Bitmap images (`DrawOp::Image`).** Re-sample the source at the new destination scale; sampler filter selection (linear vs. nearest) is unchanged but the destination texel count is.
- **SVG content.** Re-rasterize at the new scale through the same `DrawOp::VectorPath` pipeline.
- **Text (`Canvas::drawText`, `TextLayout`).** Per the original DPI plan: `TextRect::Create(rect, layoutDesc, renderScale)` is constructed fresh on the next `drawText`; `TextLayout` compares cached scale and rebuilds on mismatch.

None of these need new wiring — they all read `renderScale` (or are re-driven by it) on the next paint. What ties them together is that **a single full-tree repaint, forced by Phase F's handler, re-emits every widget's `DisplayList` against the new `renderScale`**. Every reader picks up the new value naturally.

```
WindowScaleFactorChanged
   │
   ▼
AppWindow handler (Phase F)
   │  (1) renderTarget resize: recomputeBackingDimensions at new scale
   │  (2) Compositor scale propagation: setRenderScale on every ViewRenderTarget
   │  (3) dispatchResize*ToHosts ScopedFrame — full-tree repaint
   ▼
Next paint cycle (one frame, no intermediate 1× flash)
   │  Every DrawOp re-emitted against fresh renderScale_:
   │    shapes (SDF quad re-authored), paths (re-tessellated),
   │    gradients, shadows, bitmaps (re-sampled), SVG, text
   ▼
Crisp full-resolution frame on the new monitor — every element, not just glyphs
```

Cross-plan work items pulled in by Phase F's handler: `ViewRenderTarget::scaleChanged()` (the backing-dimension rebuild trigger; lives in `DPI-Aware-Text-Plan.md`'s "Per-monitor DPI updates" section, no longer a non-goal once Phase F lands). The "every element re-rasterizes" guarantee is verified by the Phase F validator scene (shapes + gradients + drop shadows + bitmaps + text together) — see Phase F.

#### Per-platform implementation notes

- **macOS** — `CocoaAppWindow` subscribes to `-windowDidChangeBackingProperties:`. The notification's userInfo carries `NSBackingPropertyOldScaleFactorKey`; new scale is `[window backingScaleFactor]`. `suggestedRect` is empty.
- **Win32** — `Win32AppWindow` handles `WM_DPICHANGED` in its wndproc. `wParam` low-word is the new DPI; old is read from the cached previous value. `lParam` points to a `RECT *` with the suggested new bounds. Convert DPI to scale via `scale = dpi / 96.0f`. Build `WindowScaleFactorChangedParams{oldScale, newScale, suggestedRect}`.
- **GTK** — `GTKAppWindow` connects to `notify::scale-factor` on the `GtkWindow` for integer scale, and to `wp_fractional_scale_v1::preferred_scale` (via `gdk_wayland_*`) where available. `suggestedRect` is empty. X11 also needs an `XSettings` listener for `Xft/DPI` changes — out of scope for the first cut; integer scale covers the common case.

---

### 2.3 NativeItem — No Changes (Section Removed)

Under the virtual view model, every per-View OS feature originally proposed here (focus, cursor, tooltip, opacity, hit-test) has either moved to `NativeWindow` (the single OS sink — see §2.2) or to the virtual view tree (see §2.3a). `NativeItem` itself needs no new API for this proposal.

---

### 2.3a Virtual Focus, Cursor, and Tooltip in the View Tree

**Goal:** Implement the per-view features in the virtual layer where the views actually live, with a dispatcher that commits the active value to the single root NativeItem.

> **Cross-plan dependencies (this section).**
>
> **What §2.3a needs from other plans:**
> - **Tooltip rendering** needs [Overlay-Z-Order-Plan O1–O3](Overlay-Z-Order-Plan.md#9-phases) (an `OverlayHost` with the `Tooltip` tier). The dispatcher constructs a one-Label overlay and calls `present(..., OverlayTier::Tooltip, ...)`.
> - **Tooltip hover delay** needs [§2.4 `NativeTimer`](#24-nativeapp--lifecycle-arguments-timers) (recurring timer on `WidgetTreeHost`).
> - **Focus** depends on nothing else — it's a producer, not a consumer.
> - **Cursor shape** consumes [§2.2 `NativeWindow::setCursorShape`](#22-nativewindow--full-window-control--os-sinks) (the single per-window OS cursor sink).
>
> **What other plans need from §2.3a:**
> - [Widget-Stub Phase 4B TextInput](Widget-Stub-Implementation-Plan.md#phase-4b-textinput--blocked-on-focusmanager) is blocked on Focus **steps 1–3** (FocusPolicy + FocusManager skeleton + key routing).
> - [Widget-Stub Phase 6](Widget-Stub-Implementation-Plan.md#phase-6-overlay-and-feedback-widgets) `ContextMenu` / `Modal` dismissal returns focus → Focus **step 5** (`pushRestorationPoint` / `popAndRestore`). Modal tab-trap → Focus **step 4** (`focusNext` / `focusPrevious`).
> - [Widget-Stub Phase 4B tab traversal across forms](Widget-Stub-Implementation-Plan.md#cross-plan-dependencies) → Focus **steps 4 + 6** (traversal + `setTabOrder`).
> - [Widget-Stub Phase 9 `FocusRingHost`](Widget-Stub-Implementation-Plan.md#phase-9-accessibility-and-system-integration) reads `FocusManager::focusedView()` + `lastFocusReason()` to decide when the ring renders.
> - [Overlay-Z-Order-Plan O4 + O5](Overlay-Z-Order-Plan.md#9-phases) consume Focus steps 5 and 4 respectively.
> - [Widget-Stub Phase 6 `Tooltip`](Widget-Stub-Implementation-Plan.md#6b-tooltip) is implemented through the `Widget::setTooltip` API defined in this section.
>
> The Focus → Tooltip dependency is internal to this section: the tooltip dispatcher doesn't read FocusManager. They share `WidgetTreeHost` but otherwise compose independently.

#### Focus — virtual focus manager

There is exactly one OS focus per window (the root NativeItem is always first responder when the window is key). Per-view keyboard focus is a virtual concept managed by `WidgetTreeHost`.

##### Prior art (which abstractions WTK imports and which it doesn't)

This section is informed by the four production focus managers we expect to interoperate with in some form:

- **Chromium `views::FocusManager`** ([source](https://source.chromium.org/chromium/chromium/src/+/main:ui/views/focus/focus_manager.h)). One per top-level `Widget`. Owns `focused_view_`, `stored_focused_view_` (for restore on modal dismiss), and a `FocusTraversable` chain for nested traversal (embedded webviews, child dialogs). `FocusChangeReason` enum disambiguates direct-set vs. tab-traversal vs. restore. `AdvanceFocus(reverse)` walks the traversal tree.
- **Qt `QApplication::focusWidget()` + `QWidget::setFocusPolicy`** ([docs](https://doc.qt.io/qt-6/qwidget.html#focus-and-the-keyboard)). `Qt::FocusReason` is rich (`MouseFocusReason`, `TabFocusReason`, `BacktabFocusReason`, `ActiveWindowFocusReason`, `PopupFocusReason`, `ShortcutFocusReason`, `MenuBarFocusReason`, `OtherFocusReason`) and **drives focus-ring visibility** — the ring shows on `TabFocusReason` and friends but not on `MouseFocusReason`. `setFocusProxy` delegates focus to another widget (essential for composite controls). `setTabOrder(prev, next)` overrides creation-order traversal.
- **AppKit first-responder chain.** `makeFirstResponder:` / `acceptsFirstResponder` / `becomeFirstResponder` / `resignFirstResponder`. `nextKeyView` / `previousKeyView` form an explicit linked-list traversal.
- **GTK** `gtk_widget_grab_focus`, `gtk_widget_child_focus(GTK_DIR_TAB_FORWARD|BACKWARD|UP|DOWN|LEFT|RIGHT)`. Directional focus is **first-class** here (GTK supports D-pad / spatial navigation natively).
- **Win32** `SetFocus` / `GetFocus`; `IsDialogMessage` performs tab traversal *only* in dialog message loops.

What WTK imports verbatim: Qt's **`FocusReason`** (the only sensible way to gate focus-ring rendering) and **`FocusPolicy`** (per-View declaration of focusability). What WTK imports adapted: Chromium's **focus restoration** (modal/popover dismiss returns focus to prior owner) and Chromium's **focus traversal** model walking the virtual view tree. What WTK **defers**: directional / spatial focus (GTK-style `GTK_DIR_UP/DOWN/LEFT/RIGHT`) — useful for game-controller and TV UIs but out of scope until a concrete use case lands; and `FocusProxy` chains — useful for composite controls but easy to add later without API churn (a `View*` member on the proxying View).

##### View-side API

```cpp
// New: focus state lives on View
class View {
    // ... existing ...

    // Per-view "can this be focused, and how?"
    //   NoFocus       — never receives focus (default for shape primitives, Label).
    //   ClickFocus    — focusable only by direct mouse click + View::focus().
    //   TabFocus      — focusable only by keyboard traversal (Tab / Shift-Tab / Shortcut).
    //   StrongFocus   — ClickFocus | TabFocus. Default for interactive widgets
    //                   (Button, TextInput, Checkbox).
    //   WheelFocus    — StrongFocus | gains focus on scroll-wheel events too.
    //                   Default for editable scrollable surfaces (TextArea).
    enum class FocusPolicy : uint8_t {
        NoFocus     = 0,
        ClickFocus  = 1 << 0,
        TabFocus    = 1 << 1,
        StrongFocus = ClickFocus | TabFocus,
        WheelFocus  = StrongFocus | (1 << 2)
    };

    void setFocusPolicy(FocusPolicy policy);
    FocusPolicy focusPolicy() const;

    // Convenience predicates derived from focusPolicy().
    bool isFocusable() const;     // policy != NoFocus
    bool isClickFocusable() const;
    bool isTabFocusable() const;

    // True iff the host's focus manager has selected this view.
    bool isFocused() const;

    // Request focus from the host's focus manager. The reason is what
    // decides whether the focus ring renders (see FocusReason below).
    void focus(FocusReason reason = FocusReason::Other);
    void blur();
};

// Why focus changed. Used by FocusManager to decide whether to commit
// a focus ring; widgets that paint a custom focus indicator read it via
// View::lastFocusReason() in their rebuildStyle() hook.
enum class FocusReason : uint8_t {
    Mouse,          // ClickFocus path; ring usually suppressed
    Tab,            // FocusManager::focusNext
    Backtab,        // FocusManager::focusPrevious
    Shortcut,       // hotkey landed on this view
    ActiveWindow,   // window became key; restored focus
    Popup,          // owner of a popup got focus back when popup closed
    Restore,        // explicit clearFocus() returned focus to prior holder
    Other           // programmatic View::focus() with no reason
};
```

`FocusPolicy` is a bitmask enum (the `1<<n` shifts), not a plain enum class for ergonomics — operator `|`/`&` overloads in `View` make `policy & FocusPolicy::TabFocus` legible.

##### FocusManager — owned by WidgetTreeHost

```cpp
class FocusManager {
public:
    // Direct set / clear. Emits FocusLost on previous (with the
    // previous View's lastFocusReason), then FocusGained on new with
    // `reason`. Both events flow through NativeEventEmitter (already
    // defined as FocusGained / FocusLost in NativeEvent.h).
    void setFocus(View * view, FocusReason reason = FocusReason::Other);
    View * focusedView() const;
    FocusReason lastFocusReason() const;
    void clearFocus();

    // Traversal. Walks the View tree under WidgetTreeHost's root,
    // skipping NoFocus / !TabFocus views. Returns false if no
    // focusable view exists in the requested direction.
    bool focusNext();        // FocusReason::Tab
    bool focusPrevious();    // FocusReason::Backtab

    // Tab-order override. Without this, traversal follows the View
    // tree in pre-order (parent, then children, then siblings) —
    // matching DOM tab-order, which is what users intuit.
    // setTabOrder(a, b) forces b to come immediately after a in
    // traversal regardless of tree order.
    void setTabOrder(View * a, View * b);

    // Focus restoration. push() captures the current focusedView();
    // popAndRestore() re-focuses it with FocusReason::Popup
    // (so the focus ring re-appears if it was tab-focused before).
    // Used by Modal / Popover / ContextMenu dismiss.
    void pushRestorationPoint();
    void popAndRestore();
};
```

##### Routing keyboard events

When the AppWindow receives a `KeyDown` / `KeyUp` from its root NativeItem, the WidgetTreeHost:

1. If the key is `Tab` (no modifiers): `focusManager->focusNext()`, consume.
2. If the key is `Shift+Tab`: `focusManager->focusPrevious()`, consume.
3. Otherwise: forward the event to `focusManager->focusedView()->getDelegate()->onKeyDown(...)` (or `nullptr` short-circuit if no view is focused).

This replaces the current broadcast pipe — today, with no `FocusManager`, key events have no defined target, which is exactly why TextInput is blocked on this section. Step (1)/(2) intentionally happen *before* delegate dispatch so a focused TextInput cannot swallow Tab; if a widget genuinely needs Tab (a Tab character in a code editor), it sets `View::setTabCaptured(true)` to opt into delegate dispatch first — but that's a Phase 4B+ knob, not part of the base API.

##### Focus on mouse click

The hit-test dispatcher (the same one driving `CursorEnter`/`CursorExit` and overlay dismissal) walks `View::containsPoint` top-down. When a mouseDown lands on a view with `ClickFocus | StrongFocus`, it calls `focusManager->setFocus(view, FocusReason::Mouse)` before delivering the mouseDown to the delegate. This is the AppKit / Qt order and avoids the surprising case where a widget receives mouseDown without being focused.

##### Focus ring visibility — the headline behavior

A widget's `rebuildStyle()` (the Button / TextInput pattern from [Widget-Stub-Implementation-Plan §4A](Widget-Stub-Implementation-Plan.md#phase-4a-button--base-implementation)) reads:

```cpp
bool shouldDrawFocusRing = view->isFocused() &&
    isKeyboardReason(focusManager->lastFocusReason());

constexpr bool isKeyboardReason(FocusReason r) {
    return r == FocusReason::Tab        || r == FocusReason::Backtab ||
           r == FocusReason::Shortcut   || r == FocusReason::Popup   ||
           r == FocusReason::ActiveWindow;
}
```

This is the same gate Qt and Chromium use. A user who tab-traversed to a button sees the ring; a user who clicked it does not. Programmatic `focus(FocusReason::Other)` does not show the ring — the default is conservative.

##### Implementation order

| Step | What lands                                                                                         | Blocks                              |
|------|----------------------------------------------------------------------------------------------------|-------------------------------------|
| 1    | `View::FocusPolicy` + `setFocusPolicy`/`isFocusable`/`focus(reason)`/`blur` + `FocusReason` enum   | —                                   |
| 2    | `FocusManager` skeleton with `setFocus`/`focusedView`/`clearFocus` and FocusGained/Lost emission   | nothing (broadcast still works)     |
| 3    | `WidgetTreeHost` routes KeyDown/KeyUp to `focusedView()`; removes broadcast                        | TextInput v0 can land               |
| 4    | `focusNext`/`focusPrevious` tree walk + Tab/Shift-Tab interception                                 | tab-traversal across forms          |
| 5    | `pushRestorationPoint`/`popAndRestore`                                                              | Modal / Popover dismissal           |
| 6    | `setTabOrder(a, b)` override                                                                       | hand-tuned form order               |

Steps 1–3 unblock TextInput. Steps 4–6 are independent and can land in any order.

#### Cursor shape — declarative on View, committed by the dispatcher

```cpp
class View {
    // ... existing ...
    void setCursorShape(CursorShape shape);   // Declarative: "if cursor is over me, show this"
    CursorShape cursorShape() const;
};
```

The hover dispatcher in `WidgetTreeHost` (the same one that emits virtual `CursorEnter` / `CursorExit`) calls `nativeWindow->setCursorShape(...)` whenever the topmost hovered virtual view changes. Views never touch the OS cursor directly.

#### Tooltip — per-Widget, virtual popup

`NSView.toolTip` and `gtk_widget_set_tooltip_text` only work on real native views. Since virtual views aren't native views, tooltips must be rendered by WTK itself — a small composited popup window (or sibling layer) shown after a hover delay.

> **API placement: `Widget`.** A tooltip is a logical-control concept (one control, one tooltip), not a per-render-surface concept. Anchoring on `Widget` matches Qt's `QWidget::setToolTip` (the conventional placement) and keeps the API at the same altitude as the rest of the input surface (`Widget::setOnPress`, `Widget::setEnabled` once we add it, etc.). The hover dispatcher already knows how to resolve a hit `View` back to its owning `Widget` (the same path that delivers per-Widget events), so per-Widget tooltips cost nothing the per-View design would have saved.

```cpp
class Widget {
    // ... existing ...
    void setTooltip(const OmegaCommon::String & text);
    void clearTooltip();
    const OmegaCommon::String & tooltip() const;   // empty => no tooltip
};
```

##### Activation pipeline

`WidgetTreeHost` owns:

1. A `NativeTimer` (§2.4) configured for the platform-conventional hover delay (500ms on Win32 / macOS / GTK).
2. A `currentTooltipPopup_` — an overlay surface rendered by the in-window overlay layer (see [Overlay-Z-Order-Plan.md](Overlay-Z-Order-Plan.md), tier `Tooltip`).

The hover dispatcher already emits `CursorEnter` / `CursorExit` per `View`. The tooltip dispatcher resolves the hit View to its owning Widget (via the `View::ownerWidget()` accessor `WidgetTreeHost` already maintains for per-Widget event routing) and subscribes to:

- `CursorEnter(view)`: resolve `widget = view->ownerWidget()`. If `widget->tooltip()` is non-empty, (re)start the hover timer with the widget's tooltip string captured.
- `CursorMove(view)`: pet the timer's "cursor still inside" flag. Restart the delay only on *widget* change, not on view change — matching macOS / Qt behavior so moving the cursor inside a multi-view composite control does not retrigger the delay.
- `CursorExit(view)`: if the new hit Widget differs from the timer's captured Widget, cancel the timer and dismiss `currentTooltipPopup_` if it was for the previous Widget.
- Timer fires: present `currentTooltipPopup_` anchored to the cursor (offset +12px down/right, with edge-clamping) at tier `Tooltip`.
- Any `mouseDown` / `KeyDown`: dismiss immediately.

##### Sub-View granularity (when a single tooltip per Widget is not enough)

A composite Widget that genuinely needs different tooltips for different sub-views (e.g. an inspector row with separate "name" and "value" sub-views) handles this itself by routing its own `CursorEnter`/`CursorExit` on each sub-view to swap its single `widget->setTooltip(...)` string at hover time. The public API does not multiply to accommodate this case — it is rare enough that the widget's own dispatcher is the right place for the logic.

##### Rendering surface

The implementation uses the in-window overlay tier (`OverlayHost::Tier::Tooltip`). Tooltips, like every other overlay, are **window-bound** — they are clipped to the hosting `AppWindow` (or `AppPanel`, which has its own independent `OverlayHost`). A tooltip anchored near the window edge is repositioned by the dispatcher's edge-clamping math (the same math that flips `AboveWidget` to `BelowWidget` when the widget is near the window's top); a tooltip with no usable rect after clamping is suppressed for that frame. There is no escape path into an `AppPanel` — `AppPanel` is for separate top-level UI (tool palettes, tear-off inspectors), not for spilling overlays past a window boundary ([Overlay-Z-Order-Plan §7](Overlay-Z-Order-Plan.md#7-relationship-to-apppanel--they-are-separate-not-coupled)).

> **Note:** Hit-testing is already covered by `View::containsPoint`. Tooltips are non-interactive — they never absorb hits (`OverlayHost::Tier::Tooltip` overlays render with `hitTestEnabled=false`).

##### Customization — follow-up, not v0

The v0 `Widget::setTooltip(text)` is intentionally minimal: a plain string with platform-default placement, font, and chrome (a `controlBackground` rectangle, a hairline `separator` border, `controlForeground` text, and a small drop shadow). That covers the conventional case and matches what `NSView.toolTip` / `gtk_widget_set_tooltip_text` give users by default.

Real-world tooltip use ultimately needs three knobs on top of that. Each is opt-in via an overload that takes a descriptor, so the existing one-argument call continues to work unchanged:

```cpp
enum class TooltipPlacement : uint8_t {
    Cursor,            // default — anchored to cursor with +12px down/right gap (v0 behavior)
    AboveWidget,       // edge-centered above the Widget's window rect
    BelowWidget,
    LeftOfWidget,
    RightOfWidget,
    InsideWidget       // overlays the widget itself; rare, for hover-state previews
};

struct TooltipDesc {
    OmegaCommon::UString text {};
    TooltipPlacement     placement = TooltipPlacement::Cursor;
    float                gap       = 4.f;          // pixels between anchor edge and tooltip
    Core::Optional<StylePtr> style {};             // see "StyleRules" below
    bool                 dropShadow = true;        // opt-out for a flat, chromeless tooltip
};

class Widget {
    void setTooltip(const OmegaCommon::String & text);   // v0, preserved
    void setTooltip(const TooltipDesc & desc);            // follow-up
};
```

1. **Placement.** `placement` selects where the tooltip's rect anchors relative to the Widget's window-space rect (or the cursor, when `Cursor`). Edge clamping against window bounds is still automatic — `AboveWidget` near the top of the window flips to `BelowWidget` exactly the way every native toolkit does. The dispatcher resolves placement at present time; the API user states the *preference*, not the final rect.

2. **StyleRules.** The tooltip's root `UIView` is styled like any other widget — `style` is a regular `StylePtr` (`bg` fill / border / text font and color, plus `dropShadow` per the next bullet). When `style` is unset, the dispatcher synthesizes one from the current `ThemeDesc` (the v0 defaults above). This is the same pattern Phase 4A Button uses for theme-derived styling, so nothing new on the rendering side — just exposing the existing surface.

3. **Drop shadow.** `dropShadow = true` is the v0 default and the right default: every native tooltip on every platform has a soft shadow. The dispatcher applies a small dark drop shadow via `Style::dropShadow` on the tooltip's root view at present time (offset 0,2 / radius 4 / opacity 0.25 — matching the macOS / GTK convention). `dropShadow = false` removes it; an API user who needs a different shadow sets one through `style` directly (which takes precedence over the dispatcher-applied default).

The customization surface is **tooltip-specific**. For every other overlay tier — popovers, dropdown menus, modals, sheets, snackbars — the overlay's *content* is a `Widget` the API user constructs themselves, with the full `Style` surface already at their disposal. The `OverlayHost` doesn't need a TooltipDesc-shaped knob for those; see [Overlay-Z-Order-Plan §4.3](Overlay-Z-Order-Plan.md#43-ornamentation--drop-shadow-is-the-only-baseline-host-provides) for the rule.

---

### 2.4 NativeApp — Lifecycle, Arguments, Timers

**Goal:** Expose command-line arguments, open-file/URL delegate, and run-loop timers.

```cpp
INTERFACE NativeAppDelegate {
    virtual void onAppReady() {}
    virtual void onAppWillTerminate() {}
    virtual void onOpenFile(const OmegaCommon::FS::Path & path) {}
    virtual void onOpenURL(const OmegaCommon::String & url) {}
    virtual ~NativeAppDelegate() = default;
};

class NativeApp {
public:
    virtual int runEventLoop() = 0;
    virtual void terminate() = 0;

    virtual void setDelegate(NativeAppDelegate * delegate) = 0;

    virtual const NativeAppLaunchArgs & launchArgs() const = 0;
    virtual OmegaCommon::Vector<OmegaCommon::String> commandLineArgs() const = 0;
};
```

#### Timers

New header `NativeTimer.h`:

```cpp
INTERFACE NativeTimer {
public:
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual ~NativeTimer() = default;
};

typedef SharedHandle<NativeTimer> NativeTimerPtr;
NativeTimerPtr make_native_timer(float intervalSec, bool repeats,
                                 std::function<void()> callback);
```

Backed by `NSTimer`, `SetTimer`/`KillTimer`, `g_timeout_add`.

---

### 2.5 NativeTheme — Rich Theme Descriptor ✅ Done (data model only)

Implemented. See `NativeTheme.h`: `ThemeAppearance` (Light/Dark), `ThemeDesc` with `Colors` and `Typography` sub-structs, `queryCurrentTheme()`.

> **Forward pointer:** the *application* story — wiring `NativeTheme.colors.background` into the per-frame clear color so a `UIView` with no explicit background inherits the OS surface color uniformly on all three platforms (currently macOS works by accident, Windows clears to engine default, GTK + Vulkan clears to pitch black), plus the custom-`Theme` override that follows OS dark mode — lives in [Native-Theme-Application-Plan.md](Native-Theme-Application-Plan.md). GTK `queryCurrentTheme()` is now implemented in `wtk/src/Native/gtk/GTKTheme.cpp` per that plan's Tier 1 §5.3.1: appearance from `gtk-application-prefer-dark-theme` (with `-dark` theme-name fallback), colors from `gtk_style_context_lookup_color` against the toplevel + button contexts, typography from the parsed `gtk-font-name` PangoFontDescription. The observer wiring (`notify::gtk-application-prefer-dark-theme` / `notify::gtk-theme-name`) is left for Native-Theme-Application-Plan §5.3 to wire alongside the cross-platform `NativeThemeObserver` dispatch path.

---

### 2.6 NativeClipboard (New)

**Goal:** Read and write system clipboard (text, file paths).

New header `NativeClipboard.h`:

```cpp
enum class ClipboardDataType : int {
    PlainText,
    HTML,
    Image,
    FilePaths
};

INTERFACE NativeClipboard {
public:
    virtual bool hasType(ClipboardDataType type) const = 0;
    virtual OmegaCommon::String getText() const = 0;
    virtual void setText(const OmegaCommon::String & text) = 0;
    virtual OmegaCommon::Vector<OmegaCommon::FS::Path> getFilePaths() const = 0;
    virtual void setFilePaths(const OmegaCommon::Vector<OmegaCommon::FS::Path> & paths) = 0;
    virtual void clear() = 0;
    virtual ~NativeClipboard() = default;
};

typedef SharedHandle<NativeClipboard> NativeClipboardPtr;
NativeClipboardPtr get_native_clipboard();
```

---

### 2.7 NativeDragDrop (New)

**Goal:** Enable drag-and-drop between views and from/to external apps.

New header `NativeDragDrop.h`:

```cpp
struct DragData {
    OmegaCommon::String text;
    OmegaCommon::Vector<OmegaCommon::FS::Path> filePaths;
};

enum class DropEffect : int {
    None, Copy, Move, Link
};

INTERFACE NativeDragSource {
public:
    virtual void beginDrag(const DragData & data, NativeItemPtr sourceItem) = 0;
    virtual ~NativeDragSource() = default;
};

INTERFACE NativeDropTarget {
public:
    virtual DropEffect onDragEnter(const DragData & data, Composition::Point2D position) { return DropEffect::None; }
    virtual DropEffect onDragOver(const DragData & data, Composition::Point2D position) { return DropEffect::None; }
    virtual void onDragLeave() {}
    virtual DropEffect onDrop(const DragData & data, Composition::Point2D position) { return DropEffect::None; }
    virtual ~NativeDropTarget() = default;
};

void register_drop_target(NativeItemPtr item, NativeDropTarget * target);
void unregister_drop_target(NativeItemPtr item);
SharedHandle<NativeDragSource> make_native_drag_source();
```

---

### 2.8 NativeDialog — Confirmation Dialog and Dialog Result ✅ Done

**Goal:** Add a standard confirmation/alert dialog and give `NativeNoteDialog` a result. Add file-type filters to `NativeFSDialog`.

**Implemented across all three platforms.** `NativeAlertDialog` (macOS `NSAlert`, Win32 `TaskDialogIndirect`, GTK `GtkMessageDialog`); `FileFilter` + `allowMultiple` on `NativeFSDialog::Descriptor`; the GTK dialog backend (previously a `nullptr` stub) now lives in `wtk/src/Native/gtk/GTKDialog.cpp` covering FS, alert, and note dialogs. `AppWindow::openAlertDialog` exposes the new dialog to UI code. macOS is compile-verified; Win32/GTK are source-complete pending CI.

**Deltas from the original sketch:**
- `NativeFSDialog::getResult()` now returns `Async<Vector<FS::Path>>` (was `Async<String>`) so `allowMultiple` has somewhere to land and the result type matches the descriptor's `FS::Path`. Cancelled dialogs resolve to an empty vector.
- `NativeAlertDialog::Result` is derived from the clicked button's label (case-insensitive match against OK/Cancel/Yes/No; unmatched → OK for the first button, Cancel otherwise), documented on `Descriptor::buttonLabels`.
- `NativeNoteDialog` is left as fire-and-forget; `NativeAlertDialog` is the result-bearing dialog the goal refers to.

**Known follow-up:** the macOS FS backend uses `-setAllowedFileTypes:` (deprecated in macOS 12 in favor of `-allowedContentTypes` / `UTType`); functional but warns.

```cpp
class NativeAlertDialog : public NativeDialog {
protected:
    NativeAlertDialog(NWH parentWindow);
public:
    enum class Style : int { Info, Warning, Error };
    enum class Result : int { OK, Cancel, Yes, No };

    struct Descriptor {
        OmegaCommon::String title;
        OmegaCommon::String message;
        Style style = Style::Info;
        OmegaCommon::Vector<OmegaCommon::String> buttonLabels;
    };

    static SharedHandle<NativeAlertDialog> Create(const Descriptor & desc, NWH nativeWindow);
    virtual OmegaCommon::Async<Result> getResult() = 0;
};
```

Update `NativeFSDialog::Descriptor` to support file-type filters:

```cpp
struct FileFilter {
    OmegaCommon::String label;
    OmegaCommon::Vector<OmegaCommon::String> extensions;
};

struct Descriptor {
    Type type;
    OmegaCommon::FS::Path openLocation;
    OmegaCommon::Vector<FileFilter> filters;
    bool allowMultiple = false;
};
```

---

### 2.9 NativeScreen (New) — **prerequisite for §2.2's DPI consumer, Phase F, and Phase H**

**Goal:** **The cross-platform API for everything that is fundamentally a property of a *display*, not a window.** Enumerate connected screens, query their geometry, DPI, refresh rate / vsync source, color/HDR capabilities (future), and resolve the screen any given `AppWindow` currently lives on. Pick which screen a new `AppWindow` opens on.

> **Sequencing — this lands first.** Three other pieces in the broader plan family consume `NativeScreen`:
>
> - [§2.2 NativeWindow](#22-nativewindow--full-window-control--os-sinks)'s `scaleFactor()` becomes a forwarder to `currentScreen().scaleFactor`. Today each backend re-queries its native DPI API directly; after §2.9 there is one canonical source.
> - [UIView-Render-Redesign-Plan Phase F](UIView-Render-Redesign-Plan.md#phase-f-follow-up--window-resize-always-relayouts--repaints-no-resize-opt-in)'s DPI-scale-change handler reads `newScreen.scaleFactor` (since the canonical trigger is "the window moved to a different screen, what's the new scale?").
> - [UIView-Render-Redesign-Plan Phase H](UIView-Render-Redesign-Plan.md#phase-h-follow-up--frame-pacing-vsync-aligned-production--real-frametime--load-aware-frame-gating-folds-frame-pacing-plan)'s `FramePacer` binds to `currentScreen().displayLink()` for its vsync signal — vsync is **per-screen**, not per-window, and the pacer rebinds when the window crosses screen boundaries.
>
> All three consumers can implement against the existing per-window APIs as a transitional stop-gap (today's situation), but the right shape is one screen-level source with windows resolving through it. Landing §2.9 first lets §2.2's DPI consumer + Phase F + Phase H all consume the canonical surface.

New header `NativeScreen.h`:

```cpp
namespace OmegaWTK::Native {

// Identity + geometry of a connected display.
struct NativeScreenDesc {
    unsigned id = 0;
    Composition::Rect frame;            // Virtual-screen coordinates (DIPs)
    Composition::Rect visibleFrame;     // frame minus menu bars / docks / panels
    float scaleFactor = 1.f;            // Combined logical→physical (matches NativeWindow::scaleFactor() once §2.9 lands; before that, kept in sync per-platform)
    bool isPrimary = false;

    // Refresh-rate descriptor for this screen. Variable-refresh displays
    // (ProMotion / G-Sync / FreeSync) report the *maximum* refresh as
    // `maxRefreshHz`; the actual per-frame interval is delivered through
    // the display link (see NativeDisplayLink below).
    float refreshHz = 60.f;              // Fixed-rate displays: the rate. VRR: max rate.
    float minRefreshHz = 60.f;           // VRR floor; equals refreshHz on fixed-rate displays.
    bool  variableRefreshRate = false;   // True iff the OS reports VRR for this display.
};

OMEGAWTK_EXPORT OmegaCommon::Vector<NativeScreenDesc> enumerateScreens();
OMEGAWTK_EXPORT NativeScreenDesc primaryScreen();

// Resolve a screen by id; returns primaryScreen() when id is unknown so
// callers always have a valid target.
OMEGAWTK_EXPORT NativeScreenDesc screenById(unsigned id);

// Per-screen display link. One real OS object per physical display;
// multiple windows on the same screen share the same NativeDisplayLink
// instance (the platform impl caches by screen id). Consumed by
// Phase H's FramePacer.
//
// The display link is logically a property of the *screen*, not any
// window — vsync timing is per-display. The FramePacer asks
// `displayLinkForScreen(currentScreen())` whenever the owning AppWindow
// crosses screen boundaries, so the pacer always speaks to the right
// physical vsync source.
INTERFACE NativeDisplayLink {
public:
    // Subscribe to vsync notifications for this screen. The callback
    // fires on every refresh tick (or every variable-refresh delivery
    // under VRR) with the predicted *next* presentation time and the
    // measured interval since the previous fire.
    //
    // Threading: the callback fires on the platform's display-link
    // thread (NOT the UI thread). FramePacer marshals to the UI thread
    // before running FrameBuilder.
    virtual void subscribe(std::function<void(std::uint64_t presentationTimeNs,
                                              std::uint64_t intervalNs)> cb) = 0;
    virtual void unsubscribe() = 0;

    // Current interval, in nanoseconds. 16'666'666 @ 60Hz, 8'333'333
    // @ 120Hz, variable under VRR (returns most-recent measured).
    virtual std::uint64_t expectedFrameIntervalNs() const = 0;

    virtual ~NativeDisplayLink() = default;
};
typedef SharedHandle<NativeDisplayLink> NativeDisplayLinkPtr;

NativeDisplayLinkPtr displayLinkForScreen(const NativeScreenDesc & screen);

}
```

`NativeDisplayLink` wraps the platform display-link object (`CADisplayLink` on macOS, the `IDXGIOutput::WaitForVBlank` pacer thread or `DCompositionWaitForCompositorClock` on Windows, `wl_surface.frame` + `wp_presentation_feedback` under Wayland, `GLX_INTEL_swap_event` under X11). The per-platform wiring details that previously lived in [Phase H](UIView-Render-Redesign-Plan.md#phase-h-follow-up--frame-pacing-vsync-aligned-production--real-frametime--load-aware-frame-gating-folds-frame-pacing-plan) move here — Phase H consumes `displayLinkForScreen(screen)` and doesn't care which platform produced it.

#### Placement contract for `AppWindow` / `AppWindowManager`

Without a screen contract, GTK currently lets the WM pick — which on multi-monitor setups frequently lands the window on a secondary monitor with a different `scaleFactor` than the primary. That gives the wrong DPI at construction time and forces the visual tree to rebuild on the first `WindowScaleFactorChanged` event before the user has even seen the window.

`NativeScreen` fixes this by making the target screen an explicit choice:

```cpp
class AppWindowManager {
public:
    // ... existing ...

    // Default screen for newly-created AppWindows. Defaults to primaryScreen().
    // AppWindow constructors that don't specify a screen explicitly resolve
    // through this.
    void setDefaultScreen(const NativeScreenDesc & screen);
    NativeScreenDesc defaultScreen() const;
};

class AppWindow {
public:
    // Existing constructor: uses AppWindowManager::defaultScreen().
    AppWindow(Composition::Rect rect, AppWindowDelegate * delegate);

    // New: opt-in screen targeting. The `rect` is interpreted as
    // screen-local DIPs; the window is placed at
    // `screen.frame.origin + rect.pos`, sized `rect.{w,h}`.
    AppWindow(Composition::Rect rect,
              const NativeScreenDesc & screen,
              AppWindowDelegate * delegate);

    NativeScreenDesc currentScreen() const;   // Screen the window is on right now
    void moveToScreen(const NativeScreenDesc & screen);
};
```

Behavior contract:

- **Rect is screen-local at construction.** The constructor adds the chosen screen's `frame.origin` to `rect.pos` before forwarding to the native layer. `setRect` / `getRect` continue using virtual-screen absolute coordinates (no behavior change there) — the screen-local interpretation is a one-shot at construction, matching what most app developers actually want.
- **Initial scale comes from the chosen screen.** `NativeWindow::scaleFactor()` is seeded from `screen.scaleFactor` rather than from whatever monitor the WM happens to pick. This eliminates the "first frame at wrong DPI, then jump" sequence.
- **Cross-screen moves still emit `WindowScaleFactorChanged`** (§2.2). `currentScreen()` re-resolves on every call from the native window's current position; `moveToScreen` is a convenience wrapper that calls `setRect` with the destination screen's origin.

#### Per-platform implementation notes

- **macOS** — `NSScreen.screens`. Primary is `[NSScreen mainScreen]`. `frame` is `[NSScreen frame]` in points; `visibleFrame` from `visibleFrame`. `scaleFactor` from `backingScaleFactor`. `refreshHz` from `NSScreen.maximumFramesPerSecond` (returns 120 on ProMotion displays, 60 elsewhere); `minRefreshHz` from the equivalent `minimumRefreshInterval` reciprocal when available, else equal to `refreshHz`. `variableRefreshRate` true iff `NSScreen.maximumFramesPerSecond > NSScreen.minimumRefreshInterval`-reciprocal. `displayLinkForScreen` wraps `CADisplayLink` bound to the screen via `-[CADisplayLink initWithTarget:selector:]` on the matching `CGDirectDisplayID` (`[NSScreen deviceDescription][@"NSScreenNumber"]`). AppWindow construction sets `[window setFrame:display:]` with the screen-translated rect.
- **Win32** — `EnumDisplayMonitors` + `GetMonitorInfo`. Primary is the monitor with `MONITORINFOF_PRIMARY`. `scaleFactor` from `GetDpiForMonitor(...) / 96.0`. `visibleFrame` from `rcWork`. `refreshHz` from `EnumDisplaySettings(..., ENUM_CURRENT_SETTINGS).dmDisplayFrequency`; VRR detection via `IDXGIOutput6::CheckHardwareCompositionSupport` (`DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN_VIDEO` + variable-refresh capability flag). `displayLinkForScreen` wraps an `IDXGIOutput::WaitForVBlank` pacer thread bound to the `IDXGIOutput` matching the screen's `HMONITOR`, or `DCompositionWaitForCompositorClock` where available (Windows 10+ desktop composition). AppWindow construction passes the screen-translated rect to `CreateWindowEx` / `SetWindowPos`.
- **GTK / Linux** — `gdk_display_get_monitors` (or `gdk_display_get_n_monitors` + `gdk_display_get_monitor` in GTK3). Primary is `gdk_display_get_primary_monitor`. `frame`/`visibleFrame` from `gdk_monitor_get_geometry` / `gdk_monitor_get_workarea`. `scaleFactor` is the **combined** product `gdk_screen_get_resolution()/96 × gdk_monitor_get_scale_factor()` to match `GTKAppWindow::scaleFactor()` (see `DPI-Aware-Text-Plan.md`). `refreshHz` from `gdk_monitor_get_refresh_rate()` (returns millihertz; divide by 1000). VRR not exposed by GDK directly; treat as `variableRefreshRate = false` until a backend opts in. `displayLinkForScreen` under Wayland wraps `wl_surface.frame` callbacks combined with `wp_presentation_feedback` for predicted presentation times — the pacer subscribes per surface but the underlying signal is the compositor's display refresh (per-screen at the compositor level). Under X11 it wraps `GLX_INTEL_swap_event` where available, falling back to a frame timer derived from the monitor's `XRRGetScreenInfo` rate and `EGL_KHR_fence_sync` for swap completion. AppWindow construction translates `rect.pos` by the monitor's `geometry.{x,y}`.

#### Status — interim primary-monitor anchoring on GTK

`GTKAppWindow` currently anchors construction-time placement to `gdk_display_get_primary_monitor()` directly: query the primary monitor, add its geometry origin to the constructor's `rect.pos`, and seed `integerScale_` from the primary monitor (rather than from the unrealized `gtk_widget_get_scale_factor`, which returns 1 before `show`). This is a stop-gap to keep mixed-DPI Linux setups usable until §2.9 lands.

When `NativeScreen` and the `AppWindow` screen-targeting constructor arrive, the GTK ctor's hardcoded `queryPrimaryMonitor()` call is replaced by the screen passed through (or `AppWindowManager::defaultScreen()` if unspecified), and the same logic applies uniformly to macOS and Win32.

---

### 2.10 NativeAccessibility (New, Stub)

**Goal:** Skeleton API for platform backends to implement over time (NSAccessibility, MSAA/UIA, ATK).

New header `NativeAccessibility.h`:

```cpp
enum class AccessibilityRole : int {
    None, Button, Label, TextField, Image,
    ScrollArea, Slider, Checkbox, Group, Window, Application
};

struct AccessibilityDesc {
    AccessibilityRole role = AccessibilityRole::None;
    OmegaCommon::String label;
    OmegaCommon::String value;
    OmegaCommon::String hint;
    bool isEnabled = true;
    bool isFocused = false;
};

INTERFACE NativeAccessibilityBridge {
public:
    // Bound to virtual views, not NativeItems — the bridge is responsible
    // for synthesizing accessibility elements that the platform a11y tree
    // can walk, since virtual views have no NSView/HWND of their own.
    virtual void setAccessibility(View * view, const AccessibilityDesc & desc) = 0;
    virtual void clearAccessibility(View * view) = 0;
    virtual void announceForAccessibility(const OmegaCommon::String & message) = 0;
    virtual ~NativeAccessibilityBridge() = default;
};

SharedHandle<NativeAccessibilityBridge> get_native_accessibility_bridge();
```

> **Virtual-view caveat:** Because virtual Views are not real NSViews/HWNDs, the macOS implementation will need to synthesize `NSAccessibilityElement` children of the root NativeItem (and override `accessibilityChildren` / `accessibilityHitTest:` on the root). Win32 needs an `IRawElementProviderFragment` tree rooted at the HWND. GTK needs an `AtkObject` factory. This is non-trivial — initial impls can be no-ops while the API surface is locked.

---

### 2.11 NativeNote / NotificationCenter ✅ Done

Implemented across all three platforms. See `NativeNote.h`, `Notification.h`/`.cpp`, `CocoaNote.mm`, `WinNote.cpp`, `GTKNote.cpp`.

**Caveats:**
- **Win32:** Sending a toast requires an AUMID set via `SetCurrentProcessExplicitAppUserModelID` before first send.
- **GTK:** Falls back to no-op if `libnotify` is absent (CMake prints a warning). Scheduling uses `g_timeout_add`.

---

### 2.12 NativeMenu / Menu ✅ Done

Implemented across all three platforms. See `NativeMenu.h`, `Menu.h`/`.cpp`, `CocoaMenu.mm`, `WinMenu.cpp`/`.h`, `GTKMenu.cpp`/`.h`.

**Caveats:**
- **Win32:** Shortcuts are *displayed* in menus but actual key-press routing requires `TranslateAccelerator` in the message loop — not yet wired in `WinApp.cpp`.
- **Win32 / GTK:** `onValidateItem` only fires for context menus (and GTK menu-bar `show` signal). Win32 menu-bar validation requires `WM_INITMENUPOPUP` in the wndproc — out of scope for 2.12.
- **Icons:** Deferred (Phase 2).

---

### 2.13 Linux/X11 — Direct X11 Surface Ownership (Root + NativeViewHost paths)

**Goal:** On Linux, WTK owns its X11 surfaces directly — no GTK widgets in the rendering or embedding path. This covers two cases that share the same surface-ownership philosophy:

1. **The single root NativeItem** (per the virtual view model above) is *not* a `GtkDrawingArea` hosted inside a `GtkWindow` — it falls through to the `GTKAppWindow` itself. The compositor renders directly into the X11 `Window` underneath `GTKAppWindow`'s `GdkWindow`.

2. **The NativeViewHost path** (see `NativeViewHost-Adoption-Plan.md`) uses X11 child Windows allocated and managed by a small `X11SurfaceHost` class — no `GtkSocket`, no nested `GtkDrawingArea`, no `GTKItem` for embedded surfaces. The Linux `VKVisualTree` (a Native subsystem per §2.14, not a Composition class) owns the `hostId → child Window` map that `FrameBuilder` reconfigures directly during layout.

GTK is reduced to providing the toplevel, event dispatch, menus, and a realized `GdkWindow` we can extract an XID from. Every native rendering surface beneath the toplevel — root or embedded — is owned by WTK.

This is consistent with — and a sharpening of — the architecture note at the top of this document: there is one NativeItem per window for the root, and embedded native surfaces don't use `NativeItem` at all (they're attached as `NativeContentNode`s on the visual tree).

#### Motivation

`GTKItem.cpp:356` currently constructs `gtk_drawing_area_new()` and pulls the X11 XID off of *that* widget's GdkWindow at `GTKItem.cpp:681` (`gdk_x11_window_get_xid`). The drawing area exists only to give us a GdkWindow whose XID we can hand to GTE. It serves no other purpose:

- We already disable GTK's painting on it (`gtk_widget_set_double_buffered(widget, FALSE)`, `gtk_widget_set_app_paintable(widget, TRUE)` — `GTKItem.cpp:359,361`).
- We never draw with cairo into its `draw` signal.
- Its event wiring (`onButtonPressEvent`, `onMotionNotifyEvent`, etc.) is generic `GtkWidget` signal plumbing — nothing on it is `GtkDrawingArea`-specific.

The drawing area is a placeholder for a GdkWindow we could have asked the toplevel for directly. Collapsing it eliminates one widget, one realized GdkWindow, one parent/child resize path, and one alignment risk where the drawing-area allocation doesn't match the toplevel client area.

#### Architecture change

**Before (today):**
```
GTKAppWindow (GtkWindow)
  └── content child (GtkBox / GtkDrawingArea)
        └── GdkWindow → X11 Window  ← GTE renders here
```

**After:**
```
GTKAppWindow (GtkWindow, app-paintable, double-buffer off)
  └── GdkWindow → X11 Window  ← GTE renders here (this is also the root NativeItem's surface)
```

`GTKItem`'s role for the root item shrinks to: a thin handle around `GTKAppWindow`'s underlying GdkWindow, exposing the same `getXDisplay()` / `getXWindow()` accessors GTE already consumes. For non-root items there *is* no `GTKItem` anymore — the virtual view tree never asks the backend for one (per the architecture note, "Views are now purely virtual").

#### Hit-testing and events on `GtkWindow` — yes, this still works

The user-facing question. Short answer: every event signal `GTKItem` listens for today is a `GtkWidget` signal, and `GtkWindow` is a `GtkWidget`. The same handlers attach to the toplevel with identical semantics:

| Today (drawing area)              | After (GtkWindow)                  | Event coords         |
|-----------------------------------|------------------------------------|----------------------|
| `button-press-event`              | `button-press-event` on GtkWindow  | `event->x/y` are window-local — exactly what `View::containsPoint` and the virtual hover dispatcher already expect |
| `button-release-event`            | same on GtkWindow                  | window-local |
| `motion-notify-event`             | same on GtkWindow                  | window-local |
| `key-press-event` / `key-release-event` | same on GtkWindow            | already toplevel-only on most GTK setups (`GTK_WIDGET_HAS_FOCUS` lives on the toplevel) |
| `enter-notify-event` / `leave-notify-event` | same on GtkWindow         | window-local |
| `scroll-event`                    | same on GtkWindow                  | window-local |
| `configure-event`                 | same on GtkWindow                  | gives the new size for swapchain resize — better than the drawing-area `size-allocate`, since this is the real OS-level resize signal |

Event masks need to be added on the GtkWindow itself before realize (today they go on the drawing area):

```cpp
gtk_widget_add_events(GTK_WIDGET(window),
    GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
    GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
    GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
    GDK_STRUCTURE_MASK);
```

Hit-testing itself is *already* virtual: `View::containsPoint` does the work, and the host-side hover dispatcher (§2.3a) walks the virtual tree given a `(x, y)` from the incoming event. None of that machinery cares whether the event arrived via a drawing area or directly on the toplevel.

One nuance worth flagging: GTK event propagation runs **child → parent**. So if any GTK child widget remains inside the window (menu bar, popover; see below), it gets a first crack at the event before the toplevel handler fires. Returning `GDK_EVENT_PROPAGATE` (`FALSE`) from those children — which is already the convention in the existing handlers — lets unhandled events bubble up to the GtkWindow handler. Menu activations naturally consume their own events, so the menu bar coexists cleanly.

#### Menu coexistence

Menus are the one remaining real child widget on GTK (per §2.12). Two options:

1. **Keep the menu bar as a sibling.** Put the GtkMenuBar in a `GtkBox` along with… nothing else; the toplevel still owns the GdkWindow, the box just occupies the top strip. GTE's render rect must be inset by the menu bar's allocated height. The hover dispatcher must subtract that inset from `event->y` before hit-testing (or simpler: install the menu bar with a fixed-y inset that we cache on `configure-event`).
2. **Render menus virtually.** Drop the GtkMenuBar entirely; let WTK render its own menu bar via the same compositor that renders Widgets. Loses native menu appearance; gains a clean, single-child-free toplevel and uniform input coordinates.

Recommendation: option 1 first (preserves the working §2.12 menu wiring), with option 2 as a follow-up once the virtual Widget tree is mature enough to draw a credible menu bar. The choice is reversible; this section doesn't have to settle it.

#### X11-only — Wayland is out of scope for this change

Owning the underlying surface as an X11 `Window` is, by definition, an X11 contract. Under Wayland, GDK manages a `wl_surface`/`xdg_surface` that the compositor controls; there is no equivalent of "grab the XID and draw into it from a foreign renderer" without going through GDK's `GdkWaylandWindow`/EGL plumbing (or, in GTK4, a `GdkSurface` with a custom renderer).

This proposal commits to the X11 path explicitly:

- **Require `GDK_BACKEND=x11`** in the GTKAppWindow ctor (or assert on `GDK_IS_X11_DISPLAY(display)` at realize-time and refuse with a clear error). The existing `gdk_x11_*` calls already fail silently on Wayland; this change makes the constraint explicit.
- **Wayland support is a future, separate workstream.** Likely either GTK4 + `GdkSurface` custom rendering or a non-GTK Wayland backend (raw `wl_compositor` + `xdg_shell`). Either way it shares no code with this X11 path.

The interim primary-monitor anchoring noted in §2.9 already assumes X11 multi-monitor semantics (`gdk_display_get_primary_monitor`); this is consistent with that assumption.

#### X11SurfaceHost — child surface lifetime and realize gate

The NativeViewHost path needs to allocate child X11 Windows under the toplevel — for video surfaces, GPU surfaces, and overlay surfaces. Doing this directly from each consumer would scatter X resource ownership across `VKVisualTree`, `VideoViewWidget`, `GTEViewWidget`, and anything else that asks for a native surface. `X11SurfaceHost` is the single per-window owner of those child surfaces.

```cpp
// wtk/src/Composition/X11SurfaceHost.h
class X11SurfaceHost {
public:
    explicit X11SurfaceHost(Display * dpy);
    ~X11SurfaceHost();

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
```

`GTKAppWindow` constructs the host in its ctor (display handle in hand), passes it to the `VKVisualTree` at tree-construction time, and tears it down in its dtor *after* the visual tree (which itself tears down all `NativeContentNode`s). This guarantees child Windows are destroyed before the toplevel.

Realization timing: a `NativeContentNode` factory called before the toplevel is realized returns a node in the non-ready state; the actual child Window allocation is queued via `runOnRealize` and fires when the realize signal arrives. This eliminates the "first frame at wrong DPI then rebuild" sequence at startup that motivated §2.9's interim primary-monitor anchoring.

#### Surface lifecycle and realization timing

The X11 `Window` only exists after the GtkWindow is realized. GTE's surface must be created on realize, not in the ctor:

```cpp
g_signal_connect(window, "realize", G_CALLBACK(onWindowRealize), this);
// onWindowRealize:
//   auto *gdk = gtk_widget_get_window(GTK_WIDGET(window));   // toplevel GdkWindow
//   Window xid = gdk_x11_window_get_xid(gdk);
//   Display *xdpy = gdk_x11_display_get_xdisplay(gtk_widget_get_display(GTK_WIDGET(window)));
//   gte->createSurfaceForX11(xdpy, xid);
```

GTKAppWindow already paths through `gtk_widget_get_window(GTK_WIDGET(window))` in several places (`GTKAppWindow.cpp:426`, `:530`, `:551`) — those become the canonical GdkWindow accessor for the root surface, replacing the drawing-area's GdkWindow that `GTKItem` currently resolves through.

Resize handling moves from the drawing-area `size-allocate` to GtkWindow `configure-event`. The `configure-event` payload includes the new `(width, height)` in window coordinates — multiply by `GTKAppWindow::scaleFactor()` (existing — see `GTKAppWindow.cpp:137`) to get physical pixels for the GTE swapchain.

Background painting needs to be turned off on the toplevel so GTK doesn't fill the GdkWindow with the theme background before GTE renders the first frame:

```cpp
gtk_widget_set_app_paintable(GTK_WIDGET(window), TRUE);
gtk_widget_set_double_buffered(GTK_WIDGET(window), FALSE);
```

These flags currently live on the drawing area (`GTKItem.cpp:359, 361`); they move up to the GtkWindow.

#### Per-file change summary

- `wtk/src/Native/gtk/GTKAppWindow.cpp` — install event masks + signal handlers (button/motion/key/scroll/enter/leave/configure) directly on the GtkWindow; emit `NativeEvent`s through the AppWindow's `eventEmitter()` (no longer routed through a child GTKItem); add `realize` handler that constructs the `X11SurfaceHost` and hands the toplevel's XID/Display to it (and through it to the compositor); turn on `app_paintable`/double-buffer-off on the GtkWindow itself.
- `wtk/src/Native/gtk/GTKItem.cpp` — for the root NativeItem, stop calling `gtk_drawing_area_new()`; instead bind to the GTKAppWindow's toplevel widget. `resolveGdkWindow()` returns `gtk_widget_get_window(GTK_WIDGET(window))` of the toplevel. Non-root `GTKItem` construction goes away (no callers under the virtual view model — NativeViewHost child surfaces are X11 child Windows owned by `X11SurfaceHost`, not `GTKItem`s). `getXDisplay()`/`getXWindow()` keep their signatures; they just resolve through the toplevel.
- `wtk/include/omegaWTK/NativePrivate/gtk/GTKItem.h` — the constructor that takes a parent `GTKItem` and creates a child drawing area becomes dead code; remove or fence off.
- `wtk/src/Native/gtk/GTKAppWindow.cpp` (menu wiring) — if option 1 (keep GtkMenuBar): retain the existing `GtkBox` layout strategy but track the menu bar's allocated height so the hover dispatcher can offset incoming `event->y`. Cache the inset on `configure-event` and on `notify::default-height`.
- `wtk/include/omegaWTK/NativePrivate/gtk/GTKAppWindow.h` — expose `menuBarInset()` (a single `float` getter) so the WidgetTreeHost hover dispatcher can subtract it before hit-testing; also expose the owned `X11SurfaceHost*` for the compositor's content-node factories.
- `wtk/src/Native/gtk/X11SurfaceHost.{h,cpp}` (new, lives under Native per §2.14) — owns the toplevel `Window` reference plus every child Window allocated for `NativeContentNode`s. Defers allocation until the toplevel is realized.
- VK visual-tree rename + file move — see §2.14. `VKFallbackVisualTree` → `VKVisualTree` and the move from `wtk/src/Composition/backend/vk/` to `wtk/src/Native/gtk/` are owned by the §2.14 work item.

#### Risks / open questions

1. **Menu-bar inset coupling.** Option 1 introduces an implicit "the menu bar's height is subtracted from event Y" rule. If a future feature adds another GTK sibling (e.g. a status bar or toolbar that GTK still owns), the inset becomes a *vector* of (top, bottom, left, right) rather than a scalar. Option 2 sidesteps this entirely. Choosing option 1 means accepting that this stays single-inset until proven otherwise.
2. **Focus and `key-press-event`.** GtkWindow gets key events when it has focus *and* no child widget consumed them first. If the GtkMenuBar swallows accelerator keys (it does, when its `mnemonic-activate` matches), the toplevel handler won't see them. This is consistent with native menu behavior, but worth confirming against §2.3a's FocusManager: the virtual focus manager only sees keys the GTK toplevel saw, not the ones the menu bar already consumed.
3. **Compositing manager interaction.** Drawing a custom surface into an X11 Window owned by GTK works fine under composited X11 (GNOME/KDE/XFCE). Under a non-compositing WM (some `i3`/`dwm` setups, especially with `xrender` disabled) the X11 Window may be unmapped/clipped in ways GdkWindow normally hides. Worth a manual smoke test on at least one non-composited WM before claiming GTK parity.
4. **Verification.** Linux is the agent's native build target, so this section can be both compile- and run-verified before claiming "Done." Mark as compile/run unverified until that pass lands (per the "mark unverified backends" rule).

#### Implementation phasing

§2.13 lands in 5 reviewable steps. Each step compiles in isolation against the
preceding one. The compositor's existing `view->getX11Window()` / `view->getDisplay()`
contract in `VKLayerTree.cpp` is preserved end-to-end — the toplevel's XID/Display
satisfies it just as well as the drawing-area's did, so VKLayerTree.cpp does NOT
have to change as part of §2.13. (It moves and renames in §2.14; §2.13 leaves it
alone.)

1. **Phase 1 — X11SurfaceHost (new files, no behavior change).**
   Add `wtk/src/Native/private_include/NativePrivate/gtk/X11SurfaceHost.h` and
   `wtk/src/Native/gtk/X11SurfaceHost.cpp`. Class owns the toplevel `Window`
   reference plus a registry of child Windows, and queues `runOnRealize` callbacks
   until `onToplevelRealized` fires. `createChildWindow` / `reconfigureChildWindow`
   / `destroyChildWindow` are implemented but unused at this phase — they will
   be called by §2.14 (`VKVisualTree`) and `NativeViewHost-Adoption-Plan.md`.
   The CMake `file(GLOB)` over `src/Native/gtk/*.cpp` picks the new TU up
   automatically.

2. **Phase 2 — GTKAppWindow.h public surface (header extraction).**
   Promote the currently inline-defined `GTKAppWindow` class to a header at
   `wtk/src/Native/private_include/NativePrivate/gtk/GTKAppWindow.h`. The body
   stays in `GTKAppWindow.cpp` for now. The header exposes the accessors §2.13
   adds: `getGTKWindow()`, `menuBarInset()`, and `surfaceHost()`. The existing
   `gtk_window_from_native(NWH)` helper is preserved.

3. **Phase 3 — GTKAppWindow.cpp refactor.**
   The substantive change. Install button/motion/key/scroll/enter/leave/configure
   handlers (plus the existing `realize`, `delete-event`, `window-state-event`,
   `notify::scale-factor`) directly on the GtkWindow with the matching event mask
   added via `gtk_widget_add_events` before `gtk_widget_show*`. Turn on
   `app_paintable` and `double_buffered=FALSE` on the toplevel. Construct the
   owned `X11SurfaceHost` in the ctor (display in hand). In the realize handler,
   resolve the toplevel's XID and call `X11SurfaceHost::onToplevelRealized` —
   this is what unblocks queued `runOnRealize` callbacks (used in §2.14 for the
   first-frame Vulkan target resolve). All input event emission moves from
   `GTKItem` to the AppWindow's `eventEmitter()`. Track the menu bar's allocated
   height on `configure-event` and on `size-allocate` of `menuWidget`; expose it
   via `menuBarInset()`.

4. **Phase 4 — GTKItem.cpp/h refactor.**
   Root-mode `GTKItem` no longer creates `gtk_drawing_area_new()`. A new
   constructor variant takes a `GtkWidget *toplevel` and binds to it: `widget`
   stores the toplevel, `resolveGdkWindow()` returns
   `gtk_widget_get_window(toplevel)`. The existing event-handler installation
   on `widget` is dropped (handlers moved to GTKAppWindow in Phase 3). The
   old non-root drawing-area path becomes unreachable under the virtual view
   model — `make_native_item(rect, ItemType, parent)` is no longer called with
   a non-null `parent` anywhere in the codebase, so the implementation now
   asserts on it and the `addChildNativeItem` / `setClippedView` paths are
   reduced to no-ops. `getDisplay()` and `getX11Window()` keep their signatures.
   The size of `GTKItem` shrinks significantly.

5. **Phase 5 — Build + smoke test.**
   Configure + ninja build with clang++ out-of-source. Resolve compile errors.
   Run a WTK demo and confirm a window appears, mouse/keys still route, and
   Vulkan still presents. Linux is the native build target, so this is both
   compile- and run-verifiable on this host.

**Mark status:** compile- and run-verified on Linux/X11 once Phase 5 lands.
Wayland is out of scope for §2.13 (see "X11-only" subsection above) and
remains untouched — `WTK_NATIVE_WAYLAND` builds keep the legacy code paths
under their existing `#if` guards.

#### Dependencies

**§2.13 depends on:** Nothing else in this proposal. The X11-direct collapse is self-contained — it touches `GTKAppWindow`, `GTKItem`, and adds `X11SurfaceHost`.

**§2.13 enables:**
- **§2.14 NativeVisualTree (Linux branch only)** — `VKVisualTree` uses `X11SurfaceHost` to allocate and re-stack child Windows under the toplevel. macOS and Windows §2.14 work do not depend on §2.13.
- **`NativeViewHost-Adoption-Plan.md` Linux paths** — `VKNativeContentNode`'s underlying surface is a child Window allocated via `X11SurfaceHost::createChildWindow`.

The interim primary-monitor anchoring described at the bottom of this section is superseded by §2.9 NativeScreen when that lands; §2.13 can ship using the interim and §2.9 retrofits later (no order dependency between §2.13 and §2.9).

---

### 2.14 NativeVisualTree — Per-window Compositor Tree, Owned by Native

**Goal:** The per-window compositor visual tree (DirectComposition / CoreAnimation / X11 child-window stack) moves from the Composition layer into Native, where `FrameBuilder` can reconfigure it directly during layout — without queueing work onto the compositor thread or waiting for the compositor's next frame.

This is a platform-uniform structural change. It pairs with `NativeViewHost-Adoption-Plan.md` (which describes the consumer-facing API) and with §2.13 (which already commits the Linux side to direct X11 surface ownership).

#### Motivation

Today, the visual tree (`BackendVisualTree` and its per-platform subclasses) lives in `wtk/src/Composition/backend/`. The compositor owns it. Per-frame native-content positioning runs through a record-then-drain hook (`BackendRenderTargetContext::pendingNativeContent_` filled during the slice loop, then `BackendVisualTree::applyNativeContentCarveouts` called after the slice loop, before present).

That puts native-surface position one full compositor frame behind virtual layout. For a static UI this is invisible. For a resizing widget, an animating layout, or any case where the compositor's cadence diverges from the layout cadence (display-link vs animation tick, paused compositor, throttling), the native surface lags the virtual content it should be tracking. On a video viewport or a 3D viewport this is a visible glitch on every interaction frame.

Lifting the visual tree into Native lets `FrameBuilder` reconfigure the native surface during the same layout pass that repositions every other virtual widget — synchronously, on the main thread. The native surface moves in lockstep with virtual content.

#### Architecture change

**Before (today):**
```
AppWindow (Native)
  └── BackendResourceFactory (Composition)
        └── createVisualTreeForView → BackendVisualTree (Composition)
              ├── RootVisual (with BackendRenderTargetContext — compositor)
              └── Body visuals (with BackendRenderTargetContext)

NativeViewHost::onLayoutResolved
  → display list emits DrawOp::NativeContent
  → slice loop records BackendNativeContentRegion
  → applyNativeContentCarveouts drains records, applies to platform tree
                                              ─── one frame later ───
```

**After:**
```
AppWindow (Native)
  ├── NativeWindow
  ├── X11SurfaceHost          [Linux only — see §2.13]
  └── Native::VisualTree
        ├── rootVisual()        (Visual* — platform handle only)
        └── content nodes        (Visual* per NativeViewHost)

Compositor (Composition)
  └── Visual* → BackendRenderTargetContext  [side map, keyed by Visual*]

NativeViewHost::onLayoutResolved
  → visualTree->reconfigureContentNode(hostId, rectPx, zOrderHint)
                                              ─── synchronous ───
```

The compositor still renders into the tree's root visual every frame. It just doesn't own the tree, and it doesn't mediate native-surface positioning.

#### API surface

```cpp
namespace OmegaWTK::Native {

class VisualTree {
public:
    // Per-window content-node registry.
    void registerContentNode(NativeContentNodePtr node);
    void unregisterContentNode(std::uint64_t hostId);

    /// Called from NativeViewHost::onLayoutResolved on the main thread.
    /// Translates `rectPixels` and `zOrderHint` into the platform's
    /// native ordering primitive synchronously.
    void reconfigureContentNode(std::uint64_t hostId,
                                 const Composition::Rect & rectPixels,
                                 int zOrderHint);

    /// Compositor-side root present surface (WTK swap chain). Read-only
    /// from outside the compositor — created by AppWindow, handed to
    /// the compositor at render-target setup.
    Visual * rootVisual() const;

    /// Consumer-facing factories: content node creation + registration
    /// is atomic — the tree is the registry.
    NativeContentNodePtr createVideoContentNode(Composition::Rect rect);
    NativeContentNodePtr createGPUContentNode  (Composition::Rect rect);

    virtual ~VisualTree() = default;
};

struct Visual {
    std::uint64_t hostId = 0;
    Composition::Rect rectPixels;
    int zOrderHint = 0;
    // Platform handle (IDCompositionVisual2*/CALayer*/Window) exposed
    // by the per-platform subclass. No compositor types here.
};

}
```

#### The Visual / BackendRenderTargetContext decoupling

Today's `BackendVisualTree::Visual` owns a `std::unique_ptr<BackendRenderTargetContext>`. `BackendRenderTargetContext` is the compositor's per-render-target wrapper (~500 lines: `GENativeRenderTarget`, command queue, tessellation context, fence, `FrameRenderPass`, blur scratch, the slice loop's paint state). Moving `Visual` to Native without action would invert layering — Native depending on Composition.

The fix: `Visual` becomes a pure Native primitive (position + size + platform handle). The compositor maintains a separate `Visual* → BackendRenderTargetContext` side map (the same idiom already used by `PreCreatedResourceRegistry`) and resolves the render context at use sites. The compositor's render machinery is otherwise unchanged.

#### Removal of the carve-out drain machinery

These existing pieces are deleted because the producer-side path goes away entirely:

- `BackendNativeContentRegion` struct (`wtk/src/Composition/backend/RenderTarget.h:55`)
- `BackendRenderTargetContext::pendingNativeContent_` member + `pendingNativeContent()` / `clearPendingNativeContent()` accessors
- `BackendVisualTree::applyNativeContentCarveouts` virtual hook (`wtk/src/Composition/backend/VisualTree.h:80`)
- Per-tree overrides: `DCVisualTree::applyNativeContentCarveouts`, `MTLCALayerTree::applyNativeContentCarveouts`, `VKVisualTree::applyNativeContentCarveouts`
- The recording branch in `BackendRenderTargetContext::renderToTarget`'s `case PrimitiveOp::NativeContent` (it stops pushing to `pendingNativeContent_`; alpha-clear + AABB-cull bookkeeping remain)

`DrawOp::NativeContent` itself stays in the display list — it's still needed for alpha-clear (swap-chain transparency in the carve-out rect) and AABB cull (skip sibling ops whose bounds fall inside the rect).

#### File moves and renames

| Before                                                    | After                                                |
|-----------------------------------------------------------|------------------------------------------------------|
| `wtk/src/Composition/backend/VisualTree.h`                | `wtk/include/omegaWTK/Native/NativeVisualTree.h`     |
| `wtk/src/Composition/backend/dx/DCVisualTree.{h,cpp}`     | `wtk/src/Native/win/DCVisualTree.{h,cpp}`            |
| `wtk/src/Composition/backend/mtl/CALayerTree.{h,mm}`      | `wtk/src/Native/macos/MTLVisualTree.{h,mm}`          |
| `wtk/src/Composition/backend/vk/VKLayerTree.cpp`          | `wtk/src/Native/gtk/VKVisualTree.cpp`                |

Class renames:
- `BackendVisualTree` → `Native::VisualTree`
- `MTLCALayerTree` → `MTLVisualTree`
- `VKFallbackVisualTree` → `VKVisualTree`
- `DCVisualTree` keeps its name (already matches the new convention)

`NativeContentNode` and its per-platform subclasses also land under `OmegaWTK::Native` (earlier drafts had them tentatively under `OmegaWTK::Composition`).

#### Per-platform implementation notes

- **macOS** — `MTLVisualTree` wraps the window's content-view `CALayer`. Root visual is the WTK `CAMetalLayer`; content nodes are `CALayer` siblings/sub-layers added with explicit `zPosition`. `reconfigureContentNode` is `layer.frame = newFrame; layer.zPosition = zOrderHint;` inside a `CATransaction` (no implicit animation).
- **Windows** — `DCVisualTree` wraps the window's `IDCompositionTarget` + root `IDCompositionVisual2`. Root visual hosts the WTK swap chain via `SetContent`. Content nodes are sub-visuals with their own swap chains. `reconfigureContentNode` is `SetOffsetX/Y` + `SetTransform` + child-list reorder, then `Commit` on the desktop device.
- **Linux X11** — `VKVisualTree` collaborates with `X11SurfaceHost`. Root visual is the toplevel `Window`; content nodes are child Windows allocated via `X11SurfaceHost::createChildWindow`. `reconfigureContentNode` calls `X11SurfaceHost::reconfigureChildWindow` (`XMoveResizeWindow` + `XRestackWindows`).

#### Ownership and lifecycle

`AppWindow` is the sole owner of the per-window `Native::VisualTree`. Construction order in the AppWindow ctor: `NativeWindow` → `X11SurfaceHost` (Linux only) → `Native::VisualTree`. Destruction order is the reverse. The compositor holds a non-owning `Native::VisualTree*` and a `Visual* → BackendRenderTargetContext` side map keyed off the tree's visuals; the side map is cleared in the compositor's per-window teardown before `AppWindow` releases the tree.

`BackendResourceFactory::createVisualTreeForView`, `VisualTreeBundle`, `PreCreatedVisualTreeData`, and `PreCreatedResourceRegistry` are removed. The pre-creation queue was specifically there to bridge View constructors (main thread) to the compositor thread; that bridge no longer needs a queue because the tree is created once in the AppWindow ctor (also main thread) and looked up via `AppWindow::visualTree()`.

#### Threading

All `Native::VisualTree` mutations happen on the main thread — the same rule that already governs DComp `Commit`, `CALayer` updates inside `CATransaction`, and X11 `XMoveResizeWindow` calls. `FrameBuilder` runs on the main thread, so direct calls are safe. The compositor frame worker runs on its own thread, reads the root visual's render context (which it owns through the side map), and does not touch the tree's structure. No new locking required.

#### Per-file change summary

- `wtk/include/omegaWTK/Native/NativeVisualTree.h` (new) — abstract `Native::VisualTree`, `Native::Visual` struct, content-node factories.
- `wtk/src/Native/win/DCVisualTree.{h,cpp}` (moved from `wtk/src/Composition/backend/dx/`) — DComp implementation.
- `wtk/src/Native/macos/MTLVisualTree.{h,mm}` (moved + renamed from `wtk/src/Composition/backend/mtl/CALayerTree.{h,mm}`) — CoreAnimation implementation.
- `wtk/src/Native/gtk/VKVisualTree.cpp` (moved from `wtk/src/Composition/backend/vk/VKLayerTree.cpp`) — Vulkan + X11 implementation. The `VKFallbackVisualTree` → `VKVisualTree` rename folds in here.
- `wtk/src/Composition/backend/RenderTarget.{h,cpp}` — drop `BackendNativeContentRegion`, `pendingNativeContent_`, the `case PrimitiveOp::NativeContent` recording branch (keep the alpha-clear / AABB-cull bookkeeping).
- `wtk/src/Composition/backend/ResourceFactory.{h,cpp}` — remove `createVisualTreeForView`, `VisualTreeBundle`, `PreCreatedVisualTreeData`, `PreCreatedResourceRegistry`. Keep the pipelines / pools / heaps / effect-processor responsibilities.
- `wtk/src/Composition/Compositor.{h,cpp}` (or wherever the per-tree render contexts are managed) — add the `Visual* → BackendRenderTargetContext` side map, owned by the compositor; built when a tree is attached, torn down when detached.
- `wtk/src/UI/AppWindow.{h,cpp}` — construct `Native::VisualTree` in ctor; expose `visualTree()` accessor; tear down in dtor after `X11SurfaceHost` (Linux) and before `NativeWindow`.
- `wtk/include/omegaWTK/UI/NativeViewHost.h` — `attach` now takes `Native::NativeContentNodePtr` (was the earlier tentative `Composition::NativeContentNodePtr`). `onLayoutResolved` body calls `visualTree->reconfigureContentNode` directly.

#### Risks / open questions

1. **Side-map lifetime sequencing.** The compositor's `Visual* → BackendRenderTargetContext` map must drain before `AppWindow` releases the tree (else dangling `Visual*` keys). The compositor's per-window teardown already has a deterministic shutdown order; adding a "clear render-context side map" step is mechanical, but worth a one-time audit for window tear-down races.
2. **Composition consumers of `BackendVisualTree`.** Any code outside the moved files that names `BackendVisualTree`, `BackendVisualTree::Visual`, or the per-platform classes needs to be updated. The codedb search at the start of the work should produce the full list; it is small (the classes are mostly internal to the backend already), but worth confirming.
3. **Compositor's first-frame fallback (Linux).** `VKLayerTree.cpp` today has a "deferred native target resolve" path for the case where the GdkWindow isn't realized at tree-construction time (`resolveDeferredNativeTarget`). After the move, the same mechanism lives in `VKVisualTree.cpp` and is wired through `X11SurfaceHost::runOnRealize` (already part of §2.13). This is a wiring change, not new functionality.
4. **Verification.** Linux is the agent's native build target — both the move and the FrameBuilder hookup are compile- and run-verifiable in-house. macOS and Windows are unverified off-platform until those builds run; mark accordingly per the "mark unverified backends" rule.

#### Dependencies

**§2.14 depends on:**
- **§2.13 X11SurfaceHost (Linux branch only)** — `VKVisualTree`'s implementation calls `X11SurfaceHost::createChildWindow` / `reconfigureChildWindow` / `destroyChildWindow` and uses the `runOnRealize` deferral path for the case where a content node is requested before the toplevel is realized. macOS (`MTLVisualTree`) and Windows (`DCVisualTree`) branches have no §2.13 dependency and can land first.

**§2.14 enables:**
- **`NativeViewHost-Adoption-Plan.md` Shared prerequisite** — and therefore Parts 1 and 2 (V1–V4, G1–G4) in their entirety. `NativeViewHost::onLayoutResolved` calls `Native::VisualTree::reconfigureContentNode` directly; without §2.14 there is no tree to call.
- **Any future native-surface widget** (web view, IME field, platform-specific media surfaces) — they all attach `NativeContentNode`s to the per-window `Native::VisualTree`.

**Staging.** §2.14 can be split across two passes if helpful:
1. *Cross-platform pass* — file moves + namespace shift + Visual/RTC decoupling + drain removal + Compositor side-map. Lands once, touches all three backends.
2. *Per-platform pass* — `reconfigureContentNode` + content-node factories per platform. Can land per-OS as build verification clears each one. Linux is the gated one (waits on §2.13).

§2.14 has no other dependencies in this proposal. It can land in parallel with §2.2, §2.3a, §2.4, §2.6, §2.7, §2.9, §2.10.

---

## 3. Implementation Priority

| Priority | Feature | Rationale |
|----------|---------|-----------|
| ~~**P0**~~ **Done** | 2.1 NativeEvent | Implemented |
| ~~**P0**~~ **Done** | 2.5 NativeTheme | Implemented |
| ~~P1~~ **Mostly done** | 2.2 NativeWindow (state, scaleFactor, opacity, cursor sink, key-window) | Header surface + all three backends shipped including the `WindowScaleFactorChanged` emit. Remaining: AppWindow's auto-propagation handler for the scale-change event (one cross-platform `.cpp` change) and `ViewRenderTarget::scaleChanged()` (cross-plan to `DPI-Aware-Text-Plan.md`). |
| **P1** | 2.3a View focus + cursor + Widget tooltip + WidgetTreeHost FocusManager | Per-view keyboard routing, hover cursor, tooltip popups |
| **P1** | 2.4 NativeApp (args, delegate, timers) | App lifecycle and command-line |
| **P1** | 2.6 NativeClipboard | Copy/paste is fundamental UX |
| ~~P1~~ **Done** | 2.8 NativeDialog (alert dialog, file filters) | Implemented |
| **P2** | 2.7 NativeDragDrop | Important for content apps, less critical initially |
| **P0** | **2.9 NativeScreen** (prerequisite) | **Lands first.** The cross-platform per-screen surface that §2.2's DPI consumer, Phase F, and Phase H all resolve through (geometry, DPI, refresh rate / vsync `NativeDisplayLink`, screen targeting for `AppWindow`). Also replaces the interim GTK primary-monitor anchoring. |
| ~~P1~~ **Done** | 2.11 NativeNote / NotificationCenter | Implemented |
| ~~P1~~ **Done** | 2.12 NativeMenu / Menu | Implemented |
| **P1** | 2.13 Linux/X11 direct surface ownership (root + NativeViewHost) + `X11SurfaceHost` | Pairs with §2.2/§2.9 and `NativeViewHost-Adoption-Plan.md` — locks Linux to a single, canonical surface-ownership model before per-platform features and the NativeViewHost-based widgets (`VideoViewWidget`, `GTEViewWidget`) start landing |
| **P1** | 2.14 NativeVisualTree (Composition → Native move, FrameBuilder direct path, drain removal, file moves + class renames) | Prerequisite for `NativeViewHost-Adoption-Plan.md` — without the move, native surfaces lag virtual layout by a compositor frame. Should land alongside §2.13 since both touch the same per-window structure |
| **P3** | 2.10 NativeAccessibility | Stub now, implement per-platform over time |

---

## 4. File Change Summary

### New headers (`wtk/include/omegaWTK/Native/`)
- `NativeTimer.h`
- `NativeClipboard.h`
- `NativeDragDrop.h`
- `NativeScreen.h`
- `NativeAccessibility.h`

### Modified headers
- `NativeWindow.h` — window state APIs, `scaleFactor`, `setOpacity`, `setCursorShape` sink, `isKeyWindow`/`becomeKeyWindow`, `CursorShape` enum; remove `#ifdef TARGET_MACOS` guards around `rect` and `eventEmitter`
- `NativeEvent.h` — add `WindowScaleFactorChanged` enum case + `WindowScaleFactorChangedParams` struct (one-line delta to the otherwise-Done §2.1)
- `NativeItem.h` — **no changes** (virtual view model removes the per-View item)
- `NativeApp.h` — NativeAppDelegate, commandLineArgs(), launchArgs()
- `NativeDialog.h` — NativeAlertDialog, FileFilter + allowMultiple in FS descriptor, `getResult()` → `Async<Vector<FS::Path>>` ✅
- `GTKDialog.cpp` (new, GTK backend), `GTKApp.h`/`GTKAppWindow.cpp` (`gtk_window_from_native` helper), `AppWindow.h`/`.cpp` (`openAlertDialog`) ✅
- `View.h` (UI) — `setFocusable`/`isFocusable`/`isFocused`/`focus`/`blur`, `setCursorShape`/`cursorShape`
- `Widget.h` (UI) — `setTooltip`/`clearTooltip`
- `WidgetTreeHost` (UI, internal) — owns `FocusManager` and tooltip-hover timer; routes keyboard events to focused View; commits hovered View's cursor shape to the root NativeWindow
- `AppWindow.h` / `AppWindow.cpp` (UI) — screen-targeting constructor overload (`AppWindow(rect, NativeScreenDesc, delegate)`), `currentScreen()`, `moveToScreen()`; existing constructor resolves through `AppWindowManager::defaultScreen()`
- `AppWindowManager` (UI) — `setDefaultScreen` / `defaultScreen` for app-wide default targeting

### GTK backend — §2.13 (X11-direct root surface)
- `wtk/src/Native/gtk/GTKAppWindow.cpp` — install root event masks + button/motion/key/scroll/enter/leave/configure/realize handlers directly on the GtkWindow; route emissions through the AppWindow `eventEmitter()`; turn on `app_paintable` + double-buffer-off on the toplevel; menu-bar inset tracking
- `wtk/src/Native/gtk/GTKItem.cpp` — root NativeItem binds to the toplevel GdkWindow; remove `gtk_drawing_area_new()` path for the root; non-root construction becomes unreachable under the virtual view model
- `wtk/include/omegaWTK/NativePrivate/gtk/GTKItem.h` / `GTKAppWindow.h` — drop the child-of-`GTKItem` constructor; expose `GTKAppWindow::menuBarInset()` for the hover dispatcher

### NativeVisualTree — §2.14 (Composition → Native move, all platforms)
- `wtk/include/omegaWTK/Native/NativeVisualTree.h` (new) — abstract `Native::VisualTree`, `Native::Visual`, content-node factories
- `wtk/src/Native/win/DCVisualTree.{h,cpp}` (moved from `wtk/src/Composition/backend/dx/`)
- `wtk/src/Native/macos/MTLVisualTree.{h,mm}` (moved + renamed from `wtk/src/Composition/backend/mtl/CALayerTree.{h,mm}`; class `MTLCALayerTree` → `MTLVisualTree`)
- `wtk/src/Native/gtk/VKVisualTree.cpp` (moved from `wtk/src/Composition/backend/vk/VKLayerTree.cpp`; the `VKFallbackVisualTree` → `VKVisualTree` rename folds in here)
- `wtk/src/Native/gtk/X11SurfaceHost.{h,cpp}` (new — see §2.13)
- `wtk/src/Composition/backend/RenderTarget.{h,cpp}` — drop `BackendNativeContentRegion`, `pendingNativeContent_`, the `case PrimitiveOp::NativeContent` recording branch (keep alpha-clear / AABB-cull bookkeeping)
- `wtk/src/Composition/backend/ResourceFactory.{h,cpp}` — remove `createVisualTreeForView`, `VisualTreeBundle`, `PreCreatedVisualTreeData`, `PreCreatedResourceRegistry`
- `wtk/src/Composition/Compositor.{h,cpp}` — add the `Visual* → BackendRenderTargetContext` side map; build on tree attach, tear down on detach
- `wtk/src/UI/AppWindow.{h,cpp}` — own `Native::VisualTree`; expose `visualTree()`; lifecycle ordering with `NativeWindow` and `X11SurfaceHost`
- `wtk/include/omegaWTK/UI/NativeViewHost.h` — `attach` takes `Native::NativeContentNodePtr`; `onLayoutResolved` calls `visualTree->reconfigureContentNode` directly

### Already modified (done)
- `NativeEvent.h` — complete ✅
- `NativeTheme.h` — complete ✅
- `NativeNote.h` — complete ✅
- `NativeMenu.h` — complete ✅
- `Notification.h` / `Notification.cpp` (UI) — complete ✅
- `Menu.h` / `Menu.cpp` (UI) — complete ✅

### New source files per platform
Each backend needs:
- Timer implementation
- Clipboard implementation
- DragDrop implementation
- Screen enumeration

### Cross-platform dispatchers (`wtk/src/Native/`)
- `NativeTimer.cpp`
- `NativeClipboard.cpp`
- `NativeDragDrop.cpp`
- `NativeScreen.cpp`
- `NativeAccessibility.cpp`

---

## 5. References

- Current headers: `wtk/include/omegaWTK/Native/`
- macOS backend: `wtk/src/Native/macos/`
- Win32 backend: `wtk/src/Native/win/`
- GTK backend: `wtk/src/Native/gtk/`
- UI layer consumers: `wtk/include/omegaWTK/UI/` (View.h, Widget.h, AppWindow.h, Menu.h, Notification.h)
