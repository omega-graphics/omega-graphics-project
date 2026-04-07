# Native View Architecture Plan

OmegaWTK creates one native platform view (NSView, HWND, GtkWidget) per
Widget. Every production UI toolkit that has solved the performance and
correctness problems OmegaWTK is hitting has moved away from this model.
This plan documents why the current approach fails, what production
systems do instead, and how OmegaWTK should transition.

---

## The current architecture

Each OmegaWTK `Widget` owns a `View`. Each `View` creates exactly one
native platform view:

- **macOS:** `OmegaWTKCocoaView` (NSView subclass) via `CocoaItem`
- **Windows:** child HWND via `HWNDItem`
- **Linux:** GtkWidget placed in a `GtkFixed` container via `GTKItem`

When a container widget adds a child, `Container::wireChild()` calls
`view->addSubView()`, which calls `addChildNativeItem()` on the platform
layer. This creates a real native view tree that mirrors the widget tree
exactly: 10 widgets = 10 NSViews = 10 HWNDs = 10 GtkWidgets.

### Platform-specific child attachment

**macOS** (`CocoaItem.mm:307–319`):
```objc
cocoaview->_ptr.translatesAutoresizingMaskIntoConstraints = NO;
cocoaview->_ptr.autoresizingMask = NSViewNotSizable;
[_ptr addSubview:cocoaview->_ptr];
```
Children are added as real NSView subviews with auto layout disabled and
no autoresizing mask. Positioning relies entirely on `setFrame:` calls
from the OmegaWTK layout system.

**Windows** (`HWNDItem.cpp:228–241`):
```cpp
SetParent(child, hwnd);
const auto childY = (wndrect.h - hwndItem->wndrect.pos.y - hwndItem->wndrect.h) * scaleFactor;
SetWindowPos(child, HWND_TOP, childX, childY, childW, childH, ...);
```
Children become real child HWNDs via `SetParent`. Y-coordinates are
manually inverted (Win32 uses top-left origin, OmegaWTK uses
bottom-left).

**GTK** (`GTKItem.cpp:511–533`):
```cpp
gtk_fixed_put(GTK_FIXED(container), item->widget,
    toGtkCoordinate(item->rect.pos.x), toGtkCoordinate(item->rect.pos.y));
```
Children are placed at absolute pixel positions in a `GtkFixed` container.

---

## Observed problems

### 1. Child NSViews reset to (0,0) during resize

The `OmegaWTKCocoaView` does not override `isFlipped`. NSView's default
coordinate system has its origin at the **bottom-left** with y increasing
upward. When a parent view resizes (grows taller), child views pinned to
low y-coordinates appear to "slide down" — their frame origin hasn't
changed, but their visual position relative to the top of the window has.

Combined with `autoresizingMask = NSViewNotSizable` and
`translatesAutoresizingMaskIntoConstraints = NO`, children have no
mechanism to track their intended position during resize. The layout
system must re-set every child's frame after every resize event, and if
the resize path and the frame-setting path race, the child appears at
(0,0).

Additionally, the view uses **layer-hosting mode** (sets `self.layer`
before `self.wantsLayer`), which means AppKit does not manage layer
geometry — OmegaWTK must do it manually. But the resize path
(`CocoaItem::resize()`) explicitly sets the root layer's frame and
position while noting "Child layer geometry is owned by the compositor
backend. Do not re-anchor/reframe sublayers here during live resize."
This creates a window where the parent has resized but child layers
haven't yet been updated.

### 2. HWND positioning and flicker

On Windows, child HWNDs are not automatically repositioned when the
parent receives `WM_SIZE`. OmegaWTK must call `SetWindowPos` for every
child on every resize event. Without `BeginDeferWindowPos` /
`EndDeferWindowPos` batching, each `SetWindowPos` can cause a
synchronous repaint, producing visible flicker as children update
one-by-one.

The Y-coordinate inversion (`wndrect.h - pos.y - h`) depends on the
parent's current height — if the parent's size and the child's position
update out of order, the child jumps to the wrong location for one
frame.

