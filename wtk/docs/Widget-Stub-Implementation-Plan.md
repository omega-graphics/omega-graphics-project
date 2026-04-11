# Widget Stub Implementation Plan

## Goal

Implement every widget type listed in the [Widget-Type-Catalog-Proposal](Widget-Type-Catalog-Proposal.md) that is currently either missing or stubbed as a `WIDGET_CONSTRUCTOR()` placeholder. Each phase is buildable and testable independently.

Related documents:

- [Widget-Type-Catalog-Proposal.md](Widget-Type-Catalog-Proposal.md) — Full catalog and customization contract
- [UI-Engine-Roadmap.md](UI-Engine-Roadmap.md) — High-level engine roadmap
- [UI-Header-Implementation-Split-Plan.md](done/UI-Header-Implementation-Split-Plan.md) — Header hygiene (prerequisite context)

---

## Current State

| Header | Widgets | Status |
|--------|---------|--------|
| `BasicWidgets.h` | `Container` | Fully implemented (clamp policy, child geometry, wire/unwire) |
| `BasicWidgets.h` | `ScrollableContainer` | Stub (`WIDGET_CONSTRUCTOR()` only) |
| `Containers.h` | `StackWidget`, `HStack`, `VStack` | Fully implemented (flex grow/shrink, alignment, spacing) |
| `Primatives.h` | `Label`, `Icon`, `Image` | Stubs (`WIDGET_CONSTRUCTOR()` only) |
| `UserInputs.h` | `TextInput`, `Button`, `Dropdown`, `Slider` | Stubs (`WIDGET_CONSTRUCTOR()` only) |

Everything else in the catalog has no header declaration at all.

## Infrastructure Available

These are the building blocks each widget implementation will use:

- **`Widget`** — base class with paint lifecycle (`onMount`, `onPaint`, `invalidate`), geometry (`requestRect`, `setRect`, `clampChildRect`), layout style, observer pattern, `WidgetState<T>` reactive state.
- **`Container`** — child management (`wireChild`/`unwireChild`/`addChild`/`removeChild`), clamp policy, `layoutChildren()` override point.
- **`View`** / **`CanvasView`** — layer tree, canvas creation, composition sessions, `submitPaintFrame`.
- **`UIView`** — element-based composition (`UIViewLayoutV2`, `StyleSheet`, shapes, text, effects, animation). Best for widgets with discrete visual elements (backgrounds, labels, icons) that need style/theme transitions.
- **`ScrollView`** — native scroll container with delegate for scroll events.
- **`VideoView`** / **`SVGView`** — specialized media views, already implemented.
- **`ViewDelegate`** — mouse enter/exit, left/right mouse down/up, key down/up event handling.
- **`LayoutStyle`** / **`LayoutBehavior`** — flex/grid/overlay/custom display modes, measure/arrange protocol.
- **`StyleSheet`** — background, border, brush, text styling, drop shadow, blur, animation curves, layout properties.
- **`WidgetState<T>`** + **`WidgetStateObserver`** — reactive state binding with observer notification.

## Conventions For All Widget Implementations

These conventions apply to every widget built under this plan:

### 1. Backing View Strategy

| Widget category | Backing view | Rationale |
|-----------------|-------------|-----------|
| Shape primitives (`Rectangle`, `Ellipse`, etc.) | `UIView` | Shapes map directly to `UIViewLayoutV2` elements with `StyleSheet` theming. |
| Text widgets (`Label`, `TextInput`, `TextArea`) | `UIView` | Text elements use `UIView`'s text layout, font, color, and wrapping support. |
| Composite input widgets (`Button`, `Toggle`, etc.) | `UIView` via `Container` | Multi-element composition (background shape + label + icon) with stylesheet state transitions. |
| Collection widgets (`ListView`, `TableView`) | `ScrollView` child of `Container` | Scroll viewport with virtualized child widgets. |
| Media wrappers (`VideoViewWidget`, `SVGViewWidget`) | Wrap existing `VideoView`/`SVGView` | Delegate to existing specialized view implementations. |
| Custom drawing (`CanvasWidget`) | `CanvasView` | Direct canvas access for immediate-mode drawing. |

### 2. Widget Anatomy Pattern

Every non-trivial widget should follow:

```
WidgetType
  ├── Props struct          — construction-time and mutable configuration
  ├── State (WidgetState<T>) — runtime reactive state (value, pressed, focused, etc.)
  ├── Style integration     — StyleSheet for visual appearance, theme response
  ├── ViewDelegate subclass — event handling (pointer, keyboard, focus)
  ├── onMount()             — initial element setup, delegate wiring
  ├── onPaint()             — element update from state, style application
  └── Create() factory      — public construction via WIDGET_CONSTRUCTOR pattern
```

### 3. Element Tag Convention

UIView-backed widgets use string tags to identify sub-elements:

- `"bg"` — background shape
- `"border"` — border shape
- `"label"` — primary text
- `"icon"` — icon/glyph element
- `"track"` — slider/progress track
- `"thumb"` — slider thumb
- `"indicator"` — toggle/checkbox indicator
- `"fill"` — progress fill

### 4. State Model

Widgets that have user-modifiable values expose:
- A `WidgetState<T>` for the value (controlled mode: caller owns state; uncontrolled mode: widget owns state).
- A callback/observer for value-change notification.
- Interactive state tracked internally: `idle`, `hovered`, `pressed`, `focused`, `disabled`.

---

## Phase 0: Widget Infrastructure Prep [Implemented]

**Goal:** Establish shared types and utilities that multiple widget phases depend on.

### 0A. Interactive State Enum

Add to `Widget.h` or a new `WidgetTypes.h`:

```cpp
enum class InteractiveState : uint8_t {
    Idle,
    Hovered,
    Pressed,
    Focused,
    Disabled
};
```

### 0B. Base Interactive Widget Mixin

