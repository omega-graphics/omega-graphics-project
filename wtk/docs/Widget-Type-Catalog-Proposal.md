# OmegaWTK Widget Type Catalog Proposal

## Objective
Define a complete, practical widget catalog that makes OmegaWTK a fully usable UI toolkit while preserving full customization for rendering, behavior, layout, animation, and input handling.

## Customization Contract (Applies to Every Widget)
Every widget type below should support:

1. Full style override (background, border, radius, brush, text, effects, transitions).
2. Full layout override (explicit rect, intrinsic size, min/max, flex/stack/grid participation).
3. Full state override (controlled/uncontrolled mode, external state binding).
4. Composition override (`onPaint`, custom layers, custom canvas draws).
5. Event override (pointer, keyboard, focus, accessibility actions).
6. Animation override (timeline-driven, transition-driven, or custom curve/controller).

## A. Foundation Widgets
These are mandatory building blocks.

| Widget Type | Purpose | Notes |
|---|---|---|
| `Widget` | Base visual/control unit | Already present; keep low-level extensibility primary. |
| `Container` | Parent-managed child host with clamp policy | New geometry authority for child proposals. |
| `AnimatedContainer` | Container with timeline/state transitions built-in | Existing placeholder; should layer on animation runtime. |
| `ScrollableContainer` | Container with scroll viewport and content host | Existing placeholder; integrate with `ScrollView` behavior. |
| `UIViewWidget` | Widget wrapper around `UIView` layout+stylesheet | High-level shape/text composition surface. |

## B. Layout Widgets
For application-scale page composition.

| Widget Type | Purpose | Notes |
|---|---|---|
| `HStack` | Horizontal flow layout | Already present; migrate to `Container` geometry delegation. |
| `VStack` | Vertical flow layout | Already present; migrate to `Container` geometry delegation. |
| `ZStack` | Overlay/absolute stacking with z-order | Required for dialogs, badges, HUDs. |
| `Grid` | Row/column layout with span support | Essential for forms, dashboards. |
| `Spacer` | Flexible empty slot | Needed for ergonomic stack layout APIs. |
| `SplitView` | Resizable pane layout | For editors/tools apps. |
| `Tabs` + `TabItem` | Section switching container | Needed for multi-panel app UX. |

## C. Display/Primitive Widgets
Core visual primitives as first-class widgets.

| Widget Type | Purpose | Notes |
|---|---|---|
| `Rectangle` | Filled/stroked rect | Core primitive, corner cases for DPI and stroke alignment. |
| `RoundedRectangle` | Filled/stroked rounded rect | Must support independent corner radii. |
| `Ellipse` | Filled/stroked ellipse | Already exercised in compositor tests. |
| `Path` | Vector path stroke/fill | Must support joins/caps/contour/fill rules. |
| `Image` | Bitmap display with fit modes | Include crop, contain, cover, center. |
| `Icon` | Symbol/glyph display widget | Token-based icon theming support. |
| `Separator` | Horizontal/vertical divider | Styling + inset support. |

## D. Text and Input Widgets
Minimum required interactive UI set.

| Widget Type | Purpose | Notes |
|---|---|---|
| `Label` | Static/rich text display | Existing placeholder; add wrapping/overflow/multiline controls. |
| `TextInput` | Single-line text input | Existing placeholder; selection/caret/IME required. |
| `TextArea` | Multi-line text editing | Needed for forms/editors. |
| `Button` | Action trigger | Existing placeholder; support icon+text composition. |
| `Toggle` | On/off switch | Controlled/uncontrolled support. |
| `Checkbox` | Multi-select boolean | Tri-state optional. |
| `RadioGroup` + `RadioButton` | Single-select options | Group state model. |
| `Slider` | Continuous value input | Horizontal/vertical, step support. |
| `Stepper` | Discrete numeric input | Pair with text value display. |
| `Select`/`Dropdown` | Option list selection | Searchable variant optional. |
| `ProgressBar` | Determinate/indeterminate progress | Required for async operations. |
| `Spinner` | Busy indicator | Lightweight loading state widget. |

## E. Collection and Data Widgets
For real application data surfaces.

| Widget Type | Purpose | Notes |
|---|---|---|
| `ListView` | Virtualized vertical/horizontal list | Cell reuse and stable key model required. |
| `TreeView` | Hierarchical expandable list | Required for file/navigation tools. |
| `TableView` | Columnar data view | Sorting, column resize, selection model. |
| `CollectionView` | Grid/flow item collection | Reusable item renderer API. |
| `PropertyGrid` | Label/value editing panel | Useful for tooling and inspectors. |

## F. Navigation and App Structure Widgets
Needed for multipage/product-grade apps.

| Widget Type | Purpose | Notes |
|---|---|---|
| `NavigationStack` | Push/pop page flow | Mobile-style and wizard-style flows. |
| `Sidebar` | Section/category navigation | Desktop app shell. |
| `Breadcrumb` | Path-based navigation | Works with tree/file navigation. |
| `Toolbar` | Action strip | Icon/button slots and overflow behavior. |
| `StatusBar` | Global status/info strip | App-wide status integration. |

## G. Overlay and Feedback Widgets
Critical for modern UX.

| Widget Type | Purpose | Notes |
|---|---|---|
| `Tooltip` | Hover/focus contextual hint | Positioning and collision strategy. |
| `Popover` | Anchored floating panel | Rich interactive overlay. |
| `Menu`/`ContextMenu` | Command list | Keyboard navigation and role mapping. |
| `ModalDialog` | Blocking workflow container | Focus trap + accessibility semantics. |
| `Toast`/`Banner` | Non-blocking notifications | Queueing and timeout policy hooks. |
| `Sheet` | Partial-screen/modal panel | Desktop/mobile adaptable behavior. |

## H. Media and Document Widgets
Aligned with OmegaWTK roadmap.

| Widget Type | Purpose | Notes |
|---|---|---|
| `VideoViewWidget` | Playback/capture display | Backed by Media API; controls composable. |
| `AudioPlayerWidget` | Transport + waveform/level | Media API integration. |
| `SVGViewWidget` | Vector document rendering | Backed by `Core::XML` parser pipeline. |
| `PDFView` | Multi-page document view | Optional, high-value for desktop workflows. |
| `CanvasWidget` | Immediate-mode custom drawing surface | Escape hatch for bespoke rendering. |

## I. Accessibility and System Integration Widgets
These keep the toolkit production-ready.

| Widget Type | Purpose | Notes |
|---|---|---|
| `FocusRingHost` | Centralized focus visuals | Cross-widget focus consistency. |
| `ShortcutHint` | Keyboard shortcut annotation | Useful in menus/toolbars. |
| `InspectorPanel` | Runtime property inspection UI | Devtools/runtime debugging. |

## Recommended Delivery Order
1. Foundation + Layout (`Container`, `HStack`, `VStack`, `ZStack`, `Grid`).
2. Text/Input essentials (`Label`, `TextInput`, `Button`, `Toggle`, `Select`).
3. Collection widgets (`ListView`, `TableView`, `TreeView`).
4. Overlays (`Tooltip`, `Popover`, `ModalDialog`, `Toast`).
5. Media/Document widgets (`VideoViewWidget`, `SVGViewWidget`, `CanvasWidget`).

## API Consistency Rules
To keep full customization without API sprawl:

1. Every widget exposes `Props`, `State`, and `Style` objects.
2. Every widget has a `render override` escape hatch for custom paint.
3. Every widget supports both declarative data binding and imperative control.
4. Every widget supports effect/animation hooks through the same surface.
5. Every widget has stable test IDs/tags for diagnostics and automation.
