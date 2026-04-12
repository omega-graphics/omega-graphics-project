# Animation API Simplification Plan

## Goal

Collapse the current `ViewAnimator` / `LayerAnimator` / `LayerClip` / `ViewClip` /
`AnimationRuntimeRegistry` machinery into a single per-view `Animator` that can
animate **anything visible on screen** — primitives (rects, colors), layer
effects (shadow, transform), and view resizes — through one uniform surface.

The current API forces callers to obtain a `ViewAnimator`, then a
`LayerAnimator` per layer, then build a `LayerClip` / `ViewClip` of optional
keyframe tracks for one of four hard-coded fields. UIView only uses the result
as a *clock source* for its own per-property tween engine, and the documented
public surface duplicates work the per-property engine already does in pure
C++. The registry's lane worker is the only piece carrying its weight — it's
the path that submits compositor packets paced against telemetry — and it has
to survive in some form.

## Decisions

| Decision | Choice |
|---|---|
| API shape | **Option B** — generic `animate<T>(track, apply, timing)` for callback-driven targets, plus a small sealed set of compositor-aware methods (`animateLayerRect`, `animateLayerShadow`, `animateLayerTransform`, `animateViewResize`) that submit packets through `CompositorClientProxy::beginRecord`/`endRecord` and progress against compositor packet telemetry. |
| Ownership | **One `Animator` per `View`**, owned by the view. Parallel animations on the same view are supported by the animator's instance map. |
| UIView per-element tweens | **Fully delegated.** `UIView::Impl::advanceAnimations()` becomes a thin call into `animator->tick()`. The per-property `from`/`to`/`curve`/`startTime` bookkeeping moves into the animator's instance state. |
| Registry | **`AnimationRuntimeRegistry` is removed.** Its lane-worker logic moves into `Animator::Impl` so it dies when the animator dies (no more global lane map keyed by `syncLaneId`). |
| Kept primitives | `AnimationHandle`, `AnimationCurve`, `KeyframeTrack`, `KeyframeValue`, `KeyframeLerp` specializations, `ScalarTraverse`, `TimingOptions`, `AnimationState`, `FillMode`, `Direction`, `ClockMode`, and the `LayerEffect::DropShadowParams` / `LayerEffect::TransformationParams` lerp specializations. |
| Removed | `LayerAnimator`, `ViewAnimator`, `LayerClip`, `ViewClip`, `AnimationRuntimeRegistry`, the `friend class LayerAnimator` / `ViewAnimator` declarations across the codebase, and the documented sections in `wtk/docs/API.rst`. |

## Thread affinity

There are two execution contexts in the new design, mirroring the two ways
work actually has to flow today:

1. **UI thread (callback animations).** Generic `animate<T>` /
   `tween<T>` register a sample-and-apply closure. The animator samples the
   track and fires `apply()` synchronously from `Animator::tick()`, which
   UIView calls from its existing tick slot (where `advanceAnimations()` is
   called today). No marshaling, no locking on user state. This is where every
   primitive, color, scalar UIView property, and the old "composition clock"
   identity-transform animation now live.

2. **Lane worker thread (compositor-aware animations).** The four
   compositor-aware methods feed an internal lane worker — the same loop
   that's in `runLane()` today, lifted out of the registry into
   `Animator::Impl` and scoped to a single view. The worker reads telemetry
   via `Compositor::getLaneTelemetrySnapshot`, computes hybrid wall/presented
   progress, samples the track, and submits deltas through
   `proxy->beginRecord()` / `endRecord()`. User-supplied callbacks never run
   on this thread — only the four built-in compositor effect appliers do
   (`queueLayerResizeDelta`, `queueViewResizeDelta`, layer
   shadow/transform setters).

This split is the whole reason Option B exists: we keep compositor-correct
packet submission async and packet-paced for the things that need it, while
keeping user `apply()` callbacks UI-thread-safe so the simple "animate this
color" path has zero threading concerns.

## Header sketch — `wtk/include/omegaWTK/Composition/Animation.h`