A reusable `ViewDelegate` subclass that tracks hover/press/focus state and calls back into the owning widget:

```cpp
class WidgetInteractionDelegate : public ViewDelegate {
protected:
    InteractiveState state = InteractiveState::Idle;
    Widget *owner = nullptr;

    void onMouseEnter(Native::NativeEventPtr event) override;
    void onMouseExit(Native::NativeEventPtr event) override;
    void onLeftMouseDown(Native::NativeEventPtr event) override;
    void onLeftMouseUp(Native::NativeEventPtr event) override;
    void onKeyDown(Native::NativeEventPtr event) override;
    void onKeyUp(Native::NativeEventPtr event) override;
};
```

This avoids duplicating hover/press tracking in every interactive widget.

### 0C. File Organization

New headers under `wtk/include/omegaWTK/Widgets/`:

| File | Contents |
|------|----------|
| `WidgetTypes.h` | `InteractiveState`, shared enums, forward declarations |
| `Primatives.h` | Shape primitives + `Label`, `Icon`, `Image`, `Separator` (exists, expand) |
| `UserInputs.h` | All input widgets (exists, expand) |
| `Containers.h` | Stack/Grid/ZStack/Split/Tabs layout containers (exists, expand) |
| `Collections.h` | `ListView`, `TreeView`, `TableView`, `CollectionView`, `PropertyGrid` |
| `Navigation.h` | `NavigationStack`, `Sidebar`, `Breadcrumb`, `Toolbar`, `StatusBar` |
| `Overlays.h` | `Tooltip`, `Popover`, `Menu`, `ContextMenu`, `ModalDialog`, `Toast`, `Sheet` |
| `MediaWidgets.h` | `VideoViewWidget`, `AudioPlayerWidget`, `SVGViewWidget`, `CanvasWidget` |

New source files under `wtk/src/Widgets/` mirror the headers (one `.cpp` per header, split further if a file grows large).

### 0D. Build System

Update `wtk/CMakeLists.txt`:

```cmake
file(GLOB WIDGET_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Widgets/*.cpp")
```

Confirm this glob already exists or add it. New `.cpp` files under `src/Widgets/` should be picked up automatically.

### Verification

- New headers compile with zero widget implementations (forward declarations and empty classes).
- Existing `Container`, `HStack`, `VStack` continue to build and pass tests.

---

## Phase 1: Display Primitives [Implemented]

**Goal:** Implement the shape primitives and `Separator` from catalog section C.

### Widgets

| Widget | Backing | Key behavior |
|--------|---------|-------------|
| `Rectangle` | `UIView` | Single `Shape::Rect` element, fill brush + optional stroke via `StyleSheet`. |
| `RoundedRectangle` | `UIView` | `Shape::RoundedRect` element. Props: independent corner radii. |
| `Ellipse` | `UIView` | `Shape::Ellipse` element. |
| `Path` | `UIView` | `Shape::Path` element. Props: joins, caps, stroke width, fill rule, close. |
| `Separator` | `UIView` | Thin rect element. Props: `Orientation` (horizontal/vertical), thickness, inset. |

### Props Structs

```cpp
struct RectangleProps {
    SharedHandle<Composition::Brush> fill = nullptr;
    SharedHandle<Composition::Brush> stroke = nullptr;
    float strokeWidth = 0.f;
};

struct RoundedRectangleProps {
    SharedHandle<Composition::Brush> fill = nullptr;
    SharedHandle<Composition::Brush> stroke = nullptr;
    float strokeWidth = 0.f;
    float topLeft = 0.f, topRight = 0.f, bottomLeft = 0.f, bottomRight = 0.f;
};

// Similar for Ellipse, Path, Separator.
```

### Implementation Pattern (Rectangle example)

```cpp
class Rectangle : public Widget {
    RectangleProps props_;
protected:
    void onMount() override;   // set up UIViewLayoutV2 element with tag "bg", apply StyleSheet
    void onPaint(PaintReason reason) override; // update element rect to match widget rect, reapply brush
public:
    explicit Rectangle(Composition::Rect rect, const RectangleProps & props = {});
    void setProps(const RectangleProps & props);
};
```

All shape primitives follow the same pattern. Constructor creates a `UIView`, `onMount` adds the shape element, `onPaint` updates it.

### Source Files

- `wtk/src/Widgets/Primatives.cpp` — implementations for all five shape widgets plus `Separator`.

### Verification

- Each shape renders correctly at various sizes.
- Resize correctly updates the shape element bounds.
- StyleSheet overrides (brush, border, shadow) work.

### Notes

 - RoundedRectangle stores per-corner radii in props (matching the plan API)   
  but uses std::max of the four as uniform rad_x/rad_y since GRoundedRect       
  doesn't support per-corner yet                                         
  - Path supports strokeWidth and closePath — joins/caps/fill-rule deferred     
  until the Shape system exposes them                                         
  - Separator computes a thin rect centered in the widget bounds, respecting    
  Orientation, thickness, and inset
---

## Phase 2: Text and Image Primitives

**Goal:** Implement `Label`, `Icon`, and `Image` from catalog sections C and D.

### Label

Replace the stub. Backed by `UIView`.

Props:
```cpp
struct LabelProps {
    OmegaCommon::UString text {};
    SharedHandle<Composition::Font> font = nullptr;
    Composition::Color textColor {0.f, 0.f, 0.f, 1.f};
    Composition::TextLayoutDescriptor::Alignment alignment = Composition::TextLayoutDescriptor::Alignment::Leading;
    Composition::TextLayoutDescriptor::Wrapping wrapping = Composition::TextLayoutDescriptor::Wrapping::WordWrap;
    unsigned lineLimit = 0; // 0 = unlimited
};
```

