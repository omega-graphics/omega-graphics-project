# Layout API — Detailed Specification

This document defines a concrete, implementation-oriented API for layout across **Widget**, **View**, and **UIView** in OmegaWTK. It extends the high-level plan in [View-Widget-Detailed-Layout-API-Plan.md](View-Widget-Detailed-Layout-API-Plan.md) with full type definitions, method signatures, lifecycle, and per-class integration.

---

## 1. Namespace and Headers

| Component | Header | Namespace |
|-----------|--------|-----------|
| Core layout types | `omegaWTK/UI/Layout.h` | `OmegaWTK` |
| Widget layout API | `omegaWTK/UI/Widget.h` | `OmegaWTK` |
| View layout API | `omegaWTK/UI/View.h` | `OmegaWTK` |
| UIView layout API | `omegaWTK/UI/UIView.h` | `OmegaWTK` |

All layout types live in `OmegaWTK` and use existing project types: `Composition::Rect`, `Core::Optional`, `SharedHandle`, `OmegaCommon::Vector`, `OmegaCommon::Map`, `OmegaCommon::String`, `OmegaCommon::UString`.

---

## 2. Core Types (`Layout.h`)

### 2.1 Units and lengths

```cpp
namespace OmegaWTK {

enum class LayoutUnit : std::uint8_t {
    Auto,      // resolve from content or parent
    Px,        // physical pixels (legacy interop)
    Dp,        // device-independent units
    Percent,   // 0..1 relative to parent content axis
    Fr,        // flexible fraction (grid/flex)
    Intrinsic  // content-measured
};

struct OMEGAWTK_EXPORT LayoutLength {
    LayoutUnit unit = LayoutUnit::Auto;
    float value = 0.f;

    static LayoutLength Auto();
    static LayoutLength Px(float v);
    static LayoutLength Dp(float v);
    static LayoutLength Percent(float v);  // 0.f .. 1.f
    static LayoutLength Fr(float v);
    static LayoutLength Intrinsic();

    bool isAuto() const;
    bool isIntrinsic() const;
    bool isFixed() const;  // Px or Dp
};

struct OMEGAWTK_EXPORT LayoutEdges {
    LayoutLength left;
    LayoutLength top;
    LayoutLength right;
    LayoutLength bottom;

    static LayoutEdges Zero();
    static LayoutEdges All(LayoutLength value);
    static LayoutEdges Symmetric(LayoutLength horizontal, LayoutLength vertical);
};

struct OMEGAWTK_EXPORT LayoutClamp {
    LayoutLength minWidth  = LayoutLength::Dp(1.f);
    LayoutLength minHeight = LayoutLength::Dp(1.f);
    LayoutLength maxWidth  = LayoutLength::Auto();
    LayoutLength maxHeight = LayoutLength::Auto();
};

} // namespace OmegaWTK
```

### 2.2 Display and alignment

```cpp
enum class LayoutDisplay : std::uint8_t {
    Stack,   // block flow (vertical by default)
    Flex,    // flexbox (direction + wrap)
    Grid,    // grid (columns/rows)
    Overlay, // absolute children over content
    Custom   // LayoutBehavior-driven
};

enum class LayoutPositionMode : std::uint8_t {
    Flow,     // in flow
    Absolute  // out of flow, positioned by insets
};

enum class LayoutAlign : std::uint8_t {
    Start, Center, End, Stretch, Baseline
};

enum class FlexDirection : std::uint8_t {
    Row, RowReverse, Column, ColumnReverse
};

enum class FlexWrap : std::uint8_t {
    NoWrap, Wrap, WrapReverse
};
```

### 2.3 Layout style (per-node)