```cpp
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/MultiThreading.h"

#include "omegaWTK/Native/NativeItem.h"
#include "CompositorClient.h"
#include "Geometry.h"
#include "Layer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

#ifndef OMEGAWTK_COMPOSITION_ANIMATION_H
#define OMEGAWTK_COMPOSITION_ANIMATION_H

namespace OmegaWTK {
    class View;
}
namespace OmegaWTK::Composition {
    struct CanvasFrame;

    namespace detail {
        class AnimatorLaneWorker;
    }

    // ---- ScalarTraverse: unchanged ---------------------------------------
    class OMEGAWTK_EXPORT ScalarTraverse { /* unchanged */ };

    // ---- AnimationCurve: unchanged --------------------------------------
    struct OMEGAWTK_EXPORT AnimationCurve { /* unchanged */ };

    using AnimationId = std::uint64_t;

    enum class AnimationState : std::uint8_t {
        Pending, Running, Paused, Completed, Cancelled, Failed
    };
    enum class FillMode : std::uint8_t { None, Forwards, Backwards, Both };
    enum class Direction : std::uint8_t {
        Normal, Reverse, Alternate, AlternateReverse
    };
    enum class ClockMode : std::uint8_t { WallClock, PresentedClock, Hybrid };

    struct TimingOptions {
        std::uint32_t durationMs = 300;
        std::uint32_t delayMs = 0;
        float playbackRate = 1.0f;
        float iterations = 1.0f;
        std::uint16_t frameRateHint = 60;
        FillMode fillMode = FillMode::Forwards;
        Direction direction = Direction::Normal;
        ClockMode clockMode = ClockMode::Hybrid;
        std::uint8_t maxCatchupSteps = 1;
        bool preferResizeSafeBudget = true;
    };

    class OMEGAWTK_EXPORT AnimationHandle {
        struct StateBlock;
        SharedHandle<StateBlock> stateBlock;
        explicit AnimationHandle(const SharedHandle<StateBlock> & stateBlock);
        friend class Animator;
        friend class detail::AnimatorLaneWorker;
        void setStateInternal(AnimationState state);
        void setProgressInternal(float normalized);
        void setSubmittedPacketIdInternal(std::uint64_t packetId);
        void setPresentedPacketIdInternal(std::uint64_t packetId);
        void incrementDroppedPacketCountInternal();
        void setFailureReasonInternal(const OmegaCommon::String & reason);
    public:
        AnimationHandle();
        static AnimationHandle Create(AnimationId id,
                                      AnimationState initialState = AnimationState::Pending);
        AnimationId id() const;
        AnimationState state() const;
        float progress() const;
        float playbackRate() const;
        std::uint64_t lastSubmittedPacketId() const;
        std::uint64_t lastPresentedPacketId() const;
        std::uint32_t droppedPacketCount() const;
        Core::Optional<OmegaCommon::String> failureReason() const;
        bool valid() const;
        void pause();
        void resume();
        void cancel();
        void seek(float normalized);
        void setPlaybackRate(float rate);
    };

    // ---- KeyframeValue / KeyframeTrack / KeyframeLerp: unchanged --------
    template<typename T> struct KeyframeValue { /* unchanged */ };
    namespace detail { /* clamp01, lerp, KeyframeLerp specializations */ }
    template<typename T> class KeyframeTrack { /* unchanged */ };

    // ---- The new Animator -----------------------------------------------

    /// @brief Per-view animation runtime.
    ///
    /// Drives two kinds of animation:
    ///   * **Generic, callback-driven** — `animate<T>` and `tween<T>`. The
    ///     animator samples the track and fires the supplied `apply` callback
    ///     synchronously from `tick()`, which the host (typically the View's
    ///     UI tick) calls. Use this for any primitive, color, scalar UIView
    ///     property, or anything else whose effect is described by user code.
    ///   * **Compositor-aware** — `animateLayerRect`, `animateLayerShadow`,
    ///     `animateLayerTransform`, `animateViewResize`. These run on an
    ///     internal per-view lane worker thread and submit deltas through
    ///     `CompositorClientProxy::beginRecord` / `endRecord`. Their progress
    ///     is paced against compositor packet telemetry, not just wall time.
    ///
    /// One `Animator` per `View`. Parallel animations on the same view (or
    /// the same layer) are supported — each call returns an independent
    /// `AnimationHandle`. Coalescing is the caller's responsibility.
    class OMEGAWTK_EXPORT Animator {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend class detail::AnimatorLaneWorker;
        friend class ::OmegaWTK::View;

        // Internal type-erased registration used by the templated public
        // entry points. The `sampleAndApply` closure is invoked once per
        // tick with the current normalized progress in [0,1].
        AnimationHandle animateErased(
                std::function<void(float)> sampleAndApply,
                TimingOptions timing);
    public:
        explicit Animator(CompositorClientProxy & client,
                          Native::NativeItemPtr nativeView);
        ~Animator();

        // ---- Generic (UI-thread tick, callback) -------------------------

        template<typename T>
        AnimationHandle animate(KeyframeTrack<T> track,
                                std::function<void(const T &)> apply,
                                TimingOptions timing = {});

        template<typename T>
        AnimationHandle tween(T from,
                              T to,
                              std::function<void(const T &)> apply,
                              TimingOptions timing = {},
                              SharedHandle<AnimationCurve> curve =
                                      AnimationCurve::Linear());

        // ---- Compositor-aware (lane-worker tick, packet-paced) ----------

        AnimationHandle animateLayerRect(
                Layer & layer,
                KeyframeTrack<Composition::Rect> track,
                TimingOptions timing = {});

        AnimationHandle animateLayerShadow(
                Layer & layer,
                KeyframeTrack<LayerEffect::DropShadowParams> track,
                TimingOptions timing = {});

        AnimationHandle animateLayerTransform(
                Layer & layer,
                KeyframeTrack<LayerEffect::TransformationParams> track,
                TimingOptions timing = {});

        AnimationHandle animateViewResize(
                KeyframeTrack<Composition::Rect> track,
                TimingOptions timing = {});

        // ---- Lifecycle --------------------------------------------------

        /// Pump callback-driven (generic) animations. Call from the UI tick.
        /// Compositor-aware animations tick independently on the lane worker
        /// and do **not** require this to be called.
        void tick();

        void pauseAll();
        void resumeAll();
        void cancelAll();

        void setFrameRateHint(std::uint16_t fps);
    };

    template<typename T>
    AnimationHandle Animator::animate(KeyframeTrack<T> track,
                                      std::function<void(const T &)> apply,
                                      TimingOptions timing){
        return animateErased(
                [track = std::move(track), apply = std::move(apply)](float t){
                    apply(track.sample(t));
                },
                timing);
    }

    template<typename T>
    AnimationHandle Animator::tween(T from,
                                    T to,
                                    std::function<void(const T &)> apply,
                                    TimingOptions timing,
                                    SharedHandle<AnimationCurve> curve){
        KeyframeValue<T> a { 0.f, from, curve };
        KeyframeValue<T> b { 1.f, to,   nullptr };
        return animate<T>(KeyframeTrack<T>::From({ a, b }),
                          std::move(apply),
                          timing);
    }
}

#endif
```