Key behaviors:
- `onMount`: Add text element to `UIViewLayoutV2` with tag `"label"`.
- `onPaint`: Update text content, apply font/color/alignment/wrapping from props or StyleSheet.
- `setText(UString)`: Update text content and invalidate.
- `measureSelf`: Return intrinsic text size using font metrics (requires text measurement from `Composition::Font`).

### Icon

Props:
```cpp
struct IconProps {
    OmegaCommon::String token {};  // named icon identifier
    float size = 16.f;
    Composition::Color tintColor {0.f, 0.f, 0.f, 1.f};
};
```

Implementation deferred question: Icon rendering can be glyph-based (font icon) or SVG-based. Start with glyph-based (render a single Unicode codepoint from an icon font). SVG icon support can layer on top via `SVGView` later.

### Image

Props:
```cpp
enum class ImageFitMode : uint8_t { Contain, Cover, Fill, Center, Crop };

struct ImageProps {
    SharedHandle<Media::BitmapImage> source = nullptr;
    ImageFitMode fitMode = ImageFitMode::Contain;
};
```

Backed by `CanvasView` (uses `drawImage`). `onPaint` computes the destination rect based on `fitMode` and the widget's current bounds.

### Source Files

- Expand `wtk/src/Widgets/Primatives.cpp` or split into `Primatives.Text.cpp` and `Primatives.Image.cpp` if it grows.

### Verification

- Label renders single-line and multi-line text with wrapping.
- Label `measureSelf` returns correct intrinsic size for layout participation.
- Image displays with each fit mode at various aspect ratios.
- Icon renders a glyph at the specified size and tint.

---

## Phase 2A: Text Measurement

**Goal:** Replace the heuristic `Label::measureSelf` with accurate text measurement using platform font metrics.

### Problem

`Label::measureSelf` currently estimates intrinsic size using `fontSize * 0.6 * charCount` (width) and `fontSize * 1.2` (height). This is inaccurate for proportional fonts, multi-line text with wrapping, and non-Latin scripts.

### Required Infrastructure

Add a text measurement method to `Composition::Font`:

```cpp
struct TextMeasurement {
    float width;
    float height;
    unsigned lineCount;
};

class Font {
public:
    // ... existing ...
    virtual TextMeasurement measureText(const OmegaCommon::UString & text,
                                        float maxWidth,
                                        const TextLayoutDescriptor & layout) = 0;
};
```

Each platform backend (`DWriteFont`, `CTFont`, `PangoFont`) implements `measureText` using its native text layout engine:
- **Windows:** `IDWriteTextLayout::GetMetrics`
- **macOS:** `CTFramesetterSuggestFrameSizeWithConstraints`
- **Linux:** `pango_layout_get_pixel_size`

### Label Integration

Replace the heuristic in `Label::measureSelf`:

```cpp
MeasureResult Label::measureSelf(const LayoutContext & ctx) {
    if (!props_.font || props_.text.empty()) {
        return {rect().w / ctx.dpiScale, rect().h / ctx.dpiScale};
    }
    TextLayoutDescriptor desc;
    desc.alignment = props_.alignment;
    desc.wrapping = props_.wrapping;
    desc.lineLimit = props_.lineLimit;
    float maxWidthPx = ctx.availableRectPx.w;
    auto m = props_.font->measureText(props_.text, maxWidthPx, desc);
    return {m.width / ctx.dpiScale, m.height / ctx.dpiScale};
}
```

### Verification

- `measureSelf` returns accurate dimensions for single-line and wrapped multi-line text.
- Layout containers using `Label` children produce correct geometry without manual sizing.
- Non-Latin scripts (CJK, Arabic) measure correctly via ICU/platform shaping.

---

## Phase 2B: Icon Rendering from Image/SVG

**Goal:** Replace the glyph-based `Icon` placeholder with image-based and SVG-based icon rendering.

### Problem

`Icon` currently renders the `token` string as a text glyph via UIView. Real icon systems use rasterized icon sheets (sprite atlases) or inline SVG. The glyph approach is a placeholder; production icons need:
- Resolution-independent rendering (SVG preferred).
- Named lookup from an icon registry so widget code uses semantic names (`"arrow-left"`, `"settings"`) rather than raw glyphs.

### Design

```cpp
struct IconSource {
    enum class Kind : uint8_t { Glyph, Image, SVG };
    Kind kind = Kind::Glyph;
    OmegaCommon::String token {};                  // glyph: the codepoint string
    SharedHandle<Media::BitmapImage> image = nullptr; // image: raster icon
    OmegaCommon::FS::Path svgPath {};              // svg: path to SVG asset
};

struct IconProps {
    IconSource source {};
    float size = 16.f;
    Composition::Color tintColor {0.f, 0.f, 0.f, 1.f};
};
```

- **Glyph mode:** Current behavior (text element via UIView).
- **Image mode:** Backed by `CanvasView`, draws the raster icon scaled to `size × size`.
- **SVG mode:** Backed by `SVGView`, loads and renders the SVG asset at `size × size`.

### Icon Registry (optional follow-up)

A global `IconRegistry` maps semantic names to `IconSource` entries, loaded from an asset manifest at app startup:

```cpp
class IconRegistry {
public:
    static IconRegistry & instance();
    void registerIcon(const OmegaCommon::String & name, const IconSource & source);
    Core::Optional<IconSource> resolve(const OmegaCommon::String & name) const;
};
```

Widgets can then use `Icon(rect, IconProps{.source = IconRegistry::instance().resolve("settings").value()})`.

### Verification

- SVG icons render at correct size and tint.
- Image icons render without distortion at various DPI scales.
- Glyph fallback continues to work when no image/SVG source is provided.

---

## Phase 3: Layout Containers

**Goal:** Add `ZStack`, `Grid`, `Spacer`, `SplitView`, and `Tabs`/`TabItem` from catalog section B.

### ZStack

Extends `Container`. Children are stacked along the z-axis (paint order = child order). No main-axis layout — children are positioned by alignment within the container bounds.