### 3. GtkFixed absolute positioning

`GtkFixed` places children at absolute pixel positions. GTK's own
documentation warns that this approach produces display bugs with themes,
font changes, and translations. More importantly, `GtkFixed` does not
re-layout children on parent resize — OmegaWTK must call
`gtk_fixed_move()` for every child after every resize event.

### 4. Performance ceiling

Apple's documentation states that ~100 NSViews is the practical limit
before noticeable performance degradation. Microsoft's documentation
explicitly warns against using child windows indiscriminately:
"For best performance, an application should [...] divide its main
window [...] in the window procedure of the main window rather than by
using child windows."

A non-trivial OmegaWTK application with nested containers, labels,
buttons, and separators will easily reach dozens to hundreds of widgets.
Each one carries the overhead of a native view: a system resource (HWND,
NSView, GtkWidget), a compositor layer, event dispatch overhead, and
platform coordinate management.

---

## How production systems solve this

### Chromium: one native view per window, virtual widget tree

Chromium creates **one primary native view per top-level window**. All
browser chrome UI (tabs, toolbar, buttons, omnibox, menus) is a virtual
`views::View` tree painted onto that single native surface using Skia.

| Platform | Native views per browser window |
|----------|-------------------------------|
| macOS | 1 `BridgedContentView` (NSView) + 1 `RenderWidgetHostViewCocoa` per visible tab |
| Windows | 1 HWND + 1 legacy accessibility HWND |
| Linux | 1 X11/Wayland surface |

`views::View` objects have no native platform backing. They are purely
virtual rectangles in a tree. The framework performs:

- **Custom painting:** Each View paints via `OnPaint()` into a shared
  canvas (backed by a compositor layer). The Widget does a pre-order
  traversal of the View tree, clipping and translating for each View.
- **Custom hit testing:** The `RootView` receives all native events and
  uses `ViewTargeter` to walk the tree and find the deepest View whose
  bounds contain the cursor.
- **Custom layout:** Views are positioned and sized by a layout system
  that operates entirely in virtual coordinates — no platform calls.

The only native views used are for content that requires platform
integration: web content (`RenderWidgetHostView`), embedded native
controls (`NativeViewHost`), and IME text input fields.

**Why Chromium moved away from native views:** The original design used
native Windows controls (HWNDs for buttons, checkboxes, etc.). They
abandoned it because native windows don't support transparency, and
event handling via window subclassing was "tedious" and
"unsatisfactory" (Chromium design docs).

### macOS / Cocoa best practices

Apple recommends that for UIs with hundreds of visual elements, a single
`NSView` should manage lightweight custom objects and draw them all in
its `drawRect:` implementation, rather than creating one `NSView` per
element. Layer-backed views with
`NSViewLayerContentsRedrawOnSetNeedsDisplay` redraw policy reduce
resize overhead by stretching cached content during resize.

### Windows / WPF / WinUI

**WPF** (2006) and **WinUI 3** (2021) both use a single HWND per window.
All child controls are "windowless" — rendered via DirectX (WPF) or the
Visual Layer (WinUI) into the single surface. WPF uses a retained-mode
rendering model with composition nodes in unmanaged memory (`milcore`).

The "airspace problem" — inability to visually overlap native HWNDs with
WPF content — is what drove Microsoft to eliminate child HWNDs entirely
in WinUI 3.

### GTK4

GTK4 uses **one `GdkSurface` per toplevel**. Child widgets have no
native surface. GTK4 introduced GSK (GTK Scene Graph Kit): each widget
produces render nodes during a snapshot phase, the framework caches
unchanged subtrees, and a backend renderer (GL, Vulkan, or Cairo) draws
everything into the single surface.

This was a deliberate migration from GTK2 (one X11 window per
interactive widget) → GTK3 (fewer native windows) → GTK4 (single
surface + scene graph).

### Qt: alien widgets (since 4.4)