## Shape decisions baked into the sketch

These are the bits to push back on before we start cutting code:

1. **`animate<T>` is a header-only template trampoline** that erases to
   `std::function<void(float)>`. The lane registry only ever sees
   `void(float)`, so `Impl` doesn't need to be templated and the .cpp stays
   compilable. `KeyframeTrack<T>` is captured by value into the lambda.
2. **`Animator` ctor takes `Native::NativeItemPtr`** so `animateViewResize`
   has a target without needing a separate setter — it carries the same data
   `ViewAnimator` carried.
3. **Compositor-aware methods take `Layer &` directly** rather than going
   through a per-layer sub-animator. This is the biggest break from the old
   shape: no more "get a `LayerAnimator` from the `ViewAnimator`". You get the
   layer reference at the call site.
4. **No `animateOnLane(..., laneId)` overload.** The lane id comes from the
   proxy, same as today's `getSyncLaneId()` path. If per-call lane control
   becomes necessary, add an overload at that point.
5. **`tick()` is on `Animator`, not on a separate timer object.** UIView calls
   `impl_->animator->tick()` from where `advanceAnimations()` is called today
   and keeps its own per-element dirty/value bookkeeping, but registers the
   actual interpolation with the animator instead of running its own loop.
6. **No `View::layerAnimator(layer)`-style cache.** A new compositor-aware
   animation is a fresh handle each time. Two `animateLayerRect` calls on the
   same layer race each other on that layer's rect. Coalescing
   (cancel-prior-on-collision) is **not** built in — open question whether to
   add it later as a default or as an opt-in flag in `TimingOptions`.