```cpp
enum class ZStackAlignment : uint8_t {
    TopLeft, Top, TopRight,
    Left, Center, Right,
    BottomLeft, Bottom, BottomRight
};

struct ZStackOptions {
    ZStackAlignment alignment = ZStackAlignment::Center;
};

class ZStack : public Container {
    ZStackOptions options_;
protected:
    void layoutChildren() override; // position each child based on alignment
public:
    explicit ZStack(Composition::Rect rect, const ZStackOptions & options = {});
    explicit ZStack(ViewPtr view, const ZStackOptions & options = {});
};
```

`layoutChildren`: For each child, compute position within content bounds based on alignment and child's intrinsic/current size.

### Grid

Extends `Container`. Row/column grid with span support.

```cpp
struct GridOptions {
    unsigned columns = 1;
    float rowSpacing = 0.f;
    float columnSpacing = 0.f;
    StackCrossAlign cellAlign = StackCrossAlign::Start;
};

struct GridSlot {
    unsigned columnSpan = 1;
    unsigned rowSpan = 1;
};

class Grid : public Container {
    GridOptions options_;
    OmegaCommon::Vector<GridSlot> childSlots_;
protected:
    void layoutChildren() override; // compute column widths, row heights, place children
public:
    explicit Grid(Composition::Rect rect, const GridOptions & options = {});
    WidgetPtr addChild(const WidgetPtr & child) override;
    WidgetPtr addChild(const WidgetPtr & child, const GridSlot & slot);
};
```

### Spacer

A non-visual widget that participates in stack layout to consume available space.

```cpp
class Spacer : public Widget {
public:
    explicit Spacer(Composition::Rect rect = {Composition::Point2D{0.f,0.f}, 1.f, 1.f});
    bool isLayoutResizable() const override { return true; }
};
```

When placed inside an `HStack`/`VStack` with `flexGrow > 0` in its `StackSlot`, it expands to fill remaining space. The `Spacer` itself has no `onPaint`.

### SplitView

Extends `Container`. Two children separated by a draggable divider.

```cpp
enum class SplitOrientation : uint8_t { Horizontal, Vertical };

struct SplitViewOptions {
    SplitOrientation orientation = SplitOrientation::Horizontal;
    float dividerWidth = 4.f;
    float initialRatio = 0.5f;
    float minPaneSize = 50.f;
};

class SplitView : public Container {
    SplitViewOptions options_;
    float ratio_;
    // ViewDelegate for divider drag
protected:
    void layoutChildren() override;
public:
    explicit SplitView(Composition::Rect rect, const SplitViewOptions & options = {});
    void setRatio(float ratio);
    float ratio() const;
};
```

### Tabs + TabItem

`Tabs` is a `Container` that shows one child at a time, with a tab bar for switching.

```cpp
struct TabItemProps {
    OmegaCommon::UString title {};
    OmegaCommon::String iconToken {};
};

class TabItem : public Container {
    TabItemProps props_;
public:
    explicit TabItem(Composition::Rect rect, const TabItemProps & props);
    const TabItemProps & tabProps() const;
};

class Tabs : public Container {
    std::size_t activeIndex_ = 0;
    // Internal: tab bar widget (HStack of tab buttons)
protected:
    void layoutChildren() override;
public:
    explicit Tabs(Composition::Rect rect);
    WidgetPtr addTab(const WidgetPtr & tabItem);
    void setActiveIndex(std::size_t index);
    std::size_t activeIndex() const;
};
```

### Source Files

- `wtk/src/Widgets/Containers.cpp` — expand with `ZStack`, `Grid` implementations.
- `wtk/src/Widgets/Containers.SplitView.cpp` — `SplitView` (divider drag logic is complex enough to warrant its own TU).
- `wtk/src/Widgets/Containers.Tabs.cpp` — `Tabs` + `TabItem`.

### Verification

- `ZStack` correctly overlays children with each alignment mode.
- `Grid` lays out children in row-major order with correct spans.
- `Spacer` inside `HStack`/`VStack` pushes siblings to edges.
- `SplitView` divider drag adjusts pane ratio within min/max bounds.
- `Tabs` shows only the active tab's content; tab bar switches correctly.

---

## Phase 4: Core Input Widgets

**Goal:** Implement the essential interactive widgets from catalog section D.

All input widgets use the `WidgetInteractionDelegate` from Phase 0 for hover/press/focus tracking.

### Button

Replace the stub. A `Container` (to support icon + label composition) backed by `UIView`.

```cpp
struct ButtonProps {
    OmegaCommon::UString text {};
    OmegaCommon::String iconToken {};
    bool enabled = true;
};

class Button : public Container {
    ButtonProps props_;
    InteractiveState interactiveState_ = InteractiveState::Idle;
    std::function<void()> onPress_ = nullptr;
    // Internal delegate for mouse events
protected:
    void onMount() override;
    void onPaint(PaintReason reason) override;
public:
    explicit Button(Composition::Rect rect, const ButtonProps & props = {});
    void setOnPress(std::function<void()> callback);
    void setProps(const ButtonProps & props);
};
```

`onPaint` applies different `StyleSheet` states for idle/hovered/pressed/disabled.

### TextInput

Replace the stub. Single-line text entry.

```cpp
struct TextInputProps {
    OmegaCommon::UString placeholder {};
    OmegaCommon::UString initialValue {};
    bool enabled = true;
};

class TextInput : public Container {
    TextInputProps props_;
    OmegaCommon::UString text_ {};
    std::size_t caretPosition_ = 0;
    Core::Optional<std::pair<std::size_t, std::size_t>> selection_ {};
    InteractiveState interactiveState_ = InteractiveState::Idle;
    std::function<void(const OmegaCommon::UString &)> onValueChange_ = nullptr;
    // Internal: key event delegate, caret blink timer
protected:
    void onMount() override;
    void onPaint(PaintReason reason) override;
public:
    explicit TextInput(Composition::Rect rect, const TextInputProps & props = {});
    void setText(const OmegaCommon::UString & text);
    const OmegaCommon::UString & text() const;
    void setOnValueChange(std::function<void(const OmegaCommon::UString &)> callback);
};
```

