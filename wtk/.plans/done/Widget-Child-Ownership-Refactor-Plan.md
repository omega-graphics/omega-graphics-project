# Widget Child Ownership Refactor Plan

## Problem

Widget has two paths for establishing parent-child relationships:

1. **Constructor path** -- `Widget(ViewPtr view, WidgetPtr parent)` does view wiring and child tracking when `parent != nullptr`.
2. **Container/StackWidget `addChild` path** -- calls `child->setParentWidget(this)` which does the same view wiring through `setParentWidgetImpl`.

Both paths converge on the same operations (`view->addSubView`, `children.push_back`, `setTreeHostRecurse`), but the existence of two entry points creates an out-of-sync risk between:

- Widget's `children` vector (used by `WidgetTreeHost` for compositor observation, tree init, resize dispatch)
- Container's `containerChildren` vector (used for Container layout)
- StackWidget's `stackChildren` vector (used for flex layout)

When children are added via `Container::addChild` / `StackWidget::addChild`, the child correctly lands in Widget's `children` (via `setParentWidgetImpl`) **and** the container-specific list. But the compositor's awareness of child layer trees depends on when `observeWidgetLayerTreesRecurse` runs relative to when children are added. If the tree host has already observed the parent before children are attached, those children's layer trees may not be observed until the next `initWidgetTree` call.

Additionally, Widget stores a `children` vector even though raw Widgets should never logically have children -- only Container types should.

## Goal

1. Remove parent attachment from the Widget constructor.
2. Remove Widget's `children` vector. Widget becomes a leaf by default.
3. Only Container can own children. StackWidget becomes a subtype of Container.
4. Eliminate the dual-path ambiguity.

## Design

### Widget

```
Widget(ViewPtr view)   // no parent parameter
```

Widget keeps:
- `ViewPtr view` -- its own view (unchanged)
- `Widget *parent` -- back-pointer to parent (set by Container when attached, cleared on detach)
- `WidgetTreeHost *treeHost` -- set by Container propagation or directly by WidgetTreeHost

Widget loses:
- `OmegaCommon::Vector<Widget *> children` -- moves to Container types
- `setParentWidget(WidgetPtr)` / `setParentWidget(Widget *)` -- replaced by Container::addChild
- `detachFromParent()` -- replaced by Container::removeChild
- `setParentWidgetImpl(Widget *, WidgetPtr)` -- deleted
- `removeChildWidget(Widget *)` -- deleted
- `onChildAttached(Widget *)` / `onChildDetached(Widget *)` -- deleted (Container handles internally)
- `acceptsChildWidget(const Widget *)` -- deleted

Widget gains:
- `virtual OmegaCommon::Vector<Widget *> childWidgets() const { return {}; }` -- returns empty by default. Container/StackWidget override this to return their child lists. This is the single point WidgetTreeHost uses to walk the tree.

Widget keeps (moved to private or Container-friend):
- `setTreeHostRecurse(WidgetTreeHost *)` -- still needed, but now walks `childWidgets()`

### Container

```
Container(ViewPtr view)   // no parent parameter
```

Container owns:
- `OmegaCommon::Vector<Widget *> children` -- protected member (renamed from `containerChildren`). The single source of truth for child widgets. Accessible to subclasses (StackWidget).
- `WidgetPtr addChild(const WidgetPtr & child)` -- virtual, does all wiring:
  1. `child->parent = this`
  2. `view->addSubView(child->view.get())`
  3. `children.push_back(child.get())`
  4. `child->setTreeHostRecurse(treeHost)` (propagate compositor if already attached)
  5. If compositor is live, `compositor->observeLayerTree(child->view->getLayerTree(), syncLaneId)` recursively
  6. `relayout()`
- `bool removeChild(const WidgetPtr & child)` -- virtual, reverses all wiring
- Overrides `childWidgets()` to return `children`

### StackWidget : public Container

StackWidget becomes a subtype of Container instead of Widget directly. This gives it access to Container's protected `children` vector and its wiring logic.

```
StackWidget(StackAxis axis, ViewPtr view, const StackOptions & options = {})
```

StackWidget adds:
- `OmegaCommon::Vector<StackSlot> childSlots` -- parallel to Container's `children`, stores per-child layout metadata (flex grow/shrink, basis, margins, alignment). Indexed 1:1 with `children`.
- Overrides `addChild` to accept a slot: `WidgetPtr addChild(const WidgetPtr & child, const StackSlot & slot = {})`. Calls `Container::addChild(child)` for the wiring, then appends the slot to `childSlots`.
- Overrides `removeChild` to call `Container::removeChild(child)` and erase the corresponding slot entry.
- `bool setSlot(const WidgetPtr & child, const StackSlot & slot)` / `bool setSlot(std::size_t idx, const StackSlot & slot)` -- update slot metadata and relayout.
- Layout logic reads `children` (from Container) paired with `childSlots` for flex resolution.

StackWidget loses:
- `stackChildren` vector of `ChildEntry` structs -- replaced by Container's `children` + the parallel `childSlots` vector.
- `ChildEntry` struct's `widget` field -- the widget pointer now lives in Container's `children`.
- `ChildEntry` struct is replaced by `StackSlot` plus per-child cached preferred sizes stored separately or computed on the fly during layout.

StackWidget keeps:
- `ChildEntry` may be retained as a layout-time struct (populated during `layoutChildren` from `children` + `childSlots`), but it is no longer the persistent storage.

HStack/VStack remain thin constructors over StackWidget, unchanged.

### WIDGET_CONSTRUCTOR Macro