```cpp
struct OMEGAWTK_EXPORT LayoutStyle {
    LayoutDisplay display       = LayoutDisplay::Stack;
    LayoutPositionMode position = LayoutPositionMode::Flow;

    LayoutLength width  = LayoutLength::Auto();
    LayoutLength height = LayoutLength::Auto();
    LayoutClamp clamp {};

    LayoutEdges margin  = LayoutEdges::Zero();
    LayoutEdges padding = LayoutEdges::Zero();

    // Absolute positioning (when position == Absolute)
    LayoutLength insetLeft   = LayoutLength::Auto();
    LayoutLength insetTop    = LayoutLength::Auto();
    LayoutLength insetRight  = LayoutLength::Auto();
    LayoutLength insetBottom = LayoutLength::Auto();

    LayoutLength gap = LayoutLength::Dp(0.f);
    LayoutAlign alignSelfMain  = LayoutAlign::Start;
    LayoutAlign alignSelfCross = LayoutAlign::Start;

    float flexGrow   = 0.f;
    float flexShrink = 1.f;
    Core::Optional<float> aspectRatio {};

    // Flex-specific (when parent display == Flex)
    FlexDirection flexDirection = FlexDirection::Column;
    FlexWrap flexWrap = FlexWrap::NoWrap;
    LayoutAlign justifyContent = LayoutAlign::Start;
    LayoutAlign alignItems     = LayoutAlign::Start;
};
```

### 2.4 Layout context and measure result

```cpp
struct OMEGAWTK_EXPORT LayoutContext {
    Composition::Rect availableRectPx {};  // host/content bounds in physical pixels
    float dpiScale = 1.f;          // px per dp (e.g. 1.f, 1.5f, 2.f)
    std::uint64_t resizeSessionId = 0;
    bool liveResize = false;

    /// Convert a dp value to px using dpiScale.
    float dpToPx(float dp) const;
    /// Convert available rect from px to dp for layout math.
    Composition::Rect availableRectDp() const;
};

struct OMEGAWTK_EXPORT MeasureResult {
    float measuredWidthDp  = 1.f;
    float measuredHeightDp = 1.f;
};
```

### 2.5 Layout node and behavior (abstract)

```cpp
class LayoutNode;

class OMEGAWTK_EXPORT LayoutBehavior {
public:
    virtual ~LayoutBehavior() = default;
    virtual MeasureResult measure(LayoutNode & node, const LayoutContext & ctx) = 0;
    virtual void arrange(LayoutNode & node, const LayoutContext & ctx) = 0;
};

using LayoutBehaviorPtr = SharedHandle<LayoutBehavior>;
```

---

## 3. Widget layout API

Widgets own layout *policy*; they do not own animation handles. Geometry is resolved in dp, then converted to px and committed to the root View.

### 3.1 Public API

```cpp
// In Widget (omegaWTK/UI/Widget.h)

class Widget : public Native::NativeThemeObserver {
public:
    // --- Layout (new) ---
    void setLayoutStyle(const LayoutStyle & style);
    const LayoutStyle & layoutStyle() const;
    void setLayoutBehavior(LayoutBehaviorPtr behavior);  // optional; null = use default/legacy
    LayoutBehaviorPtr layoutBehavior() const;
    void requestLayout();

    // --- Existing geometry (unchanged) ---
    Composition::Rect & rect();
    void setRect(const Composition::Rect & newRect);
    bool requestRect(const Composition::Rect & requested,
                     GeometryChangeReason reason = GeometryChangeReason::ChildRequest);
    // ...
protected:
    /// Return intrinsic size in dp. Default uses root View rect and content.
    virtual MeasureResult measureSelf(const LayoutContext & ctx);
    /// Invoked after layout resolve with final rect in px (after clamp/delegate).
    virtual void onLayoutResolved(const Composition::Rect & finalRectPx);

    // Existing hooks unchanged:
    virtual Composition::Rect clampChildRect(const Widget & child, const GeometryProposal & proposal) const;
    virtual void onChildRectCommitted(const Widget & child,
                                      const Composition::Rect & oldRect,
                                      const Composition::Rect & newRect,
                                      GeometryChangeReason reason);
};
```

