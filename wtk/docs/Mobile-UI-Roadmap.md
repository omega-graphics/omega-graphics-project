# OmegaWTK: Mobile UI Roadmap and Widget Plan

This document defines the **roadmap and widget set for the mobile version** of OmegaWTK (iOS and Android). OmegaWTK is cross-platform: **the vast majority of code is shared**. Mobile builds use the same core—Widget, View, UIView, Layout API, StyleSheet, composition—and add a mobile host layer, touch/gesture handling, safe-area awareness, and mobile-first navigation and widgets where needed.

Related documents:

- [UI-Engine-Roadmap.md](UI-Engine-Roadmap.md) — Desktop/cross-platform roadmap and full widget list
- [Widget-Type-Catalog-Proposal.md](Widget-Type-Catalog-Proposal.md) — Catalog and customization contract
- [Layout-API-Detailed-Spec.md](Layout-API-Detailed-Spec.md) — Layout types (shared)

---

## 1. Cross-Platform Principle: Shared Base, Mobile Target

### What Is Shared (No Fork)

All of the following are **single codebases** used on desktop and mobile:

| Layer | Components | Mobile relevance |
|-------|------------|------------------|
| **Core** | `Widget`, `View`, `CanvasView`, `WidgetTreeHost`, paint lifecycle | Same; mobile is another host. |
| **Layout** | `Layout.h` / `Layout.cpp`, `LayoutStyle`, `LayoutContext`, `resolveClampedRect`, DPI, transitions | Critical; mobile uses high DPI and safe-area insets. |
| **Composition** | `UIView`, `UIViewLayout` / `UIViewLayoutV2`, `StyleSheet`, shapes, text, effects, animators | Same; rendering is backend-agnostic. |
| **Containers** | `Container`, `StackWidget`, `HStack`, `VStack`; (future) `ZStack`, `Grid`, `Spacer`, `ScrollableContainer` | Same; stacks/grid/scroll are universal. |
| **Primitives** | (Future) `Rectangle`, `RoundedRectangle`, `Ellipse`, `Path`, `Image`, `Icon`, `Separator` | Same. |
| **Text & input** | (Future) `Label`, `TextInput`, `TextArea`, `Button`, `Toggle`, `Checkbox`, `RadioGroup`, `Slider`, `Stepper`, `Select`, `ProgressBar`, `Spinner` | Same; touch targets and IME are platform adapters. |
| **Collections** | (Future) `ListView`, `CollectionView`; (optional) `TreeView`, `TableView` | Same; list/grid are primary on mobile. |

Platform-specific code is limited to:

- **Host:** `AppWindow` / windowing and surface creation (iOS `UIWindow`/`UIViewController`, Android `Activity`/`View`).
- **Input:** Touch and gesture translation into the same pointer/keyboard/focus events the rest of the toolkit uses.
- **System UI:** Safe areas, status bar, home indicator, keyboard avoidance—expressed as insets or layout context, not duplicate widgets.
- **Optional platform chrome:** Native nav bar, tab bar, or “material” vs “iOS” look when using platform-specific wrappers.

### Build Targets

The project already defines:

- **`TARGET_IOS`** — iOS (Metal).
- **`TARGET_ANDROID`** — Android (Vulkan).
- **`TARGET_MOBILE`** — Common mobile define for both.

Mobile builds link the same UI library; only the app host and native input/surface code differ.

---

## 2. Mobile-Specific Considerations

These drive the **mobile roadmap** and **mobile-first or mobile-only widgets**, not a second widget set.

| Concern | Approach |
|---------|----------|
| **Touch-first** | Large hit targets (e.g. 44pt min); no hover-only affordances; tap, long-press, swipe as first-class. |
| **Safe areas** | Insets for status bar, notch, home indicator; layout and full-bleed content respect them via a shared primitive (e.g. `SafeAreaContainer` or layout context). |
| **DPI / density** | `LayoutContext::dpiScale` and dp-based layout already support this; ensure mobile host supplies correct scale. |
| **Keyboard** | Avoid overlapping focused input (keyboard avoidance); same `TextInput`/`TextArea`, platform resizes or insets. |
| **Navigation** | Stack (push/pop) and tab bar are primary; bottom nav common; sidebar and breadcrumb are low priority. |
| **Overlays** | Bottom sheet and full-screen modal are primary; tooltip/popover secondary. |
| **Gestures** | Pull-to-refresh, swipe-to-dismiss, swipe actions on list items; either in shared widgets with mobile behavior or thin mobile adapters. |
| **Platform conventions** | iOS vs Material patterns (e.g. tab bar position, back gesture, FAB) can be theming or small adapter layers on shared widgets. |