Elements: `"bg"` background rect, `"label"` for displayed text / placeholder, caret as a thin `"caret"` rect element toggled by a timer.

Key event handling: `onKeyDown` appends/deletes characters, moves caret. Selection via shift+arrow. IME support is a follow-up.

### TextArea

Multi-line variant of `TextInput`. Wraps a `ScrollView` internally for overflow.

```cpp
class TextArea : public Container {
    // Similar to TextInput but with line-break handling and vertical scroll
public:
    explicit TextArea(Composition::Rect rect, const TextInputProps & props = {});
};
```

### Toggle

On/off switch.

```cpp
class Toggle : public Widget {
    bool value_ = false;
    InteractiveState interactiveState_ = InteractiveState::Idle;
    std::function<void(bool)> onToggle_ = nullptr;
protected:
    void onMount() override;  // "track" + "thumb" elements
    void onPaint(PaintReason reason) override;  // animate thumb position
public:
    explicit Toggle(Composition::Rect rect, bool initialValue = false);
    void setValue(bool value);
    bool value() const;
    void setOnToggle(std::function<void(bool)> callback);
};
```

### Checkbox

```cpp
enum class CheckboxState : uint8_t { Unchecked, Checked, Indeterminate };

class Checkbox : public Widget {
    CheckboxState checkState_ = CheckboxState::Unchecked;
    // "bg" box + "indicator" checkmark element
public:
    explicit Checkbox(Composition::Rect rect, CheckboxState initial = CheckboxState::Unchecked);
    void setCheckState(CheckboxState state);
    CheckboxState checkState() const;
    void setOnChange(std::function<void(CheckboxState)> callback);
};
```

### RadioGroup + RadioButton

```cpp
class RadioGroup;

class RadioButton : public Widget {
    OmegaCommon::UString label_ {};
    bool selected_ = false;
    RadioGroup *group_ = nullptr;
    // "bg" circle + "indicator" filled circle
public:
    explicit RadioButton(Composition::Rect rect, const OmegaCommon::UString & label);
};

class RadioGroup : public Container {
    std::size_t selectedIndex_ = 0;
    std::function<void(std::size_t)> onSelectionChange_ = nullptr;
public:
    explicit RadioGroup(Composition::Rect rect);
    WidgetPtr addOption(const WidgetPtr & radioButton);
    void setSelectedIndex(std::size_t index);
    std::size_t selectedIndex() const;
};
```

### Slider

Replace the stub.

```cpp
struct SliderProps {
    float min = 0.f;
    float max = 1.f;
    float step = 0.f; // 0 = continuous
    bool vertical = false;
};

class Slider : public Widget {
    SliderProps props_;
    float value_ = 0.f;
    // "track" + "fill" + "thumb" elements; drag delegate on thumb
public:
    explicit Slider(Composition::Rect rect, const SliderProps & props = {});
    void setValue(float value);
    float value() const;
    void setOnValueChange(std::function<void(float)> callback);
};
```

### Stepper

```cpp
class Stepper : public Container {
    float value_ = 0.f;
    float step_ = 1.f;
    float min_ = 0.f;
    float max_ = 100.f;
    // Internal: minus button, value label, plus button (HStack layout)
public:
    explicit Stepper(Composition::Rect rect, float initialValue = 0.f, float step = 1.f);
};
```

### Select / Dropdown

Replace the `Dropdown` stub. Rename to `Select` (keep `Dropdown` as typedef for backward compat).

```cpp
struct SelectOption {
    OmegaCommon::UString label {};
    OmegaCommon::String value {};
};

class Select : public Container {
    OmegaCommon::Vector<SelectOption> options_ {};
    Core::Optional<std::size_t> selectedIndex_ {};
    // Internal: display button + dropdown overlay (Phase 6 dependency for overlay)
public:
    explicit Select(Composition::Rect rect);
    void setOptions(const OmegaCommon::Vector<SelectOption> & options);
    void setSelectedIndex(std::size_t index);
};

using Dropdown = Select;
```

Note: The dropdown overlay popup depends on Phase 6 (Overlays). Phase 4 implements the trigger button and selection state. The popup rendering is wired in Phase 6.

### ProgressBar

```cpp
class ProgressBar : public Widget {
    float progress_ = 0.f; // 0..1, or < 0 for indeterminate
    // "track" + "fill" elements
public:
    explicit ProgressBar(Composition::Rect rect, float progress = 0.f);
    void setProgress(float progress);
    float progress() const;
    bool isIndeterminate() const;
};
```

### Spinner

```cpp
class Spinner : public Widget {
    bool active_ = true;
    // Animated rotating arc element
public:
    explicit Spinner(Composition::Rect rect, bool active = true);
    void setActive(bool active);
};
```

### Source Files

- `wtk/src/Widgets/UserInputs.Button.cpp`
- `wtk/src/Widgets/UserInputs.TextInput.cpp`
- `wtk/src/Widgets/UserInputs.TextArea.cpp`
- `wtk/src/Widgets/UserInputs.Controls.cpp` — Toggle, Checkbox, RadioGroup, RadioButton, Slider, Stepper, ProgressBar, Spinner
- `wtk/src/Widgets/UserInputs.Select.cpp`

### Verification

- Button fires `onPress` on click, shows hover/press visual states.
- TextInput accepts keyboard input, displays caret, supports selection.
- Toggle animates between on/off states.
- Checkbox cycles through states on click.
- RadioGroup enforces single selection.
- Slider thumb drags smoothly, snaps to step values.
- ProgressBar fills proportionally; indeterminate mode animates.
- Spinner rotates when active.

---

