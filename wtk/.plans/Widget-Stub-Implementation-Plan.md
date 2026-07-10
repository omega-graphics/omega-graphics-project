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
| Collection widgets (`List`, `Table`) | `ScrollView` child of `Container` | Scroll viewport with virtualized child widgets. |
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
| `Collections.h` | `List`, `Tree`, `Table`, `Collection`, `PropertyGrid` |
| `Navigation.h` | `NavigationStack`, `Sidebar`, `Breadcrumb`, `Toolbar`, `StatusBar` |
| `Overlays.h` | `Tooltip`, `Popover`, `PopupMenu`, `ContextMenu`, `Modal`, `Snackbar`, `Sheet` |
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

> **Superseded by Phase 2B.** The concrete glyph `Icon` above is folded into an abstract `Icon` base with three subclasses — `GlyphIcon` (this glyph behavior), `SVGIcon`, `ImageIcon`. `IconProps` is renamed `GlyphIconProps`. See [Phase 2B: Icon Type Hierarchy](#phase-2b-icon-type-hierarchy-glyph--svg--image).

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

## Phase 2B: Icon Type Hierarchy (Glyph / SVG / Image)

**Goal:** Grow the single glyph-only `Icon` into a small polymorphic family — an
abstract `Icon` base with one concrete subclass per backing view: `GlyphIcon`,
`SVGIcon`, `ImageIcon`. Widget code can hold `SharedHandle<Icon>` and not care
which kind it is.

### Problem

`Icon` currently renders the `token` string as a text glyph via `UIView`. Real
icon systems also need resolution-independent vector icons (SVG) and raster icon
assets (PNG sprite sheets). The glyph path is only one of three, and each needs a
*different backing view*: glyph and raster both live on `UIView`, but a vector
icon must render through `SVGView`. `Widget` binds exactly one concrete backing
view at construction (`explicit Widget(ViewPtr view)` — verified in
`wtk/include/omegaWTK/UI/Widget.h:264`), so a single class that swaps its backing
at runtime fights the base class.

### Design decision — hierarchy, not a tagged union

An earlier sketch of this phase used a discriminated `IconSource { Kind; token;
image; svgPath; }` union inside one `Icon` class that swapped backings by `kind`.
That is **superseded**: because the backing view is fixed at construction, the
union forced a class that owns a `UIView` it might never use (SVG) or an
`SVGView` it might never use (glyph). A subclass-per-backing hierarchy matches the
architecture instead — each concrete icon constructs exactly the view it needs and
nothing else.

`Icon` becomes the **abstract base** (the polymorphic "any icon" type). It carries
the two properties every icon shares — a square `size` and an optional `tint` —
plus the shared lifecycle wiring, and declares a pure-virtual `rebuildContent()`.
It does **not** construct a backing view; each subclass does that and forwards the
`ViewPtr` to the protected `Widget(ViewPtr)` ctor.

```cpp
// Abstract base. Not directly constructible.
class OMEGAWTK_EXPORT Icon : public Widget {
protected:
    float                              size_ = 16.f;
    // Unset tint => the subclass falls through to its backing view's
    // default color (glyph: UA-sheet `icon` black; svg/image: native
    // asset color). Matches the Label/Icon inline-default-strip idiom.
    Core::Optional<Composition::Color> tint_ {};

    explicit Icon(ViewPtr view, float size, Core::Optional<Composition::Color> tint);

    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    // Base wires the shared lifecycle: onMount + resize both funnel through
    // the subclass's rebuildContent(). Only the element-authoring differs.
    void onMount() override;                       // calls rebuildContent()
    void resize(Composition::Rect & newRect) override; // view->resize + rebuildContent + invalidate
    virtual void rebuildContent() = 0;             // subclass authors its backing view

public:
    // An icon's intrinsic size is size_ × size_ — shared measure for all kinds.
    Composition::Rect measureSelf(/* ...same signature as Widget::measureSelf... */) override;

    float size() const;
    void  setSize(float size);
    Core::Optional<Composition::Color> tint() const;
    void  setTint(Core::Optional<Composition::Color> tint);
};
```

Per-kind props stay flat (matching `LabelProps` / `RectangleProps`), each embedding
the shared `size` + `tintColor`:

```cpp
struct OMEGAWTK_EXPORT GlyphIconProps {           // was IconProps
    OmegaCommon::String                token {};  // codepoint / icon-font glyph
    float                              size = 16.f;
    Core::Optional<Composition::Color> tintColor {};
};

struct OMEGAWTK_EXPORT SVGIconProps {
    // Exactly one source is used; svgString wins if both are set.
    OmegaCommon::String                svgString {}; // inline SVG markup
    OmegaCommon::FS::Path              svgPath {};   // path to an .svg asset
    float                              size = 16.f;
    Core::Optional<Composition::Color> tintColor {}; // deferred — see below
};

struct OMEGAWTK_EXPORT ImageIconProps {
    SharedHandle<OmegaCommon::Img::BitmapImage> source = nullptr;
    float                              size = 16.f;
    Core::Optional<Composition::Color> tintColor {}; // deferred — see below
};
```

| Subclass    | Backing view        | `rebuildContent()` authors                                                                 |
|-------------|---------------------|--------------------------------------------------------------------------------------------|
| `GlyphIcon` | `UIView` (`"icon"`) | Text element from `token`; `Style::textColor("icon", tint)` when tint set (today's behavior). |
| `ImageIcon` | `UIView` (`"icon"`) | Image element (`spec.image` / `spec.imageRect`), contain-fit into the `size × size` box, reusing the `computeFitRect` helper from `Image`. |
| `SVGIcon`   | `SVGView`           | `setSourceString` / `setSourceStream` from props; `SVGViewRenderOptions.scaleMode = Meet` so the vector fits the icon box aspect-correctly. |

### Tint scope — glyph only in this phase

Only `GlyphIcon` honors `tintColor` (it maps to the element's text color, which
already works). `SVGIcon` and `ImageIcon` render at their native asset color;
their `tintColor` is accepted but is a **documented no-op** in this phase.
Recoloring a parsed SVG (per-path fill override in the `DisplayList`) and raster
tint-multiply are real compositor work and are their own follow-ups — see the
follow-up table. This keeps the phase honest and inside the small-feature ceiling.

### Backward compatibility

omega-codedb confirms **no code outside `Primatives.{h,cpp}` constructs the `Icon`
widget or `IconProps`** — Button/Toggle etc. render glyphs as raw `iconToken`
strings inside their own `UIView`, not via the Icon widget. So renaming the
concrete glyph type to `GlyphIcon` and repurposing `Icon` as the abstract base is
free: there are no call sites to migrate, and no compat alias is introduced.

### Implementation phasing

Each sub-phase is independently buildable and visually verifiable.

- **2B.1 — Extract the abstract base + `GlyphIcon`.** Add the abstract `Icon`
  base (shared `size_`/`tint_`, `onMount`/`resize`/`measureSelf`, pure-virtual
  `rebuildContent`). Rename `IconProps` → `GlyphIconProps` and move today's
  `Icon::rebuildContent` body verbatim into `GlyphIcon::rebuildContent`. No behavior
  change for the glyph path. *Verify:* existing glyph rendering is pixel-identical
  (BasicAppTest / any current Icon use) after the refactor.

- **2B.2 — `ImageIcon`.** `UIView`-backed. `rebuildContent` adds one image element
  contain-fit into the `size × size` box via the existing `computeFitRect(... Contain)`
  from the `Image` implementation (extract it to file scope in `Primatives.cpp` if it
  is still in the anonymous namespace). Tint deferred. *Verify:* a PNG icon renders
  crisp and undistorted at a couple of sizes.

- **2B.3 — `SVGIcon`.** `SVGView`-backed (`Widget(ViewPtr(new SVGView(rect, nullptr)))`).
  `rebuildContent` loads `svgString` (else opens `svgPath` → `setSourceStream`), sets
  `scaleMode = Meet`, and resizes the view to the icon rect. Tint deferred. *Verify:*
  a vector icon renders sharp and scales aspect-correctly.

- **2B.4 — `IconTest` + registry note.** New `wtk/tests/IconTest/main.cpp` renders a
  glyph, an SVG, and a raster icon side by side (per AGENTS.md Visual Debugging — user
  supplies the screenshot). Leave the registry as a documented follow-up (below).

### Source files

- `wtk/include/omegaWTK/Widgets/Primatives.h` — abstract `Icon` + three subclasses + three props structs.
- `wtk/src/Widgets/Primatives.cpp` — base lifecycle + `GlyphIcon`; `ImageIcon`; `SVGIcon`. Split into `Primatives.Icon.cpp` only if the TU grows past the reading-chunk guidance.
- `wtk/tests/IconTest/main.cpp` — visual test.

### Follow-ups (out of scope for 2B)

| Follow-up                         | Depends on                                                        |
|-----------------------------------|-------------------------------------------------------------------|
| SVG tint / recolor                | per-path fill override in the parsed `Composition::DisplayList`    |
| Image / raster icon tint (recolor + multiply, color + gradient) | **Composition-Extension-Plan Phase 12** (`BitmapTint`, Mask/Multiply). `ImageIcon` uses `Mask` `ColorTint` for recolor |
| `IconRegistry` semantic-name lookup | becomes a **factory** returning `SharedHandle<Icon>` (a concrete subclass) for a name + rect, loaded from an asset manifest — not the old value-returning `resolve` (the `IconSource` union it returned no longer exists) |

### Verification

- Glyph, SVG, and raster icons all render at the requested `size`, side by side.
- `GlyphIcon` tint applies; `SVGIcon`/`ImageIcon` render native color (tint no-op, as documented).
- Resizing an icon re-fits its content (glyph rect, image contain-box, SVG Meet-scale).
- Code holding `SharedHandle<Icon>` can paint any of the three kinds polymorphically.

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

### Phase 4A: Button — Base Implementation

Replace the stub. **Leaf `Widget`** backed by a single `UIView` with three element tags. The earlier sketch had `Button : Container`; the leaf-Widget shape is preferred to match the rest of the catalog (`Label`, `Rectangle`, planned `Toggle`/`Checkbox`/`RadioButton`) — there is no use case for arbitrary child widgets inside a base Button, and adopting `Container` would expose `addChild`/`removeChild`/clamp policy on a leaf control. Composite buttons (icon-plus-label-plus-badge) can layer on top by adding extra tags or by a future `CompositeButton : Container` variant; the base stays narrow.

#### Header — `wtk/include/omegaWTK/Widgets/UserInputs.h`

```cpp
struct OMEGAWTK_EXPORT ButtonProps {
    OmegaCommon::UString text {};
    OmegaCommon::String  iconToken {};            // empty => no icon
    bool                 enabled    = true;
    float                cornerRadius = 4.f;
    // Optional overrides — when unset, the theme colors are used.
    Core::Optional<Composition::Color> tintOverride {};      // bg accent in hover/pressed
    Core::Optional<Composition::Color> labelColorOverride {};
};

class OMEGAWTK_EXPORT Button : public Widget {
    ButtonProps                                          props_;
    Native::ThemeDesc                                    theme_ {};
    InteractiveState                                     state_ = InteractiveState::Idle;
    std::function<void()>                                onPress_ {};
    Core::UniquePtr<class ButtonInteractionDelegate>     delegate_;

protected:
    void onMount() override;
    void onThemeSet(Native::ThemeDesc & desc) override;
    void resize(Composition::Rect & newRect) override;

    // Two rebuild paths, split intentionally:
    //  - rebuildContent(): element list + sub-rects (icon/label boxes).
    //    Runs on onMount / resize / setProps when the layout changes.
    //  - rebuildStyle():   per-state Style (bg fill, text color).
    //    Runs on every interaction state change — much cheaper than
    //    re-laying out the element list.
    void rebuildContent();
    void rebuildStyle();

    void onInteractionStateChanged(InteractiveState newState, bool clickConfirmed);

public:
    explicit Button(Composition::Rect rect, const ButtonProps & props = {});
    ~Button() override;

    void setProps(const ButtonProps & props);
    const ButtonProps & props() const;

    void setOnPress(std::function<void()> callback);
    void setEnabled(bool enabled);
    bool isEnabled() const;

    InteractiveState interactionState() const;   // for tests / introspection
};
```

#### Element-tag layout

The `UIView` carries three element tags:

| Tag       | Shape                  | Purpose                                                  |
|-----------|------------------------|----------------------------------------------------------|
| `"bg"`    | `Shape::RoundedRect`   | Background. Fill comes from `Style`, not `RectangleProps`. |
| `"icon"`  | text element (glyph)   | Optional. Square, vertically centered, left-aligned in content rect with leading padding. Hidden when `iconToken.empty()`. |
| `"label"` | text element           | Centered (or right of the icon when present). Fills the remaining content rect. |

`rebuildContent()` is the single source of truth for sub-rect math — `onMount`, `resize`, and `setProps` all go through it. Roughly:

```cpp
constexpr float kHPad = 12.f;
constexpr float kIconGap = 6.f;

void Button::rebuildContent() {
    auto & uv = viewAs<UIView>();
    auto & lv2 = uv.layoutV2();
    lv2.clear();

    Composition::Rect r = rect();
    Composition::RoundedRect bg { r.pos, r.w, r.h, props_.cornerRadius, props_.cornerRadius };

    UIElementLayoutSpec bgSpec;
    bgSpec.tag = "bg";
    bgSpec.shape = Shape::RoundedRect(bg);
    lv2.element(bgSpec);

    float cursorX = r.pos.x + kHPad;
    if (!props_.iconToken.empty()) {
        float iconSide = std::min(r.h - 4.f, 16.f);
        UIElementLayoutSpec iconSpec;
        iconSpec.tag = "icon";
        iconSpec.text = OmegaCommon::UString(props_.iconToken.begin(), props_.iconToken.end());
        iconSpec.textRect = { {cursorX, r.pos.y + (r.h - iconSide) * 0.5f}, iconSide, iconSide };
        lv2.element(iconSpec);
        cursorX += iconSide + kIconGap;
    }

    UIElementLayoutSpec labelSpec;
    labelSpec.tag = "label";
    labelSpec.text = props_.text;
    labelSpec.textRect = { {cursorX, r.pos.y}, r.pos.x + r.w - kHPad - cursorX, r.h };
    lv2.element(labelSpec);

    rebuildStyle();
}
```

#### State → Style mapping

`rebuildStyle()` derives the per-state Style from `theme_` and `state_`. The mapping:

| State      | `bg` fill                                           | `label`/`icon` color                          | Alpha |
|------------|------------------------------------------------------|-----------------------------------------------|-------|
| `Idle`     | `controlBackground`                                  | `controlForeground`                           | 1.0   |
| `Hovered`  | `controlBackground` blended 10% toward `accent`      | `controlForeground`                           | 1.0   |
| `Pressed`  | `tintOverride.value_or(accent)`                      | contrast of pressed bg (white on dark accent) | 1.0   |
| `Focused`  | same as Idle + 2px `accent` focus ring on `"bg"`     | `controlForeground`                           | 1.0   |
| `Disabled` | `controlBackground`                                  | `controlForeground`                           | 0.4   |

The focus ring is drawn by setting `border("bg", true)` + `borderColor("bg", accent)` + `borderWidth("bg", 2.f)`. The 2px width is the wire convention; a real `Style::focusRing` helper can land later.

The contrast color for `Pressed` is computed from the resolved bg using a simple luminance threshold (`r*0.299 + g*0.587 + b*0.114 > 0.5 ? black : white`). This is the same heuristic Chromium's `views::Button` uses for its disabled-on-accent text color and is good enough until a designer overrides via `labelColorOverride`.

#### Event wiring

```cpp
class ButtonInteractionDelegate : public WidgetInteractionDelegate {
    Button * button_;
    bool     pendingClick_ = false;   // true between mouseDown and mouseUp inside
public:
    explicit ButtonInteractionDelegate(Button * b)
        : WidgetInteractionDelegate(b), button_(b) {}

    void onLeftMouseDown(Native::NativeEventPtr e) override {
        WidgetInteractionDelegate::onLeftMouseDown(e);
        if (state == InteractiveState::Pressed) pendingClick_ = true;
    }
    void onLeftMouseUp(Native::NativeEventPtr e) override {
        bool fired = pendingClick_;
        pendingClick_ = false;
        WidgetInteractionDelegate::onLeftMouseUp(e);    // Pressed -> Hovered
        if (fired) button_->onInteractionStateChanged(state, /*clickConfirmed=*/true);
        else       button_->onInteractionStateChanged(state, false);
    }
    void onMouseExit(Native::NativeEventPtr e) override {
        pendingClick_ = false;                          // cancel click on drag-off
        WidgetInteractionDelegate::onMouseExit(e);
    }
};
```

The "click confirmed" flag is what disambiguates `mouseUp-while-still-inside` (a click) from `mouseUp-after-drag-off` (a cancel). The cursor-exit cancels the click, matching every native button on all three platforms.

`onInteractionStateChanged` updates `state_`, calls `rebuildStyle()`, calls `invalidate(StateChanged)`, and (when `clickConfirmed`) invokes `onPress_`. The state change does **not** call `rebuildContent()` — only the Style is dirtied.

#### Construction + lifetime

```cpp
Button::Button(Composition::Rect rect, const ButtonProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "button"))),
      props_(props),
      delegate_(Core::make_unique<ButtonInteractionDelegate>(this)) {
    view->setDelegate(delegate_.get());
    if (!props_.enabled) delegate_->setDisabled(true);
}
```

The delegate is owned by the Button (via `UniquePtr`) and outlives the view's raw pointer because the view is also Widget-owned. `~Button` runs after `view` and `delegate_` destructors — both are members. No manual teardown needed beyond `Widget`'s base.

#### Theme integration

`onThemeSet(Native::ThemeDesc & desc)` caches `desc` into `theme_` then calls `rebuildStyle()`. This is the same pattern `Container::onThemeSet` is being slotted into for theme switching at runtime (Light → Dark).

#### Layout participation

`measureSelf` is **not** overridden in v0. The widget honors whatever rect the parent layout gives it; if a `StackWidget` needs intrinsic sizing for an auto-sized button row, `Button::measureSelf` will land alongside Phase 2A `Font::measureText` (a Button's intrinsic width is `kHPad*2 + iconWidth + kIconGap + labelWidth`).

#### Source files

- `wtk/src/Widgets/UserInputs.Button.cpp` — `Button` + `ButtonInteractionDelegate`.

#### Test

- `wtk/tests/ButtonTest/main.cpp` — renders four buttons (Idle / disabled / icon-only / text-and-icon) and a fifth that counts clicks via `setOnPress`. Visual + interactive verification per AGENTS.md "Visual Debugging" — user supplies the screenshot.

#### Phase-4A verification checklist

- [ ] Idle / hover / pressed / disabled visuals match the state table.
- [ ] `onPress` fires on mouseUp **inside** the widget after a mouseDown, and **does not** fire when the cursor leaves before mouseUp.
- [ ] `setEnabled(false)` greys out the widget and suppresses `onPress`.
- [ ] Theme switch (Light → Dark) re-tints without rebuilding the element list (no flicker, no resize).
- [ ] Resizing the parent (StackWidget) re-runs `rebuildContent` so the label rect updates.
- [ ] Cursor-exit during a press visually returns to Idle and cancels the click.

---

### Phase 4B: TextInput — **v0 COMPLETE (runtime-verified macOS 2026-07-10)**

> **Status.** Base TextInput v0 is done and developer-verified on macOS
> (BasicAppTest: focus ring + caret on click/Tab, printable insert,
> Backspace/Delete/arrows/Home/End editing, mirror-Label echo via
> onValueChange, click-away defocus). Runtime testing surfaced and fixed three
> latent backend bugs the widget was the first to exercise — see
> [Native-API-Completion-Proposal §2.3a status row](Native-API-Completion-Proposal.md#completion-status):
> macOS/Win32 had no keyboard delivery wired (only GTK did), the macOS
> `key_code_from_ns` special-key table was mismapped, and M1 click-focus did
> not resign focus on a click-miss. macOS + GTK paths are complete; **Win32 key
> delivery is written but still needs a WSL build to verify** (backend parity
> item, tracked in that status row — it does not gate the widget itself). The
> follow-ups below (caret blink, measured caret X, selection, clipboard, IME,
> multi-line) remain out of v0 scope.
>
> The FocusManager landed (Native-API-Completion-Proposal §2.3a
> Focus block F1–F6 + M1), so TextInput v0 is now built:
> `wtk/src/Widgets/UserInputs.TextInput.cpp` + the `TextInput` /
> `TextInputProps` in `wtk/include/omegaWTK/Widgets/UserInputs.h`. The UIView
> opts into `StrongFocus`; an internal `TextInputDelegate` observes the new
> `ViewDelegate::onFocusGained`/`onFocusLost` callbacks (added for this — see
> below) to toggle the caret + accent focus-ring, and `onKeyDown` to run the
> edit ops. Element tags `"bg"` / `"label"` / `"caret"` (the `"border"` from
> the sketch below is folded into `elementBorder("bg", …)`, matching Button's
> focus-ring convention). Wired into BasicAppTest (a field + a mirror Label
> that echoes edits via `onValueChange`). **Compile+link clean; this is also
> the first runtime exercise of the FocusManager key-routing path (F3), so it
> awaits an interactive/visual check per AGENTS.md Visual Debugging.**
>
> **New shared infra this required:** `ViewDelegate::onFocusGained` /
> `onFocusLost` virtuals + their dispatch in `ViewDelegate::onRecieveEvent`
> (`FocusManager::setFocus` already emits `FocusGained`/`FocusLost` through the
> view's emitter; nothing routed them to a delegate callback before). Purely
> additive — empty default bodies, two new `case`s.
>
> **Deltas from the v0 sketch below:** (1) caret X is a monospace
> approximation (`kApproxCharAdvance`), *not* measured — a synchronous
> `measureText("label")` right after an edit reads a one-keystroke-stale memo
> (cache keyed on `(tag,width)`, invalidated a frame later in
> `resolveStyles()`), which looks worse than a clean approximation; per-glyph
> caret is a follow-up that drives `measureText` (now width-capable, see
> Text-Measurement-API-Plan §6) from the layout phase. (2) `Home`/`End` caret
> moves added. Everything else matches the sketch.

The original v0 shape (design of record):

```cpp
struct OMEGAWTK_EXPORT TextInputProps {
    OmegaCommon::UString placeholder {};
    OmegaCommon::UString initialValue {};
    bool                 enabled = true;
    float                cornerRadius = 4.f;
};

class OMEGAWTK_EXPORT TextInput : public Widget {
    TextInputProps                                       props_;
    OmegaCommon::UString                                 text_ {};
    std::size_t                                          caretPosition_ = 0;
    Native::ThemeDesc                                    theme_ {};
    std::function<void(const OmegaCommon::UString &)>    onValueChange_ {};
    Core::UniquePtr<class TextInputDelegate>             delegate_;
    // Phase 4B+: selection_, caret blink timer, IME pre-edit string.
protected:
    void onMount() override;
    void onThemeSet(Native::ThemeDesc & desc) override;
    void resize(Composition::Rect & newRect) override;
    void rebuildContent();    // bg / border / label / caret element rects
    void rebuildStyle();      // theme-driven colors, focus-ring on/off
public:
    explicit TextInput(Composition::Rect rect, const TextInputProps & props = {});
    ~TextInput() override;
    void setText(const OmegaCommon::UString & text);
    const OmegaCommon::UString & text() const;
    void setOnValueChange(std::function<void(const OmegaCommon::UString &)> callback);
};
```

Element tags: `"bg"` (RoundedRect background), `"border"` (focus ring, hidden when not `View::isFocused()`), `"label"` (displayed text or dimmed placeholder), `"caret"` (1px-wide `Shape::Rect`, only present when focused).

v0 scope:
- Single line; no wrapping.
- Editing: append (printable chars), backspace, delete, left/right arrow. **No** selection, **no** IME, **no** clipboard.
- Caret X is computed from `props_.font` size × `caretPosition_` with a `* 0.6f` mono-approximation until Phase 2A `Font::measureText` ships. Accurate caret positioning is Phase 4B+.
- Caret blink: deferred to a follow-up that introduces a timer surface (see [Native-API-Completion-Proposal §2.4 `NativeTimer`](Native-API-Completion-Proposal.md#24-nativeapp--lifecycle-arguments-timers)).

Out of scope for the base — each is its own follow-up:

| Follow-up               | Depends on                                          |
|-------------------------|------------------------------------------------------|
| Caret blink             | `NativeTimer` (§2.4)                                |
| Accurate caret X        | Phase 2A `Font::measureText`                         |
| Selection (Shift+arrow, double-click word, click+drag) | accurate caret X |
| Clipboard (Cmd/Ctrl+C/V/X) | `NativeClipboard` (§2.6)                        |
| IME pre-edit + commit   | platform IME bridges (separate plan)                 |
| Tab-traversal sequencing | `FocusManager::focusNext/Previous`                  |
| Multi-line (`TextArea`) | line breaking + scroll                               |

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

### 5B. List

Virtualized vertical (or horizontal) list. Only children visible in the scroll viewport are mounted.

```cpp
class ListDataSource {
public:
    virtual ~ListDataSource() = default;
    virtual std::size_t itemCount() = 0;
    virtual float itemHeight(std::size_t index) = 0; // or itemSize for horizontal
    virtual WidgetPtr createItem(std::size_t index) = 0;
    virtual void recycleItem(std::size_t index, WidgetPtr widget) {}
};

class List : public Widget {
    ListDataSource *dataSource_ = nullptr;
    // Internal: ScrollableContainer + item pool + visible range tracking
public:
    explicit List(Composition::Rect rect);
    void setDataSource(ListDataSource *source);
    void reloadData();
    void scrollToItem(std::size_t index);
};
```

### 5C. Tree

Hierarchical expandable list built on top of `List`.

```cpp
class TreeNode {
public:
    virtual ~TreeNode() = default;
    virtual OmegaCommon::UString label() = 0;
    virtual std::size_t childCount() = 0;
    virtual TreeNode *childAt(std::size_t index) = 0;
    virtual bool isExpandable() { return childCount() > 0; }
};

class Tree : public Widget {
    TreeNode *rootNode_ = nullptr;
    // Internal: flattened visible list driven through List
public:
    explicit Tree(Composition::Rect rect);
    void setRootNode(TreeNode *root);
    void expandNode(TreeNode *node);
    void collapseNode(TreeNode *node);
};
```

### 5D. Table

Columnar data with sorting and column resize.

```cpp
struct TableColumn {
    OmegaCommon::UString title {};
    float width = 100.f;
    float minWidth = 40.f;
    bool resizable = true;
    bool sortable = false;
};

class TableDataSource {
public:
    virtual ~TableDataSource() = default;
    virtual std::size_t rowCount() = 0;
    virtual OmegaCommon::UString cellValue(std::size_t row, std::size_t column) = 0;
};

class Table : public Widget {
    OmegaCommon::Vector<TableColumn> columns_ {};
    TableDataSource *dataSource_ = nullptr;
    // Internal: header row (HStack) + virtualized row list (List)
public:
    explicit Table(Composition::Rect rect);
    void setColumns(const OmegaCommon::Vector<TableColumn> & columns);
    void setDataSource(TableDataSource *source);
};
```

### 5E. Collection

Grid/flow layout collection with item reuse.

```cpp
class CollectionDataSource {
public:
    virtual ~CollectionDataSource() = default;
    virtual std::size_t itemCount() = 0;
    virtual Composition::Rect itemSize(std::size_t index) = 0;
    virtual WidgetPtr createItem(std::size_t index) = 0;
};

class Collection : public Widget {
    CollectionDataSource *dataSource_ = nullptr;
    // Internal: ScrollableContainer + Grid-like placement + item pool
public:
    explicit Collection(Composition::Rect rect);
    void setDataSource(CollectionDataSource *source);
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

- `wtk/src/Widgets/Collections.List.cpp`
- `wtk/src/Widgets/Collections.Tree.cpp`
- `wtk/src/Widgets/Collections.Table.cpp`
- `wtk/src/Widgets/Collections.Collection.cpp`
- `wtk/src/Widgets/Collections.PropertyGrid.cpp`

### Verification

- `ScrollableContainer` scrolls content beyond viewport bounds.
- `List` renders 10,000 items without mounting all of them (check child count).
- `Tree` expand/collapse toggles visibility of subtree items.
- `Table` column headers drag-resize; sort callback fires.
- `Collection` reflows items on resize.

---

## Phase 6: Overlay and Feedback Widgets

**Goal:** Implement overlays from catalog section G.

> **Authoritative spec for the overlay layer itself:** [Overlay-Z-Order-Plan.md](Overlay-Z-Order-Plan.md). That plan resolves the long-standing "are overlays Widgets, Views, or something else?" question, defines the per-window `OverlayHost`, and specifies how the FrameBuilder paint walk gets overlays above the main `WidgetSlice` set. This Phase 6 section is **just the per-widget recipes** — `Tooltip`, `Popover`, `ContextMenu`, `Modal`, etc. — which assume that plan's infrastructure is in place.
>
> **One-line summary from that plan:** Overlays are **Widgets**. They live in the `OverlayHost` slot on `WidgetTreeHost` (not in the main widget tree). The FrameBuilder paint walker visits the main tree first, then visits the overlay slot last, so overlay `WidgetSlice`s land above every main-tree slice in the resulting `CompositeFrame`. Overlays are **window-bound** — they are clipped to the hosting `AppWindow` (or `AppPanel`, which has its own independent `OverlayHost`). Content that needs to live *outside* an `AppWindow` (floating tool palettes, tear-off inspectors) is a separate construct — an `AppPanel` ([Panels-And-Window-Customization-Plan Part A](Panels-And-Window-Customization-Plan.md#part-a--detached-panels)) — not an overlay.

### 6A. Overlay Infrastructure

The `OverlayHost` API is defined in [Overlay-Z-Order-Plan.md §3](Overlay-Z-Order-Plan.md#3-overlayhost-api). The recipes below use it as:

```cpp
class OverlayHost {                          // defined in Overlay-Z-Order-Plan.md
public:
    // Tier governs both paint order and dismissal precedence.
    enum class Tier : uint8_t { Floating, Modal, Tooltip, DragGhost };

    OverlayHandle present(WidgetPtr overlay,
                          Tier tier,
                          const OverlayAnchor & anchor,
                          OverlayDismissPolicy policy);
    void dismiss(OverlayHandle handle);
    void dismissAll(Tier tier);
};
```

The host lives on `WidgetTreeHost` (one per `AppWindow` / `AppPanel`); widgets access it via `treeHost->overlayHost()`.

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

### 6D. PopupMenu / ContextMenu

Command list with keyboard navigation.

```cpp
struct PopupMenuItem {
    OmegaCommon::UString title {};
    OmegaCommon::String shortcut {};
    std::function<void()> action = nullptr;
    bool enabled = true;
    bool separator = false;
    OmegaCommon::Vector<PopupMenuItem> submenu {};
};

class ContextMenu : public Widget {
    OmegaCommon::Vector<PopupMenuItem> items_ {};
public:
    explicit ContextMenu(const OmegaCommon::Vector<PopupMenuItem> & items);
    void present(Widget *anchor, Composition::Point2D screenPos);
    void dismiss();
};
```

Note: The existing `Menu.h` handles native menus (`Menu`/`MenuItem`, backed by `NativeMenu`). `PopupMenu`/`ContextMenu` here are in-view rendered menus for custom styling — hence the distinct `PopupMenuItem` model.

### 6E. Modal

Blocking workflow container with focus trap.

```cpp
class Modal : public Container {
    bool visible_ = false;
    // Backdrop overlay + centered content container
public:
    explicit Modal(Composition::Rect rect);
    void present();
    void dismiss();
    void setOnDismiss(std::function<void()> callback);
};
```

### 6F. Snackbar / Banner

Non-blocking notifications with auto-dismiss.

```cpp
enum class SnackbarPosition : uint8_t { Top, Bottom, TopRight, BottomRight };

struct SnackbarProps {
    OmegaCommon::UString message {};
    float durationMs = 3000.f;
    SnackbarPosition position = SnackbarPosition::Bottom;
};

class Snackbar : public Widget {
public:
    static void show(Widget *host, const SnackbarProps & props);
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
- `wtk/src/Widgets/Overlays.cpp` — Tooltip, Popover, PopupMenu, ContextMenu, Modal, Snackbar, Sheet

### Verification

- Tooltip appears on hover after delay, dismisses on exit.
- Popover anchors correctly to each edge of the target widget.
- ContextMenu keyboard navigation (arrow keys, Enter, Escape).
- Modal traps focus; backdrop click dismisses.
- Snackbar auto-dismisses after timeout; queue prevents stacking.
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

### Cross-plan dependencies

The graph above is intra-plan only. The phases below also pull in work tracked in other `.plans/` documents. Listed by which phase here consumes which external piece. Plans that *consume from* this plan (e.g. [Overlay-Z-Order-Plan §12](Overlay-Z-Order-Plan.md#12-cross-plan-dependencies)) list their inward dependencies in their own dependencies section.

| This plan, phase                                                                                          | External dep                                                                                                                                                                       | Why                                                                                                                                                                                       |
|-----------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Phase 4A Button** (base)                                                                                | None at the plan-doc level. Uses existing Phase 0 (`WidgetInteractionDelegate`), `UIView`, `Style`, `ThemeDesc`.                                                                    | Buildable today.                                                                                                                                                                          |
| **Phase 4B TextInput base** (deferred)                                                                    | [Native-API-Completion-Proposal §2.3a Focus](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) **steps 1–3** (`FocusPolicy`, `FocusManager` skeleton, key routing)    | TextInput must claim keyboard focus and receive `KeyDown` from `WidgetTreeHost`. Without FocusManager the routing is undefined.                                                            |
| **Phase 4B caret blink** follow-up                                                                        | [Native-API-Completion-Proposal §2.4 NativeTimer](Native-API-Completion-Proposal.md#timers)                                                                                          | Blink needs a recurring timer.                                                                                                                                                            |
| **Phase 4B accurate caret X** follow-up                                                                    | This plan's own **Phase 2A Text Measurement** (`Composition::Font::measureText`)                                                                                                     | Caret position must align with glyph advances.                                                                                                                                            |
| **Phase 4B clipboard (Cmd/Ctrl+C/V/X)** follow-up                                                          | [Native-API-Completion-Proposal §2.6 NativeClipboard](Native-API-Completion-Proposal.md#26-nativeclipboard-new)                                                                       | Clipboard read/write.                                                                                                                                                                     |
| **Phase 4B tab traversal across forms** follow-up                                                          | [Native-API-Completion-Proposal §2.3a Focus](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) **steps 4 + 6** (`focusNext` / `focusPrevious`, `setTabOrder`)           | Tab/Shift-Tab across multiple TextInputs.                                                                                                                                                 |
| **Phase 6 overlay widget hosts** (Tooltip, Popover, Snackbar, Sheet)                                       | [Overlay-Z-Order-Plan O1–O3](Overlay-Z-Order-Plan.md#9-phases) (OverlayHost + paint walk + dismissal)                                                                                | These widgets are mounted via `OverlayHost::present`; their paint/dismiss is owned by the host.                                                                                            |
| **Phase 6 `ContextMenu` / `PopupMenu` focus return on dismiss**                                            | [Overlay-Z-Order-Plan O4](Overlay-Z-Order-Plan.md#9-phases) + Native-API §2.3a Focus **step 5**                                                                                       | Dismissing the menu must return focus to the opener with `FocusReason::Popup` (so the focus ring re-appears).                                                                              |
| **Phase 6 `Modal` focus trap**                                                                             | [Overlay-Z-Order-Plan O5](Overlay-Z-Order-Plan.md#9-phases) + Native-API §2.3a Focus **step 4**                                                                                       | Tab/Shift-Tab inside a Modal must not reach widgets behind it.                                                                                                                            |
| **Phase 6 `Select` / dropdown windowing**                                                                  | [Overlay-Z-Order-Plan O1–O3](Overlay-Z-Order-Plan.md#9-phases) only.                                                                                                                | Dropdowns are window-bound like every other overlay. Long lists near the window edge clip or reposition via anchor edge-clamping; there is no `AppPanel` escape hatch ([Overlay-Z-Order-Plan §7](Overlay-Z-Order-Plan.md#7-relationship-to-apppanel--they-are-separate-not-coupled)). |
| **Phase 6 `Tooltip` rendering surface**                                                                    | [Native-API-Completion-Proposal §2.3a Tooltip](Native-API-Completion-Proposal.md#tooltip--per-widget-virtual-popup) (the dispatcher and `Widget::setTooltip` API)                    | Tooltips are dispatcher-constructed (single-Label overlay); the per-widget recipe in Phase 6 is just defaults + how widgets call `setTooltip`.                                              |
| **Phase 6 `Tooltip` activation timer**                                                                     | [Native-API-Completion-Proposal §2.4 NativeTimer](Native-API-Completion-Proposal.md#timers)                                                                                          | Hover delay before the tooltip appears.                                                                                                                                                   |
| **Phase 9 `FocusRingHost`**                                                                                | [Native-API-Completion-Proposal §2.3a Focus](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) (full)                                                                  | Reads `FocusManager::focusedView()` + `lastFocusReason()` to know when to draw the ring.                                                                                                  |
| **Phase 9 `InspectorPanel`**                                                                               | [Native-API-Completion-Proposal §2.10 NativeAccessibility](Native-API-Completion-Proposal.md#210-nativeaccessibility-new) (when promoted past stubs)                                  | Inspector reads accessibility metadata from widgets.                                                                                                                                      |

Three external chains gate most of the cross-plan blocking work:

1. **Native-API §2.3a Focus + Tooltip** — unblocks Phase 4B TextInput, Phase 6 ContextMenu/Modal/Tooltip, Phase 9 FocusRingHost. Internally landed in 6 steps; only steps 1–3 are needed for the immediate-next batch.
2. **Native-API §2.4 NativeTimer** — unblocks Phase 4B caret blink, Phase 6 Tooltip activation. Independent of (1) and (3).
3. **Overlay-Z-Order-Plan** — unblocks all of Phase 6. Internally depends on (1) for its O4/O5 phases. O1–O3 land independently of (1).

`AppPanel` ([Panels-And-Window-Customization-Plan Part A](Panels-And-Window-Customization-Plan.md#part-a--detached-panels)) is *not* in any of those chains. It is a separate top-level surface for content that lives outside an `AppWindow` (tool palettes, tear-off inspectors), not a render mode for overlays — overlays are window-bound by design ([Overlay-Z-Order-Plan §7](Overlay-Z-Order-Plan.md#7-relationship-to-apppanel--they-are-separate-not-coupled)). An application that wants window-escaping floating UI constructs an `AppPanel` directly; the Widget Stub plan does not depend on Panels Part A for any phase.

No cycles.

1. **Icon system**: Font-based glyphs or SVG icons? Or both with a unified `IconProvider` interface?
2. **Text measurement**: Does `Composition::Font` currently expose text extent measurement? If not, that's a prerequisite for `Label::measureSelf` and all text-based intrinsic sizing.
3. ~~**Focus management**: Is there a global focus tracking system, or does each widget manage focus independently? Overlays and `Modal` need focus trapping.~~ **Resolved.** [Native-API-Completion-Proposal §2.3a](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) defines a per-window `FocusManager` owned by `WidgetTreeHost`, with `FocusPolicy` per View, a `FocusReason` enum that gates focus-ring visibility (keyboard reasons show the ring, mouse reasons don't), and `pushRestorationPoint`/`popAndRestore` for modal/popover dismissal. Modal tab-trap is [Overlay-Z-Order-Plan O5](Overlay-Z-Order-Plan.md#9-phases).
4. **Theme system**: Should interactive state styles (hover, pressed, disabled) be driven by `StyleSheet` state variants, or by swapping entire stylesheets?
5. **Select popup**: Should the dropdown overlay use the native `Menu` system (from `Menu.h`) or a custom in-view overlay? Native gives platform-correct behavior; custom gives full style control.
6. **Button inheritance**: The current stub has `Button : public Container`. The catalog suggests it should support icon + text composition, which justifies Container. But simple buttons could just be `Widget`. Keep `Container` to support flexible content?
7. **Controlled vs uncontrolled state**: The catalog mandates both modes for every widget. How should this be surfaced in the API? Optional `WidgetState<T>` binding parameter in `Create`?
