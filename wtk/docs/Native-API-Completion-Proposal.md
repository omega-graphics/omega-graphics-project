# Native API Completion Proposal

This document proposes the API additions and changes needed to bring the WTK Native abstraction layer to a production-complete state. It is organized by subsystem, starting with the highest-impact gaps.

---

## Completion status

| Section | Description | Status |
|---------|-------------|--------|
| **2.1 NativeEvent** | Complete event model (ModifierFlags, mouse/key params, KeyCode, new event types, multi-receiver emitter) | **Done** |
| **2.5 NativeTheme** | ThemeAppearance, populated ThemeDesc (colors, typography), `queryCurrentTheme()` on macOS & Win32 | **Done** |
| 2.2 NativeWindow | Full window control (minimize/maximize/fullscreen, scaleFactor, etc.) | Not started |
| 2.3 NativeItem | Focus, cursor shape, tooltip, opacity, hit-test | Not started |
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
| `NativeEvent.h` | Event types and emitter/processor | macOS, Win32; GTK incomplete |
| `NativeMenu.h` | Menu and menu item abstraction | macOS, Win32, GTK |
| `NativeDialog.h` | File dialog, note dialog | macOS, Win32; GTK missing |
| `NativeNote.h` | System notification (NoteCenter) | macOS, Win32; GTK missing |
| `NativeTheme.h` | Theme query and observer | macOS, Win32; GTK missing |

### Key Gaps

1. **NativeEvent** — ~~No mouse coordinates, no modifier keys, no mouse-move/drag events, no focus events, no gesture events. The `KeyCode` enum is commented out. Mouse/key param structs are empty.~~ **Implemented (2.1):** ModifierFlags, MouseEventParams, CursorMoveParams, KeyCode, KeyDownParams/KeyUpParams, new event types, multi-receiver emitter; Cocoa and Win32 backends updated.
2. **NativeWindow** — No minimize/maximize/fullscreen API; `rect` is macOS-only; no DPI/scale query; constructor/fields are platform-gated.
3. **NativeItem** — No focus management, no cursor (mouse pointer) control, no tooltip, no hit-testing.
4. **NativeApp** — No command-line argument access, no open-URL/open-file handler, no idle/timer callback.
5. **NativeTheme** — ~~`ThemeDesc` is empty; cannot distinguish light/dark, accent color, font defaults.~~ **Implemented (2.5):** ThemeAppearance, ThemeDesc colors + typography, queryCurrentTheme() on macOS & Win32.
6. **NativeDialog** — No confirmation dialog (OK/Cancel/Yes/No); `NativeNoteDialog` returns no result.
7. **Missing subsystems** — Clipboard, drag-and-drop, cursor (mouse pointer) shapes, timers, accessibility stubs, screen/display info.
8. **GTK platform** — Missing Dialog, Note, Theme, and full Event implementations.

---

## 2. Proposed Extensions

### 2.1 NativeEvent — Complete Event Model

**Goal:** Deliver rich event data so the UI layer can handle mouse position, modifier keys, focus, and gestures without platform-specific code.

#### 2.1a New event types

Add to `NativeEvent::EventType`:

```cpp
// Mouse movement
CursorMove,
// Drag (mouse held + moved)
DragBegin,
DragMove,
DragEnd,
// Focus
FocusGained,
FocusLost,
// Touch / trackpad gestures (future mobile + macOS trackpad)
GesturePinch,
GesturePan,
GestureRotate,
// Application-level
AppActivate,
AppDeactivate
```

#### 2.1b Enrich event param structs

```cpp
struct ModifierFlags {
    bool shift = false;
    bool control = false;
    bool alt = false;     // Option on macOS
    bool meta = false;    // Cmd on macOS, Win key on Win32
    bool capsLock = false;
};

struct MouseEventParams {
    Core::Position position;      // Local to the NativeItem
    Core::Position screenPosition;
    ModifierFlags modifiers;
    unsigned clickCount = 1;
};

struct CursorMoveParams {
    Core::Position position;
    Core::Position screenPosition;
    ModifierFlags modifiers;
};

struct CursorEnterParams {
    Core::Position position;
};

struct CursorExitParams {
    Core::Position position;
};

struct LMouseDownParams : MouseEventParams {};
struct LMouseUpParams   : MouseEventParams {};
struct RMouseDownParams : MouseEventParams {};
struct RMouseUpParams   : MouseEventParams {};
```