## Phase 5: ScrollableContainer and Collection Widgets

**Goal:** Implement `ScrollableContainer` and the collection widgets from catalog section E.

### 5A. ScrollableContainer

Replace the stub. Wraps a `ScrollView` to provide scrollable content hosting.

```cpp
struct ScrollableContainerOptions {
    bool verticalScroll = true;
    bool horizontalScroll = false;
};

class ScrollableContainer : public Widget {
    ScrollableContainerOptions options_;
    Container *contentContainer_ = nullptr;  // the inner container that holds children
    // Uses ScrollView as backing view; ScrollViewDelegate for scroll events
public:
    explicit ScrollableContainer(Composition::Rect rect, const ScrollableContainerOptions & options = {});
    WidgetPtr addChild(const WidgetPtr & child);
    bool removeChild(const WidgetPtr & child);
    void scrollTo(float x, float y);
    Composition::Point2D scrollOffset() const;
};
```

### 5B. ListView

Virtualized vertical (or horizontal) list. Only children visible in the scroll viewport are mounted.

```cpp
class ListViewDataSource {
public:
    virtual ~ListViewDataSource() = default;
    virtual std::size_t itemCount() = 0;
    virtual float itemHeight(std::size_t index) = 0; // or itemSize for horizontal
    virtual WidgetPtr createItem(std::size_t index) = 0;
    virtual void recycleItem(std::size_t index, WidgetPtr widget) {}
};

class ListView : public Widget {
    ListViewDataSource *dataSource_ = nullptr;
    // Internal: ScrollableContainer + item pool + visible range tracking
public:
    explicit ListView(Composition::Rect rect);
    void setDataSource(ListViewDataSource *source);
    void reloadData();
    void scrollToItem(std::size_t index);
};
```

### 5C. TreeView

Hierarchical expandable list built on top of `ListView`.

```cpp
class TreeViewNode {
public:
    virtual ~TreeViewNode() = default;
    virtual OmegaCommon::UString label() = 0;
    virtual std::size_t childCount() = 0;
    virtual TreeViewNode *childAt(std::size_t index) = 0;
    virtual bool isExpandable() { return childCount() > 0; }
};

class TreeView : public Widget {
    TreeViewNode *rootNode_ = nullptr;
    // Internal: flattened visible list driven through ListView
public:
    explicit TreeView(Composition::Rect rect);
    void setRootNode(TreeViewNode *root);
    void expandNode(TreeViewNode *node);
    void collapseNode(TreeViewNode *node);
};
```

### 5D. TableView

Columnar data with sorting and column resize.

```cpp
struct TableColumn {
    OmegaCommon::UString title {};
    float width = 100.f;
    float minWidth = 40.f;
    bool resizable = true;
    bool sortable = false;
};

class TableViewDataSource {
public:
    virtual ~TableViewDataSource() = default;
    virtual std::size_t rowCount() = 0;
    virtual OmegaCommon::UString cellValue(std::size_t row, std::size_t column) = 0;
};

class TableView : public Widget {
    OmegaCommon::Vector<TableColumn> columns_ {};
    TableViewDataSource *dataSource_ = nullptr;
    // Internal: header row (HStack) + virtualized row list (ListView)
public:
    explicit TableView(Composition::Rect rect);
    void setColumns(const OmegaCommon::Vector<TableColumn> & columns);
    void setDataSource(TableViewDataSource *source);
};
```

### 5E. CollectionView

Grid/flow layout collection with item reuse.

```cpp
class CollectionViewDataSource {
public:
    virtual ~CollectionViewDataSource() = default;
    virtual std::size_t itemCount() = 0;
    virtual Composition::Rect itemSize(std::size_t index) = 0;
    virtual WidgetPtr createItem(std::size_t index) = 0;
};

class CollectionView : public Widget {
    CollectionViewDataSource *dataSource_ = nullptr;
    // Internal: ScrollableContainer + Grid-like placement + item pool
public:
    explicit CollectionView(Composition::Rect rect);
    void setDataSource(CollectionViewDataSource *source);
    void reloadData();
};
```

### 5F. PropertyGrid

Label/value editing panel for tooling.

```cpp
struct PropertyGridEntry {
    OmegaCommon::UString label {};
    OmegaCommon::UString value {};
    bool editable = true;
};

class PropertyGrid : public Widget {
    OmegaCommon::Vector<PropertyGridEntry> entries_ {};
    // Internal: two-column Grid with Label + TextInput per row
public:
    explicit PropertyGrid(Composition::Rect rect);
    void setEntries(const OmegaCommon::Vector<PropertyGridEntry> & entries);
};
```

### Source Files

- `wtk/src/Widgets/Collections.ListView.cpp`
- `wtk/src/Widgets/Collections.TreeView.cpp`
- `wtk/src/Widgets/Collections.TableView.cpp`
- `wtk/src/Widgets/Collections.CollectionView.cpp`
- `wtk/src/Widgets/Collections.PropertyGrid.cpp`

### Verification

- `ScrollableContainer` scrolls content beyond viewport bounds.
- `ListView` renders 10,000 items without mounting all of them (check child count).
- `TreeView` expand/collapse toggles visibility of subtree items.
- `TableView` column headers drag-resize; sort callback fires.
- `CollectionView` reflows items on resize.

---

## Phase 6: Overlay and Feedback Widgets

**Goal:** Implement overlays from catalog section G. Requires a `ZStack`-based overlay host or a top-level overlay layer strategy.

### 6A. Overlay Infrastructure

Before individual overlay widgets, establish an overlay hosting mechanism:

```cpp
class OverlayHost {
    // Manages a ZStack-like layer above the main widget tree
    // Overlays are positioned relative to an anchor widget
public:
    void present(WidgetPtr overlay, Widget *anchor, /* positioning params */);
    void dismiss(WidgetPtr overlay);
};
```

