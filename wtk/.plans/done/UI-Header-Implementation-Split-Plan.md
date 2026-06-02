# UI Header Implementation Split Plan

## Goal

Reduce the amount of implementation detail exposed from `wtk/include/omegaWTK/UI` while also breaking up the largest `src/UI` translation units into smaller, easier-to-maintain files.

This plan treats the following as public API for now:

- `ViewResizeCoordinator` in `include/omegaWTK/UI/View.h`
- `LayoutBehavior` and related public layout abstractions in `include/omegaWTK/UI/Layout.h`

This plan treats the following as internal implementation details:

- `WidgetTreeHost`
- resize session tracking and validation internals currently in `WidgetTreeHost.h`
- concrete `UIView`, `View`, `Widget`, and `AppWindow` private state
- concrete layout behavior implementations that do not need to be public

## Constraints

- Keep downstream includes such as `#include <omegaWTK/UI/View.h>` and `#include <omegaWTK/UI/Layout.h>` working.
- Do not move `ViewResizeCoordinator` or `LayoutBehavior` out of the installed public headers yet.
- Prefer private headers under `wtk/src/UI` so UI implementation files can share internals without growing public headers.
- Keep the refactor incremental so the library stays buildable after each phase.

## Current Pressure Points

- `wtk/include/omegaWTK/UI/UIView.h` carries a large amount of private animation, layout, and dirty-state bookkeeping.
- `wtk/include/omegaWTK/UI/View.h` exposes `View` internals and the full definition of `ViewResizeCoordinator`.
- `wtk/include/omegaWTK/UI/Widget.h` carries paint state, layout state, host pointers, and observer plumbing.
- `wtk/include/omegaWTK/UI/AppWindow.h` exposes internal composition and hosting members.
- `wtk/include/omegaWTK/UI/WidgetTreeHost.h` is currently installed even though usage is internal to the UI implementation.
- `wtk/src/UI/UIView.cpp` is the largest UI translation unit and should be split first.

## Target File Layout

### Public headers to keep installed

- `wtk/include/omegaWTK/UI/App.h`
- `wtk/include/omegaWTK/UI/AppWindow.h`
- `wtk/include/omegaWTK/UI/CanvasView.h`
- `wtk/include/omegaWTK/UI/Layout.h`
- `wtk/include/omegaWTK/UI/Menu.h`
- `wtk/include/omegaWTK/UI/Notification.h`
- `wtk/include/omegaWTK/UI/ScrollView.h`
- `wtk/include/omegaWTK/UI/SVGView.h`
- `wtk/include/omegaWTK/UI/UIView.h`
- `wtk/include/omegaWTK/UI/VideoView.h`
- `wtk/include/omegaWTK/UI/View.h`
- `wtk/include/omegaWTK/UI/Widget.h`

### Private headers to add under `wtk/src/UI`

- `wtk/src/UI/WidgetTreeHost.h`
- `wtk/src/UI/UIViewImpl.h`
- `wtk/src/UI/ViewImpl.h`
- `wtk/src/UI/WidgetImpl.h`
- `wtk/src/UI/AppWindowImpl.h`
- `wtk/src/UI/LayoutBehaviors.h`

### Translation unit split targets

- `wtk/src/UI/UIView.cpp` into:
  - `wtk/src/UI/UIView.Core.cpp`
  - `wtk/src/UI/UIView.Style.cpp`
  - `wtk/src/UI/UIView.Layout.cpp`
  - `wtk/src/UI/UIView.Animation.cpp`
  - `wtk/src/UI/UIView.Update.cpp`
- `wtk/src/UI/View.cpp` into:
  - `wtk/src/UI/View.Core.cpp`
  - `wtk/src/UI/View.ResizeCoordinator.cpp`
- `wtk/src/UI/Widget.cpp` into:
  - `wtk/src/UI/Widget.Core.cpp`
  - `wtk/src/UI/Widget.Geometry.cpp`
  - `wtk/src/UI/Widget.Paint.cpp`
  - `wtk/src/UI/Widget.Layout.cpp`

If smaller files prove sufficient, `AppWindow.cpp` can remain a single file after its private state is moved behind `AppWindowImpl`.

## Build System Changes

Update `wtk/CMakeLists.txt` so internal code can include private headers from `src/UI` without exposing that path publicly.

### Exact change

After the `OmegaWTK` target is created, add:

```cmake
target_include_directories("OmegaWTK" PRIVATE "${OMEGAWTK_SOURCE_DIR}/src/UI")
```