#### 2.1c Keyboard: reinstate KeyCode enum

Uncomment and finalize the `KeyCode` enum. Add it to `KeyDownParams` / `KeyUpParams` alongside the existing `Unicode32Char`:

```cpp
enum class KeyCode : int {
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5,
    Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Tab, CapsLock, Space, Enter, Backspace, Delete,
    LeftShift, RightShift, LeftControl, RightControl,
    LeftAlt, RightAlt, LeftMeta, RightMeta,
    ArrowUp, ArrowDown, ArrowLeft, ArrowRight,
    Home, End, PageUp, PageDown,
    Unknown
};

struct KeyDownParams {
    KeyCode code = KeyCode::Unknown;
    Unicode32Char key = 0;
    ModifierFlags modifiers;
    bool isRepeat = false;
};

struct KeyUpParams {
    KeyCode code = KeyCode::Unknown;
    Unicode32Char key = 0;
    ModifierFlags modifiers;
};
```

#### 2.1d Multi-receiver event emitter

`NativeEventEmitter` currently holds a single `NativeEventProcessor *`. Change to a vector of weak observers so multiple delegates (e.g. a View + an accessibility bridge) can listen:

```cpp
class NativeEventEmitter {
    OmegaCommon::Vector<NativeEventProcessor *> receivers;
public:
    void addReceiver(NativeEventProcessor *receiver);
    void removeReceiver(NativeEventProcessor *receiver);
    void emit(NativeEventPtr event);
};
```

Preserve `setReciever` as a compatibility alias that clears the list and adds one.

---

### 2.2 NativeWindow — Full Window Control

**Goal:** Platform-uniform window management (minimize, maximize, fullscreen, DPI).

```cpp
INTERFACE NativeWindow {
    // ... existing ...

    // -- New methods --
    virtual void minimize() = 0;
    virtual void maximize() = 0;
    virtual void restore() = 0;
    virtual void toggleFullscreen() = 0;
    virtual bool isMinimized() const = 0;
    virtual bool isMaximized() const = 0;
    virtual bool isFullscreen() const = 0;
    virtual bool isVisible() const = 0;

    virtual Core::Rect getRect() const = 0;
    virtual void setRect(const Core::Rect & rect) = 0;

    virtual float scaleFactor() const = 0;  // DPI / backing scale

    virtual void setMinSize(float w, float h) = 0;
    virtual void setMaxSize(float w, float h) = 0;

    virtual void setResizable(bool resizable) = 0;

    virtual void orderFront() = 0;  // Bring to front
    virtual void orderBack() = 0;

    // -- Platform-uniform event emitter access --
    NativeEventEmitter * eventEmitter() const;
};
```

Remove the `#ifdef TARGET_MACOS` guards around `rect` and `eventEmitter` so all platforms carry these uniformly.

---

### 2.3 NativeItem — Focus, Cursor, Tooltip, Hit-Test

**Goal:** Items need focus management for keyboard navigation, cursor shape for hover states, and tooltip support.

```cpp
// New enum
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

INTERFACE NativeItem {
    // ... existing ...

    // Focus
    virtual void focus() = 0;
    virtual void blur() = 0;
    virtual bool isFocused() const = 0;
    virtual void setFocusable(bool focusable) = 0;

    // Cursor shape
    virtual void setCursorShape(CursorShape shape) = 0;

    // Tooltip
    virtual void setTooltip(const OmegaCommon::String & text) = 0;
    virtual void clearTooltip() = 0;

    // Opacity
    virtual void setOpacity(float alpha) = 0;
    virtual float getOpacity() const = 0;

    // Hit test
    virtual bool hitTest(Core::Position point) const = 0;
};
```

