# View/Widget Detailed Layout API Plan

## Goal
Define a richer, unified layout API for `View`, `Widget`, and `UIView` content that supports:

1. Full geometry customization.
2. Deterministic live resizing.
3. DPI-aware scaling.
4. Animation/transition of layout and visual properties.
5. Backward compatibility with current `UIViewLayout` and `StyleSheet`.

## Current Baseline (From Existing API)
The current codebase already has strong building blocks:

1. `Widget` geometry ownership:
   - `setRect(...)`, `requestRect(...)`, `clampChildRect(...)`, `onChildRectCommitted(...)`.
2. `ViewResizeCoordinator`:
   - policy set: `Fixed`, `Fill`, `FitContent`, `Proportional`.
   - clamp support via `ChildResizeSpec`.
3. `UIViewLayout`:
   - flat element list (`Text`, `Shape`) with optional absolute `textRect`.
4. `StyleSheet`:
   - entry list keyed by `viewTag`/`elementTag`.
   - background/border/text/effects plus animation entries.

This baseline is functional, but it is not yet a complete layout system.

## Gaps To Address
1. No first-class length units (`dp`, `%`, `fr`, intrinsic, auto).
2. No unified measure/arrange lifecycle across `Widget` children and `UIView` elements.
3. Layout semantics are split across several classes and widget implementations.
4. No direct API for custom layout algorithms on a per-node basis.
5. Animation is available but not exposed as a unified layout-transition model.
6. DPI scaling behavior is implicit rather than an explicit layout input.

## Design Principles
1. Keep ownership boundaries:
   - `Widget` and container classes own layout policy.
   - `View`/`UIView` own animation playback via composition animators.
2. One layout model for both widget trees and `UIView` element trees.
3. Deterministic output for the same input state, including live resize sessions.
4. Strict compatibility path from existing APIs to the new API.

## Legacy Mapping Overview

This layout model is an evolution of the current API, not a replacement. All existing behavior has a defined mapping:

1. `ViewResizeCoordinator` and `ChildResizeSpec`:
   - `ChildResizePolicy::Fill` → `LayoutDisplay::Stack` / `Flex` with:
     - `width = LayoutLength::Percent(1.0f)`
     - `height = LayoutLength::Percent(1.0f)`
     - `flexGrow > 0` for flexible children.
   - `ChildResizePolicy::Proportional` → `LayoutDisplay::Stack` / `Flex` with:
     - `width`/`height` in `Percent` derived from `baselineChildRect / baselineParentRect`.
     - optional `aspectRatio` to preserve shape semantics.
   - `ChildResizePolicy::Fixed` / `FitContent` → `LayoutLength::Intrinsic()` plus clamps from `ResizeClamp`.
2. `ResizeClamp` → `LayoutClamp`:
   - `minWidth`/`minHeight` map directly to `LayoutClamp::minWidth/minHeight` using `LayoutLength::Dp(...)`.
   - `maxWidth`/`maxHeight` map to `LayoutClamp::maxWidth/maxHeight` using `LayoutLength::Dp(...)` or `LayoutLength::Auto()` for infinity.
3. `UIViewLayout` → `UIViewLayoutV2`:
   - Each `UIViewLayout::Element` becomes a `UIElementLayoutSpec`:
     - `tag` unchanged.
     - `type == Text` → `text` set; `textRect` (if present) → `style.width/height` in `Px` and insets.
     - `type == Shape` → `shape` set; `shape.rect` → `style.width/height` in `Px`.
   - When no explicit `LayoutStyle` is provided for an element, the adapter creates an equivalent `Px`-sized style that matches current rendering.
4. `StyleSheet::Entry`:
   - Existing APIs (`backgroundColor`, `elementBrush`, `textColor`, etc.) continue to append entry records.
   - Internally, those entries are interpreted as style rule declarations that set visual and (new) layout properties on matching nodes.
   - Evaluation order and effective values must match current behavior when no new layout properties are used.

## Proposed API Surface

