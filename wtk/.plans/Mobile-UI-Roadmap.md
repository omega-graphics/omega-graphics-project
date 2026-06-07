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

### Phase M1.1 — Detailed breakdown (OmegaWTKM base host)

OmegaWTKM is the **mobile host module** that lands the iOS / Android side of M1.1. The shared widget tree, Composition, Layout, StyleSheet, and FrameBuilder are unchanged — OmegaWTKM only adds the host, the navigator, and the event bridge. The breakdown below resolves five design decisions agreed in chat before this phase opens.

#### Decisions locked in

| Key | Decision | Rationale |
|-----|----------|-----------|
| A1 | `OmegaWTK::Mobile::Native::NativeApp` is a **parallel** interface to `OmegaWTK::Native::NativeApp`, not a shared base. | Lifecycle differs (no user-driven terminate, scene/activity recreation, foreground/background) — faking it through the desktop interface leaks mobile-only states into desktop call sites. |
| B  | Strip `AppWindowManager` to single-window form via `#ifdef TARGET_MOBILE`. | Mobile apps only ever have one window; the multi-window API (`addWindow`, `windows` vector, `WindowIndex`) is dead weight and trains app code into a desktop assumption. `rootWindow` survives as the single window. |
| D  | Extend `omegaWTK/Native/NativeEvent.h` (the **shared** header) with the missing gesture machinery. | Shared widget code (`Button`, `ScrollableContainer`, list cells) consumes one event stream. Desktop backends simply never emit the mobile-only sub-types. |
| 5  | Collapse `GesturePinch / GesturePan / GestureRotate` into a single `Gesture` event with a `GestureSubtype` discriminator covering `Pinch / Pan / Rotate / Tap / LongPress / Swipe`. | One umbrella event type → one switch arm in every consumer. Sub-type payload lives in a `GestureParams` struct. No in-tree code emits the old three event types (only the params-destructor switch in `wtk/src/Native/NativeEvent.cpp:67–69` references them) — safe rename. |
| E1 | Touch is synthesized into `LMouseDown / CursorMove / LMouseUp` for single-finger interactions. | Lets every existing pointer-driven widget work unmodified on day one. First-class `Touch*` events get added later when a widget actually needs multi-touch beyond what `Gesture` covers. |

#### Module boundary

| Concern | Location |
|---------|----------|
| Namespace | `OmegaWTK::Mobile::Native` |
| Public headers | `wtk/include/omegaWTKM/...` |
| Mobile impl | `wtk/src/Mobile/Native/{ios,android}/...` (already scaffolded) |
| Private headers | `wtk/src/Mobile/Native/private_include/NativePrivate/{ios,android}/...` (already scaffolded) |
| Build defines | `TARGET_IOS` / `TARGET_ANDROID` / `TARGET_MOBILE` (already defined) |

#### Header publish list (`wtk/include/omegaWTKM/`)

```
omegaWTKM/
  Mobile.h                       # umbrella include
  Native/
    NativeMobileApp.h            # OmegaWTK::Mobile::Native::NativeApp (parallel to desktop)
    NativeWindow.h               # mobile NativeWindow — single per app
    NativeWindowNavigator.h      # iOS UIWindowScene / Android Activity wrapper; surfaces the one window
    NativeViewNavigator.h        # push / pop view stack inside the single window
    NativeItem.h                 # mobile NativeItem (UIView / SurfaceView wrapper)
    Notification.h               # NotificationCenter (already referenced by AndroidNotification.cpp)
    SafeArea.h                   # safe-area insets (M1.4 prerequisite; header lands now)
```

There is no `NativeGesture.h` — gesture event types live in the shared `omegaWTK/Native/NativeEvent.h` per decision D.

#### Sub-step breakdown

##### M1.1.a — Publish OmegaWTKM headers
Land the header tree above. Headers only, no impl change. Backstops the contracts the existing iOS/Android scaffolds already include (`omegaWTKM/Native/NativeWindowNavigator.h`, `omegaWTKM/Native/NativeMobileApp.h`, `omegaWTKM/Native/NativeViewNavigator.h`, `omegaWTKM/Native/Notification.h`) so those `.mm`/`.cpp` files compile-clean.

The mobile `NativeWindow` interface differs from desktop: no minimize/maximize/restore/fullscreen, no per-window menu, no resizable/key-window state, no per-window opacity. It does keep `setRect`/`getRect` (used by Layout for the available rect), `currentScreen()` (DPI source), the frame-flush callback (Tier A), and the `isNativeReady` / `onFirstRealize` / `onRealize` realize signals — mobile genuinely needs the realize signals because scene re-attach (iOS) and surface destroyed/recreated (Android) are normal events, not edge cases.

`NativeItem::addChildNativeItem` is aligned to the desktop signature (by-value `SharedHandle<NativeItem>`, per `omegaWTK/Native/NativeItem.h:23`); both existing scaffolds currently take by-reference and will be updated in c/d.

##### M1.1.b — Engine integration
Three coordinated changes in the shared `omegaWTK` headers/impl:

1. **`AppInst::start()` mobile branch.** `#ifdef TARGET_MOBILE` selects `Mobile::Native::make_native_app(data)` instead of `Native::make_native_app(data)`. `AppInst` holds the result in a minimal `NativeAppBase` seam that exposes only `run()` and `terminate()` — A1 rules out a full shared interface, not a two-method run/terminate seam.