This can live on `AppWindow` or be a standalone utility that widgets access through their `treeHost`.

### 6B. Tooltip

```cpp
struct TooltipProps {
    OmegaCommon::UString text {};
    float delayMs = 500.f;
};

class Tooltip : public Widget {
    TooltipProps props_;
    // Presented via OverlayHost on hover of anchor widget
public:
    explicit Tooltip(Composition::Rect rect, const TooltipProps & props);
    static void attachTo(Widget *target, const TooltipProps & props);
};
```

### 6C. Popover

Anchored floating panel with arbitrary child content.

```cpp
enum class PopoverEdge : uint8_t { Top, Bottom, Left, Right };

class Popover : public Container {
    PopoverEdge preferredEdge_ = PopoverEdge::Bottom;
    Widget *anchor_ = nullptr;
public:
    explicit Popover(Composition::Rect rect, PopoverEdge edge = PopoverEdge::Bottom);
    void present(Widget *anchor);
    void dismiss();
};
```

### 6D. Menu / ContextMenu

Command list with keyboard navigation.

```cpp
struct MenuItem {
    OmegaCommon::UString title {};
    OmegaCommon::String shortcut {};
    std::function<void()> action = nullptr;
    bool enabled = true;
    bool separator = false;
    OmegaCommon::Vector<MenuItem> submenu {};
};

class ContextMenu : public Widget {
    OmegaCommon::Vector<MenuItem> items_ {};
public:
    explicit ContextMenu(const OmegaCommon::Vector<MenuItem> & items);
    void present(Widget *anchor, Composition::Point2D screenPos);
    void dismiss();
};
```

Note: The existing `Menu.h` handles native menus. `ContextMenu` here is an in-view rendered menu for custom styling.

### 6E. ModalDialog

Blocking workflow container with focus trap.

```cpp
class ModalDialog : public Container {
    bool visible_ = false;
    // Backdrop overlay + centered content container
public:
    explicit ModalDialog(Composition::Rect rect);
    void present();
    void dismiss();
    void setOnDismiss(std::function<void()> callback);
};
```

### 6F. Toast / Banner

Non-blocking notifications with auto-dismiss.

```cpp
enum class ToastPosition : uint8_t { Top, Bottom, TopRight, BottomRight };

struct ToastProps {
    OmegaCommon::UString message {};
    float durationMs = 3000.f;
    ToastPosition position = ToastPosition::Bottom;
};

class Toast : public Widget {
public:
    static void show(Widget *host, const ToastProps & props);
};
```

### 6G. Sheet

Partial-screen modal panel (slides in from edge).

```cpp
enum class SheetEdge : uint8_t { Bottom, Right, Left };

class Sheet : public Container {
    SheetEdge edge_ = SheetEdge::Bottom;
    float height_ = 300.f; // or width for side sheets
public:
    explicit Sheet(Composition::Rect rect, SheetEdge edge = SheetEdge::Bottom);
    void present();
    void dismiss();
};
```

### Source Files

- `wtk/src/Widgets/Overlays.Infrastructure.cpp`
- `wtk/src/Widgets/Overlays.cpp` — Tooltip, Popover, ContextMenu, ModalDialog, Toast, Sheet

### Verification

- Tooltip appears on hover after delay, dismisses on exit.
- Popover anchors correctly to each edge of the target widget.
- ContextMenu keyboard navigation (arrow keys, Enter, Escape).
- ModalDialog traps focus; backdrop click dismisses.
- Toast auto-dismisses after timeout; queue prevents stacking.
- Sheet slides in/out with animation.

### Phase 6 also wires Select/Dropdown popup

With overlay infrastructure in place, wire the `Select` widget's dropdown popup.

---

## Phase 7: Navigation and App Structure Widgets

**Goal:** Implement catalog section F.

### NavigationStack

Push/pop page flow.

```cpp
class NavigationStack : public Container {
    OmegaCommon::Vector<WidgetPtr> pageStack_ {};
public:
    explicit NavigationStack(Composition::Rect rect);
    void push(const WidgetPtr & page);
    WidgetPtr pop();
    std::size_t depth() const;
};
```

`layoutChildren` shows only the top page. Push/pop can animate a slide transition.

### Sidebar

```cpp
class Sidebar : public Container {
    std::size_t selectedIndex_ = 0;
    // Internal: VStack of clickable items
public:
    explicit Sidebar(Composition::Rect rect);
    WidgetPtr addSection(const OmegaCommon::UString & title, const WidgetPtr & content);
    void setSelectedIndex(std::size_t index);
};
```

### Breadcrumb

```cpp
class Breadcrumb : public Widget {
    OmegaCommon::Vector<OmegaCommon::UString> segments_ {};
    std::function<void(std::size_t)> onNavigate_ = nullptr;
    // Internal: HStack of clickable labels + separator labels
public:
    explicit Breadcrumb(Composition::Rect rect);
    void setSegments(const OmegaCommon::Vector<OmegaCommon::UString> & segments);
    void setOnNavigate(std::function<void(std::size_t)> callback);
};
```

### Toolbar

```cpp
class Toolbar : public Container {
    // Internal: HStack with overflow detection
public:
    explicit Toolbar(Composition::Rect rect);
    WidgetPtr addItem(const WidgetPtr & item);
    void addSeparator();
};
```

### StatusBar

```cpp
class StatusBar : public Container {
    // Internal: HStack with left/center/right zones
public:
    explicit StatusBar(Composition::Rect rect);
    void setLeftContent(const WidgetPtr & widget);
    void setCenterContent(const WidgetPtr & widget);
    void setRightContent(const WidgetPtr & widget);
};
```

### Source Files

- `wtk/src/Widgets/Navigation.cpp`

### Verification

- `NavigationStack` push/pop updates visible page.
- `Sidebar` selection change fires callback and highlights active item.
- `Breadcrumb` renders segments with separators; click navigates.
- `Toolbar` lays out items horizontally.
- `StatusBar` has three layout zones.

