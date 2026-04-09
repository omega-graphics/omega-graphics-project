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

### Single native view per window, owned by the backend

Each backend's `AppWindow` implementation internally creates and owns
exactly one native platform view. This is not exposed to the widget
layer — it is a private implementation detail of the native backend.

- **macOS:** `CocoaAppWindow` creates one `CocoaItem` as its internal
  `rootView`, set as the window's `contentViewController`
- **Windows:** The `HWNDAppWindow` creates one HWND per top-level window
- **Linux:** The `GTKAppWindow` creates one GtkWidget per top-level window

All widget `View` objects are purely virtual — they hold bounds, can
paint, and can receive events, but they never create or reference a
`NativeItem`. The widget tree is a purely in-memory hierarchy managed
by OmegaWTK. The `View` class has no concept of "root" vs "child" —
all Views are virtual. The native root is below the widget layer.

### WindowLayer elimination

The current `Composition::WindowLayer` class wraps a `NWH` (native
window handle), a rect reference, and a canvas. All access to the
native window already goes through `WindowLayer::native_window_ptr`,
making WindowLayer a pass-through with no independent logic. Its
responsibilities are absorbed:

- **Native window handle:** `AppWindow::Impl` holds the `NWH` directly
- **Window-level render target:** `AppWindow::Impl` holds the
  `ViewRenderTarget` directly (already does — created from
  `native_window_ptr->getRootView()`)
- **Window surface/canvas:** Moves to the compositor, which owns the
  single surface per window

`WindowLayer` is deleted. `AppWindow::getLayer()` is removed from the
public API.

### What changes

| Current | Proposed |
|---------|----------|
| `View` creates a `NativeItem` via `make_native_item()` | `View` holds virtual bounds only — no `NativeItem` |
| `WindowLayer` wraps `NWH` + rect + canvas | Deleted. `AppWindow::Impl` holds `NWH` directly |
| `AppWindow::setRootWidget()` calls `addNativeItem()` | `AppWindow::setRootWidget()` registers widget tree with compositor — no native view creation |
| `Container::wireChild()` calls `addSubView()` → `addChildNativeItem()` | `Container::wireChild()` adds child to virtual tree |
| Platform layer manages child positions | OmegaWTK layout manages all positions |
| Each widget has a native compositor layer | All widgets render into the window's single surface |
| Platform dispatches events to individual native views | Backend AppWindow receives all events, OmegaWTK hit-tests the virtual tree |
| N widgets = N NSViews/HWNDs/GtkWidgets | N widgets = 1 NSView/HWND/GtkWidget (internal to backend) |

### Phase 1 — Pure virtual Views + WindowLayer removal [COMPLETED]

**Goal:** Views become purely virtual. The root native view is an
internal detail of each backend's `AppWindow`. `WindowLayer` is
deleted. (NativeItem pointers remain on Views for render target
access — full removal deferred to Phase 3.)

The `View` class loses all `NativeItem` references. There is no
`isRootView()` — the concept doesn't exist at the View level. The
native root view lives inside the backend `AppWindow` implementation,
which already creates it today (`CocoaAppWindow::rootView`,
`HWNDAppWindow`'s HWND, `GTKAppWindow`'s widget).

```cpp
class View {
    // Purely virtual — no NativeItem, no native platform calls
    Core::Rect rect_;
    OmegaCommon::Vector<ViewPtr> children_;
    View * parent_ = nullptr;
    // ... layer tree, compositor proxy, etc.
};
```

**AppWindow::Impl changes:**

```cpp
struct AppWindow::Impl {
    // WindowLayer is gone. NWH held directly.
    Native::NWH nativeWindow;
    SharedHandle<Composition::ViewRenderTarget> rootViewRenderTarget;
    Composition::CompositorClientProxy proxy;
    // ...
};
```

`AppWindow::setRootWidget()` no longer calls `addNativeItem()`. It
registers the widget tree with the compositor and connects the
compositor to the window's single render target. The native backend
never sees widget Views.

**Platform layer changes:**