---

## 3. Mobile Roadmap Phases

Phases assume the **shared** [UI-Engine-Roadmap](UI-Engine-Roadmap.md) advances in parallel (layout maturity, shared widgets). Mobile phases focus on **running on device**, **input**, **safe area**, **navigation**, and **mobile-only or mobile-first widgets**.

### Phase M1 — Mobile host and input

**Goal:** OmegaWTK apps run on iOS and Android with correct display and touch input.

| Milestone | Deliverables |
|-----------|--------------|
| M1.1 Mobile app host | iOS: window + root view controller hosting `WidgetTreeHost` (or equivalent). Android: Activity + surface hosting same. Single shared UI tree. |
| M1.2 Touch and gestures | Touch events mapped to existing pointer/focus model; tap, long-press, optional swipe; same `ViewDelegate` / event path. |
| M1.3 DPI and scaling | `LayoutContext::dpiScale` and available rect from mobile host; dp-based layout correct on device. |
| M1.4 Safe area awareness | API to obtain safe insets (top/bottom/left/right); feed into layout (e.g. root container or layout context) so content is not obscured. |

**Exit criteria:** A minimal app (e.g. single full-screen `VStack` + `Label` + `Button`) runs on iOS and Android, respects safe area, and responds to touch.

---

### Phase M2 — Shared widget mobile readiness

**Goal:** Shared widgets are usable and pleasant on mobile without new widget types.

| Milestone | Deliverables |
|-----------|--------------|
| M2.1 Touch targets | Minimum tap size (e.g. 44×44 dp) for `Button`, `Toggle`, list rows, tab items; configurable. |
| M2.2 ScrollableContainer on mobile | `ScrollableContainer` (shared) works with touch scrolling and momentum; optional overscroll/bounce per platform. |
| M2.3 Keyboard avoidance | When `TextInput`/`TextArea` focus, layout or host adjusts so field stays visible (inset or resize). |
| M2.4 ListView/CollectionView on mobile | Shared `ListView`/`CollectionView` work with touch scroll and cell reuse on mobile. |

**Exit criteria:** Forms (labels, inputs, buttons) and simple lists work on device with comfortable touch and no keyboard overlap.

---

### Phase M3 — Mobile navigation and shell

**Goal:** Standard mobile navigation patterns are available using shared + thin mobile-specific pieces.

| Milestone | Deliverables |
|-----------|--------------|
| M3.1 NavigationStack | Shared push/pop stack; optional platform back gesture (iOS swipe-from-edge, Android back). |
| M3.2 TabBar (mobile) | Tab bar for switching content; mobile: often bottom-aligned; shared `Tabs`/`TabItem` with platform-aware default. |
| M3.3 BottomNavBar | Mobile-only or mobile variant: bottom bar with 3–5 items (icons + labels); integrates with `Tabs` or custom content. |
| M3.4 AppBar (mobile) | Top bar: title, optional back, actions; shared concept, mobile-friendly default height and safe top inset. |
| M3.5 Sheet (full & bottom) | Shared `Sheet`; on mobile, “bottom sheet” variant (peek, half, full) with drag-to-dismiss. |

**Exit criteria:** Multi-screen flows (stack + tabs or bottom nav) and modal sheets are buildable with shared + mobile shell.

---

### Phase M4 — Mobile-first and mobile-only widgets

**Goal:** Patterns that are essential on mobile (and optional on desktop) exist as first-class widgets or behaviors.

| Milestone | Deliverables |
|-----------|--------------|
| M4.1 FAB (Floating Action Button) | Floating button over content (e.g. bottom-right); shared widget with mobile as primary use. |
| M4.2 PullToRefresh | Attach to scrollable (ListView/ScrollableContainer); pull-down triggers refresh callback; shared behavior, mobile-first. |
| M4.3 SwipeActions | On list/collection item: swipe to reveal actions (e.g. delete, archive); shared widget extension or mobile list cell behavior. |
| M4.4 Card (mobile style) | Elevated container for list items or content blocks; shared primitive with mobile-friendly default (padding, radius, shadow). |
| M4.5 BottomSheet | Dedicated bottom-sheet container (or Sheet variant): drag handle, snap points, dismiss gesture. |

