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
| 2.13 GTK root NativeItem collapse | GTK backend owns the underlying X11 surface directly; root NativeItem falls through to the GTKAppWindow (no GtkDrawingArea indirection) | Not started |
| 2.2 NativeWindow | Full window control (minimize/maximize/fullscreen, scaleFactor, opacity, cursor sink, DPI scale change events) | Not started |
| ~~2.3 NativeItem~~ | **Obsolete under virtual view model — no new NativeItem APIs.** See §2.3 below. | Removed |
| 2.3a View / Widget / TreeHost focus + cursor + tooltip | Virtual focus manager, declarative cursor shape, virtual tooltip popups | Not started |
| 2.4 NativeApp | Delegate, command-line args, timers | Not started |
| 2.6 NativeClipboard | New subsystem | Not started |
| 2.7 NativeDragDrop | New subsystem | Not started |
| **2.8 NativeDialog** | Alert dialog (Result), file filters + multi-select, GTK backend | **Done** |
| 2.9 NativeScreen | New subsystem; owns AppWindow → screen targeting (replaces GTK's interim primary-monitor anchoring) | Not started |
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
| `NativeTheme.h` | Theme query and observer | macOS, Win32; GTK missing |

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

**Default AppWindow behavior — automatic propagation.** Apps must not be required to subscribe explicitly to keep text and bitmaps crisp. The platform `AppWindow` subscribes to its own `NativeWindow`'s emitter and runs this default handler:

1. Tell the visual tree the new scale: `view->setRenderScale(newScale)` recurses through the View tree, updating each `Composition::ViewRenderTarget::renderScale_`.
2. Trigger a render-target rebuild: each `ViewRenderTarget` reallocates its backing surface at `rect.{w,h} * newScale` (the existing `setRenderScale` mutator already exists at `wtk/src/Composition/CompositorClient.cpp:175`; the missing piece is the rebuild trigger — call out to a new `ViewRenderTarget::scaleChanged()` that re-runs the same path the constructor takes for `backingWidth_` / `backingHeight_`).
3. Invalidate every View so a full repaint occurs at the next vsync.
4. On Win32, also call `nativeWindow->setRect(*params.suggestedRect)` if present, before step 1, so the OS-suggested physical-size-preserving rect lands.

App-level code that needs to react beyond the default (e.g. a custom asset cache keyed on scale) attaches its own listener to the same emitter — `NativeEventEmitter` is multi-receiver per §2.1.

#### Connection to TextRect / TextLayout

The DPI plan (`DPI-Aware-Text-Plan.md`) already routes `renderScale` through `View::getRenderScale()` into the text pipeline. The change-event flow does not introduce a new path — it re-runs the existing one:

- **Today's `Canvas::drawText`** (no caching): every call constructs a fresh `TextRect::Create(rect, layoutDesc, renderScale)`. After the AppWindow handler updates `renderScale_` and triggers a repaint, the next `drawText` reads the new value through `ownerView_->getRenderScale()` and the `TextRect` is built at the right physical size. **No additional wiring needed.**

- **Phase 6 `TextLayout` cache** (`Composition-Extension-Plan.md` §6.3): `Canvas::drawTextLayout` reads `ownerView_->getRenderScale()` on every draw and passes it through to the handle's resolve. The handle compares to the cached scale; mismatch → layout-dirty rebuild. So when the AppWindow handler updates `renderScale_`, every held `TextLayout` rebuilds its `TextRect` and re-uploads its `GETexture` on the very next paint. **No additional wiring needed in Phase 6 either** — the per-draw re-read was designed for exactly this event.

In other words, the event landing on `NativeWindow` propagates through:

```
WindowScaleFactorChanged
   │
   ▼
AppWindow default handler
   │  view->setRenderScale(newScale)  →  ViewRenderTarget::renderScale_
   │  view->setNeedsDisplay()
   ▼
Next paint cycle
   │  Canvas::drawText / drawTextLayout reads ownerView_->getRenderScale()
   │  → mismatch detected → TextRect rebuilt at new scale
   ▼
Crisp glyphs on the new monitor
```

The only cross-plan work item is `ViewRenderTarget::scaleChanged()` (the rebuild trigger). That belongs in `DPI-Aware-Text-Plan.md`'s "Per-monitor DPI updates" section (no longer a non-goal once §2.2 lands).

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

#### Focus — virtual focus manager

There is exactly one OS focus per window (the root NativeItem is always first responder when the window is key). Per-view keyboard focus is a virtual concept managed by `WidgetTreeHost`.

```cpp
// New: focus state lives on View
class View {
    // ... existing ...
    void setFocusable(bool focusable);
    bool isFocusable() const;
    bool isFocused() const;     // True iff the host's focus manager has selected this view
    void focus();               // Request focus from the host's focus manager
    void blur();
};

// New: the focus manager. Owned by WidgetTreeHost (one per AppWindow).
class FocusManager {
public:
    void setFocus(View * view);     // Emits FocusLost on previous, FocusGained on new
    View * focusedView() const;
    void clearFocus();

    // Tab traversal — host-level keyboard navigation.
    void focusNext();
    void focusPrevious();
};
```

When the AppWindow receives a `KeyDown` / `KeyUp` event from its root NativeItem, the WidgetTreeHost routes it to `focusManager->focusedView()` instead of broadcasting. `FocusGained` / `FocusLost` events (already defined in `NativeEvent.h`) are emitted virtually by the focus manager — no native call required.

#### Cursor shape — declarative on View, committed by the dispatcher

```cpp
class View {
    // ... existing ...
    void setCursorShape(CursorShape shape);   // Declarative: "if cursor is over me, show this"
    CursorShape cursorShape() const;
};
```

The hover dispatcher in `WidgetTreeHost` (the same one that emits virtual `CursorEnter` / `CursorExit`) calls `nativeWindow->setCursorShape(...)` whenever the topmost hovered virtual view changes. Views never touch the OS cursor directly.

#### Tooltip — virtual popup on Widget

`NSView.toolTip` and `gtk_widget_set_tooltip_text` only work on real native views. Since virtual views aren't native views, tooltips must be rendered by WTK itself — a small composited popup window (or sibling layer) shown after a hover delay.

```cpp
class Widget {
    // ... existing ...
    void setTooltip(const OmegaCommon::String & text);
    void clearTooltip();
};
```

The `WidgetTreeHost` runs the hover-delay timer (via `NativeTimer`, §2.4) and owns the popup surface. On platforms that support a borderless toplevel (`NSPanel`/`WS_POPUP`/`GtkPopover`) this is straightforward; on macOS specifically, an alternative is to set `NSView.toolTip` on the *root* NativeItem and rewrite it on hover — but the WTK-rendered popup keeps platforms uniform.

> **Note:** Hit-testing is already covered by `View::containsPoint`.

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

### 2.5 NativeTheme — Rich Theme Descriptor ✅ Done

Implemented. See `NativeTheme.h`: `ThemeAppearance` (Light/Dark), `ThemeDesc` with `Colors` and `Typography` sub-structs, `queryCurrentTheme()`.

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

### 2.9 NativeScreen (New)

**Goal:** Enumerate connected displays, query their properties, and pick which screen a new `AppWindow` opens on.

New header `NativeScreen.h`:

```cpp
struct NativeScreenDesc {
    unsigned id = 0;
    Composition::Rect frame;            // Virtual-screen coordinates (DIPs)
    Composition::Rect visibleFrame;     // frame minus menu bars / docks / panels
    float scaleFactor = 1.f;            // Combined logical→physical (matches NativeWindow::scaleFactor())
    bool isPrimary = false;
};

OMEGAWTK_EXPORT OmegaCommon::Vector<NativeScreenDesc> enumerateScreens();
OMEGAWTK_EXPORT NativeScreenDesc primaryScreen();

// Resolve a screen by id; returns primaryScreen() when id is unknown so
// callers always have a valid target.
OMEGAWTK_EXPORT NativeScreenDesc screenById(unsigned id);
```

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

- **macOS** — `NSScreen.screens`. Primary is `[NSScreen mainScreen]`. `frame` is `[NSScreen frame]` in points; `visibleFrame` from `visibleFrame`. `scaleFactor` from `backingScaleFactor`. AppWindow construction sets `[window setFrame:display:]` with the screen-translated rect.
- **Win32** — `EnumDisplayMonitors` + `GetMonitorInfo`. Primary is the monitor with `MONITORINFOF_PRIMARY`. `scaleFactor` from `GetDpiForMonitor(...) / 96.0`. `visibleFrame` from `rcWork`. AppWindow construction passes the screen-translated rect to `CreateWindowEx` / `SetWindowPos`.
- **GTK / Linux** — `gdk_display_get_monitors` (or `gdk_display_get_n_monitors` + `gdk_display_get_monitor` in GTK3). Primary is `gdk_display_get_primary_monitor`. `frame`/`visibleFrame` from `gdk_monitor_get_geometry` / `gdk_monitor_get_workarea`. `scaleFactor` is the **combined** product `gdk_screen_get_resolution()/96 × gdk_monitor_get_scale_factor()` to match `GTKAppWindow::scaleFactor()` (see `DPI-Aware-Text-Plan.md`). AppWindow construction translates `rect.pos` by the monitor's `geometry.{x,y}`.

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

### 2.13 GTK Backend — Root NativeItem Collapses Into GTKAppWindow (X11-Direct Surface Ownership)

**Goal:** On the GTK backend, the single root NativeItem (per the virtual view model above) is *not* a GtkDrawingArea hosted inside a GtkWindow — it falls through to the `GTKAppWindow` itself. GTE renders directly into the X11 `Window` underneath `GTKAppWindow`'s `GdkWindow`. WTK owns the X11 surface; GTK is reduced to providing the toplevel, event dispatch, menus, and a realized GdkWindow we can extract an XID from.

This is consistent with — and a sharpening of — the architecture note at the top of this document: there is one NativeItem per window, and on GTK that NativeItem and the platform AppWindow are the *same* object rather than a parent/child pair.

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

- `wtk/src/Native/gtk/GTKAppWindow.cpp` — install event masks + signal handlers (button/motion/key/scroll/enter/leave/configure) directly on the GtkWindow; emit `NativeEvent`s through the AppWindow's `eventEmitter()` (no longer routed through a child GTKItem); add `realize` handler that hands the toplevel's XID/Display to GTE; turn on `app_paintable`/double-buffer-off on the GtkWindow itself.
- `wtk/src/Native/gtk/GTKItem.cpp` — for the root NativeItem, stop calling `gtk_drawing_area_new()`; instead bind to the GTKAppWindow's toplevel widget. `resolveGdkWindow()` returns `gtk_widget_get_window(GTK_WIDGET(window))` of the toplevel. Non-root `GTKItem` construction goes away (no callers under the virtual view model). `getXDisplay()`/`getXWindow()` keep their signatures; they just resolve through the toplevel.
- `wtk/include/omegaWTK/NativePrivate/gtk/GTKItem.h` — the constructor that takes a parent `GTKItem` and creates a child drawing area becomes dead code; remove or fence off.
- `wtk/src/Native/gtk/GTKAppWindow.cpp` (menu wiring) — if option 1 (keep GtkMenuBar): retain the existing `GtkBox` layout strategy but track the menu bar's allocated height so the hover dispatcher can offset incoming `event->y`. Cache the inset on `configure-event` and on `notify::default-height`.
- `wtk/include/omegaWTK/NativePrivate/gtk/GTKAppWindow.h` — expose `menuBarInset()` (a single `float` getter) so the WidgetTreeHost hover dispatcher can subtract it before hit-testing.

#### Risks / open questions

1. **Menu-bar inset coupling.** Option 1 introduces an implicit "the menu bar's height is subtracted from event Y" rule. If a future feature adds another GTK sibling (e.g. a status bar or toolbar that GTK still owns), the inset becomes a *vector* of (top, bottom, left, right) rather than a scalar. Option 2 sidesteps this entirely. Choosing option 1 means accepting that this stays single-inset until proven otherwise.
2. **Focus and `key-press-event`.** GtkWindow gets key events when it has focus *and* no child widget consumed them first. If the GtkMenuBar swallows accelerator keys (it does, when its `mnemonic-activate` matches), the toplevel handler won't see them. This is consistent with native menu behavior, but worth confirming against §2.3a's FocusManager: the virtual focus manager only sees keys the GTK toplevel saw, not the ones the menu bar already consumed.
3. **Compositing manager interaction.** Drawing a custom surface into an X11 Window owned by GTK works fine under composited X11 (GNOME/KDE/XFCE). Under a non-compositing WM (some `i3`/`dwm` setups, especially with `xrender` disabled) the X11 Window may be unmapped/clipped in ways GdkWindow normally hides. Worth a manual smoke test on at least one non-composited WM before claiming GTK parity.
4. **Verification.** Linux is the agent's native build target, so this section can be both compile- and run-verified before claiming "Done." Mark as compile/run unverified until that pass lands (per the "mark unverified backends" rule).

---

## 3. Implementation Priority

| Priority | Feature | Rationale |
|----------|---------|-----------|
| ~~**P0**~~ **Done** | 2.1 NativeEvent | Implemented |
| ~~**P0**~~ **Done** | 2.5 NativeTheme | Implemented |
| **P1** | 2.2 NativeWindow (state, scaleFactor, opacity, cursor sink, key-window) | Basic window management + the OS sinks the virtual tree commits to |
| **P1** | 2.3a View focus + cursor + Widget tooltip + WidgetTreeHost FocusManager | Per-view keyboard routing, hover cursor, tooltip popups |
| **P1** | 2.4 NativeApp (args, delegate, timers) | App lifecycle and command-line |
| **P1** | 2.6 NativeClipboard | Copy/paste is fundamental UX |
| ~~P1~~ **Done** | 2.8 NativeDialog (alert dialog, file filters) | Implemented |
| **P2** | 2.7 NativeDragDrop | Important for content apps, less critical initially |
| **P1** | 2.9 NativeScreen | Multi-monitor support — also the proper home for AppWindow screen targeting; replaces the interim GTK primary-monitor anchoring |
| ~~P1~~ **Done** | 2.11 NativeNote / NotificationCenter | Implemented |
| ~~P1~~ **Done** | 2.12 NativeMenu / Menu | Implemented |
| **P1** | 2.13 GTK root NativeItem collapse (X11-direct) | Pairs with §2.2/§2.9 — locks GTK to a single, canonical surface ownership model before more per-platform features pile on it |
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