### 3.2 Default behavior

- If `layoutStyle()` has never been set and `layoutBehavior()` is null: layout is performed by the **legacy path** (existing `ViewResizeCoordinator` + root View rect).
- If `layoutStyle()` is set but `layoutBehavior()` is null: a built-in **default LayoutBehavior** is used (e.g. stack/fill semantics consistent with current coordinator).
- If `layoutBehavior()` is non-null: that behavior is used for this widget’s subtree; it receives a `LayoutNode` that wraps this widget and its child widgets/views.

### 3.3 Layout lifecycle (widget tree)

1. Host or parent calls **layout resolver** with `LayoutContext` (bounds in px, `dpiScale`, `resizeSessionId`).
2. Resolver converts `availableRectPx` → dp, then for root widget:
   - Calls `measureSelf(ctx)` to get intrinsic size (or uses `layoutStyle()` width/height).
   - Calls `LayoutBehavior::measure` for children (or legacy coordinator).
   - Calls `LayoutBehavior::arrange` (or legacy `ViewResizeCoordinator::resolve`) to get final dp rects.
3. For each widget, resolver converts final rect dp → px, then:
   - Optionally calls `WidgetGeometryDelegate::clampChildRect` for the child.
   - Calls `widget->onLayoutResolved(finalRectPx)`.
4. Widget’s `onLayoutResolved` updates `rect()` and the root **View**’s rect (and thus native/layer geometry).

---

## 4. View layout API

Views are the composition and native layer; they do not own layout policy but receive resolved rects and may participate in layout *nodes* when the resolver is view-centric.

### 4.1 Public API

```cpp
// In View (omegaWTK/UI/View.h)

using LayoutNodeId = std::uint64_t;
constexpr LayoutNodeId kInvalidLayoutNodeId = 0;

class View : public Native::NativeEventEmitter {
public:
    // --- Layout (new) ---
    LayoutNodeId createLayoutNode(const LayoutStyle & style);
    bool setLayoutStyle(LayoutNodeId id, const LayoutStyle & style);
    bool setLayoutBehavior(LayoutNodeId id, LayoutBehaviorPtr behavior);
    void requestLayout();
    /// Called by layout resolver or host; context.availableRectPx is this view's content area in px.
    void resolveLayout(const LayoutContext & context);

    // --- Existing (unchanged) ---
    Composition::Rect & getRect();
    virtual void resize(Composition::Rect newRect);
    ViewResizeCoordinator & getResizeCoordinator();
    const ViewResizeCoordinator & getResizeCoordinator() const;
    // ...
};
```

### 4.2 Semantics

- `createLayoutNode(style)` allocates a layout node tied to this View, returns a stable id. The View’s rect is updated when that node is arranged.
- `resolveLayout(context)` runs the layout pass for this View’s subtree of layout nodes (and/or legacy `ViewResizeCoordinator`). Typically invoked by the owning Widget or host after setting `LayoutContext::availableRectPx` to the View’s current content rect.
- When layout is resolved, final rects are in px; the View applies them via `resize(finalRect)` (or via animation bridge; see Transition API below).
- **Legacy:** If no layout nodes are created and no new API is used, `ViewResizeCoordinator` and `resize()` behave exactly as today.

---

## 5. UIView layout API

UIView owns a flat list of elements (text, shape) and their styles. The new API adds a layout model so element geometry can be expressed in dp, percent, or intrinsic, and resolved from the UIView’s bounds.

### 5.1 Element spec and layout v2