The `WIDGET_CONSTRUCTOR` macro exists for concrete widget subclasses (Label, Button, TextInput, etc.) that know their own View type and create it internally. Users of these widgets pass a `Composition::Rect` and the `Create()` method handles View construction. With the parent parameter removed from Widget, the macro simplifies:

Before:
```cpp
#define WIDGET_CONSTRUCTOR(...) static SharedHandle<Widget> Create(Composition::Rect rect, WidgetPtr parent, ## __VA_ARGS__);
```

After:
```cpp
#define WIDGET_CONSTRUCTOR(...) static SharedHandle<Widget> Create(Composition::Rect rect, ## __VA_ARGS__);
```

`WIDGET_CONSTRUCTOR_IMPL` follows the same change. The `WIDGET_CREATE` alias (`make`) is unchanged.

This means concrete widget subclasses have a clean two-tier API:
- **Low-level**: `Widget(ViewPtr view)` — for custom subclasses that manage their own View.
- **High-level**: `Label::Create(rect, ...)` / `Button::Create(rect, ...)` — for implementation widgets where the View is an internal detail.

In both cases, parent attachment happens afterward via `Container::addChild`, never at construction.

### WidgetTreeHost

All recursive tree walks change from `parent->children` to `parent->childWidgets()`:

- `initWidgetRecurse(Widget *w)` -- walks `w->childWidgets()`
- `observeWidgetLayerTreesRecurse(Widget *w)` -- walks `w->childWidgets()`
- `unobserveWidgetLayerTreesRecurse(Widget *w)` -- walks `w->childWidgets()`
- `invalidateWidgetRecurse(Widget *w, ...)` -- walks `w->childWidgets()`
- `beginResizeCoordinatorSessionRecurse(Widget *w, ...)` -- walks `w->childWidgets()`
- `detectAnimatedTreeRecurse(Widget *w)` -- walks `w->childWidgets()`

### Widget destructor

Currently Widget's destructor clears `children` and removes itself from parent. With this change:
- Widget's destructor no longer touches children (it has none).
- If `parent != nullptr`, the parent (a Container) should be notified. Options:
  - Widget destructor calls `parent->removeChild(this)` -- but Widget doesn't hold its own shared_ptr.
  - Widget destructor sets `parent = nullptr` and trusts the Container's destructor to clean up.
  - Container's destructor already clears its children lists.

Safest: Widget destructor nulls `parent`. Container's destructor clears `children` and nulls each child's parent pointer. StackWidget's destructor clears `childSlots` then delegates to Container.

### AppWindow

No further changes needed beyond what was just done (`setRootWidget`). The root widget's view is the content view.

## File Change Summary

| File | Change |
|------|--------|
| `Widget.h` | Remove `children`, `setParentWidget`, `detachFromParent`, `removeChildWidget`, `onChildAttached`, `onChildDetached`, `acceptsChildWidget`. Remove parent from constructor. Add virtual `childWidgets()`. |
| `Widget.cpp` | Delete `setParentWidgetImpl`, `removeChildWidget`, `onChildAttached`, `onChildDetached`, `acceptsChildWidget`. Simplify destructor. Update `setTreeHostRecurse` / `onThemeSetRecurse` to use `childWidgets()`. |
| `BasicWidgets.h` | Simplify `WIDGET_CONSTRUCTOR` / `WIDGET_CONSTRUCTOR_IMPL` macros to `(Composition::Rect rect, ...)` (drop parent). Update Container constructor. `children` becomes protected. Make `addChild`/`removeChild` virtual. Remove `onChildAttached`/`onChildDetached` overrides. Override `childWidgets()`. |
| `BasicWidgets.cpp` | Container::addChild does full wiring (view, parent ptr, tree host, compositor). Container::removeChild reverses it. Remove `onChildAttached`/`onChildDetached`. |
| `Containers.h` | StackWidget inherits from Container (was Widget). Replace `stackChildren` with `childSlots` parallel vector. Override `addChild`/`removeChild`. Constructor becomes `(StackAxis, ViewPtr view, ...)`. |
| `Containers.cpp` | StackWidget::addChild calls `Container::addChild` then appends slot. StackWidget::removeChild calls `Container::removeChild` then erases slot. Layout reads Container's `children` paired with `childSlots`. |
| `WidgetTreeHost.h` | No API changes. |
| `WidgetTreeHost.cpp` | All `parent->children` loops become `parent->childWidgets()`. |
| `AppWindow.cpp` | No changes (already uses `setRootWidget`). |
| `Layout.cpp` | `runWidgetLayout` walks `childWidgets()` instead of `children`. |
| Test files (7) | Remove parent parameter from Widget/Container/StackWidget constructors. |

## Migration for Test Files

Before:
```cpp
auto widget = make<MyWidget>(View::Create(rect), WidgetPtr{});
```

After:
```cpp
auto widget = make<MyWidget>(View::Create(rect));
```

Container child addition stays the same:
```cpp
container->addChild(child);          // Container
stack->addChild(child, slot);        // StackWidget
```

## Risk

Low-medium. The refactor is structural but the external behavior is identical:
- Container/StackWidget addChild already does all the wiring via `setParentWidget` today. We're just removing the redundant Widget-level path.
- The `childWidgets()` virtual adds one vtable call per recursion step. Acceptable for a UI framework tree walk.
- Biggest risk: any code that constructs a Widget with a non-null parent and expects constructor-time attachment will break. Grep for `Widget(.*,.*parent` / `WIDGET_CREATE` call sites.