This is needed so code outside `src/UI` can include internal UI headers later if needed, while keeping them out of the public include surface.

The existing:

```cmake
file(GLOB UI_SRCS "${OMEGAWTK_SOURCE_DIR}/src/UI/*.cpp")
```

can remain in place as long as the new split `.cpp` files stay directly under `src/UI`.

## Phase 1: Make `WidgetTreeHost` Private

Status: Completed on April 5, 2026.

### Files to change

- Remove installed header:
  - `wtk/include/omegaWTK/UI/WidgetTreeHost.h`
- Add private header:
  - `wtk/src/UI/WidgetTreeHost.h`
- Update includes in:
  - `wtk/src/UI/AppWindow.cpp`
  - `wtk/src/UI/Widget.cpp`
  - `wtk/src/UI/WidgetTreeHost.cpp`

### Exact code changes

1. Move the full contents of `wtk/include/omegaWTK/UI/WidgetTreeHost.h` into `wtk/src/UI/WidgetTreeHost.h`.
2. Change internal includes from:

```cpp
#include "omegaWTK/UI/WidgetTreeHost.h"
```

to:

```cpp
#include "WidgetTreeHost.h"
```

3. Leave forward declarations of `WidgetTreeHost` in public headers that need them:
   - `wtk/include/omegaWTK/UI/AppWindow.h`
   - `wtk/include/omegaWTK/UI/Widget.h`

### Expected result

- `WidgetTreeHost` is no longer part of the installed/public API.
- Existing public headers still compile because they only require a forward declaration.

## Phase 2: Hide `UIView` Private State Behind `Impl`

Status: Completed on April 5, 2026.

### Files to change

- `wtk/include/omegaWTK/UI/UIView.h`
- `wtk/src/UI/UIViewImpl.h`
- `wtk/src/UI/UIView.Core.cpp`
- `wtk/src/UI/UIView.Style.cpp`
- `wtk/src/UI/UIView.Layout.cpp`
- `wtk/src/UI/UIView.Animation.cpp`
- `wtk/src/UI/UIView.Update.cpp`

### Exact header change

Replace the current large private section in `UIView` with:

```cpp
struct Impl;
OmegaCommon::UniquePtr<Impl> impl_;
```

Keep these public nested types in `UIView.h`:

- `UpdateDiagnostics`
- `AnimationDiagnostics`
- `EffectState` only if it is intentionally public API

Move the following private types and members into `UIViewImpl.h`:

- `ElementDirtyState`
- `PropertyAnimationState`
- `PathNodeAnimationState`
- effect animation key enum
- `framesPerSec`
- `tag`
- `currentLayout`
- `currentStyle`
- `layoutDirty`
- `styleDirty`
- `firstFrameCoherentSubmit`
- `styleDirtyGlobal`
- `styleChangeRequiresCoherentFrame`
- `rootCanvas`
- `animationViewAnimator`
- `animationLayerAnimators`
- `elementDirtyState`
- `activeTagOrder`
- `elementAnimations`
- `pathNodeAnimations`
- `lastResolvedElementColor`
- `lastResolvedEffects`
- `previousShapeByTag`
- `fallbackTextFont`
- `currentLayoutV2_`
- `lastResolvedV2Rects_`
- `diagnosticSink_`
- `lastUpdateDiagnostics`
- `lastAnimationDiagnostics`
- `lastObservedDroppedPacketCount`
- `hasObservedLaneDiagnostics`

Move the following private methods into `UIViewImpl.h` and define them in the split `.cpp` files:

- `markAllElementsDirty`
- `markElementDirty`
- `isElementDirty`
- `clearElementDirty`
- `ensureAnimationViewAnimator`
- `ensureAnimationLayerAnimator`
- `beginCompositionClock`
- `startOrUpdateAnimation`
- `advanceAnimations`
- `animatedValue`
- `applyAnimatedColor`
- `applyAnimatedShape`
- `resolveFallbackTextFont`
- `convertLegacyLayoutToV2`

### Translation unit split

- `UIView.Core.cpp`: constructor, `layout()`, `setLayout()`, `setStyleSheet()`, `getStyleSheet()`, diagnostics getters, `layoutV2()`, `setLayoutV2()`, `setDiagnosticSink()`
- `UIView.Style.cpp`: style resolution, color/effect resolution, dirtying from stylesheet changes
- `UIView.Layout.cpp`: V1 to V2 conversion, layout rect resolution, transition application
- `UIView.Animation.cpp`: animation state setup, ticking, composition-clock interaction
- `UIView.Update.cpp`: `update()` and any final submission path