### 1) Core Units and Constraints
```cpp
enum class LayoutUnit : uint8_t {
    Auto,
    Px,         // physical pixels (legacy interop)
    Dp,         // device-independent units
    Percent,    // relative to parent content axis
    Fr,         // flexible fraction (grid/flex)
    Intrinsic   // content-measured
};

struct LayoutLength {
    LayoutUnit unit = LayoutUnit::Auto;
    float value = 0.f;

    static LayoutLength Auto();
    static LayoutLength Px(float v);
    static LayoutLength Dp(float v);
    static LayoutLength Percent(float v); // 0..1
    static LayoutLength Fr(float v);
    static LayoutLength Intrinsic();
};

struct LayoutEdges {
    LayoutLength left, top, right, bottom;
    static LayoutEdges Zero();
};

struct LayoutClamp {
    LayoutLength minWidth = LayoutLength::Dp(1.f);
    LayoutLength minHeight = LayoutLength::Dp(1.f);
    LayoutLength maxWidth = LayoutLength::Auto();
    LayoutLength maxHeight = LayoutLength::Auto();
};
```

### 2) Node Style and Positioning
```cpp
enum class LayoutDisplay : uint8_t {
    Stack,
    Flex,
    Grid,
    Overlay,
    Custom
};

enum class LayoutPositionMode : uint8_t {
    Flow,
    Absolute
};

enum class LayoutAlign : uint8_t {
    Start,
    Center,
    End,
    Stretch,
    Baseline
};

struct LayoutStyle {
    LayoutDisplay display = LayoutDisplay::Stack;
    LayoutPositionMode position = LayoutPositionMode::Flow;

    LayoutLength width = LayoutLength::Auto();
    LayoutLength height = LayoutLength::Auto();
    LayoutClamp clamp {};

    LayoutEdges margin = LayoutEdges::Zero();
    LayoutEdges padding = LayoutEdges::Zero();

    LayoutLength insetLeft = LayoutLength::Auto();
    LayoutLength insetTop = LayoutLength::Auto();
    LayoutLength insetRight = LayoutLength::Auto();
    LayoutLength insetBottom = LayoutLength::Auto();

    LayoutLength gap = LayoutLength::Dp(0.f);
    LayoutAlign alignSelfMain = LayoutAlign::Start;
    LayoutAlign alignSelfCross = LayoutAlign::Start;

    float flexGrow = 0.f;
    float flexShrink = 1.f;
    Core::Optional<float> aspectRatio {};
};
```

### 3) Layout Context and Passes
```cpp
struct LayoutContext {
    Composition::Rect availableRectPx {}; // host bounds in physical pixels
    float dpiScale = 1.f;
    std::uint64_t resizeSessionId = 0;
    bool liveResize = false;
};

struct MeasureResult {
    float measuredWidthDp = 1.f;
    float measuredHeightDp = 1.f;
};

class LayoutNode;

class LayoutBehavior {
public:
    virtual ~LayoutBehavior() = default;
    virtual MeasureResult measure(LayoutNode & node,const LayoutContext & ctx) = 0;
    virtual void arrange(LayoutNode & node,const LayoutContext & ctx) = 0;
};
```

### DP ↔ PX Conversion Boundary

- All layout math (measure/arrange) is defined in device-independent units (dp).
- Host inputs (`availableRectPx`) and final committed geometry are in px.
- The resolver performs:
  1. Convert `availableRectPx` → dp using `dpiScale`.
  2. Measure/arrange in dp.
  3. Convert resulting dp rects → px once, on commit, and pass them to:
     - `Widget::setRect` / `Widget::requestRect`
     - `View::resize`
     - `UIView` element layer geometry.

Acceptance:

1. For `dpiScale == 1.0`, results match legacy px behavior.
2. For other scales, layout is stable in dp and visually scaled in px.

### 4) View/Widget Integration API
```cpp
using LayoutNodeId = std::uint64_t;

class View {
public:
    LayoutNodeId createLayoutNode(const LayoutStyle & style);
    bool setLayoutStyle(LayoutNodeId id,const LayoutStyle & style);
    bool setLayoutBehavior(LayoutNodeId id,SharedHandle<LayoutBehavior> behavior);
    void requestLayout();
    void resolveLayout(const LayoutContext & context);
};

class Widget {
public:
    void setLayoutStyle(const LayoutStyle & style);
    const LayoutStyle & layoutStyle() const;
    void requestLayout();

protected:
    /// Default: use the root View's rect and intrinsic content.
    virtual MeasureResult measureSelf(const LayoutContext & ctx);
    /// Final px rect after clamp/delegate.
    virtual void onLayoutResolved(const Composition::Rect & finalRectPx);
};
```

### LayoutBehavior and Legacy Geometry Hooks