Qt 4.4 (2008) introduced "alien widgets" — only top-level `QWidget`s
get a native window handle. All child widgets are drawn into the
parent's native window via an offscreen buffer. This "significantly
speeds up widget painting, resizing, and removes flicker." Qt Quick/QML
goes further: a GPU-accelerated scene graph renders all items into a
single `QQuickWindow`.

### Flutter

Flutter uses a **single native surface per window** and draws everything
itself using Skia/Impeller. No native views exist for Flutter widgets.
When a native platform view is needed (maps, WebView), Flutter uses
"platform views" with hybrid composition at a performance cost.

### Industry trend summary

| Year | Toolkit | Model |
|------|---------|-------|
| Pre-2006 | Win32, GTK2, Qt <4.4 | One native view per widget |
| 2006 | WPF | Single HWND, DirectX rendering |
| 2008 | Qt 4.4 | Alien widgets (single native window) |
| 2009 | Chromium Views | Single native view, Skia rendering |
| 2017 | Flutter | Single surface, Skia rendering |
| 2020 | GTK4 | Single surface, scene graph |
| 2021 | WinUI 3 | Single HWND, composition rendering |

Every major toolkit has converged on the same architecture: **one native
view per top-level window, virtual widget tree, custom rendering into a
single surface.**

---

## Proposed architecture

### Single native view per window

Each `AppWindow` creates exactly one native platform view:

- **macOS:** One `OmegaWTKCocoaView` as the window's `contentView`
- **Windows:** One HWND per top-level window
- **Linux:** One GtkWidget per top-level window

All child widgets are virtual — they have bounds, can paint, and can
receive events, but they have no native platform view. The widget tree
is a purely in-memory hierarchy managed by OmegaWTK.

### What changes

| Current | Proposed |
|---------|----------|
| `View` creates a `NativeItem` | `View` holds virtual bounds only |
| `Container::wireChild()` calls `addSubView()` → `addChildNativeItem()` | `Container::wireChild()` adds child to virtual tree |
| Platform layer manages child positions | OmegaWTK layout manages all positions |
| Each widget has a native compositor layer | All widgets render into the window's single surface |
| Platform dispatches events to individual native views | Root view receives all events, OmegaWTK hit-tests the virtual tree |
| N widgets = N NSViews/HWNDs/GtkWidgets | N widgets = 1 NSView/HWND/GtkWidget |

### Phase 1 — Root-only native view

**Goal:** Only the window-level View creates a native platform view.
Child Views become virtual.

Introduce a distinction between **root views** and **child views**:

```cpp
class View {
    // ...
    bool isRootView() const;  // true for the top-level View owned by AppWindow

    // For root views: owns the native item
    std::shared_ptr<Native::NativeItem> nativeItem_;

    // For child views: no native item, just virtual bounds
    // Rendering goes through the root view's surface
};
```

`View::addSubView()` no longer calls `addChildNativeItem()`. Instead,
it adds the child to a virtual child list. The child's position and
size are tracked in OmegaWTK coordinates only — no platform calls.

**Platform layer changes:**

| Platform | Change |
|----------|--------|
| macOS | `CocoaItem::addChildNativeItem()` becomes a no-op for virtual views. Root `OmegaWTKCocoaView` is the only NSView. Override `isFlipped` to return YES for top-left origin. |
| Windows | `HWNDItem::addChildNativeItem()` becomes a no-op. Root HWND is the only window. No more `SetParent`/`SetWindowPos` for children. |
| GTK | `GTKItem::addChildNativeItem()` becomes a no-op. Root widget is the only GtkWidget. No more `gtk_fixed_put` for children. |

### Phase 2 — Virtual hit testing

**Goal:** Route native events from the root view to the correct virtual
widget.

The root native view receives all mouse, keyboard, and touch events.
OmegaWTK walks the virtual widget tree to find the target:

```cpp
Widget * View::hitTest(const Core::Position &point) const {
    // Walk children in reverse z-order (front to back)
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->containsPoint(point)) {
            // Recurse into child, translating to child-local coordinates
            auto *hit = (*it)->hitTest(
                {point.x - (*it)->rect().pos.x,
                 point.y - (*it)->rect().pos.y});
            if (hit != nullptr) return hit;
        }
    }
    // No child hit — this view is the target
    return owningWidget();
}
```