| Platform | Change |
|----------|--------|
| macOS | `CocoaAppWindow::addNativeItem()` removed. Root `CocoaItem` is the only NSView — internal to `CocoaAppWindow`. Override `isFlipped` → YES on the root view for top-left origin. |
| Windows | `HWNDAppWindow::addNativeItem()` removed. Root HWND is the only window — internal to the backend. No more `SetParent`/`SetWindowPos` for children. |
| GTK | `GTKAppWindow::addNativeItem()` removed. Root widget is the only GtkWidget — internal to the backend. No more `gtk_fixed_put` for children. |
| `NativeWindow` interface | `addNativeItem()` removed. `getRootView()` becomes private to the backend (compositor accesses it through `AppWindow`, not through `View`). |

### Phase 2 — Virtual hit testing [COMPLETED]

**Goal:** Route native events from the backend's root native view to
the correct virtual widget.

The backend `AppWindow` receives all mouse, keyboard, and touch events
from its single native view. It forwards them to the `WidgetTreeHost`,
which walks the virtual widget tree to find the target:

```cpp
Widget * WidgetTreeHost::hitTest(const Core::Position &point) const {
    // Delegate to the root widget's view tree
    return hitTestView(root->getView(), point);
}

Widget * WidgetTreeHost::hitTestView(View *view, const Core::Position &point) const {
    // Walk children in reverse z-order (front to back)
    for (auto it = view->children_.rbegin(); it != view->children_.rend(); ++it) {
        if ((*it)->containsPoint(point)) {
            auto *hit = hitTestView(*it,
                {point.x - (*it)->rect().pos.x,
                 point.y - (*it)->rect().pos.y});
            if (hit != nullptr) return hit;
        }
    }
    return view->owningWidget();
}
```

This replaces the current model where each native view independently
receives its own events from the platform. The backend `AppWindow`
translates platform coordinates to OmegaWTK coordinates once (at the
window boundary), then the hit test operates entirely in virtual
coordinates.

**Prior art:** Chromium's `ViewTargeter` performs exactly this traversal.
The `RootView` receives the native event, calls
`GetEventHandlerForPoint()`, and walks the View tree from leaf to root.

### Phase 3 — Single-surface rendering [COMPLETED]

**Goal:** All widgets render into the window's single backing surface.

Currently each View has its own compositor surface (render target,
layer texture, Metal/GL backing). In the new model, the compositor
owns one surface per window, backed by the render target that
`AppWindow::Impl` creates from the backend's root native view. All
widget Views paint by recording drawing commands that the compositor
composites into this single surface.

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
    Widget B ──┼─→ Compositor → Window Surface (from AppWindow backend) → Present
    Widget C ──┘
    (1 present, 1 native view internal to backend)