**Exit criteria:** List-based and action-driven mobile UIs (lists with swipe, FAB, pull-to-refresh, cards) are straightforward to build.

---

### Phase M5 — Platform polish and accessibility

**Goal:** iOS and Android feel native where it matters; accessibility is supported.

| Milestone | Deliverables |
|-----------|--------------|
| M5.1 Platform chrome | Optional use of native navigation bar, tab bar, or system buttons where desired (adapter over shared content). |
| M5.2 Haptics | Optional haptic feedback on tap/selection (platform API behind small abstraction). |
| M5.3 Transitions | Optional platform-idiomatic transition animations (e.g. iOS push, Android shared element) where applicable. |
| M5.4 Screen reader | Accessibility labels and roles for key widgets; VoiceOver (iOS) and TalkBack (Android) support. |
| M5.5 Dynamic type / scaling | Respect system font size or scaling where supported; shared layout and text measure adapt. |

**Exit criteria:** Apps can follow platform guidelines and meet basic accessibility expectations on both platforms.

---

## 4. Widget List: Shared vs Mobile

### 4.1 Legend

- **Shared:** Implemented once; used on desktop and mobile (possibly with platform-specific defaults or adapters).
- **Mobile-first:** Shared widget with mobile as the primary or equal target; may have mobile-only options (e.g. bottom sheet variant).
- **Mobile-only:** Widget or variant that exists only on mobile (or is meaningless on desktop).

### 4.2 Foundation and layout

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `Widget` | ✅ | — | Base; shared. |
| `Container` | ✅ | High | Shared. |
| `AnimatedContainer` | ✅ | High | Shared; animations matter on mobile. |
| `ScrollableContainer` | ✅ | High | Shared; primary scroll primitive on mobile. |
| `UIViewWidget` | ✅ | High | Shared. |
| `HStack` / `VStack` | ✅ | High | Shared. |
| `ZStack` | ✅ | High | Shared; overlays, FAB. |
| `Grid` | ✅ | Medium | Shared; forms, grids. |
| `Spacer` | ✅ | High | Shared. |
| `SplitView` | ✅ | Low | Shared; tablet or foldables only. |
| `Tabs` + `TabItem` | ✅ | High | Shared; mobile often bottom tab bar. |
| **SafeAreaContainer** | ✅ | **High** | Wrapper that applies safe-area insets to child; shared, critical on mobile. |

### 4.3 Display primitives

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `Rectangle`, `RoundedRectangle`, `Ellipse`, `Path` | ✅ | High | Shared. |
| `Image`, `Icon` | ✅ | High | Shared. |
| `Separator` | ✅ | Medium | Shared. |

### 4.4 Text and input

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `Label` | ✅ | High | Shared. |
| `TextInput`, `TextArea` | ✅ | High | Shared; IME and keyboard avoidance on mobile. |
| `Button` | ✅ | High | Shared; touch target size. |
| `Toggle`, `Checkbox`, `RadioGroup` | ✅ | High | Shared. |
| `Slider`, `Stepper` | ✅ | Medium | Shared. |
| `Select` / `Dropdown` | ✅ | High | Shared; on mobile often full-screen or bottom sheet picker. |
| `ProgressBar`, `Spinner` | ✅ | High | Shared. |

### 4.5 Collections

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `ListView` | ✅ | **High** | Shared; primary list on mobile. |
| `CollectionView` | ✅ | High | Shared; grid of items. |
| `TreeView` | ✅ | Low | Shared; settings or nested lists. |
| `TableView` | ✅ | Low | Shared; tablet or dense data. |
| `PropertyGrid` | ✅ | Low | Shared; tooling. |

