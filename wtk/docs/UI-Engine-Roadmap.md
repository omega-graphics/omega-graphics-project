# OmegaWTK: Roadmap to a Fully Capable UI Engine

This document outlines a phased roadmap and the complete widget set required to bring OmegaWTK to the level of a **fully capable UI engine**—suitable for building production desktop and embedded applications with modern layout, input, collections, overlays, and accessibility.

Related documents:

- [Widget-Type-Catalog-Proposal.md](Widget-Type-Catalog-Proposal.md) — Detailed catalog and customization contract
- [Layout-API-Detailed-Spec.md](Layout-API-Detailed-Spec.md) — Layout types and resolver semantics
- [View-Widget-Detailed-Layout-API-Plan.md](View-Widget-Detailed-Layout-API-Plan.md) — Layout API evolution

---

## 1. Current State Summary

### What Exists Today

| Area | Status | Notes |
|------|--------|--------|
| **Core** | ✅ | `Widget`, `View`, `CanvasView`, `ViewResizeCoordinator`, `WidgetTreeHost`, paint lifecycle |
| **Layout** | ✅ In progress | `Layout.h` / `Layout.cpp`: units, `LayoutStyle`, `resolveClampedRect`, `LayoutContext`, transitions, diagnostics; `UIViewLayoutV2`; widget layout API (slice B); StyleSheet layout entries |
| **Composition** | ✅ | `UIView`, `UIViewLayout`, `StyleSheet`, shapes (Rect, RoundedRect, Ellipse, Path), text, effects (shadow, blur), `ViewAnimator` / `LayerAnimator` |
| **Containers** | ✅ Partial | `Container` (clamp policy, child geometry), `StackWidget` → `HStack` / `VStack` (stack layout); `AnimatedContainer`, `ScrollableContainer` placeholders |
| **Input widgets** | ⚠️ Stub | `Label`, `TextInput`, `Button` (constructors only; no real behavior) |
| **Native** | ✅ | `AppWindow`, `Menu`, native menus/dialogs, theme observer; platform layers (macOS, Windows, GTK) |
| **Media** | ✅ | `VideoView`, `SVGView`, media/document hooks |

### Gaps to Close

- **Layout**: Full widget-tree driver using new layout API; `ZStack`, `Grid`, `Spacer`, `SplitView`, `Tabs`; optional diagnostic sink in production.
- **Primitives**: First-class shape widgets (`Rectangle`, `RoundedRectangle`, `Ellipse`, `Path`), `Image`, `Icon`, `Separator`.
- **Text & input**: Real `Label` (wrapping, overflow), `TextInput` (caret, selection, IME), `TextArea`, `Button` (content, states), `Toggle`, `Checkbox`, `RadioGroup`, `Slider`, `Stepper`, `Select`, `ProgressBar`, `Spinner`.
- **Collections**: Virtualized `ListView`, `TreeView`, `TableView`, `CollectionView`, `PropertyGrid`.
- **Navigation & shell**: `NavigationStack`, `Sidebar`, `Breadcrumb`, `Toolbar`, `StatusBar`.
- **Overlays**: `Tooltip`, `Popover`, in-view `Menu`/`ContextMenu`, `ModalDialog`, `Toast`/`Banner`, `Sheet`.
- **Media/document**: `VideoViewWidget`, `AudioPlayerWidget`, `SVGViewWidget`, `PDFView` (optional), `CanvasWidget`.
- **Accessibility & tooling**: `FocusRingHost`, keyboard/shortcut hints, optional `InspectorPanel`.

---

## 2. Roadmap Phases

### Phase 1 — Layout & composition maturity *(current / short-term)*

**Goal:** Layout and composition are the single source of truth for geometry and styling; no regressions for existing apps.

| Milestone | Deliverables |
|-----------|----------------|
| 1.1 Layout API complete | Widget layout driver uses `runWidgetLayout`; `StackLayoutBehavior`, `LegacyResizeCoordinatorBehavior`; DPI in `LayoutContext`; optional diagnostic sink. |
| 1.2 UIView v2 default path | `useLayoutV2()` as supported path; StyleSheet layout merge; transition bridge for element rect deltas. |
| 1.3 Container/Stack alignment | `Container` and `StackWidget` use new layout behaviors where applicable; deterministic resize. |
| 1.4 Testing & hardening | Layout unit tests, `LayoutResizeStressTest`, multi-DPI; BasicAppTest, TextCompositorTest, EllipsePathCompositorTest, ContainerClampAnimationTest as regression gate. |

**Exit criteria:** All layout slices (A–F) implemented and tested; legacy path unchanged; v2 path ready for adoption.

---

### Phase 2 — Foundation and layout completeness

**Goal:** All core layout widgets exist and can be composed for full-page layouts.

| Milestone | Deliverables |
|-----------|----------------|
| 2.1 ZStack | Overlay layout behavior; z-order and insets; dialogs/badges/HUDs. |
| 2.2 Grid | Row/column layout with span; forms and dashboards. |
| 2.3 Spacer | Flexible empty slot in stack layouts. |
| 2.4 SplitView | Resizable panes; divider drag. |
| 2.5 Tabs + TabItem | Tab bar + content switching. |
| 2.6 ScrollableContainer | Real implementation using `ScrollView`; scrollable content host. |