### Expected result

- `UIView.h` becomes much smaller.
- Changes inside `UIView` no longer force broad recompilation.

## Phase 3: Hide `View` Private State While Keeping `ViewResizeCoordinator` Public

### Files to change

- `wtk/include/omegaWTK/UI/View.h`
- `wtk/src/UI/ViewImpl.h`
- `wtk/src/UI/View.Core.cpp`
- `wtk/src/UI/View.ResizeCoordinator.cpp`

### Exact header change

Keep these public in `View.h`:

- `ResizeClamp`
- `ChildResizePolicy`
- `ChildResizeSpec`
- `ViewResizeCoordinator`
- `View`
- `ViewDelegate`

Inside `class View`, replace the current private state with:

```cpp
struct Impl;
Core::UniquePtr<Impl> impl_;
```

Move these `View` members into `ViewImpl.h`:

- `subviews`
- `renderTarget`
- `proxy`
- `ownLayerTree`
- `parent_ptr`
- `rect`
- `delegate`
- `preCreatedVisualTree_`

Do not move `ViewResizeCoordinator` out of `View.h`, but change it so its internal storage is also hidden if desired. There are two valid options:

1. Keep the current full `ViewResizeCoordinator` definition public for now.
2. Keep the public methods in `View.h`, but hide its `childState` and bookkeeping behind a nested `Impl`.

For this refactor, option 1 is the lower-risk choice because `Container.cpp` uses `ViewResizeCoordinator::clampRectToParent(...)`.

### Translation unit split

- `View.Core.cpp`: constructors, composition session methods, enable/disable, resize, delegate wiring, layer/canvas creation
- `View.ResizeCoordinator.cpp`: all `ViewResizeCoordinator` methods and any helper functions currently in `View.cpp`

### Expected result

- `ViewResizeCoordinator` remains public and usable by `src/Widgets/Containers.cpp`.
- `View` internal data stops leaking through the public header.

## Phase 4: Hide `Widget` Private State

### Files to change

- `wtk/include/omegaWTK/UI/Widget.h`
- `wtk/src/UI/WidgetImpl.h`
- `wtk/src/UI/Widget.Core.cpp`
- `wtk/src/UI/Widget.Geometry.cpp`
- `wtk/src/UI/Widget.Paint.cpp`
- `wtk/src/UI/Widget.Layout.cpp`

### Exact header change

Preserve the public and protected API used by widgets:

- `WidgetGeometryDelegate`
- `PaintMode`, `PaintReason`, `GeometryChangeReason`, `GeometryProposal`, `PaintOptions`
- `GeometryTraceContext`
- protected virtual hooks such as `onMount`, `onPaint`, `measureSelf`, `onLayoutResolved`
- protected `viewAs<T>()`
- protected `makeSubView<T>()`

Inside `Widget`, replace most private data with:

```cpp
struct Impl;
Core::UniquePtr<Impl> impl_;
```

Keep `ViewPtr view` protected for now unless all derived widgets can be migrated to an accessor. This keeps the refactor smaller and avoids touching all widget subclasses.

Move the following into `WidgetImpl.h`:

- `initialDrawComplete`
- `hasMounted`
- `paintInProgress`
- `hasPendingInvalidate`
- `pendingPaintReason`
- `mode`
- `options`
- `layoutStyle_`
- `layoutBehavior_`
- `hasExplicitLayoutStyle_`
- `observers`

Move these methods into the split implementation files:

- `onThemeSetRecurse`
- `executePaint`
- `handleHostResize`
- `notifyObservers`
- `geometryTraceLoggingEnabled`
- `geometryTraceContext`
- `requestRect`
- `setRect`
- layout accessors and `requestLayout`

### Translation unit split

- `Widget.Core.cpp`: constructors, destructor, show/hide, observer management
- `Widget.Geometry.cpp`: `rect()`, `requestRect()`, `setRect()`, geometry tracing
- `Widget.Paint.cpp`: `invalidate()`, `invalidateNow()`, `executePaint()`, mount/init path, theme recursion
- `Widget.Layout.cpp`: `setLayoutStyle()`, `layoutStyle()`, `setLayoutBehavior()`, `layoutBehavior()`, `requestLayout()`, `handleHostResize()`

### Expected result

- `Widget.h` stops carrying most runtime state.
- Widget subclasses still keep access to the root view through `view`.

## Phase 5: Hide `AppWindow` Private State

### Files to change

- `wtk/include/omegaWTK/UI/AppWindow.h`
- `wtk/src/UI/AppWindowImpl.h`
- `wtk/src/UI/AppWindow.cpp`