---

### 2.4 NativeApp — Lifecycle, Arguments, Timers

**Goal:** Expose command-line arguments, open-file/URL delegate, idle timers, and run-loop integration.

```cpp
struct NativeAppLaunchArgs {
    int argc = 0;
    char **argv = nullptr;
};

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

A new header `NativeTimer.h`:

```cpp
class NativeTimer;
typedef SharedHandle<NativeTimer> NativeTimerPtr;

INTERFACE NativeTimer {
public:
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual ~NativeTimer() = default;
};

NativeTimerPtr make_native_timer(float intervalSec,
                                 bool repeats,
                                 std::function<void()> callback);
```

Backed by `NSTimer`, `SetTimer`/`KillTimer`, `g_timeout_add`.

---

### 2.5 NativeTheme — Rich Theme Descriptor

**Goal:** `ThemeDesc` should carry enough data for widgets to adapt to system appearance.

```cpp
enum class ThemeAppearance : int {
    Light,
    Dark
};

struct ThemeDesc {
    ThemeAppearance appearance = ThemeAppearance::Light;

    struct {
        Composition::Color accent;
        Composition::Color background;
        Composition::Color foreground;
        Composition::Color controlBackground;
        Composition::Color controlForeground;
        Composition::Color separator;
        Composition::Color selection;
    } colors;

    struct {
        OmegaCommon::String defaultFamily;
        float defaultSize = 13.f;
        float headingSize = 17.f;
        float captionSize = 11.f;
    } typography;
};
```

---

### 2.6 NativeClipboard (New)

**Goal:** Read and write system clipboard (text, and optionally images).

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
    None,
    Copy,
    Move,
    Link
};

INTERFACE NativeDragSource {
public:
    virtual void beginDrag(const DragData & data, NativeItemPtr sourceItem) = 0;
    virtual ~NativeDragSource() = default;
};

INTERFACE NativeDropTarget {
public:
    virtual DropEffect onDragEnter(const DragData & data, Core::Position position) { return DropEffect::None; }
    virtual DropEffect onDragOver(const DragData & data, Core::Position position) { return DropEffect::None; }
    virtual void onDragLeave() {}
    virtual DropEffect onDrop(const DragData & data, Core::Position position) { return DropEffect::None; }
    virtual ~NativeDropTarget() = default;
};

void register_drop_target(NativeItemPtr item, NativeDropTarget * target);
void unregister_drop_target(NativeItemPtr item);
SharedHandle<NativeDragSource> make_native_drag_source();
```

---

### 2.8 NativeDialog — Confirmation Dialog and Dialog Result

**Goal:** Add a standard confirmation/alert dialog and give `NativeNoteDialog` a result.

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
        OmegaCommon::Vector<OmegaCommon::String> buttonLabels;  // e.g. {"OK"} or {"Yes","No","Cancel"}
    };

    static SharedHandle<NativeAlertDialog> Create(const Descriptor & desc, NWH nativeWindow);
    virtual OmegaCommon::Async<Result> getResult() = 0;
};
```

Update `NativeNoteDialog` to also return a completion signal so the caller knows the dialog was dismissed.

Update `NativeFSDialog::Descriptor` to support file-type filters:

```cpp
struct FileFilter {
    OmegaCommon::String label;                // e.g. "Images"
    OmegaCommon::Vector<OmegaCommon::String> extensions;  // e.g. {"png","jpg","gif"}
};

struct Descriptor {
    Type type;
    OmegaCommon::FS::Path openLocation;
    OmegaCommon::Vector<FileFilter> filters;
    bool allowMultiple = false;  // For Read type
};
```

---

### 2.9 NativeScreen (New)

**Goal:** Enumerate connected displays and query their properties.

New header `NativeScreen.h`:

```cpp
struct NativeScreenDesc {
    unsigned id = 0;
    Core::Rect frame;           // Full screen rect in global coords
    Core::Rect visibleFrame;    // Minus dock/taskbar
    float scaleFactor = 1.f;
    bool isPrimary = false;
};