---

## Phase 8: Media and Document Widgets

**Goal:** Wrap existing specialized views as proper widgets from catalog section H.

### VideoViewWidget

```cpp
class VideoViewWidget : public Widget {
    // Backing: VideoView (created in constructor)
public:
    explicit VideoViewWidget(Composition::Rect rect);
    VideoView & videoView();
    // Delegates to VideoView API: bindPlaybackSource, play, pause, stop, etc.
};
```

### SVGViewWidget

```cpp
class SVGViewWidget : public Widget {
    // Backing: SVGView
public:
    explicit SVGViewWidget(Composition::Rect rect);
    SVGView & svgView();
    bool setSourceString(const OmegaCommon::String & svg);
    bool setSourceDocument(Core::XMLDocument doc);
};
```

### AudioPlayerWidget

```cpp
class AudioPlayerWidget : public Container {
    // Internal: transport controls (Button: play/pause/stop) + Slider (seek) + Label (time)
    // Backed by Media::AudioPlaybackSession or similar
public:
    explicit AudioPlayerWidget(Composition::Rect rect);
};
```

This is a composite widget. Its implementation depends on the Media audio API surface.

### CanvasWidget

Escape hatch for custom immediate-mode drawing.

```cpp
class CanvasWidget : public Widget {
    std::function<void(Composition::Canvas &, const Composition::Rect &)> drawCallback_ = nullptr;
    // Backing: CanvasView
public:
    explicit CanvasWidget(Composition::Rect rect);
    void setDrawCallback(std::function<void(Composition::Canvas &, const Composition::Rect &)> callback);
    Composition::Canvas & canvas();
protected:
    void onPaint(PaintReason reason) override; // calls drawCallback_
};
```

### PDFView (Optional)

Deferred. Requires a PDF parsing/rasterization pipeline not yet present. Placeholder:

```cpp
class PDFView : public Widget {
public:
    WIDGET_CONSTRUCTOR()
};
```

### Source Files

- `wtk/src/Widgets/MediaWidgets.cpp`

### Verification

- `VideoViewWidget` plays a video file.
- `SVGViewWidget` renders an SVG string.
- `CanvasWidget` `drawCallback` fires on every paint.

---

## Phase 9: Accessibility and System Integration

**Goal:** Implement catalog section I.

### FocusRingHost

Centralized focus visual rendering. Draws a focus ring around the currently focused widget.

```cpp
class FocusRingHost : public Widget {
    Widget *focusedWidget_ = nullptr;
    // Draws a ring overlay via OverlayHost
public:
    explicit FocusRingHost(Composition::Rect rect);
    void setFocusedWidget(Widget *widget);
};
```

### ShortcutHint

Keyboard shortcut annotation overlay for menus and toolbars.

```cpp
class ShortcutHint : public Widget {
    OmegaCommon::UString shortcutText_ {};
    // Small label, typically right-aligned in a menu item
public:
    explicit ShortcutHint(Composition::Rect rect, const OmegaCommon::UString & shortcut);
};
```

### InspectorPanel

Runtime property inspection UI for development/debugging.

```cpp
class InspectorPanel : public Container {
    Widget *inspectedWidget_ = nullptr;
    // Internal: PropertyGrid showing widget rect, state, paint info
public:
    explicit InspectorPanel(Composition::Rect rect);
    void inspect(Widget *widget);
};
```

### Source Files

- `wtk/src/Widgets/Accessibility.cpp`

### Verification

- `FocusRingHost` ring follows focus changes.
- `InspectorPanel` shows live rect/state for any widget.

---

## Dependency Graph

```
Phase 0 (Infrastructure)
  │
  ├── Phase 1 (Shape Primitives)
  │     └── Phase 2 (Text/Image Primitives)
  │           └── Phase 4 (Input Widgets) ──────────────┐
  │                                                      │
  ├── Phase 3 (Layout Containers)                        │
  │     ├── Phase 5 (Collections) ← needs ScrollableContainer
  │     └── Phase 7 (Navigation) ← needs input widgets ─┘
  │
  ├── Phase 6 (Overlays) ← needs ZStack from Phase 3, input from Phase 4
  │     └── Phase 4 Select/Dropdown popup wiring
  │
  ├── Phase 8 (Media) ← independent of most phases
  │
  └── Phase 9 (Accessibility) ← needs overlays from Phase 6, PropertyGrid from Phase 5
```

Phases 1, 3, and 8 can begin in parallel after Phase 0.
Phase 2 can begin as soon as Phase 1 establishes the UIView-backed primitive pattern.
Phases 4 and 5 depend on earlier phases but can overlap.
Phase 6 depends on Phases 3 and 4.
Phases 7 and 9 come last.

---

## Open Questions

1. **Icon system**: Font-based glyphs or SVG icons? Or both with a unified `IconProvider` interface?
2. **Text measurement**: Does `Composition::Font` currently expose text extent measurement? If not, that's a prerequisite for `Label::measureSelf` and all text-based intrinsic sizing.
3. **Focus management**: Is there a global focus tracking system, or does each widget manage focus independently? Overlays and `ModalDialog` need focus trapping.
4. **Theme system**: Should interactive state styles (hover, pressed, disabled) be driven by `StyleSheet` state variants, or by swapping entire stylesheets?
5. **Select popup**: Should the dropdown overlay use the native `Menu` system (from `Menu.h`) or a custom in-view overlay? Native gives platform-correct behavior; custom gives full style control.
6. **Button inheritance**: The current stub has `Button : public Container`. The catalog suggests it should support icon + text composition, which justifies Container. But simple buttons could just be `Widget`. Keep `Container` to support flexible content?
7. **Controlled vs uncontrolled state**: The catalog mandates both modes for every widget. How should this be surfaced in the API? Optional `WidgetState<T>` binding parameter in `Create`?