```

**Sub-phases:**

**3a — Window-offset CanvasFrames.** [DONE] Add `Core::Position windowOffset`
to `CanvasFrame`. Each Canvas stamps its frame with the owning View's
window-relative position (computed by walking the View parent chain).
The backend uses this offset to place the frame's draw commands at the
correct region within the shared window surface.

**3b — Views stop creating per-View NativeItems.** [DONE] The `View`
constructor no longer calls `make_native_item()`. Views become purely
virtual: rect, children, layer tree, compositor proxy — no native
backing. The View holds a *shared* reference to a `ViewRenderTarget`
that is propagated from the window (not owned per-View).

**3c — Window render target propagation.** [DONE] `View::setWindowRenderTarget()`
accepts the window's `ViewRenderTarget`, updates the View's
`CompositorClientProxy`, and recurses to subviews.
`WidgetTreeHost::initWidgetTree()` propagates
`AppWindow::Impl::rootViewRenderTarget` down the entire widget tree.

**3d — Visual tree consolidation.** [DONE] The per-View
`PreCreatedVisualTreeData` / `PreCreatedResourceRegistry` pipeline is
removed from `View`. Only `AppWindow::Impl` pre-creates a
`BackendVisualTree` (with its root visual and native present surface)
for the window's render target. All Views submit
`CompositionRenderCommand`s that reference the window's single render
target, and the backend maps them to the window's single
`BackendCompRenderTarget` entry. The `ensureLayerSurfaceTarget()` path
in `Execution.cpp` creates layer surfaces within the window's visual
tree (not per-View visual trees). `PreCreatedResourceRegistry` stores
one entry (the window's render target → the window's visual tree data).

**3e — Virtual View operations.** [DONE] `View::resize()` updates the virtual
rect and layer rect — no `NativeItem::resize()` or
`resizeNativeLayer()` calls. `View::enable()`/`disable()` become state
flags — no `NativeItem::enable()`/`disable()` calls.

**3f — Backend viewport offset.** [DONE] In
`Compositor::executeCurrentCommand(Render)`, the backend reads
`frame->windowOffset` and sets the GPU viewport/scissor to the View's
region within the shared window surface before rendering the frame's
commands. `BackendRenderTargetContext::setViewportOverride()` positions
each View's rendering within the shared backing surface. The backing
surface grows automatically on window resize but never shrinks.

### Phase 3g — Virtual ScrollView

**Goal:** Replace the NativeItem-backed `ScrollView` with a fully virtual
implementation. After this phase, **no** View subclass creates a
NativeItem — the only path to a native view is the Phase 5
`NativeViewHost` escape hatch.

The current `ScrollView` calls `make_native_item(rect, ScrollItem)` in
its constructor and delegates everything to the platform scroll view
(`NSScrollView`, `ScrollViewer`, `GtkScrolledWindow`). This is the last
remaining use of the legacy `View(rect, NativeItemPtr, parent)`
constructor. Replacing it with a virtual scroll view:

1. Eliminates the last NativeItem dependency from View subclasses
2. Makes scroll behavior fully customizable (custom scroll bars, custom
   momentum physics, overscroll effects, pull-to-refresh)
3. Unblocks removal of the legacy View constructor and the NativeItem.h
   include from View.h
4. Brings ScrollView in line with Phase 3's single-surface model — scroll
   content renders into the window surface via viewport/scissor clipping

**Sub-phases:**

**3g-i — Virtual scroll container.** [DONE] Replace `ScrollView`'s constructor
to use the standard purely virtual `View(rect, parent)` path instead
of `View(rect, NativeItemPtr, parent)`. The ScrollView becomes a
virtual View that owns a content child and tracks a scroll offset
(`Core::Position scrollOffset`). Content is clipped to the ScrollView's
visible bounds via the compositor's scissor rect.

**3g-ii — Scroll input handling.** [DONE] Route scroll wheel / trackpad events
through the virtual hit-test path (`WidgetTreeHost::dispatchInputEvent`).
Add `NativeEvent::ScrollWheel` with delta-x/delta-y and cursor position.
`ScrollViewDelegate` receives these and updates `scrollOffset`. On macOS,
`scrollWheel:` on the root `CocoaItem` emits `ScrollWheel` events. On
Windows, `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL` emit `ScrollWheel`. On GTK,
the `scroll` signal emits `ScrollWheel`. `AppWindowDelegate` routes
`ScrollWheel` to `WidgetTreeHost::dispatchInputEvent`, which hit-tests
and delivers to the target View. A `DefaultScrollHandler` on `ScrollView`
updates `scrollOffset` when no delegate is set.

**3g-iii — Virtual scroll bars.** [DONE] Implement scroll bar indicators as
composited Layers within the ScrollView's LayerTree. Always-visible
minimal overlay style: thin rounded-rect thumbs drawn via
`Canvas::drawRoundedRect`. Thumb size and position proportional to
content size vs visible size. Repainted on each `setScrollOffset()`.

**3g-iv — Scroll content offset in compositor.** When painting, the
ScrollView applies a translation to its content child's `windowOffset`
so the compositor renders the content at `(windowOffset - scrollOffset)`.
The viewport/scissor clips to the ScrollView's visible bounds, hiding
content that has scrolled out of view. Only visible content needs to
paint.

The scroll offset is integrated into the existing `computeWindowOffset()`
parent-chain walk. `View::computeWindowOffset()` accumulates position
offsets by walking up the parent chain. At each ancestor, if that
ancestor is a `ScrollView`, its `scrollOffset` is subtracted from the
running total. This means any View inside a ScrollView automatically
gets a window offset that reflects the current scroll position — no
changes to the Canvas or Compositor are needed beyond what Phase 3f
already established.

Implementation detail: `View::computeWindowOffset()` currently walks
`impl_->parent_ptr` and sums `rect.pos`. To support scroll offsets
without coupling View to ScrollView, add a virtual method
`View::scrollOffsetContribution()` that returns `{0,0}` by default.
`ScrollView` overrides it to return its `scrollOffset`. The walk
becomes:

```cpp
Core::Position View::computeWindowOffset() const {
    Core::Position offset {0.f, 0.f};
    const View *v = this;
    while (v != nullptr) {
        offset.x += v->impl_->rect.pos.x;
        offset.y += v->impl_->rect.pos.y;
        // If this View's parent is a scrolling container, subtract
        // the scroll offset so content appears translated.
        if (v->impl_->parent_ptr != nullptr) {
            auto scroll = v->impl_->parent_ptr->scrollOffsetContribution();
            offset.x -= scroll.x;
            offset.y -= scroll.y;
        }
        v = v->impl_->parent_ptr;
    }
    return offset;
}
```

This keeps View decoupled from ScrollView — the virtual dispatch
handles the polymorphism, and non-scrolling Views pay only for a
trivial `{0,0}` return. Nested ScrollViews compose naturally: each
ancestor ScrollView subtracts its own offset during the walk.

**3g-v — Remove legacy View constructor.** [DONE] Delete `View(rect,
NativeItemPtr, parent)` constructor, the corresponding `Impl` constructor
in `ViewImpl.h`, and `View::preCreateVisualResources()`. Remove the
`#include "omegaWTK/Native/NativeItem.h"` from `View.h`. The
`NativeItem.h` include in `Layer.h` remains until `NativeLayerTreeLimb`
inheritance is refactored.