**Exit criteria:** Any full-page layout can be expressed with stacks, grid, split, tabs, and scroll.

---

### Phase 3 — Primitives, text, and input

**Goal:** All display primitives and essential text/input widgets are usable and styleable.

| Milestone | Deliverables |
|-----------|----------------|
| 3.1 Shape widgets | `Rectangle`, `RoundedRectangle`, `Ellipse`, `Path` as widgets (filled/stroked; DPI-aware). |
| 3.2 Image & Icon | `Image` (bitmap, fit modes); `Icon` (symbol/glyph, theming). |
| 3.3 Separator | Horizontal/vertical divider; insets and style. |
| 3.4 Label | Real implementation: wrapping, overflow, multiline, style from StyleSheet. |
| 3.5 Button | Content (text + optional icon), states (normal/hover/pressed/disabled), action callback. |
| 3.6 TextInput | Single-line; caret, selection, IME; focus. |
| 3.7 TextArea | Multi-line text editing. |
| 3.8 Toggle, Checkbox, RadioGroup | Boolean and single-select with controlled/uncontrolled mode. |
| 3.9 Slider, Stepper | Continuous and discrete numeric input. |
| 3.10 Select/Dropdown | Option list; optional search. |
| 3.11 ProgressBar, Spinner | Determinate/indeterminate progress; busy indicator. |

**Exit criteria:** Forms and simple wizards can be built with labels, inputs, buttons, and toggles.

---

### Phase 4 — Collections and navigation

**Goal:** Data-heavy UIs and multi-page/app-structure patterns are supported.

| Milestone | Deliverables |
|-----------|----------------|
| 4.1 List | Virtualized list; cell reuse; stable key model; vertical/horizontal. |
| 4.2 Tree | Hierarchical list; expand/collapse; selection. |
| 4.3 Table | Columnar data; sorting; column resize; selection model. |
| 4.4 Collection | Grid/flow layout with reusable item renderer. |
| 4.5 PropertyGrid | Label/value editing panel for tooling. |
| 4.6 NavigationStack | Push/pop page flow (wizard/mobile-style). |
| 4.7 Sidebar | Section/category navigation (desktop shell). |
| 4.8 Breadcrumb, Toolbar, StatusBar | Path navigation; action strip; app-wide status. |

**Exit criteria:** File browsers, settings panels, and multi-page flows are feasible.

---

### Phase 5 — Overlays and feedback

**Goal:** Modern overlay and feedback patterns are available and accessible.

| Milestone | Deliverables |
|-----------|----------------|
| 5.1 Tooltip | Hover/focus hint; positioning and collision. |
| 5.2 Popover | Anchored floating panel. |
| 5.3 Menu / ContextMenu | In-view command list; keyboard navigation; roles. |
| 5.4 ModalDialog | Blocking dialog; focus trap; accessibility. |
| 5.5 Toast / Banner | Non-blocking notifications; queue and timeout. |
| 5.6 Sheet | Partial-screen/modal panel (desktop/mobile). |

**Exit criteria:** Menus, dialogs, toasts, and contextual UI work consistently across the toolkit.

---

### Phase 6 — Media, accessibility, and tooling

**Goal:** Media/document use cases and production readiness (a11y, shortcuts, debugging).

| Milestone | Deliverables |
|-----------|----------------|
<!-- | 6.1 VideoViewWidget, AudioPlayerWidget | Widget wrappers over VideoView; transport + optional waveform/levels. |
| 6.2 SVGViewWidget, CanvasWidget | SVG document widget; immediate-mode drawing escape hatch. | -->
| 6.3 PDFViewer (optional) | Multi-page document view for desktop. |
| 6.4 FocusRingHost, shortcut hints | Centralized focus visuals; keyboard shortcut annotation. |
| 6.5 InspectorPanel (optional) | Runtime property inspection for devtools. |

**Exit criteria:** Media and document workflows are supported; focus and shortcuts are consistent; optional tooling available.

---

## 3. Widget List: Full Set for a Capable UI Engine

The following table lists every widget type that, once implemented, brings OmegaWTK to a **fully capable UI engine**. Status: **Done** (exists and functional), **Partial** (exists but incomplete), **Stub** (declared only), **Planned** (not yet implemented).

### 3.1 Foundation

| Widget | Purpose | Status |
|--------|---------|--------|
| `Widget` | Base visual/control unit | **Done** |
| `Container` | Parent-managed child host, clamp policy | **Done** |
| `ScrollableContainer` | Scroll viewport + content host | **Stub** |

### 3.2 Layout

| Widget | Purpose | Status |
|--------|---------|--------|
| `HStack` | Horizontal flow | **Done** |
| `VStack` | Vertical flow | **Done** |
| `ZStack` | Overlay stacking, z-order | **Planned** |
| `Grid` | Row/column with span | **Planned** |
| `Spacer` | Flexible empty slot | **Planned** |
| `SplitView` | Resizable panes | **Planned** |
| `Tabs` + `TabItem` | Tab bar + content switching | **Planned** |

