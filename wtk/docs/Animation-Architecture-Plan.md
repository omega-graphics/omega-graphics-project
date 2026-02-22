# OmegaWTK Animation Architecture Plan

## Goals

- Provide a deterministic, time-based animation system for `Widget`/`View`/`Layer` composition.
- Avoid frame-delta drift that can cause resize jitter, compression to corners, and inconsistent placement.
- Support upcoming test surfaces:
  - Resize stress and live resizing behavior.
  - `UIView` high-level layout transitions.
  - `SVGView` transform and opacity transitions.
  - `VideoView` position/size/opacity transitions while decode/present continues.

## Core Design

1. Time-based sampling
- Curves expose `sample(t)` where `t` is normalized `[0,1]`.
- Animation playback is evaluated by current clock time, not by incremental per-frame deltas.

2. Typed keyframe tracks
- Strongly typed tracks for scalar and structured animation data:
  - `float`
  - `Core::Rect`
  - `LayerEffect::TransformationParams`
  - `LayerEffect::DropShadowParams`
- Track sampling interpolates between adjacent keyframes with optional per-segment easing.

3. Handle-based control plane
- Each animation returns an `AnimationHandle` with id/state/progress/rate controls:
  - `pause()`, `resume()`, `cancel()`, `seek()`, `setPlaybackRate()`.

4. Compositor-first execution
- Animations are processed in compositor ticks and resolved to absolute target state per frame.
- Per-target updates are coalesced before submission to reduce queue churn.

## API Surface (planned)

- Timing and state:
  - `TimingOptions`
  - `AnimationState`, `FillMode`, `Direction`
  - `AnimationHandle`
- Curves:
  - `AnimationCurve::sample(t)`
  - Presets: `Linear`, `EaseIn`, `EaseOut`, `EaseInOut`, `CubicBezier`
- Typed tracks:
  - `KeyframeValue<T>`
  - `KeyframeTrack<T>::From(...)`
  - `KeyframeTrack<T>::sample(t)`
- Clips:
  - `LayerClip`
  - `ViewClip`

## Compositor Processing Plan

1. Add animation command channel
- Extend compositor command set with animation lifecycle commands:
  - start/pause/resume/cancel/seek/rate.

2. Add compositor animation engine
- Keep active clip instances in compositor state.
- Scheduler wakes on earliest of:
  - pending command threshold
  - next animation tick deadline

3. Evaluate + apply
- At each tick:
  - compute normalized time
  - sample tracks
  - produce absolute target values
  - coalesce by target
  - apply via existing view/layer execution path

4. Fill/loop semantics
- Respect `TimingOptions` (`iterations`, `direction`, `fillMode`, `playbackRate`).

## Slice Plan

### Slice A (implemented now)
- Add curve sampling API and easing presets.
- Add typed keyframe track evaluator and interpolation traits.
- Add animation handle state/control API.
- Add clip data types and animator entrypoints (`animate(clip, timing)`) as API stubs.

### Slice B
- Wire animation commands into compositor queue and scheduler tick.
- Add active animation registry and per-tick coalesced application.

### Slice C
- Reimplement convenience transition APIs as wrappers over clip-based animation.
- Preserve compatibility with existing `resizeTransition` and effect transition methods.

### Slice D
- Add focused animation tests for resize/`UIView`/`SVGView`/`VideoView`.

## Non-goals for Slice A

- No compositor runtime playback yet.
- No command-queue integration yet.
- No behavior changes to existing render scheduling beyond new API availability.