This replaces the current model where each native view independently
receives its own events from the platform.

**Prior art:** Chromium's `ViewTargeter` performs exactly this traversal.
The `RootView` receives the native event, calls
`GetEventHandlerForPoint()`, and walks the View tree from leaf to root.

### Phase 3 — Single-surface rendering

**Goal:** All widgets render into the root view's single backing surface.

Currently each View has its own compositor surface (render target,
layer texture, Metal/GL backing). In the new model, only the root
view has a backing surface. Child widgets paint by recording drawing
commands that are composited into the root surface.

This connects directly to the Render Execution Efficiency Plan's
Tier 1 (surface mailbox architecture). Instead of each widget depositing
into its own `CompositorSurface`, all widgets in a window deposit into
the window's single surface. The compositor's frame loop renders all
widgets' commands into one render pass and presents once.

```
Current:
    Widget A → View A → NativeView A → Surface A → Present A
    Widget B → View B → NativeView B → Surface B → Present B
    Widget C → View C → NativeView C → Surface C → Present C
    (3 presents, 3 native views)

Proposed:
    Widget A ──┐
    Widget B ──┼─→ Root View → Root NativeView → Single Surface → Present
    Widget C ──┘
    (1 present, 1 native view)
```

### Phase 4 — Coordinate system unification

**Goal:** Use a consistent top-left-origin coordinate system across all
platforms.