### 3.3 Display primitives

| Widget | Purpose | Status |
|--------|---------|--------|
| `Rectangle` | Filled/stroked rect | **Planned** |
| `RoundedRectangle` | Rounded rect, independent radii | **Planned** |
| `Ellipse` | Filled/stroked ellipse | **Planned** |
| `Path` | Vector path stroke/fill | **Planned** |
| `Image` | Bitmap, fit modes | **Planned** |
| `Icon` | Symbol/glyph, theming | **Planned** |
| `Separator` | Divider (H/V), inset | **Planned** |

### 3.4 Text and input

| Widget | Purpose | Status |
|--------|---------|--------|
| `Label` | Static/rich text, wrap/overflow | **Stub** |
| `TextInput` | Single-line, caret/selection/IME | **Stub** |
| `TextArea` | Multi-line editing | **Planned** |
| `Button` | Action trigger, icon+text | **Stub** |
| `Toggle` | On/off switch | **Planned** |
| `Checkbox` | Multi-select boolean | **Planned** |
| `RadioGroup` + `RadioButton` | Single-select options | **Planned** |
| `Slider` | Continuous value | **Planned** |
| `Stepper` | Discrete numeric | **Planned** |
| `Select` / `Dropdown` | Option list | **Planned** |
| `ProgressBar` | Determinate/indeterminate | **Planned** |
| `Spinner` | Busy indicator | **Planned** |

### 3.5 Collections and data

| Widget | Purpose | Status |
|--------|---------|--------|
| `ListView` | Virtualized list | **Planned** |
| `TreeView` | Hierarchical list | **Planned** |
| `TableView` | Columnar data, sort/resize | **Planned** |
| `CollectionView` | Grid/flow + item renderer | **Planned** |
| `PropertyGrid` | Label/value editing panel | **Planned** |

### 3.6 Navigation and app structure

| Widget | Purpose | Status |
|--------|---------|--------|
| `NavigationStack` | Push/pop pages | **Planned** |
| `Sidebar` | Section navigation | **Planned** |
| `Breadcrumb` | Path navigation | **Planned** |
| `Toolbar` | Action strip | **Planned** |
| `StatusBar` | App status strip | **Planned** |

### 3.7 Overlays and feedback

| Widget | Purpose | Status |
|--------|---------|--------|
| `Tooltip` | Hover/focus hint | **Planned** |
| `Popover` | Anchored panel | **Planned** |
| `Menu` / `ContextMenu` | Command list (in-view) | **Planned** (native `Menu` exists) |
| `ModalDialog` | Blocking dialog | **Planned** |
| `Toast` / `Banner` | Non-blocking notification | **Planned** |
| `Sheet` | Partial-screen panel | **Planned** |

### 3.8 Media and document

| Widget | Purpose | Status |
|--------|---------|--------|
| `VideoViewWidget` | Playback/capture (widget wrapper) | **Planned** |
| `AudioPlayerWidget` | Transport + waveform/level | **Planned** |
| `SVGViewWidget` | Vector document (widget wrapper) | **Planned** |
| `PDFView` | Multi-page document | **Planned** (optional) |
| `CanvasWidget` | Custom drawing surface | **Planned** |

### 3.9 Accessibility and tooling

| Widget / component | Purpose | Status |
|--------------------|---------|--------|
| `FocusRingHost` | Focus visuals | **Planned** |
| `ShortcutHint` | Keyboard shortcut annotation | **Planned** |
| `InspectorPanel` | Runtime property inspection | **Planned** (optional) |

---

## 4. Recommended delivery order

Aligns with the [Widget-Type-Catalog-Proposal](Widget-Type-Catalog-Proposal.md) and the phases above:

1. **Foundation + layout** — Phase 2: `ZStack`, `Grid`, `Spacer`, `SplitView`, `Tabs`; complete `ScrollableContainer`, `AnimatedContainer`.
2. **Text and input essentials** — Phase 3: `Label`, `TextInput`, `Button`, `Toggle`, `Select` (then remaining inputs).
3. **Collections** — Phase 4: `ListView`, `TableView`, `TreeView`.
4. **Overlays** — Phase 5: `Tooltip`, `Popover`, `ModalDialog`, `Toast`.
5. **Media and document** — Phase 6: `VideoViewWidget`, `SVGViewWidget`, `CanvasWidget`.
6. **Accessibility and tooling** — Phase 6: focus, shortcuts, optional inspector.

---

## 5. API and quality principles

- **Customization contract:** Every widget supports style override, layout override, state override (controlled/uncontrolled), composition override (`onPaint`), event override, and animation override (see [Widget-Type-Catalog-Proposal](Widget-Type-Catalog-Proposal.md)).
- **Consistency:** Props/State/Style surfaces; stable test IDs/tags; declarative and imperative usage.
- **Regression safety:** After each phase, existing tests (BasicAppTest, TextCompositorTest, EllipsePathCompositorTest, ContainerClampAnimationTest, LayoutResizeStressTest) must remain green.

This roadmap and widget list define the path to a fully capable OmegaWTK UI engine while keeping the current codebase stable and incremental.