## Rollout plan

Each step is a checkpoint where the build should still pass. Stop and report
at each one.

### Step 1 — Land the new surface alongside the old one

- Rewrite `wtk/include/omegaWTK/Composition/Animation.h` to the shape above,
  **but keep** the old `LayerClip` / `ViewClip` / `LayerAnimator` /
  `ViewAnimator` declarations temporarily so existing callsites still build.
- Add `Animator` skeleton with `Impl` defined in `wtk/src/Composition/Animation.cpp`.
- Port the lane-worker guts from `AnimationRuntimeRegistry` (`runLane`,
  `AnimationInstance`, `LaneContext`, `effectiveProgress`, telemetry sync,
  packet bookkeeping) into `Animator::Impl` / `detail::AnimatorLaneWorker`,
  scoped per-instance instead of via the global `lanes` map.
- Implement the four compositor-aware methods on top of the moved worker.
- Implement `animateErased` + `tick()` for the generic path. The internal
  instance carries: the erased closure, `TimingOptions`, `startedAt`, paused
  state, `lastQueuedProgress`, and the `AnimationHandle`. `tick()` walks the
  generic-instance list, computes wall-clock progress (Hybrid clock mode is
  meaningless without packet telemetry — generic instances always behave as
  `WallClock`), invokes the closure, advances the handle.
- Verify the build is green. Old callsites still compile against the old
  classes; nothing wired through `Animator` yet.

### Step 2 — Rewire `View::applyLayoutDelta` and `UIView::applyLayoutDelta`

- Add `SharedHandle<Composition::Animator>` to `View::Impl` (or wherever the
  view holds its compositor proxy) and construct it lazily, once, with the
  view's `compositorProxy()` and native item pointer.
- `View::applyLayoutDelta` (View.Core.cpp:173) — replace the one-shot
  `ViewAnimator` + `resizeTransition` with `animator->animateViewResize(track, timing)`,
  building a two-key `KeyframeTrack<Rect>` from `delta.fromRectPx` →
  `delta.toRectPx`.
- `UIView::applyLayoutDelta` (UIView.Layout.cpp:32) — replace
  `ensureAnimationLayerAnimator(...)->resizeTransition(...)` with
  `animator->animateLayerRect(layer, track, timing)`. The per-element
  `animationLayerAnimators` map becomes unnecessary for this path.
- Build green. Run any existing animation/layout tests.

### Step 3 — Rewire UIView's per-element tween engine

- Delete `UIView::Impl::beginCompositionClock`. The composition-clock concept
  was always vestigial — it spun up an identity-transform `LayerClip` only to
  read `progress()` back. With UIView using `Animator::animate<float>`
  directly, the clock disappears: the animator *is* the clock.
- `UIView::Impl::startOrUpdateAnimation` becomes a thin wrapper that:
  1. Cancels any existing handle for this `(tag, key)` pair.
  2. Calls `animator->tween<float>(from, to, applyClosure, timing, curve)`.
  3. The `applyClosure` writes the sampled value into the same
     `PropertyAnimationState::value` field UIView already exposes via
     `animatedValue()`, sets the dirty flag for the element, and bumps
     `styleDirty`. UIView's own `applyAnimated*` readers are untouched.