1. `ViewResizeCoordinator` remains the default implementation for legacy widgets:
   - A built-in `LayoutBehavior` (e.g. `LegacyResizeCoordinatorBehavior`) wraps `ViewResizeCoordinator` semantics.
   - When a `Widget` has no explicit `LayoutStyle`/`LayoutBehavior`, layout is delegated to this legacy behavior and behaves exactly as today.
2. `WidgetGeometryDelegate` acts as a post-layout clamp/notification hook:
   - After `LayoutBehavior::arrange(...)` produces final dp rectangles (and they are converted to px), the widget may:
     - Call `WidgetGeometryDelegate::clampChildRect(...)` as a final safety clamp.
     - Call `WidgetGeometryDelegate::onChildRectCommitted(...)` with old/new rects and a reason derived from the layout pass.
3. Custom behaviors:
   - Widgets can supply their own `LayoutBehavior` to implement grid/flex/overlay semantics for their children.
   - These behaviors run before any delegate clamp and must respect `LayoutClamp` and unit resolution rules.

### 5) UIView Element Layout v2
```cpp
struct UIElementLayoutSpec {
    UIElementTag tag {};
    LayoutStyle style {};
    Core::Optional<Shape> shape {};
    Core::Optional<OmegaCommon::UString> text {};
    Core::Optional<UIElementTag> textStyleTag {};
    int zIndex = 0; // stable stacking key; defaults to 0
};

class UIViewLayoutV2 {
public:
    UIViewLayoutV2 & element(const UIElementLayoutSpec & spec);
    bool remove(UIElementTag tag);
    void clear();
};
```

### UIViewLayoutV2 Ordering and Dirty State

- Element stacking order:
  - Primary key: `zIndex` (ascending).
  - Secondary key: insertion order for ties, preserving current `activeTagOrder` semantics.
- Dirty tracking:
  - Changes to an element’s `style`, `shape`, `text`, or `zIndex` mark the corresponding `ElementDirtyState` fields.
  - `UIViewLayoutV2` updates synchronize into existing `UIView` dirty maps via:
    - `markElementDirty(...)`
    - `syncElementDirtyState(...)`
    - `prepareElementAnimations(...)` and `prepareEffectAnimations(...)`.

Compatibility:

1. When `UIViewLayoutV2` is unused, `UIViewLayout` + `StyleSheet` behavior is unchanged.
2. When both are present for the same tag, `UIViewLayoutV2` (new layout model) defines geometry, but existing style entries still apply visual properties.

## Resizing Contract
1. Host resize begins a layout epoch with `resizeSessionId`, `dpiScale`, and bounds.
2. Root widget resolves layout via measure/arrange in device-independent units.
3. Final committed rectangles are clamped and converted once to render-space geometry.
4. During active resize, stale geometry epochs can be skipped, but monotonicity is preserved.
5. Existing `ViewResizeCoordinator` remains as a compatibility bridge until all views use the new layout resolver.

## Animation Contract (Layout + Visual)
1. Widgets declare transition intent; views execute transitions.
2. No `Composition::AnimationHandle` ownership in `Widget` classes.
3. Layout commits emit a `LayoutDelta` (`fromRect`, `toRect`, property changes).
4. `View`/`UIView` map deltas to `ViewAnimator`/`LayerAnimator` tracks.
5. Under resize pressure, animation sampling follows lane pacing and stale-step skipping rules already used by the sync engine.

### Proposed Transition Types
```cpp
enum class LayoutTransitionProperty : uint8_t {
    X, Y, Width, Height, Opacity, CornerRadius, Shadow, Blur
};

struct LayoutTransitionSpec {
    bool enabled = false;
    float durationSec = 0.f;
    SharedHandle<Composition::AnimationCurve> curve = nullptr;
    OmegaCommon::Vector<LayoutTransitionProperty> properties;
};
```

## StyleSheet Evolution
Keep `StyleSheet` but extend toward rules instead of only entry accumulation:

1. Add explicit rule blocks with selector specificity and property groups.
2. Separate static style values from transition declarations.
3. Add layout properties (`width`, `height`, `margin`, `padding`, `gap`, `alignment`) using `LayoutLength`.
4. Keep current methods (`backgroundColor`, `elementBrush`, `textColor`, etc.) as adapters into rule-based internals.

### StyleSheet Rule Model

1. Rule structure:
   - A `Rule` consists of:
     - `selector` (by `UIViewTag` / `UIElementTag`, with optional simple combinators later).
     - `specificity` (integer; higher wins on conflicts).
     - `properties` split into:
       - Visual properties (background, border, effects, text).
       - Layout properties (`width`, `height`, `margin`, `padding`, `gap`, `alignment`, `clamp`).
