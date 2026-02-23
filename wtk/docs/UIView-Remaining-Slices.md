# UIView Remaining Slices (B-E, Re-aligned to Current Architecture)

This document updates `UIView` remaining work to match:

1. Widget geometry delegation (`requestRect`, parent clamp hooks).
2. Existing `Container` implementation in `BasicWidgets`.
3. Packetized WidgetTree sync engine and lane telemetry pacing.

## Baseline (Already Landed)

1. `UIView` authoring API is live:
   - `setLayout`, `setStyleSheet`, `update`.
2. Shape rendering in `UIView::update()` supports:
   - `Rect`, `RoundedRect`, `Ellipse`, `Path`.
3. Per-element render bundles are present (`UIElementTag -> layer+canvas`).
4. Sync engine supports lane packeting, admission, and telemetry.
5. Animation runtime supports lane-aware clock modes.
6. Geometry delegation scaffolding is now present in `Widget`:
   - `GeometryProposal`, `GeometryChangeReason`,
   - `Widget::requestRect(...)`,
   - parent `clampChildRect(...)` and child attach acceptance hooks.

## Re-scoped Remaining Work

Main gaps now:

1. `UIView` still repaints/submits too broadly (`layoutDirty/styleDirty` are coarse).
2. Transition fields in `StyleSheet::Entry` are not executed by a concrete `UIView` animation driver.
3. `UIViewLayout::Element::Text` is modeled but not rendered.
4. Effect behavior is not fully unified with first-frame and resize-pressure constraints.

## Slice B: Deterministic UIView Submission and Dirty Graph

### Outcome
`UIView::update()` becomes deterministic, minimal, and lane-packet coherent.

### Implementation Plan

1. Introduce a per-tag dirty graph in `UIView`:
   - `ElementDirtyState { layoutDirty, styleDirty, contentDirty, orderDirty, visibilityDirty }`.
   - Track root dirtiness separately from element dirtiness.
2. Replace broad update pass with phased pass:
   - `prepareRootFrame()`
   - `prepareElementFrames()`
   - `submitDirtyFrames()`
3. Add stable order commit:
   - Reassert z-order every `orderDirty` pass using layout sequence.
   - Keep disabled-layer policy for missing tags.
4. Add first-frame stabilization gate:
   - First `update()` after mount/style/layout change forces a coherent packet with root+all active tags.
   - Prevent no-op transparent skip from dropping initial visible content.
5. Bind `UIView` update submission to one composition session per update:
   - Start session once, submit only dirty frames, end session once.
   - Keep packet boundaries aligned with sync-lane commit semantics.

### File Targets

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/UIView.h`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/UIView.cpp`

### Acceptance

1. First visible frame contains complete expected content (no delayed element reveal).
2. Active resize does not cause element pop-in caused by deferred first dirty submit.
3. Steady-state updates avoid resubmitting unchanged element canvases.

## Slice C: UIView Transition/Animation Driver on Lane Runtime

### Outcome
`transition` and animation style entries execute through the existing sync-aware animation runtime.

### Implementation Plan

1. Add `UIViewAnimationDriver` state to `UIView`:
   - per-tag, per-property active animation map.
   - last-writer-wins cancellation policy per key.
2. Translate style entries into animation jobs:
   - `ElementBrushAnimation`, `ElementAnimation`, `ElementPathAnimation`.
   - `transition=true` with duration maps to implicit animation job.
3. Supported animated keys in this slice:
   - color channels (`R`, `G`, `B`, `A`),
   - geometry (`Width`, `Height`),
   - path node coordinates (`PathNodeX`, `PathNodeY`).
4. Bind animation jobs to host lane:
   - use lane-aware scheduling (`animateOnLane(...)` path where available).
   - sample/apply values inside `UIView` dirty graph so animation writes stay packet-coherent.
5. Finish behavior:
   - snap exact final values at completion,
   - mark only affected tags dirty.

### File Targets

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/UIView.h`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/UIView.cpp`
3. Optional helper hooks in `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/Composition/Animation.h` only if adapter gaps exist.

### Acceptance

1. Style transitions visibly animate instead of hard switching.
2. Animations remain monotonic under lane drops/backpressure.
3. Final values are deterministic and match target style.

## Slice D: UIView Text Elements + Text Style Surface

### Outcome
`UIViewLayout::text(...)` becomes fully functional in mixed shape/text scenes.

### Implementation Plan

1. Extend `UIViewLayout::Element` text payload:
   - retain existing string data,
   - attach optional text style descriptor key.
2. Add text style entries to `StyleSheet`:
   - font handle/reference,
   - color,
   - alignment,
   - wrapping mode,
   - line limit.
3. Render text elements in `UIView::update()`:
   - use `Canvas::drawText(...)`,
   - include text elements in per-tag dirty tracking and z-order.
4. Mixed content determinism:
   - text and shape tags share same ordering pipeline.
   - text style changes only dirty affected tags.

### File Targets

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/UIView.h`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/UIView.cpp`
3. Optional font helper touchpoints in `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/Composition/FontEngine.h`.

### Acceptance

1. `UIView` renders text-only, shape-only, and mixed layouts reliably.
2. Text remains stable through resize and transition updates.
3. No upside-down or delayed-text regressions in test apps.

## Slice E: Effects in UIView Styles (First-frame + Resize-safe)

### Outcome
Drop shadow/blur become first-class style features that remain stable under first-frame and resize pressure.

### Implementation Plan

1. Add effect style entries to `StyleSheet::Entry::Kind`:
   - view-level and element-level drop shadow,
   - gaussian blur,
   - directional blur.
2. Resolve effect style during update phases:
   - root effects applied to root layer,
   - element effects applied to per-tag layer.
3. Integrate with dirty graph:
   - effect parameter change marks only corresponding target dirty.
4. Sync-engine alignment:
   - ensure first-frame stabilization gate includes effect state.
   - when lane pressure is high, follow global adaptive-quality policies rather than ad-hoc `UIView` branching.
5. Preserve transparent composition behavior:
   - avoid forced opaque fills when effects are enabled.

### File Targets

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/UIView.h`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/UIView.cpp`
3. Existing layer effect plumbing in `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/Composition/backend/RenderTarget.cpp` only if additional effect params are needed.

### Acceptance

1. Effect-enabled `UIView` content renders correctly on frame 1.
2. Effects do not disappear during active resize.
3. No black/opaque artifact regression in Text and Ellipse test scenes.

## Execution Order (Updated)

1. Slice B first: fixes determinism and packet hygiene.
2. Slice C second: enables transitions on stable submission path.
3. Slice D third: restores missing text element capability.
4. Slice E fourth: effect style unification on top of deterministic path.

## Validation Matrix

Required checks after each slice:

1. `EllipsePathCompositorTest`:
   - all three shapes visible on frame 1,
   - no delayed element reveal on first resize.
2. `TextCompositorTest`:
   - accent layer visible,
   - text remains readable and stable.
3. Resize stress:
   - active window resize for 5-10 seconds,
   - no frame dropouts where only subset of tags draw.
4. Sync diagnostics:
   - verify single coherent packet per `UIView` update where expected,
   - confirm no stale packet replay after lane drops.
