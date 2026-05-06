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
| 2.2 NativeWindow | Full window control (minimize/maximize/fullscreen, scaleFactor, opacity, cursor sink) | Not started |
| ~~2.3 NativeItem~~ | **Obsolete under virtual view model — no new NativeItem APIs.** See §2.3 below. | Removed |
| 2.3a View / Widget / TreeHost focus + cursor + tooltip | Virtual focus manager, declarative cursor shape, virtual tooltip popups | Not started |
| 2.4 NativeApp | Delegate, command-line args, timers | Not started |
| 2.6 NativeClipboard | New subsystem | Not started |
| 2.7 NativeDragDrop | New subsystem | Not started |
| 2.8 NativeDialog | Alert dialog, file filters | Not started |
| 2.9 NativeScreen | New subsystem | Not started |
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

### 2.1 NativeEvent — Complete Event Model ✅ Done

Implemented. See `NativeEvent.h`: `ModifierFlags`, `MouseEventParams`, `CursorMoveParams`, `KeyCode`, `KeyDownParams`/`KeyUpParams`, all new event types (`CursorMove`, `DragBegin`/`Move`/`End`, `FocusGained`/`FocusLost`, `GesturePinch`/`Pan`/`Rotate`, `AppActivate`/`AppDeactivate`, full window resize sequence), multi-receiver `NativeEventEmitter`.

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

### 2.8 NativeDialog — Confirmation Dialog and Dialog Result

**Goal:** Add a standard confirmation/alert dialog and give `NativeNoteDialog` a result. Add file-type filters to `NativeFSDialog`.

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

**Goal:** Enumerate connected displays and query their properties.

New header `NativeScreen.h`:

```cpp
struct NativeScreenDesc {
    unsigned id = 0;
    Composition::Rect frame;
    Composition::Rect visibleFrame;
    float scaleFactor = 1.f;
    bool isPrimary = false;
};

OMEGAWTK_EXPORT OmegaCommon::Vector<NativeScreenDesc> enumerateScreens();
OMEGAWTK_EXPORT NativeScreenDesc primaryScreen();
```

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

## 3. GTK Platform Parity

The GTK backend currently implements `NativeApp`, `NativeWindow`, `NativeItem`, `NativeMenu`, `NativeNote`. The following need GTK implementations to reach parity:

| Subsystem | macOS | Win32 | GTK | Priority |
|-----------|-------|-------|-----|----------|
| NativeDialog (FS + Alert) | Exists | Exists | Missing | P1 |
| NativeTheme | Exists | Exists | Missing | P1 |
| NativeClipboard | New | New | New | P1 |
| NativeDragDrop | New | New | New | P2 |
| NativeTimer | New | New | New | P1 |
| NativeScreen | New | New | New | P2 |
| NativeAccessibility | New | New | New | P3 |

---

## 4. Implementation Priority

| Priority | Feature | Rationale |
|----------|---------|-----------|
| ~~**P0**~~ **Done** | 2.1 NativeEvent | Implemented |
| ~~**P0**~~ **Done** | 2.5 NativeTheme | Implemented |
| **P1** | 2.2 NativeWindow (state, scaleFactor, opacity, cursor sink, key-window) | Basic window management + the OS sinks the virtual tree commits to |
| **P1** | 2.3a View focus + cursor + Widget tooltip + WidgetTreeHost FocusManager | Per-view keyboard routing, hover cursor, tooltip popups |
| **P1** | 2.4 NativeApp (args, delegate, timers) | App lifecycle and command-line |
| **P1** | 2.6 NativeClipboard | Copy/paste is fundamental UX |
| **P1** | 2.8 NativeDialog (alert dialog, file filters) | Common user-facing pattern |
| **P2** | 2.7 NativeDragDrop | Important for content apps, less critical initially |
| **P2** | 2.9 NativeScreen | Multi-monitor support |
| ~~P1~~ **Done** | 2.11 NativeNote / NotificationCenter | Implemented |
| ~~P1~~ **Done** | 2.12 NativeMenu / Menu | Implemented |
| **P3** | 2.10 NativeAccessibility | Stub now, implement per-platform over time |

---

## 5. File Change Summary

### New headers (`wtk/include/omegaWTK/Native/`)
- `NativeTimer.h`
- `NativeClipboard.h`
- `NativeDragDrop.h`
- `NativeScreen.h`
- `NativeAccessibility.h`

### Modified headers
- `NativeWindow.h` — window state APIs, `scaleFactor`, `setOpacity`, `setCursorShape` sink, `isKeyWindow`/`becomeKeyWindow`, `CursorShape` enum; remove `#ifdef TARGET_MACOS` guards around `rect` and `eventEmitter`
- `NativeItem.h` — **no changes** (virtual view model removes the per-View item)
- `NativeApp.h` — NativeAppDelegate, commandLineArgs(), launchArgs()
- `NativeDialog.h` — NativeAlertDialog, FileFilter in FS descriptor
- `View.h` (UI) — `setFocusable`/`isFocusable`/`isFocused`/`focus`/`blur`, `setCursorShape`/`cursorShape`
- `Widget.h` (UI) — `setTooltip`/`clearTooltip`
- `WidgetTreeHost` (UI, internal) — owns `FocusManager` and tooltip-hover timer; routes keyboard events to focused View; commits hovered View's cursor shape to the root NativeWindow

### Already modified (done)
- `NativeEvent.h` — complete ✅
- `NativeTheme.h` — complete ✅
- `NativeNote.h` — complete ✅
- `NativeMenu.h` — complete ✅
- `Notification.h` / `Notification.cpp` (UI) — complete ✅
- `Menu.h` / `Menu.cpp` (UI) — complete ✅

### New source files per platform
Each of the three backends (`macos/`, `win/`, `gtk/`) needs:
- Timer implementation
- Clipboard implementation
- DragDrop implementation
- Screen enumeration

GTK also needs: Dialog, Theme.

### Cross-platform dispatchers (`wtk/src/Native/`)
- `NativeTimer.cpp`
- `NativeClipboard.cpp`
- `NativeDragDrop.cpp`
- `NativeScreen.cpp`
- `NativeAccessibility.cpp`

---

## 6. References

- Current headers: `wtk/include/omegaWTK/Native/`
- macOS backend: `wtk/src/Native/macos/`
- Win32 backend: `wtk/src/Native/win/`
- GTK backend: `wtk/src/Native/gtk/`
- UI layer consumers: `wtk/include/omegaWTK/UI/` (View.h, Widget.h, AppWindow.h, Menu.h, Notification.h)