2. Existing `StyleSheet::Entry` as rule adapters:
   - Each call like `backgroundColor(viewTag, color, ...)` produces or updates an internal `Rule`:
     - `selector` matches that `viewTag` or element tag.
     - `specificity` and application order are chosen so that the resulting effective style for old code matches today’s behavior.
3. Layout properties:
   - New APIs (added over time) expose strongly typed layout setters:
     - `layoutWidth(UIElementTag elementTag, LayoutLength width);`
     - `layoutHeight(...)`, `layoutMargin(...)`, `layoutPadding(...)`, etc.
   - These APIs populate the `LayoutStyle` of corresponding layout nodes and compose with visual rules.

Acceptance:

1. For code that only uses legacy visual APIs, computed visual styles match existing builds.
2. Layout properties can be applied via selectors without breaking legacy visual-only styles.

## Compatibility and Migration

### Adapter Strategy
1. `UIViewLayout` maps to `UIViewLayoutV2` using absolute `Px` sizing by default.
2. Existing shape/text APIs remain source-compatible.
3. Existing `StyleSheet` entry APIs remain source-compatible and compile unchanged.
4. Existing `Container`/`StackWidget` behavior maps into first-party `LayoutBehavior` implementations.

### Compatibility Rules
1. If no new layout style is set, behavior stays identical to current code.
2. If both old and new layout APIs are present, new API wins per-node, old API remains fallback.
3. Legacy tests (`BasicAppTest`, `TextCompositorTest`, `EllipsePathCompositorTest`) must continue to pass unchanged.

### Example: Migrating a Simple Container

Legacy pattern:

- A `ContainerWidget` owns a root `View` and uses `ViewResizeCoordinator`:
  - Children are registered with `ChildResizeSpec` (`Fill` / `Proportional`).
  - On host resize, the container calls `resizeCoordinator.resolve(parentContentRect)`.

New pattern:

1. Container declares a `LayoutStyle`:
   - `display = LayoutDisplay::Stack`.
   - `width = LayoutLength::Percent(1.0f)`.
   - `height = LayoutLength::Percent(1.0f)`.
2. Each child widget receives:
   - `LayoutStyle` with `width`/`height` from either `Percent` or `Intrinsic` and appropriate `LayoutClamp`.
3. Layout resolver:
   - Calls `measureSelf(...)` on each widget in dp.
   - Calls `arrange(...)` with resolved dp rects.
   - Converts to px and calls `Widget::onLayoutResolved(finalRectPx)` which internally updates the root `View` rect and native items.
4. Until the migration is complete:
   - `ContainerWidget` can continue to set `ChildResizeSpec` for children.
   - An adapter maps those specs into `LayoutStyle` for the new resolver, or falls back to the legacy `ViewResizeCoordinator` behavior when no explicit `LayoutStyle` is set.

## Implementation Slices

Slices are ordered by dependency; each slice leaves the tree in a shippable state and preserves legacy behavior when new APIs are unused. See [Layout-API-Detailed-Spec.md](Layout-API-Detailed-Spec.md) for concrete types and method signatures.

### Slice dependency overview

```
A1 → A2 → A3
 ↓    ↓    ↓
 B1 → B2 → B3
  ↓    ↓    ↓
  C1 → C2
   ↓    ↓
   D1 → D2
    ↓    ↓
    E1 → E2
     ↓    ↓
     F1 → F2
```

---

### Slice A: Core Types + Resolver Skeleton

**Depends on:** none.

| Sub-slice | Scope | Deliverables |
|-----------|--------|--------------|
| **A1** | Core types only | New header `omegaWTK/UI/Layout.h`. `LayoutUnit`, `LayoutLength`, `LayoutEdges`, `LayoutClamp` with static constructors and optional helpers (`isAuto`, `isIntrinsic`, `isFixed`). No changes to Widget/View/UIView. |
| **A2** | Context + style + measure | In `Layout.h`: `LayoutDisplay`, `LayoutAlign`, `LayoutPositionMode`, `FlexDirection`, `FlexWrap`; `LayoutStyle`; `LayoutContext` (`availableRectPx`, `dpiScale`, `resizeSessionId`, `liveResize`) with `dpToPx()` and `availableRectDp()`; `MeasureResult`. |
| **A3** | Resolver scaffolding + DP/px | `LayoutBehavior` abstract class and `LayoutBehaviorPtr`. Minimal resolver (e.g. `LayoutResolver` or free functions) that: takes `LayoutContext`, resolves `LayoutLength` to float dp for a given axis and available size, applies `LayoutClamp`. Unit tests for unit resolution and clamp behavior only; no Widget/View integration yet. |