```cpp
// In UIView.h (or omegaWTK/UI/UIViewLayout.h)

struct OMEGAWTK_EXPORT UIElementLayoutSpec {
    UIElementTag tag {};
    LayoutStyle style {};
    Core::Optional<Shape> shape {};
    Core::Optional<OmegaCommon::UString> text {};
    Core::Optional<UIElementTag> textStyleTag {};
    int zIndex = 0;  // stacking; same zIndex ties broken by insertion order
};

class OMEGAWTK_EXPORT UIViewLayoutV2 {
public:
    UIViewLayoutV2 & element(const UIElementLayoutSpec & spec);
    bool remove(UIElementTag tag);
    void clear();
    const OmegaCommon::Vector<UIElementLayoutSpec> & elements() const;
    bool hasElement(UIElementTag tag) const;

private:
    OmegaCommon::Vector<UIElementLayoutSpec> elements_;
};
```

### 5.2 UIView public layout API

```cpp
// Add to class UIView

class UIView : public CanvasView, UIRenderer {
public:
    // --- Existing ---
    UIViewLayout & layout();
    void setLayout(const UIViewLayout & layout);

    // --- Layout v2 (new) ---
    UIViewLayoutV2 & layoutV2();
    void setLayoutV2(const UIViewLayoutV2 & layout);
    /// When true, layout and geometry use LayoutV2; otherwise legacy UIViewLayout.
    bool useLayoutV2() const;
    void setUseLayoutV2(bool use);

    void update();  // existing; drives layout resolve + paint
    // ...
};
```

### 5.3 Resolve order

- During `update()`, if `useLayoutV2()` is true:
  1. Build layout tree from `layoutV2().elements()` (and optional root layout style).
  2. `LayoutContext::availableRectPx` = this UIView’s getRect() (content area).
  3. Resolve measure/arrange in dp; convert to px.
  4. Update each element’s layer geometry and dirty state via existing `markElementDirty` / `syncElementDirtyState` / render target store.
- When `useLayoutV2()` is false, behavior is unchanged from current `UIViewLayout` + `elements()`.

---

## 6. Layout transition (animation bridge)

Layout commits can produce deltas that are turned into composition animations. Widgets do not hold animation handles; View/UIView do.

### 6.1 Types

```cpp
enum class LayoutTransitionProperty : std::uint8_t {
    X, Y, Width, Height,
    Opacity, CornerRadius, Shadow, Blur
};

struct OMEGAWTK_EXPORT LayoutTransitionSpec {
    bool enabled = false;
    float durationSec = 0.f;
    SharedHandle<Composition::AnimationCurve> curve = nullptr;
    OmegaCommon::Vector<LayoutTransitionProperty> properties;
};

struct OMEGAWTK_EXPORT LayoutDelta {
    Composition::Rect fromRectPx {};
    Composition::Rect toRectPx {};
    OmegaCommon::Vector<LayoutTransitionProperty> changedProperties;
};
```

### 6.2 Contract

- When the layout resolver commits a new rect for a View or UIView element, it may emit a `LayoutDelta` (from previous rect to new rect).
- View/UIView code maps `LayoutDelta` to `ViewAnimator` / `LayerAnimator` tracks (e.g. position/size over `durationSec` with `curve`).
- `LayoutTransitionSpec` can be attached to a layout node or to a StyleSheet rule so that layout-driven changes are animated; the spec is read by the view layer when applying the delta.

---

## 7. StyleSheet layout extension

StyleSheet continues to key by `UIViewTag` / `UIElementTag`. New methods populate layout-related properties that are merged into the effective `LayoutStyle` for that tag.

### 7.1 New StyleSheet methods (layout)

```cpp
// Add to struct StyleSheet

StyleSheetPtr layoutWidth(UIElementTag elementTag, LayoutLength width);
StyleSheetPtr layoutHeight(UIElementTag elementTag, LayoutLength height);
StyleSheetPtr layoutSize(UIElementTag elementTag, LayoutLength width, LayoutLength height);
StyleSheetPtr layoutMargin(UIElementTag elementTag, LayoutEdges margin);
StyleSheetPtr layoutPadding(UIElementTag elementTag, LayoutEdges padding);
StyleSheetPtr layoutClamp(UIElementTag elementTag, LayoutClamp clamp);
StyleSheetPtr layoutTransition(UIElementTag elementTag, LayoutTransitionSpec spec);
```