2. **`AppWindowManager` strip.** Under `TARGET_MOBILE`, the public surface shrinks to `rootWindow`, `setRootWindow(handle)`, `getRootWindow()`, `displayRootWindow()`, `defaultScreen()`, `setDefaultScreen()`. `addWindow`, `windows`, `WindowIndex`, `closeAllWindows`'s iteration are `#ifndef TARGET_MOBILE`-gated. `closeAllWindows()` collapses to "close the one window."

3. **`Gesture` event unification.** In `omegaWTK/Native/NativeEvent.h`:
   - Remove `GesturePinch`, `GesturePan`, `GestureRotate` from `NativeEvent::EventType`.
   - Add a single `Gesture` member.
   - Add `enum class GestureSubtype { Pinch, Pan, Rotate, Tap, LongPress, Swipe }`.
   - Add `struct GestureParams` with the subtype discriminator + `Composition::Point2D position` + `unsigned numTouches` + per-subtype payload (scale, translation, rotation, tapCount, pressDurationMs, swipeDirection). Exact field layout finalized in M1.2.
   - Update the params-destructor switch in `wtk/src/Native/NativeEvent.cpp` — replace the three no-payload arms with a single `case Gesture: delete reinterpret_cast<GestureParams *>(params); break;`.

##### M1.1.c — iOS host
Bring the existing `wtk/src/Mobile/Native/ios/` files to compile-and-link:
- `UIKitApp` implements `Mobile::Native::NativeApp` (`run` / `terminate` / `createNavigator` / `setDefaultNavigator`). Wire `OmegaWTKUIKitAppDelegate` to a real `NSObject<UIApplicationDelegate>` (the scaffold's `@interface` omits the protocol).
- `IOSWindow` implements `Mobile::Native::NativeWindow`. Holds the singleton `UIWindow` for the scene; `setRootView` actually wires the root `UIViewController.view`.
- `IOSWindowNavigator` collapses to single-window: `newWindow()` → `getWindow()` returning the cached singleton. `setKeyWindow(unsigned)` is removed; mobile does not support multiple key windows.
- `IOSItem` fixes: declare `OmegaWTKMobileUIViewController : UIViewController` (the scaffold drops the superclass — won't compile); rename the `delegate` property to avoid shadowing `UIResponder.delegate`; align `addChildNativeItem` signature with the desktop interface.

##### M1.1.d — Android host
Bring the existing `wtk/src/Mobile/Native/android/` files to compile-and-link, with the JNI safety pass:
- Fix `AndroidApp.cpp:16` self-assignment (`app = (android_app *)app;` → `(android_app *)data`).
- Cache `JavaVM*` at `AndroidApp` construction; every JNI call site goes through an `AttachCurrentThread` helper. Raw `JNIEnv *env;` fields in `AndroidItem` / `AndroidNotificationCenter` are removed.
- Wrap every cached `jobject` (`javaObj` in `AndroidApp`, `nativeItem` in `AndroidItem`, `object` in `AndroidNotificationCenter`) in a global ref (`NewGlobalRef` at construction, `DeleteGlobalRef` at destruction). Without this they get GC'd between frames.
- Add `AndroidWindowNavigator` (currently missing — only `AndroidViewNavigator` exists, which is the view-stack one). The window navigator owns the `NativeActivity` and exposes the singleton `AndroidWindow`.
- `AndroidWindow` implements `Mobile::Native::NativeWindow`, backed by `android_app->window` (`ANativeWindow*`).
- Tighten `AndroidItem`: global ref, signature alignment with desktop interface.

##### M1.1.e — Touch → pointer synthesis (E1)
Single-finger touch events are translated by the iOS/Android bridge into the existing pointer event types:
- `touchesBegan` (iOS) / `MotionEvent.ACTION_DOWN` (Android) → `NativeEvent::LMouseDown` with `LMouseDownParams { position, screenPosition, modifiers = {}, clickCount = 1 }`.
- `touchesMoved` / `ACTION_MOVE` → `NativeEvent::CursorMove`.
- `touchesEnded` / `ACTION_UP` → `NativeEvent::LMouseUp`.
- `touchesCancelled` / `ACTION_CANCEL` → `NativeEvent::LMouseUp` with a cancellation flag (extend `LMouseUpParams` with `bool cancelled = false` — cheap, desktop emits `false`).

Multi-touch (≥2 fingers) is **not** synthesized into pointer events; it goes to the `Gesture` path landing in M1.2. Verifies M1 exit criteria: VStack + Label + Button responds to touch on iOS and Android.

#### Out of scope for M1.1
- Real gesture emit (`Gesture` event with sub-types fired from `UIGestureRecognizer` / `GestureDetector`). The event type and params struct land header-only in M1.1.b; the bridges land in **M1.2**.
- Safe-area inset wiring into Layout — `SafeArea.h` lands in M1.1.a, plumbing in **M1.4**.
- Keyboard avoidance, IME — **M2.3**.
- Lifecycle hooks (foreground / background, low-memory, scene re-attach) — scaffolded in M1.1.c/d only; the full delegate surface lands when a widget needs it.

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