**Acceptance (full Slice A):**
1. No behavior change when new APIs are unused (no call sites yet).
2. Unit tests validate unit resolution (Dp, Px, Percent, Fr, Intrinsic) and clamp behavior.
3. `dpiScale` is applied correctly in `availableRectDp()` and in any resolver conversion.

---

### Slice B: Widget Tree Layout Resolver

**Depends on:** Slice A.

| Sub-slice | Scope | Deliverables |
|-----------|--------|--------------|
| **B1** | Widget layout API surface | In `Widget.h`/`.cpp`: `setLayoutStyle`, `layoutStyle`, `setLayoutBehavior`, `layoutBehavior`, `requestLayout`; protected `measureSelf(LayoutContext)`, `onLayoutResolved(Composition::Rect finalRectPx)`. Default `measureSelf` uses current root View rect (no new layout run yet). Storage for `LayoutStyle` and optional `LayoutBehaviorPtr`; when both unset, all existing code paths unchanged. |
| **B2** | Legacy behavior wrapper | `LegacyResizeCoordinatorBehavior`: implements `LayoutBehavior` by delegating to existing `ViewResizeCoordinator` (register child, resolve, clamp). When a Widget has no explicit behavior, use this so one code path can “run layout” for the widget subtree. Wire host/parent resize to call into this path when no new layout style is set. |
| **B3** | Widget layout driver + default behavior | Driver that: given root Widget and `LayoutContext`, runs measure (widget’s `measureSelf` or behavior’s measure of children), then arrange (behavior’s arrange or legacy coordinator resolve), converts dp → px once per node, optionally calls `WidgetGeometryDelegate::clampChildRect`, then `Widget::onLayoutResolved(finalRectPx)`. Widget’s `onLayoutResolved` updates `rect()` and root View’s rect. Add `StackLayoutBehavior` (or equivalent) as first non-legacy behavior; use it when `layoutStyle().display == Stack` and behavior is null. |

**Acceptance (full Slice B):**
1. Parent resize deterministically reflows children when new layout API is used.
2. When new API is not used, behavior is identical to current (legacy path).
3. Existing `clampChildRect` and `onChildRectCommitted` (and trace hooks) remain correct.

---

### Slice C: UIView Element Layout v2

**Depends on:** Slice A, Slice B (for `LayoutContext` and `LayoutStyle`; resolver can be minimal for UIView-only).

| Sub-slice | Scope | Deliverables |
|-----------|--------|--------------|
| **C1** | UIViewLayoutV2 + element spec | New type `UIElementLayoutSpec` (tag, style, shape, text, textStyleTag, zIndex). New class `UIViewLayoutV2` with `element(spec)`, `remove(tag)`, `clear()`, `elements()`, `hasElement(tag)`. On `UIView`: `layoutV2()`, `setLayoutV2`, `useLayoutV2`, `setUseLayoutV2`. When `useLayoutV2()` is false, existing `UIViewLayout` and `update()` unchanged. |
| **C2** | Resolve + sync to rendering | In `UIView::update()`, when `useLayoutV2()` is true: build flat layout input from `layoutV2().elements()`; set `LayoutContext::availableRectPx` to UIView’s content rect; run measure/arrange (stack or single-pass) in dp; convert to px; update each element’s layer geometry and dirty state via `markElementDirty` / `syncElementDirtyState` / existing render target store. Order by `zIndex` then insertion order. Ensure legacy `UIViewLayout` path still works when `useLayoutV2()` is false. |

**Acceptance (full Slice C):**
1. With `useLayoutV2()` true, element geometry can use `Percent`, `Dp`, and `Px` and scales with parent bounds.
2. Legacy `UIViewLayout` and all existing tests (e.g. `TextCompositorTest`, `EllipsePathCompositorTest`) still pass unchanged.
3. No regressions in existing StyleSheet visual styling.

---

### Slice D: Layout Transition Bridge

**Depends on:** Slice B, Slice C (layout commits exist).