- These append (or update) internal entries that are interpreted as layout property overrides for the given tag.
- When resolving layout for a UIView element, the effective `LayoutStyle` is the merge of the element’s own `UIElementLayoutSpec::style` and any layout properties from the StyleSheet for that tag (later entry wins for same property, or use specificity when rule model is introduced).

---

## 8. Built-in layout behaviors

The following behaviors are provided so that containers do not need custom code for common patterns.

| Behavior | Description | Used when |
|----------|-------------|-----------|
| `StackLayoutBehavior` | Block flow (vertical/horizontal), gap, alignment | `LayoutDisplay::Stack` |
| `FlexLayoutBehavior` | Flexbox (direction, wrap, grow/shrink) | `LayoutDisplay::Flex` |
| `OverlayLayoutBehavior` | Children positioned by insets over content | `LayoutDisplay::Overlay` |
| `LegacyResizeCoordinatorBehavior` | Wraps existing `ViewResizeCoordinator` | Default for widgets with no explicit behavior |

- `StackLayoutBehavior` and `FlexLayoutBehavior` respect `LayoutStyle::gap`, `alignSelf*`, `flexGrow`, `flexShrink`, and `LayoutClamp`.
- `OverlayLayoutBehavior` uses `insetLeft/Top/Right/Bottom` (and width/height) to place children in px after converting from dp.

---

## 9. DP ↔ PX conversion

- **Input to resolver:** `LayoutContext::availableRectPx` and `dpiScale`. Layout math is done in dp; `availableRectDp()` = `availableRectPx` scaled by `1/dpiScale` per axis.
- **Output of resolver:** Rects in dp. Conversion to px: `rectPx = rectDp * dpiScale` (per position and size component).
- **Single conversion:** Each committed rect is converted once to px and then passed to `Widget::onLayoutResolved`, `View::resize`, or UIView element layer geometry.
- **Legacy:** When `dpiScale == 1.f`, behavior matches current px-only code.

---

## 10. Summary table

| Actor | Sets layout policy | Receives resolved geometry | Runs animations |
|-------|--------------------|-----------------------------|------------------|
| **Widget** | `setLayoutStyle`, `setLayoutBehavior` | `onLayoutResolved(finalRectPx)` | No |
| **View** | `createLayoutNode`, `setLayoutStyle`, `setLayoutBehavior` | `resolveLayout` → internal arrange → `resize(...)` | Yes (ViewAnimator) |
| **UIView** | `layoutV2()` element specs + StyleSheet layout APIs | `update()` → resolve → element layer rects | Yes (LayerAnimator) |

This detailed spec is intended to be implemented slice-by-slice as defined in [View-Widget-Detailed-Layout-API-Plan.md](View-Widget-Detailed-Layout-API-Plan.md)#implementation-slices, with backward compatibility preserved at each step.

---

## 11. Implementation Slices (map to plan)

| Slice | Plan sub-slices | Spec sections |
|-------|-----------------|----------------|
| **A** | A1, A2, A3 | §2 Core types; §2.4 LayoutContext, MeasureResult; §2.5 LayoutBehavior; §9 DP↔PX |
| **B** | B1, B2, B3 | §3 Widget API; §8 LegacyResizeCoordinatorBehavior, StackLayoutBehavior |
| **C** | C1, C2 | §5 UIView layout API; §5.3 Resolve order |
| **D** | D1, D2 | §6 Layout transition |
| **E** | E1, E2 | §7 StyleSheet layout extension |
| **F** | F1, F2 | Tooling and tests (see plan Testing Matrix) |

**Recommended order:** A1 → A2 → A3 → B1 → B2 → B3 → C1 → C2 → D1 → D2 → E1 → E2 → F1 → F2. D can start after B/C have layout commits; E can run in parallel with D or after C.