### Exact header change

Inside `AppWindow`, replace the current private member block with:

```cpp
struct Impl;
Core::UniquePtr<Impl> impl_;
```

Move the following into `AppWindowImpl.h`:

- `layer`
- `rootViewRenderTarget`
- `proxy`
- `delegate`
- `widgetTreeHost`
- `rect`
- `menu`

Keep `AppWindowDelegate` public for now unless a later API cleanup decides otherwise.

### Expected result

- `AppWindow.h` becomes API-focused and no longer exposes hosting internals.

## Phase 6: Move Concrete Layout Behavior Implementations Out of Public Headers

`LayoutBehavior` stays public, but concrete internal behavior classes do not need to stay in the installed layout header.

### Files to change

- `wtk/include/omegaWTK/UI/Layout.h`
- `wtk/src/UI/LayoutBehaviors.h`
- `wtk/src/UI/Layout.cpp`

### Exact header change

Keep these public in `Layout.h`:

- `LayoutUnit`
- `LayoutLength`
- `LayoutEdges`
- `LayoutClamp`
- `LayoutDisplay`
- `LayoutPositionMode`
- `LayoutAlign`
- `FlexDirection`
- `FlexWrap`
- `LayoutStyle`
- `LayoutContext`
- `MeasureResult`
- `LayoutBehavior`
- resolver free functions
- transition and diagnostic types

Move these concrete behavior types out of `Layout.h` and into `LayoutBehaviors.h`:

- `LegacyResizeCoordinatorBehavior`
- `StackLayoutBehavior`

Update `wtk/src/UI/Layout.cpp` to include:

```cpp
#include "LayoutBehaviors.h"
```

If other internal files need these behavior classes later, include the same private header there.

### Expected result

- `Layout.h` remains public but slimmer.
- Internal behavior implementations stop expanding the public surface.

## Phase 7: Follow-Up Header Hygiene

After the major moves above, do a cleanup pass on public headers.

### Files to review

- `wtk/include/omegaWTK/UI/View.h`
- `wtk/include/omegaWTK/UI/Widget.h`
- `wtk/include/omegaWTK/UI/AppWindow.h`
- `wtk/include/omegaWTK/UI/UIView.h`
- `wtk/include/omegaWTK/UI/Layout.h`

### Exact cleanup work

- Replace heavy includes with forward declarations where possible.
- Keep destructors out of line for classes using incomplete `Impl` types.
- Remove internal-only comments that describe private engine mechanics from installed headers.
- Ensure installed headers do not include `src/UI` private headers.

## Suggested Execution Order

1. Update `wtk/CMakeLists.txt` with the private `src/UI` include directory.
2. Move `WidgetTreeHost.h` into `wtk/src/UI`.
3. Introduce `UIViewImpl` and split `UIView.cpp`.
4. Introduce `ViewImpl` and split `View.cpp`.
5. Introduce `WidgetImpl` and split `Widget.cpp`.
6. Introduce `AppWindowImpl`.
7. Move concrete layout behaviors into `LayoutBehaviors.h`.
8. Do a public-header cleanup pass.

This order removes the clearest private type first, then attacks the largest translation units.

## Verification Checklist

After each phase:

- Build the `OmegaWTK` target.
- Build the UI and widget tests under `wtk/tests`.
- Confirm no public header in `wtk/include/omegaWTK/UI` includes a file from `wtk/src/UI`.
- Confirm external-style test code still compiles with only public includes.

At the end:

- `rg "WidgetTreeHost.h" wtk/include` should return no installed header.
- `rg "struct Impl;|class Impl;" wtk/include/omegaWTK/UI` should show the classes that now hide private state.
- `wc -l wtk/include/omegaWTK/UI/*.h` should show a measurable drop in the largest public headers, especially `UIView.h`, `Widget.h`, `View.h`, and `AppWindow.h`.

## Non-Goals For This Refactor

- Do not redesign the layout API yet.
- Do not remove `ViewResizeCoordinator` from the public API yet.
- Do not remove `LayoutBehavior` from the public API yet.
- Do not change widget semantics, painting semantics, or resize behavior as part of the mechanical move.

## End State

After this refactor:

- public headers under `include/omegaWTK/UI` describe API rather than engine internals
- `WidgetTreeHost` becomes a true internal type
- `ViewResizeCoordinator` and `LayoutBehavior` remain public as requested
- the largest UI implementation files are split by responsibility
- future API simplification work can happen on top of a cleaner internal structure