The current codebase uses bottom-left origin (matching Cocoa's default)
and manually inverts Y on Windows (`HWNDItem.cpp:237`). This is a
constant source of positioning bugs.

With virtual views, OmegaWTK can use whatever coordinate system it
wants — the platform never sees child coordinates. Standardize on
**top-left origin** (matching the industry: iOS, Windows, GTK, web,
Flutter, Qt, Android):

```cpp
// All OmegaWTK coordinates: origin at top-left, y increases downward
struct Position {
    float x;  // from left edge
    float y;  // from top edge
};
```

The only coordinate conversion happens at the root view level, where
OmegaWTK coordinates are mapped to the platform's coordinate system
for the single native view.

### Phase 5 — Native view embedding (escape hatch)

**Goal:** Allow embedding real native views when platform integration
requires it.

Some widgets genuinely need a native view: text input fields (IME
integration), web content views, video players, platform-specific
controls. Provide a `NativeViewHost` widget (analogous to Chromium's
`NativeViewHost` or WPF's `HwndHost`) that:

1. Creates a real native view
2. Embeds it as a child of the root native view
3. Synchronizes its bounds with the virtual widget tree
4. Handles the "airspace" problem (native views render on top of
   virtual content)

```cpp
class NativeViewHost : public Widget {
    std::shared_ptr<Native::NativeItem> embeddedItem_;
public:
    /// Attach a real native view. It will be positioned and sized
    /// to match this widget's bounds in the virtual tree.
    void attach(std::shared_ptr<Native::NativeItem> nativeItem);
    void detach();
};
```

This is the exception, not the rule. Standard widgets (labels, buttons,
containers, separators, shapes) should never need a native view.

---

## What this fixes

### The NSView (0,0) reset problem

Eliminated entirely. There are no child NSViews to reset. All widget
positioning is virtual, managed by OmegaWTK's layout system in a
consistent coordinate space. The single root NSView's position is fixed
to the window's content area.

### Resize performance

No native view repositioning on resize. The layout system recalculates
virtual bounds, widgets repaint into the single surface, and one present
displays the result. No `SetWindowPos` per child, no `gtk_fixed_move`
per child, no `setFrame:` per child.

### Cross-platform coordinate consistency

One coordinate system (top-left origin). Platform-specific conversion
happens once, at the root view boundary. No more per-child Y-inversion,
no more missing `isFlipped` overrides, no more coordinate system
mismatches between platforms.

### Compositor simplification

Connects directly to the Render Execution Efficiency Plan. With one
surface per window, the compositor's surface mailbox has one surface per
window rather than one per widget. The frame loop collects one surface,
renders all widget commands in one render pass, and presents once. This
eliminates the staggered per-widget rendering that causes content gaps
during resize.

---

## Risk assessment

**Text input / IME integration.** Native text input fields require
platform-specific IME handling. On macOS, `NSTextInputClient` protocol
requires a native NSView. On Windows, TSF (Text Services Framework)
requires an HWND. Solution: `NativeViewHost` (Phase 5) provides an
escape hatch for the few widgets that need it. Chromium uses exactly
this pattern — its omnibox is a `NativeViewHost` embedding a native
text field.

**Accessibility.** Screen readers interact with the native view
hierarchy. With a single native view, OmegaWTK must provide an
accessibility tree that maps virtual widgets to accessibility elements.
On macOS this means implementing `NSAccessibility` protocol on the
root view and returning child accessibility elements for each virtual
widget. On Windows this means implementing UI Automation providers.
Chromium does this for all its virtual Views.

**Scroll views.** The current `ScrollView` uses `NSScrollView` on
macOS, which requires a native view hierarchy (`documentView`). With
virtual views, scrolling becomes a clipping + translation operation on
the virtual tree, with custom scroll bar widgets. Alternatively,
`NativeViewHost` can embed a real `NSScrollView` for platform-native
scroll behavior.

**Platform integration features.** Drag-and-drop, tooltips, context
menus, and focus rings may interact with the native view hierarchy. Each
must be evaluated. Chromium routes all of these through its single root
view, so there are proven solutions for every case.

---

## Relationship to other plans

**Render Execution Efficiency Plan (Tier 1):** The surface mailbox
architecture assumes one `CompositorSurface` per view. With this plan,
the number of surfaces drops from one-per-widget to one-per-window,
dramatically simplifying the compositor. Phase B's frame loop collects
one surface instead of N, and the single render pass renders all
widgets' commands.

**Render Execution Efficiency Plan (Tier 2):** Per-command rendering
optimizations (batched GPU submission, geometry caching, frame diffing)
apply within the single surface's render pass. With one surface, these
optimizations have maximum impact — all widgets' commands are batched
into one GPU submission instead of N separate ones.

**Batched Compositing Pass Plan:** With one surface per window, the
compositing step (blitting layer textures to the swapchain) simplifies
to a single blit or becomes unnecessary — the render pass can target
the swapchain directly.

**Frame Pacing Plan:** Frame pacing operates per-window rather than
per-widget. The `FramePacingMonitor` tracks one frame rate (the
window's composite rate) rather than N widget rates.

---

## Phase dependency graph

```
Phase 1: Root-only native view (foundational)
    └─→ Phase 2: Virtual hit testing (required for interaction)
    └─→ Phase 3: Single-surface rendering (required for display)
            └─→ Phase 4: Coordinate system unification (cleanup)
    └─→ Phase 5: Native view embedding (escape hatch, can parallel)
```

Phases 1 and 2 should be implemented together — without hit testing,
virtual widgets can't receive events. Phase 3 connects to the Render
Execution Efficiency Plan's Tier 1 and can be developed in coordination.
Phase 4 is a cleanup pass. Phase 5 is independent and can proceed
whenever a native-view-requiring widget is needed.

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/UI/View.h` | 1, 2 | Add `isRootView()`, virtual child list, `hitTest()`. Remove per-view `NativeItem` for child views |
| `wtk/src/UI/View.Core.cpp` | 1, 2 | `addSubView()` adds to virtual list instead of calling `addChildNativeItem()`. Root view creation unchanged |
| `wtk/src/Widgets/BasicWidgets.cpp` | 1 | `wireChild()` / `unwireChild()` use virtual tree operations |
| `wtk/src/Native/macos/CocoaItem.mm` | 1, 4 | Override `isFlipped` → YES on root view. `addChildNativeItem()` becomes no-op for virtual children |
| `wtk/src/Native/win/HWNDItem.cpp` | 1, 4 | `addChildNativeItem()` becomes no-op. Remove Y-inversion for children |
| `wtk/src/Native/gtk/GTKItem.cpp` | 1 | `addChildNativeItem()` becomes no-op. Remove `gtk_fixed_put` for children |
| `wtk/include/omegaWTK/Native/NativeItem.h` | 1 | Child management methods become optional (only used by root) |
| `wtk/src/UI/WidgetTreeHost.cpp` | 2 | Event dispatch routes through root view's `hitTest()` |
| `wtk/src/Composition/Compositor.cpp` | 3 | Surface count reduces to one per window |
| `wtk/include/omegaWTK/Composition/CompositorClient.h` | 3 | `CompositorClientProxy` deposits into window-level surface |
| `wtk/include/omegaWTK/UI/Widget.h` | 2, 4 | Coordinate system uses top-left origin |
| New: `wtk/include/omegaWTK/UI/NativeViewHost.h` | 5 | `NativeViewHost` widget for embedding real native views |
| New: `wtk/src/UI/NativeViewHost.cpp` | 5 | Platform-specific native view embedding |

---

## References

### Codebase

- Native view creation: `View.Core.cpp:73–90` (`make_native_item`)
- Child view attachment: `View.Core.cpp:102–118` (`addSubView` → `addChildNativeItem`)
- Container wiring: `BasicWidgets.cpp:157–166` (`wireChild`)
- macOS child attachment: `CocoaItem.mm:307–319` (`addSubview`)
- macOS view init (layer-hosting): `CocoaItem.mm:17–42` (`wantsLayer`, `self.layer = [CALayer layer]`)
- macOS resize: `CocoaItem.mm:256–305` (`resize`, "Child layer geometry is owned by the compositor backend")
- Windows child attachment: `HWNDItem.cpp:228–241` (`SetParent`, `SetWindowPos`, Y-inversion)
- GTK child attachment: `GTKItem.cpp:511–533` (`gtk_fixed_put`)

### External — Chromium

- Views architecture: Chromium uses a virtual `views::View` tree with one native surface per window. `views::View` has no native platform backing.
- `NativeViewHost` (`ui/views/controls/native/native_view_host.h`): embeds real native views in the virtual tree for IME, web content, and native controls
- Aura (`ui/aura/window.h`): lightweight window objects within a single native window on Windows/Linux/ChromeOS. `aura::Window` is NOT a native window — the entire tree shares one HWND
- Hit testing: `RootView` receives all events, `ViewTargeter` walks the tree to find the deepest matching `views::View`
- Painting: pre-order traversal of View tree, each View paints via `OnPaint()` into shared canvas, clipped to bounds

### External — Platform documentation

- Apple: "Approximately 100 views" threshold for NSView performance. Recommends single-view-manages-lightweight-objects for hundreds of elements
- Apple: `isFlipped` determines coordinate origin (bottom-left vs top-left). Not inherited by child views
- Apple: layer-backed (`NSViewLayerContentsRedrawOnSetNeedsDisplay`) vs layer-hosting mode
- Microsoft: "An application should not use child windows indiscriminately. For best performance, an application should [...] divide its main window [...] in the window procedure"
- Microsoft: WPF single-HWND model, milcore DirectX composition, airspace problem with mixed HWNDs
- GTK4: "Child widgets such as buttons or entries don't have their own surface; they use the surface of their toplevel"
- GTK4: GSK scene graph — snapshot phase produces render nodes, backend renders into single surface

### External — Other toolkits

- Qt 4.4+ "alien widgets": only top-level QWidgets get native windows. Children draw into parent's native window. "Significantly speeds up widget painting, resizing, and removes flicker"
- Flutter: single native surface per window, all rendering via Skia/Impeller, "platform views" with hybrid composition for native embeds
- WinUI 3: single HWND, all controls windowless
- SwiftUI: creates native views under the hood but optimizes/merges the native tree
