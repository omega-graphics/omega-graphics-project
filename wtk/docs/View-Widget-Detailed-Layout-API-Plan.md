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
    Core::Rect availableRect {};
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
    virtual MeasureResult measureSelf(const LayoutContext & ctx);
    virtual void onLayoutResolved(const Core::Rect & finalRect);
};
```

### 5) UIView Element Layout v2
```cpp
struct UIElementLayoutSpec {
    UIElementTag tag {};
    LayoutStyle style {};
    Core::Optional<Shape> shape {};
    Core::Optional<OmegaCommon::UString> text {};
    Core::Optional<UIElementTag> textStyleTag {};
};

class UIViewLayoutV2 {
public:
    UIViewLayoutV2 & element(const UIElementLayoutSpec & spec);
    bool remove(UIElementTag tag);
    void clear();
};
```

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

## Implementation Slices

### Slice A: Core Types + Resolver Skeleton
1. Add `LayoutLength`, `LayoutStyle`, `LayoutContext`, and resolver scaffolding.
2. Add DP-to-pixel conversion path with explicit `dpiScale`.

Acceptance:
1. No behavior change when new APIs are unused.
2. Unit tests validate unit resolution and clamp behavior.

### Slice B: Widget Tree Layout Resolver
1. Integrate resolver with `Widget` hierarchy.
2. Add default behaviors for `Container`, `StackWidget`, and absolute overlay.

Acceptance:
1. Parent resize deterministically reflows children.
2. Existing clamp and trace hooks remain correct.

### Slice C: UIView Element Layout v2
1. Add `UIViewLayoutV2` and per-element `LayoutStyle`.
2. Support relative sizing (`Percent`, `Dp`) for shape/text elements.

Acceptance:
1. `UIView` element geometry can scale with parent bounds without manual recomputation.
2. Legacy `UIViewLayout` calls still render as before.

### Slice D: Layout Transition Bridge
1. Emit `LayoutDelta` on arrange commits.
2. Bridge deltas to `ViewAnimator`/`LayerAnimator` in `View`/`UIView`.

Acceptance:
1. Geometry transitions run through composition animation classes.
2. No widget-level animation handle ownership is introduced.

### Slice E: StyleSheet Rule Model
1. Add rule blocks and typed layout properties.
2. Keep current entry APIs as adapters.

Acceptance:
1. Current tests remain source-compatible.
2. New layout properties can be styled by selector.

### Slice F: Hardening and Tooling
1. Add layout diagnostics dump (`measure/arrange/commit` traces per node).
2. Add resize stress tests at multiple DPI scales.

Acceptance:
1. No regressions in live resize stability.
2. Deterministic layout across repeated runs.

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
1. Slice A
2. Slice B
3. Slice C
4. Slice D
5. Slice E
6. Slice F