```
Current ScrollView:
    ScrollView → make_native_item(ScrollItem)
              → NSScrollView / ScrollViewer / GtkScrolledWindow
              → platform-managed scroll bars, clipping, momentum

Virtual ScrollView:
    ScrollView → View (purely virtual, no NativeItem)
              → scrollOffset applied as content translation
              → scissor clips to visible bounds
              → custom scroll bar Layers drawn by Canvas
              → scroll wheel events via virtual hit-test path
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
2. Requests the backend `AppWindow` to embed it as a child of the
   backend's internal root native view
3. Synchronizes its bounds with the virtual widget tree
4. Handles the "airspace" problem (native views render on top of
   virtual content)

```cpp
class NativeViewHost : public Widget {
    std::shared_ptr<Native::NativeItem> embeddedItem_;
public:
    /// Attach a real native view. The backend AppWindow will add it
    /// as a child of its internal root native view. Bounds are
    /// synchronized with this widget's position in the virtual tree.
    void attach(std::shared_ptr<Native::NativeItem> nativeItem);
    void detach();
};
```

This is the **only** path for creating native subviews. The backend
`AppWindow` exposes a method specifically for `NativeViewHost` to
embed/unembed native views — this replaces the old `addNativeItem()`
which was the general-purpose path for all widgets.

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

### WindowLayer indirection removed

`WindowLayer` was a pass-through that held the native window handle, a
rect reference, and a canvas — all of which are already directly
accessible. Every call site went through `impl_->layer->native_window_ptr`
to reach the native window. With WindowLayer deleted, `AppWindow::Impl`
holds the `NWH` directly, reducing indirection and making the ownership
model explicit: `AppWindow` owns the native window, the native window
owns the root native view, the compositor owns the single surface.

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
macOS, which requires a native view hierarchy (`documentView`). Phase 3g
replaces this with a fully virtual scroll view: scrolling becomes a
clipping + translation operation on the virtual tree, with custom
scroll bar Layers. If platform-native scroll behavior is ever needed
(e.g. rubber-banding or momentum physics that exactly match the OS),
`NativeViewHost` (Phase 5) can embed a real `NSScrollView`.

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
Phase 1: Pure virtual Views + WindowLayer removal (foundational) [COMPLETED]
    └─→ Phase 2: Virtual hit testing (required for interaction) [COMPLETED]
    └─→ Phase 3: Single-surface rendering (required for display) [COMPLETED]
        ├─ 3a: Window-offset CanvasFrames [DONE]
        ├─ 3b: Views stop creating per-View NativeItems [DONE]
        ├─ 3c: Window render target propagation [DONE]
        ├─ 3d: Visual tree consolidation (per-View → per-window) [DONE]
        ├─ 3e: Virtual View operations [DONE]
        └─ 3f: Backend viewport offset [DONE]
            └─→ Phase 3g: Virtual ScrollView [DONE]
                ├─ 3g-i: Virtual scroll container [DONE]
                ├─ 3g-ii: Scroll input handling [DONE]
                ├─ 3g-iii: Virtual scroll bars [DONE]
                ├─ 3g-iv: Scroll content offset in compositor [DONE]
                └─ 3g-v: Remove legacy View constructor [DONE]
                    └─→ Phase 4: Coordinate system unification (cleanup)
    └─→ Phase 5: Native view embedding (escape hatch, can parallel)
```

