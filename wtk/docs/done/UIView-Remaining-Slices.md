# UIView Remaining Slices (B-E, Re-evaluated Against Current Implementation)

## Policy Update (Authoritative)

1. Composition animation classes are **view-only**.
2. `Widget`/`Container` must not own or drive `Composition::AnimationHandle`, `ViewAnimator`, or `LayerAnimator`.
3. `UIView`/`View` are the only UI-layer owners of animation playback logic and handle lifecycle.
4. Geometry policy remains in container/widget layout code; animation policy remains in view code.

This document is re-evaluated against current `UIView` code in:

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/UIView.h`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/UIView.cpp`

and aligned with:

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/Container-Geometry-Delegation-Plan.md`

## Current Feature Snapshot

## Landed

1. Core API:
   - `setLayout(...)`
   - `setStyleSheet(...)`
   - `update()`
2. Layout model:
   - shape and text elements in `UIViewLayout`.
   - `textStyleTag` override for text entries.
3. Rendering coverage:
   - `Rect`, `RoundedRect`, `Ellipse`, `Path`.
   - text draw path (`Canvas::drawText`) with font fallback.
4. Style surface currently implemented:
   - root: background color, border enabled/color/width.
   - element: brush.
   - text: font, color, alignment, wrapping, line-limit field.
5. Dirty graph + submission behavior:
   - root dirty flags + per-element dirty state.
   - active tag order and visibility disable for removed tags.
   - single `startCompositionSession()` / `endCompositionSession()` per update pass.
   - first-frame coherent submit guard (`firstFrameCoherentSubmit`).
6. Animation runtime currently implemented:
   - transition and key-based animation via internal property state + `AnimationCurve`.
   - animated color channels, width/height, and path nodes.
   - per-frame sampling through `advanceAnimations()`.
7. Geometry safety for element draw bounds:
   - shape/text rect clamping through `ViewResizeCoordinator::clampRectToParent(...)`.

## Not Landed (or only partial)

1. `UIView` does not yet use `Composition::ViewAnimator` / `Composition::LayerAnimator` for playback.
2. Lane-specific animation submission (`animateOnLane`) is not wired from `UIView`.
3. Root background/border transitions are not animated through the current runtime.
4. Effect styles (drop shadow / blur) are not part of current `StyleSheet::Entry::Kind`.
5. `TextLineLimit` is carried in style state but not consumed in draw path.

## Ownership Contract (Still Required)

1. Container/UI path owns geometry policy and clamping.
2. Compositor remains execution/presentation only.
3. `UIView` animation uses Composition animators so lane/packet semantics are shared with other view types.
4. Widget-layer code must remain animation-handle free.

## Slice B: Dirty Graph Close-Out (Mostly Landed)

### Outcome
Keep current deterministic `update()` behavior and close remaining over-submit paths.

### Remaining Work

1. Keep current phased flow but tighten dirty invalidation:
   - avoid `markAllElementsDirty()` on style updates when selectors target a subset.
2. Keep first-frame coherent submit, but ensure it does not mask selector-scoped updates in follow-up frames.
3. Add small diagnostics counters in `UIView`:
   - total active tags,
   - dirty tag count,
   - submitted tag count.

### Acceptance

1. No delayed first-visible content.
2. No full-view resubmit on selector-scoped style changes.
3. Per-update submit count tracks real dirtiness.

## Slice C: Composition Animator Bridge (Major Remaining Work)

### Outcome
Migrate `UIView` animation playback from local sampler to Composition animation classes.

### Remaining Work

1. Add `UIViewAnimationBridge` internal helper:
   - property key -> active Composition handle map.
   - last-writer-wins per property key.
2. Map property domains:
   - view visual keys -> `Composition::ViewAnimator`.
   - layer/effect keys -> `Composition::LayerAnimator`.
3. Preserve current animated key coverage:
   - color channels,
   - width/height,
   - path node x/y.
4. Lane path:
   - when host lane exists, use lane-aware submission (`animateOnLane`).
5. Remove duplicate local playback logic from `UIView`:
   - retire `PropertyAnimationState` / `PathNodeAnimationState` once bridge is stable.
6. Keep widget/container path uninvolved:
   - no bridge hooks into `Widget` animation APIs.

### Acceptance

1. `transition=true` and explicit animation entries are played by Composition animators.
2. Animation progression is lane/packet coherent with sync engine.
3. Existing visual behavior remains equivalent to current runtime.
4. No widget-level animation ownership is introduced.

## Slice D: Text Surface Completion (Partially Landed)

### Outcome
Finish missing text style semantics and keep mixed content deterministic.

### Remaining Work

1. Keep current text rendering and style resolution.
2. Implement `TextLineLimit` behavior in actual text drawing path.
3. Confirm text style selector precedence (`viewTag` + `elementTag`) remains deterministic for mixed scenes.
4. Once Slice C lands, route text color transitions through the Composition bridge.

### Acceptance

1. Text respects line-limit setting.
2. Mixed text+shape scenes remain deterministic through live resize.
3. No text pop-in or orientation regressions.

## Slice E: UIView Effects + Effect Animation (Not Landed)

### Outcome
Add effect styles and effect animation support without regressing first-frame visibility/resizing.

### Remaining Work

1. Extend `StyleSheet::Entry::Kind` for effect entries:
   - drop shadow,
   - gaussian blur,
   - directional blur.
2. Style resolution + application:
   - root-level and per-element effect targeting policy.
3. Effect animation:
   - route effect transitions through `Composition::LayerAnimator`.
4. Submission safety:
   - effect-only mutations dirty only affected tags/layers.
   - preserve transparent backgrounds in element canvases.

### Acceptance

1. Effects visible on frame 1.
2. No effect pop-out during active resize.
3. No opaque quad artifacts.

## File Targets

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/UIView.h`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/UIView.cpp`
3. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/include/omegaWTK/UI/View.h` (lane/bridge plumbing as needed)
4. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/UI/View.cpp` (lane/bridge plumbing as needed)

## Recommended Execution Order

1. Slice B close-out
2. Slice C bridge migration
3. Slice D text completion
4. Slice E effects

## Validation Matrix

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/tests/EllipsePathCompositorTest/main.cpp`
   - frame-1 visibility for all three UIView-backed widgets.
   - no intermittent left-corner collapse during live resize.
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/tests/TextCompositorTest/main.cpp`
   - accent UIView and text both visible on frame 1.
   - resize keeps accent centered and text stable.
3. Animation checks
   - transition entries execute through Composition animation classes after Slice C.
   - lane/packet diagnostics remain monotonic under resize pressure.
4. Stress checks
   - 5-10 second live resize with no stale subset draw frames.