- `UIView::Impl::advanceAnimations` becomes `animator->tick()` plus the
  existing diagnostic snapshotting (it can keep collecting telemetry from
  `compositorProxy().getSyncLaneDiagnostics()` independently — that's not
  tied to the registry).
- Path-node animations (`pathNodeAnimations`) get the same treatment: each
  node's x/y becomes a `tween<float>` registered with the animator. The
  closure mutates `PathNodeAnimationState::value` and marks the element
  dirty.
- Delete `UIView::Impl::ensureAnimationViewAnimator`,
  `UIView::Impl::ensureAnimationLayerAnimator`, the `animationViewAnimator`
  field, and the `animationLayerAnimators` map from `UIViewImpl.h`.
- Build green. Animation behavior should be observably identical for
  per-property tweens.

### Step 4 — Delete the old surface

- Delete `LayerAnimator`, `ViewAnimator`, `LayerClip`, `ViewClip`,
  `AnimationRuntimeRegistry` from `Animation.h` and `Animation.cpp`.
- Remove `friend class Composition::ViewAnimator;` from
  `wtk/include/omegaWTK/UI/View.h:107` and the forward decl on line 22.
- Remove the `ViewAnimator` references in the doc comments at View.h:158
  and View.h:163; rephrase to mention `Animator` instead.
- Update `wtk/docs/API.rst`:
  - Replace the `LayerClip` / `ViewClip` (lines 1687–1709) and
    `LayerAnimator` / `ViewAnimator` (lines 1710–1797) sections with a
    single `Animator` section documenting the new entry points.
  - Update the cross-references at API.rst:1025 and API.rst:1035 that name
    `LayerAnimator` / `ViewAnimator`.
- Update any remaining doc files in `wtk/docs/` and `wtk/docs/done/` only if
  they're current planning docs that contradict the new model. Don't touch
  archived `done/` docs that are historical.
- Final build green.

### Step 5 — Verify

- Build the full WTK target.
- Run any existing animation tests / sample apps that exercise the four
  compositor effects (resize, shadow, transform) and the per-element tween
  engine (color, width, height, path nodes).
- Spot-check that two animations on the same view run in parallel without
  starving each other (decision call from the user — concurrent on the same
  view is a requirement).

## Risks and open questions

- **Lane-worker lifetime.** Today's lane is keyed by `syncLaneId` and shared
  across all views that happen to share an id. Moving the worker into the
  per-view `Animator` means each view spins its own thread. If the app has
  many views that's potentially many threads. Mitigation: lazy-start the
  worker only when the first compositor-aware animation is registered, and
  join it when the animator is destroyed or `cancelAll()` empties the
  instance set. Worst case we revisit and pool workers later — but the
  per-view ownership is the user's call and locks the simple model.
- **Generic-path clock mode.** Generic (UI-thread) animations have no packet
  telemetry, so `ClockMode::PresentedClock` and `ClockMode::Hybrid` collapse
  to `WallClock` for them. The animator should silently treat any other mode
  as `WallClock` for generic instances rather than failing — document this
  in the `TimingOptions` doc comment.
- **Coalescing.** UIView's existing `startOrUpdateAnimation` deliberately
  re-uses the existing tween if `to` matches, to avoid restarting on every
  frame for stable values. The new wrapper has to preserve that behavior, or
  callers will see animations reset on every layout pass. The cancel-and-
  restart logic in Step 3 needs the same `std::fabs(state.to - to) <= 0.0001f`
  short-circuit that exists today.
- **Diagnostics.** UIView's `lastAnimationDiagnostics` aggregates per-tick
  counters (stale steps skipped, monotonic clamps, packet counts). After the
  rewire, these need to come from the animator (which now owns the
  per-instance state) rather than from UIView's own loop. Either the animator
  exposes a snapshot getter, or UIView keeps its own counters and updates them
  inside the apply closure. Probably the latter — less coupling.
- **Path animations.** `KeyframeLerp` has no specialization for
  `Composition::Path` and probably shouldn't get one (paths are heavyweight
  and structurally varied). Path-node animations stay as one `tween<float>`
  per node coordinate, the same shape as today.