| Sub-slice | Scope | Deliverables |
|-----------|--------|--------------|
| **D1** | Transition types + delta emission | In `Layout.h` (or layout animation header): `LayoutTransitionProperty`, `LayoutTransitionSpec`, `LayoutDelta`. Where layout commits a new rect (Widget driver, UIView v2 resolve), optionally emit `LayoutDelta` (from previous rect to new rect). No animation playback yet. |
| **D2** | Bridge to ViewAnimator / LayerAnimator | In View/UIView code: when a `LayoutDelta` is received and a `LayoutTransitionSpec` is associated (e.g. per node or from StyleSheet), map delta to `ViewAnimator` or `LayerAnimator` tracks (position/size over duration with curve). No `Composition::AnimationHandle` or animation ownership in Widget. |

**Acceptance (full Slice D):**
1. Geometry transitions run through existing composition animation classes.
2. No widget-level animation handle ownership.
3. Resize + transition contention respects existing stale-step skipping / lane rules.

---

### Slice E: StyleSheet Rule Model + Layout Properties

**Depends on:** Slice A, Slice C (StyleSheet layout applies to UIView elements; can be used by Widget/View later).

| Sub-slice | Scope | Deliverables |
|-----------|--------|--------------|
| **E1** | Layout property entries | Extend `StyleSheet` with layout-related entry kinds (or new entry fields) and methods: `layoutWidth`, `layoutHeight`, `layoutSize`, `layoutMargin`, `layoutPadding`, `layoutClamp`, `layoutTransition` (all keyed by `UIElementTag`). When resolving layout for a UIView element, merge these into effective `LayoutStyle` for that tag (e.g. later entry wins per property). Keep all existing `StyleSheet` APIs unchanged and source-compatible. |
| **E2** | Rule structure (optional, incremental) | Introduce internal rule structure: selector (tag), specificity, and property groups (visual vs layout). Existing entry APIs produce rules so that effective style for legacy code matches current behavior. New layout properties can be applied via the same entry APIs and participate in merge. Document selector/specificity so future extensions are consistent. |

**Acceptance (full Slice E):**
1. All current tests and call sites remain source-compatible.
2. New layout properties can be set via StyleSheet and affect UIView element layout when using layout v2.
3. Visual-only StyleSheet usage is unchanged in behavior.

---

### Slice F: Hardening and Tooling

**Depends on:** Slice B, Slice C (layout paths exist to observe).

| Sub-slice | Scope | Deliverables |
|-----------|--------|--------------|
| **F1** | Layout diagnostics | Optional layout diagnostics: dump or callback for measure/arrange/commit per node (e.g. node id, rect in dp/px, pass type). No impact on production when disabled. |
| **F2** | Stress and DPI tests | Resize stress tests (repeated resize, multiple DPI scales 1.0, 1.25, 1.5, 2.0). Assert deterministic layout (same input → same output) and no regressions in live-resize stability. Add to testing matrix. |

**Acceptance (full Slice F):**
1. No regressions in live resize stability.
2. Deterministic layout across repeated runs at each DPI scale.
3. Diagnostics usable for debugging layout without changing production behavior.

## Testing Matrix
1. Legacy regression:
   - `BasicAppTest`
   - `TextCompositorTest`
   - `EllipsePathCompositorTest`
2. New layout tests:
   - unit tests for unit resolution (`Dp`, `Percent`, `Fr`, `Intrinsic`).
   - mixed fixed/flexible child layouts under host resize.
3. Animation tests:
   - geometry transition from one layout state to another.
   - resize + transition contention with stale-step skipping.
4. DPI tests:
   - scale factors `1.0`, `1.25`, `1.5`, `2.0`.

## Risks and Mitigations
1. Risk: behavior drift in existing tests.
   - Mitigation: adapter-first rollout, legacy-path default.
2. Risk: layout/animation ownership confusion.
   - Mitigation: keep widget API intent-only; execution remains view-owned.
3. Risk: performance regressions with larger trees.
   - Mitigation: dirty-subtree invalidation and per-epoch coalescing.

## Recommended Execution Order

Implement in this order (sub-slices within a letter can be done in one PR or split for review):

1. **Slice A:** A1 → A2 → A3  
2. **Slice B:** B1 → B2 → B3  
3. **Slice C:** C1 → C2  
4. **Slice D:** D1 → D2 (can start once B/C commit geometry)  
5. **Slice E:** E1 → E2 (can overlap with D or follow C)  
6. **Slice F:** F1 → F2  

Legacy tests (`BasicAppTest`, `TextCompositorTest`, `EllipsePathCompositorTest`) must pass after every slice.