### 4.6 Navigation and app structure

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `NavigationStack` | ✅ | **High** | Shared; primary screen stack on mobile. |
| **BottomNavBar** | Optional | **High** | Mobile-only or mobile variant; 3–5 items at bottom. |
| **AppBar** (mobile style) | ✅ | **High** | Shared with mobile-friendly default (title, back, actions). |
| `Toolbar` | ✅ | Medium | Shared; can sit in AppBar. |
| `Sidebar` | ✅ | Low | Shared; tablet or fold. |
| `Breadcrumb` | ✅ | Low | Shared. |
| `StatusBar` | ✅ | Medium | Shared; system status often used on mobile. |

### 4.7 Overlays and feedback

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `Sheet` | ✅ | **High** | Shared; bottom-sheet variant is mobile-first. |
| **BottomSheet** | Variant | **High** | Sheet with peek/half/full and drag; mobile-first or mobile-only. |
| `ModalDialog` | ✅ | High | Shared. |
| `Toast` / `Banner` | ✅ | High | Shared. |
| `Tooltip` | ✅ | Low | Shared; less common on touch. |
| `Popover` | ✅ | Medium | Shared. |
| `Menu` / `ContextMenu` | ✅ | High | Shared; long-press on mobile. |

### 4.8 Mobile-first or mobile-only widgets

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| **SafeAreaContainer** | ✅ | **High** | Applies safe-area insets; shared, critical on mobile. |
| **BottomNavBar** | N | **High** | Bottom tab bar; mobile-only or mobile variant of Tabs. |
| **FAB** (Floating Action Button) | ✅ | **High** | Floating button; shared widget, mobile primary. |
| **PullToRefresh** | ✅ | **High** | Behavior on scrollable; shared, mobile-first. |
| **SwipeActions** | ✅ | **High** | Swipe on list item to reveal actions; shared or list extension. |
| **Card** (mobile style) | ✅ | High | Elevated container; shared with mobile default. |
| **BottomSheet** | Variant | **High** | Sheet variant for mobile. |

### 4.9 Media and document

| Widget | Shared | Mobile priority | Notes |
|--------|--------|------------------|--------|
| `VideoViewWidget` | ✅ | High | Shared. |
| `AudioPlayerWidget` | ✅ | Medium | Shared. |
| `SVGViewWidget`, `CanvasWidget` | ✅ | Medium | Shared. |
| `PDFView` | ✅ | Low | Shared; optional. |

### 4.10 Accessibility and tooling

| Widget / component | Shared | Mobile priority | Notes |
|--------------------|--------|------------------|--------|
| Focus / a11y labels | ✅ | High | Shared; VoiceOver/TalkBack on mobile. |
| `FocusRingHost` | ✅ | Medium | Shared; less visible on touch. |
| `ShortcutHint` | ✅ | Low | Shared; desktop-first. |
| `InspectorPanel` | ✅ | Low | Shared; dev only. |

---

## 5. Recommended delivery order (mobile)

Aligned with shared [UI-Engine-Roadmap](UI-Engine-Roadmap.md) and this doc:

1. **M1** — Mobile host, touch, DPI, safe area (no new widgets; enable running on device).
2. **Shared layout + ScrollableContainer** — From main roadmap; then M2 (touch targets, keyboard avoidance, scroll on mobile).
3. **Shared primitives + text/input** — Label, Button, TextInput, etc.; validate on mobile (M2).
4. **NavigationStack + Sheet** — Shared; then M3 (TabBar, BottomNavBar, AppBar, bottom-sheet variant).
5. **ListView / CollectionView** — Shared; then M4 (PullToRefresh, SwipeActions, Card, FAB, BottomSheet).
6. **M5** — Platform chrome, haptics, transitions, screen reader, dynamic type.

---

## 6. Summary

- **Base is shared:** Widget, View, UIView, Layout, StyleSheet, composition, and almost all widgets are one codebase for desktop and mobile.
- **Mobile adds:** Host (iOS/Android), touch/gesture input, safe areas, keyboard avoidance, and a small set of **mobile-first or mobile-only** widgets (SafeAreaContainer, BottomNavBar, FAB, PullToRefresh, SwipeActions, Card, BottomSheet).
- **Roadmap:** M1 (host + input) → M2 (shared widgets mobile-ready) → M3 (navigation + shell) → M4 (mobile-first widgets) → M5 (platform polish + a11y).

This keeps OmegaWTK a single, cross-platform UI engine with a clear path to a fully capable mobile experience.