Phases 1, 2, and 3 are complete. Phase 3 sub-phases 3a–3g are done —
per-View render targets, NativeItems, visual trees, and GPU surfaces
have been consolidated to a single-per-window model. Phase 3g virtualized
ScrollView, removing the last NativeItem dependency from View subclasses
and the legacy View constructor. No View subclass creates a NativeItem.
The only path to a native view is the Phase 5 `NativeViewHost` escape
hatch. Phase 3
connects to the Render Execution Efficiency Plan's Tier 1 and can be
developed in coordination. Phase 4 is a cleanup pass. Phase 5 is
independent and can proceed whenever a native-view-requiring widget is
needed.

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/UI/View.h` | 1 | Remove `NativeItem` dependency entirely. View becomes purely virtual: bounds, virtual child list, layer tree, compositor proxy. Remove `isRootView()`. Remove `NativeItem.h` include |
| `wtk/src/UI/View.Core.cpp` | 1 | `addSubView()` adds to virtual child list only — no `addChildNativeItem()`. View constructor no longer calls `make_native_item()` |
| `wtk/include/omegaWTK/Composition/Layer.h` | 1 | Delete `WindowLayer` class. Remove `NativeWindow.h` include |
| `wtk/src/Composition/Layer.cpp` | 1 | Delete `WindowLayer` implementation |
| `wtk/src/UI/AppWindowImpl.h` | 1 | Replace `UniqueHandle<WindowLayer>` with `Native::NWH` held directly. Remove `WindowLayer` include |
| `wtk/src/UI/AppWindow.cpp` | 1 | `setRootWidget()` registers widget tree with compositor — no `addNativeItem()` call. All `impl_->layer->native_window_ptr` becomes `impl_->nativeWindow`. Remove `getLayer()` |
| `wtk/include/omegaWTK/UI/AppWindow.h` | 1 | Remove `getLayer()` from public API. Remove `WindowLayer` forward declaration |
| `wtk/include/omegaWTK/Native/NativeWindow.h` | 1 | Remove `addNativeItem()` from interface. `getRootView()` stays but is accessed only by `AppWindow::Impl`, not by `View` |
| `wtk/src/Native/macos/CocoaAppWindow.mm` | 1, 4 | Remove `addNativeItem()`. Override `isFlipped` → YES on root view. Root `CocoaItem` remains internal |
| `wtk/src/Native/win/HWNDAppWindow.cpp` | 1, 4 | Remove `addNativeItem()`. Remove child Y-inversion |
| `wtk/src/Native/gtk/GTKAppWindow.cpp` | 1 | Remove `addNativeItem()`. Remove child `gtk_fixed_put` |
| `wtk/include/omegaWTK/Native/NativeItem.h` | 1 | Remove `addChildNativeItem()` and child management methods entirely |
| `wtk/src/Widgets/BasicWidgets.cpp` | 1 | `wireChild()` / `unwireChild()` use virtual tree operations only |
| `wtk/include/omegaWTK/UI/View.h` | 2 | **Done.** Added `containsPoint()` — bounds check for hit testing |
| `wtk/src/UI/View.Core.cpp` | 2 | **Done.** Implemented `containsPoint()` |
| `wtk/src/UI/WidgetTreeHost.h` | 2 | **Done.** Declared `hitTest()`, `hitTestWidget()`, `dispatchInputEvent()`, `hoveredView_` |
| `wtk/src/UI/WidgetTreeHost.cpp` | 2 | **Done.** Implemented hit test (reverse z-order widget tree walk), event dispatch with hover tracking |
| `wtk/src/UI/AppWindow.cpp` | 2 | **Done.** `AppWindowDelegate::onRecieveEvent` routes input events to `WidgetTreeHost::dispatchInputEvent` |
| `wtk/src/UI/AppWindowImpl.h` | 2 | **Done.** Root NativeItem's `event_emitter` wired to AppWindow for input event routing |
| `wtk/include/omegaWTK/Composition/Canvas.h` | 3a | Add `Core::Position windowOffset` to `CanvasFrame` |
| `wtk/src/Composition/Canvas.cpp` | 3a | `nextFrame()` stamps `windowOffset` via `View::computeWindowOffset()` |
| `wtk/include/omegaWTK/UI/View.h` | 3b,3c,3e | Add `setWindowRenderTarget()`, `computeWindowOffset()`. Remove `NativeItem.h` include. `enable()`/`disable()` become virtual state |
| `wtk/src/UI/View.Core.cpp` | 3b,3c,3e | Remove `make_native_item()` from constructor. Remove `preCreateVisualResources()`. `resize()`/`enable()`/`disable()` become purely virtual. Implement `setWindowRenderTarget()`, `computeWindowOffset()` |
| `wtk/src/UI/ViewImpl.h` | 3b,3d | Render target becomes shared/propagated from window. Remove `PreCreatedVisualTreeData` ownership. Add `enabled_` flag |
| `wtk/src/UI/WidgetTreeHost.cpp` | 3c | `initWidgetTree()` propagates window's `rootViewRenderTarget` to all Views |
| `wtk/src/Composition/backend/Execution.cpp` | 3d,3f | `ensureLayerSurfaceTarget()` uses window's visual tree. `executeCurrentCommand(Render)` applies `windowOffset` viewport/scissor |
| `wtk/include/omegaWTK/Composition/Layer.h` | 3b | Remove `NativeItem.h` include |
| `wtk/src/Composition/Compositor.cpp` | 3 | Surface count reduces to one per window, obtained from `AppWindow`'s render target |
| `wtk/include/omegaWTK/Composition/CompositorClient.h` | 3 | `CompositorClientProxy` deposits into window-level surface |
| `wtk/include/omegaWTK/UI/ScrollView.h` | 3g | Remove `NativeItem` dependency. `ScrollView` becomes virtual View with `scrollOffset`, custom scroll bar Layers. `ScrollViewDelegate` receives scroll-wheel events instead of platform callbacks |
| `wtk/src/UI/ScrollView.cpp` | 3g | Replace `make_native_item(ScrollItem)` constructor with `View(rect,parent)`. Implement virtual scroll: offset tracking, scissor clipping, scroll bar painting. Remove `getNativePtr()` calls |
| `wtk/include/omegaWTK/Native/NativeEvent.h` | 3g-ii | Add `NativeEvent::ScrollWheel` type with delta-x/delta-y params |
| `wtk/src/Native/macos/CocoaAppWindow.mm` | 3g-ii | Forward `scrollWheel:` events from root CocoaItem to the event emitter |
| `wtk/src/Native/win/WinAppWindow.cpp` | 3g-ii | Forward `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` as ScrollWheel events |
| `wtk/src/Native/gtk/GTKAppWindow.cpp` | 3g-ii | Forward `scroll` signal as ScrollWheel events |
| `wtk/include/omegaWTK/UI/View.h` | 3g-v | Remove `View(rect, NativeItemPtr, parent)` constructor. Remove `#include "omegaWTK/Native/NativeItem.h"`. Remove `preCreateVisualResources()` |
| `wtk/src/UI/View.Core.cpp` | 3g-v | Delete legacy NativeItem constructor and `preCreateVisualResources()` stub |
| `wtk/src/UI/ViewImpl.h` | 3g-v | Delete legacy `Impl(owner, renderTargetValue, rect, parent)` constructor |
| `wtk/include/omegaWTK/UI/Widget.h` | 4 | Coordinate system uses top-left origin |
| New: `wtk/include/omegaWTK/UI/NativeViewHost.h` | 5 | `NativeViewHost` widget — the only path to create a native subview |
| New: `wtk/src/UI/NativeViewHost.cpp` | 5 | Platform-specific native view embedding via backend `AppWindow` |

---

## References

### Codebase

- `WindowLayer` (to be deleted): `Layer.h:235–248` — wraps `NWH` + rect + canvas, pass-through only
- `AppWindow::Impl`: `AppWindowImpl.h:10–28` — creates `WindowLayer`, holds `ViewRenderTarget` from `native_window_ptr->getRootView()`
- `AppWindow::setRootWidget()`: `AppWindow.cpp:63–69` — calls `addNativeItem()` (to be removed)
- `CocoaAppWindow` root view: `CocoaAppWindow.mm:49` — creates `CocoaItem` as internal root, sets as `contentViewController`
- `CocoaAppWindow::addNativeItem()`: `CocoaAppWindow.mm:93–114` — adds widget view as native subview (to be removed)
- `NativeWindow::addNativeItem()`: `NativeWindow.h:34` — interface method (to be removed)
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