OMEGAWTK_EXPORT OmegaCommon::Vector<NativeScreenDesc> enumerateScreens();
OMEGAWTK_EXPORT NativeScreenDesc primaryScreen();
```

---

### 2.10 NativeAccessibility (New, Stub)

**Goal:** Provide a skeleton that platform backends can implement over time (NSAccessibility, MSAA/UIA, ATK).

New header `NativeAccessibility.h`:

```cpp
enum class AccessibilityRole : int {
    None,
    Button,
    Label,
    TextField,
    Image,
    ScrollArea,
    Slider,
    Checkbox,
    Group,
    Window,
    Application
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
    virtual void setAccessibility(NativeItemPtr item, const AccessibilityDesc & desc) = 0;
    virtual void clearAccessibility(NativeItemPtr item) = 0;
    virtual void announceForAccessibility(const OmegaCommon::String & message) = 0;
    virtual ~NativeAccessibilityBridge() = default;
};

SharedHandle<NativeAccessibilityBridge> get_native_accessibility_bridge();
```

Initial implementations can be no-ops; the point is to lock the API so the UI layer (Widget, View, UIView) can start annotating their native items.

---

## 3. GTK Platform Parity

The GTK backend currently only implements `NativeApp`, `NativeWindow`, `NativeItem`, and `NativeMenu`. The following need GTK implementations to reach parity:

| Subsystem | macOS | Win32 | GTK | Priority |
|-----------|-------|-------|-----|----------|
| NativeEvent (rich params) | Needs update | Needs update | Missing entirely | P0 |
| NativeDialog (FS + Alert) | Exists | Exists | Missing | P1 |
| NativeTheme | Exists | Exists | Missing | P1 |
| NativeNote | Exists | Exists | Missing | P2 |
| NativeClipboard | New | New | New | P1 |
| NativeDragDrop | New | New | New | P2 |
| NativeTimer | New | New | New | P1 |
| NativeScreen | New | New | New | P2 |
| NativeAccessibility | New | New | New | P3 |

---

## 4. Implementation Priority

| Priority | Feature | Rationale |
|----------|---------|-----------|
| **P0** | 2.1 NativeEvent (rich params, KeyCode, modifiers, mouse coords) | Unblocks all input handling; every widget needs this |
| **P0** | 2.5 NativeTheme (populated ThemeDesc) | Light/dark is essential for any real UI |
| **P1** | 2.2 NativeWindow (minimize/maximize/fullscreen, scaleFactor, cross-platform rect) | Basic window management |
| **P1** | 2.3 NativeItem (focus, cursor shape, tooltip) | Text input and interactive UI |
| **P1** | 2.4 NativeApp (args, delegate, timers) | App lifecycle and command-line |
| **P1** | 2.6 NativeClipboard | Copy/paste is fundamental UX |
| **P1** | 2.8 NativeDialog (alert dialog, file filters) | Common user-facing pattern |
| **P2** | 2.7 NativeDragDrop | Important for content apps, less critical initially |
| **P2** | 2.9 NativeScreen | Multi-monitor support |
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
- `NativeEvent.h` — ModifierFlags, MouseEventParams, CursorMoveParams, KeyCode enum, enriched param structs, multi-receiver emitter
- `NativeWindow.h` — minimize/maximize/fullscreen/scaleFactor/setRect; remove macOS ifdefs around rect and emitter
- `NativeItem.h` — CursorShape, focus/blur/tooltip/opacity/hitTest
- `NativeApp.h` — NativeAppDelegate, commandLineArgs(), launchArgs()
- `NativeTheme.h` — ThemeAppearance, populated ThemeDesc (colors, typography)
- `NativeDialog.h` — NativeAlertDialog, FileFilter in FS descriptor

### New source files per platform
Each of the three backends (macos/, win/, gtk/) needs:
- Timer implementation
- Clipboard implementation
- DragDrop implementation
- Screen enumeration

GTK also needs initial implementations for Dialog, Note, Theme, and rich Event params.

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
