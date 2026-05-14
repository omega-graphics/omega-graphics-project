# Animation Scheduler Plan

**Status:** Proposal. Nothing below is implemented yet.
**Scope:** Replace `ViewAnimator` / `LayerAnimator` / `LayerClip` / `ViewClip` /
`AnimationRuntimeRegistry` with a single per-window **`AnimationScheduler`**
that animates any property on any `SceneNode` (`View`) and writes resolved
values into a side table the Paint phase reads from. Transitions feed in
internally from `StyleResolver`; user code uses property and callback
animations directly.
**Prerequisite reading:** [UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md),
[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md),
[Style-StyleSheet-Refactor-Plan.md](Style-StyleSheet-Refactor-Plan.md),
[Native-API-Completion-Proposal.md](Native-API-Completion-Proposal.md).
**Non-goals:** Changing `KeyframeTrack` / `KeyframeValue` / `KeyframeLerp` /
`AnimationCurve` / `ScalarTraverse`. Designing the scene-tree, lifecycle,
or style cascade (covered by the prerequisite plans).
**Supersedes:** `stale/Animation-API-Simplification-Plan.md`. That plan was
written against the per-view `CompositorClientProxy`, per-view `Layer`, and
async lane-worker submission model. All three substrates have since been
designed away.

---

## 1. What changed since the old plan

The previous Animation API plan ("Animation-API-Simplification-Plan") was a
consolidation: collapse a four-class compositor-aware animator hierarchy
into one per-view `Animator`. Its load-bearing assumptions were:

1. Each `View` owns a `CompositorClientProxy` and submits its own packets.
2. Each `View` has its own `Layer` / `LayerTree` to mutate.
3. A per-view lane-worker thread paces compositor packets against
   `Compositor::getLaneTelemetrySnapshot` telemetry.
4. UIView's per-property tween engine reads animation values directly from
   the animator during paint.

All four assumptions are gone:

1. **One `CompositorClientProxy` per window** (Render Redesign §3.3, Native
   API §2.3). Per-view proxy is deleted.
2. **One `LayerTree` per window** (Render Redesign §3.3). Per-view layers
   are gone; paint emits `DrawOp`s into a window-wide `DisplayList`.
3. **Frame-paced commit, one packet per window per frame** (Render Redesign
   §3.7, Lifecycle §3.10). The lane-worker thread has no place — Tick →
   Style → Layout → Paint → Commit are sequential phases on the
   FrameBuilder's thread, with phase assertions catching cross-phase work.
4. **Animation values live in a per-window side table keyed by
   `(NodeId, PropertyKey)`** (Render Redesign §3.6, Lifecycle §3.8). Paint
   reads the table; the table is written during Tick. Animation is *out
   of* the paint path, not a layer of it.

The right shape under the new architecture isn't a per-view animator —
it's a **per-window scheduler** with node-keyed instance maps. The per-view
ownership rationale (each view animates its own state) collapses into the
side-table model (every node's animated values live in one place the Paint
phase walks). Conveniently, this also matches how Flutter, Unity UI
Toolkit, and Slate organize animation: one driver per surface, many
animations active at once, paint reads resolved state.

---

## 2. Decisions

| Decision | Choice |
|---|---|
| Ownership | **One `AnimationScheduler` per `AppWindow`**, owned by the window. Constructed alongside the window's `FrameBuilder`. |
| Animation count | **No fixed cap.** Active animations are kept in node-keyed maps. Tick walks the active set, not the scene tree, so cost scales with the number of *animations*, not the number of *nodes*. |
| Public API surface | **Property animations** (write to side table) and **callback animations** (fire user `apply()` from Tick). Property animations are primary; callbacks are for non-visual app state. |
| Transitions | **Internal to `StyleResolver`**. The scheduler exposes `transition(...)` only to the resolver (`friend class StyleResolver`). Apps cannot author transitions through the scheduler API; they author them as `Transition` records in `StyleSheet`. |
| Tick driver | **`FrameBuilder` Phase 1 (Tick)** calls `scheduler.tick(frameTime)` once per frame. The scheduler does not own a thread, timer, or clock. |
| Threading | **Single-threaded.** Tick runs on the FrameBuilder thread. No lane worker. No async packet submission. |
| Side table | **`(NodeId, PropertyKey)` → typed value**, owned by the scheduler, read by Paint. Cleared per-animation when the animation completes. Falls back to `ComputedStyle` on miss. |
| Removed | `LayerAnimator`, `ViewAnimator`, `LayerClip`, `ViewClip`, `AnimationRuntimeRegistry`, the `friend class LayerAnimator` / `ViewAnimator` declarations, the four compositor-aware methods (`animateLayerRect`, `animateLayerShadow`, `animateLayerTransform`, `animateViewResize`), `ClockMode::Hybrid` / `ClockMode::PresentedClock`, and the packet-id telemetry fields on `AnimationHandle`. |
| Kept | `AnimationHandle` (slimmed), `AnimationCurve`, `KeyframeTrack`, `KeyframeValue`, `KeyframeLerp` specializations, `ScalarTraverse`, `TimingOptions` (slimmed), `AnimationState`, `FillMode`, `Direction`, the `LayerEffect::DropShadowParams` / `LayerEffect::TransformationParams` lerp specializations. |

---

## 3. Architecture

### 3.1 Where the scheduler sits

```
AppWindow
  ├── NativeWindow (the only NativeItem)
  ├── compositorProxy        — one per window
  ├── styleSheets            — global stylesheet stack
  ├── FrameBuilder
  │     ├── DisplayList      — one per frame
  │     ├── PaintContext     — scratch
  │     └── runs phases:     Tick → Style → Layout → Paint → Commit
  └── AnimationScheduler  ◀── this plan
        ├── PropertyTable    — (NodeId, PropertyKey) → AnimatedValue
        ├── Active animations:
        │     ├── property anims keyed by (NodeId, PropertyKey)
        │     └── callback anims keyed by AnimationId
        └── Transition driver (friend StyleResolver)
```

The scheduler is a peer of `FrameBuilder` under `AppWindow`. `FrameBuilder`
calls into it during Phase 1 (Tick); `Paint` (Phase 4) reads the property
table; `StyleResolver` (Phase 2) feeds it transitions. There is no
per-`SceneNode` animator object — nodes are referenced by `NodeId`.

### 3.2 The side table

```cpp
namespace OmegaWTK::Composition {

    using NodeId = std::uint64_t;

    enum class PropertyKey : std::uint16_t {
        // Visuals (read by Paint)
        BackgroundColor,
        BorderColor,
        BorderWidth,
        Opacity,
        FillBrush,
        ShadowOffsetX, ShadowOffsetY, ShadowBlur, ShadowColor,
        TransformX, TransformY,
        TransformScaleX, TransformScaleY,
        TransformRotation,

        // Text
        TextColor, TextSize,

        // Layout (read by Layout phase)
        LayoutWidth, LayoutHeight,
        LayoutX, LayoutY,

        // Sub-indexed properties
        PathNodeX,   // sub-index = node-of-path index
        PathNodeY,

        // Sentinel
        UserDefined = 0x8000   // app-allocated keys start here
    };

    struct PropertyTableKey {
        NodeId      node;
        PropertyKey key;
        std::uint32_t subIndex = 0;   // for PathNodeX/Y, brush stops, etc.
        // hash + equality
    };

    using AnimatedValue = std::variant<
        float, int, std::uint32_t,
        Composition::Color,
        Composition::Point2D,
        Composition::Rect,
        BrushPtr,
        LayerEffect::DropShadowParams,
        LayerEffect::TransformationParams
    >;

}
```

The Paint phase reads `scheduler.value<T>(node, key)`. If the optional has
a value, that value wins over `ComputedStyle`. If empty, Paint falls back
to the resolved style.

### 3.3 The scheduler

```cpp
class OMEGAWTK_EXPORT AnimationScheduler {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class StyleResolver;   // for transition(...)
public:
    explicit AnimationScheduler(AppWindow & window);
    ~AnimationScheduler();

    // ---- Property animations (written to side table) ----------------

    template<typename T>
    AnimationHandle animateProperty(
            NodeId node,
            PropertyKey key,
            KeyframeTrack<T> track,
            TimingOptions timing = {});

    template<typename T>
    AnimationHandle tweenProperty(
            NodeId node,
            PropertyKey key,
            T from, T to,
            TimingOptions timing = {},
            SharedHandle<AnimationCurve> curve = AnimationCurve::Linear());

    // Sub-indexed variant (path nodes, gradient stops, etc.)
    template<typename T>
    AnimationHandle animatePropertyAt(
            NodeId node,
            PropertyKey key,
            std::uint32_t subIndex,
            KeyframeTrack<T> track,
            TimingOptions timing = {});

    // ---- Callback animations (fire apply() from Tick) ---------------

    template<typename T>
    AnimationHandle animate(
            KeyframeTrack<T> track,
            std::function<void(const T &)> apply,
            TimingOptions timing = {});

    template<typename T>
    AnimationHandle tween(
            T from, T to,
            std::function<void(const T &)> apply,
            TimingOptions timing = {},
            SharedHandle<AnimationCurve> curve = AnimationCurve::Linear());

    // ---- Side-table reads (Paint phase) -----------------------------

    template<typename T>
    Core::Optional<T> value(NodeId node, PropertyKey key,
                            std::uint32_t subIndex = 0) const;

    bool hasAnyAnimationFor(NodeId node) const;

    // ---- Lifecycle -------------------------------------------------

    /// Phase 1 (Tick). Advances all active animations. Writes to the
    /// property table (for property anims) and fires user apply()
    /// callbacks (for callback anims). Marks animated nodes dirty
    /// with DirtyBit::Paint (or DirtyBit::Layout for layout properties).
    void tick(FrameTime now);

    /// Called by SceneNode dtor (or by the FrameBuilder when a node
    /// is removed from the tree). Cancels and removes all animations
    /// targeting `node`.
    void cancelAllForNode(NodeId node);

    void pauseAll();
    void resumeAll();
    void cancelAll();

private:
    // ---- Internal: transitions (StyleResolver only) ----------------

    /// Called from StyleResolver Phase 2 when a ComputedStyle delta is
    /// detected for a property that has a Transition spec. The scheduler
    /// either starts a fresh property animation or, if one is already
    /// running for this (node, key), retargets it to the new `to`
    /// value while preserving current progress.
    AnimationHandle transition(
            NodeId node,
            PropertyKey key,
            AnimatedValue from,
            AnimatedValue to,
            const Transition & spec);
};
```

The two animation paths differ only in what happens during Tick:

- **Property animations** sample the track and write the result to the
  side table; mark the node `DirtyBit::Paint` (or `Layout` for layout
  properties — see §3.6); update the `AnimationHandle`.
- **Callback animations** sample the track and invoke `apply(value)`;
  update the `AnimationHandle`. They do not touch the side table.

Both share `TimingOptions`, `AnimationHandle`, and the keyframe machinery.
There is no separate "compositor-aware" path — under the new architecture
every visual change ultimately becomes a `DrawOp` in the next Paint phase,
so every animation is "compositor-aware" in the trivial sense.

### 3.4 `TimingOptions` after the trim

```cpp
struct TimingOptions {
    std::uint32_t durationMs   = 300;
    std::uint32_t delayMs      = 0;
    float         playbackRate = 1.0f;
    float         iterations   = 1.0f;
    FillMode      fillMode     = FillMode::Forwards;
    Direction     direction    = Direction::Normal;
};
```

Removed:

- `frameRateHint` — frame rate is determined by the window's vsync pacer.
- `clockMode` (`WallClock` / `PresentedClock` / `Hybrid`) — there is one
  clock now: `FrameTime` from the pacer, monotonic, vsync-aligned. The
  three-mode complexity existed solely because the lane worker reconciled
  wall-clock progress against compositor packet telemetry.
- `maxCatchupSteps` — frames are paced by vsync; the scheduler does not
  catch up to a target rate.
- `preferResizeSafeBudget` — there is no per-animation budget anymore.

### 3.5 `AnimationHandle` after the trim

```cpp
class OMEGAWTK_EXPORT AnimationHandle {
    struct StateBlock;
    SharedHandle<StateBlock> stateBlock;
    explicit AnimationHandle(const SharedHandle<StateBlock> &);
    friend class AnimationScheduler;
public:
    AnimationHandle();

    AnimationId id() const;
    AnimationState state() const;
    float progress() const;
    float playbackRate() const;
    Core::Optional<OmegaCommon::String> failureReason() const;
    bool valid() const;

    void pause();
    void resume();
    void cancel();
    void seek(float normalized);
    void setPlaybackRate(float rate);
};
```

Removed: `lastSubmittedPacketId`, `lastPresentedPacketId`,
`droppedPacketCount`, `setSubmittedPacketIdInternal`,
`setPresentedPacketIdInternal`, `incrementDroppedPacketCountInternal`. Per-
animation packet bookkeeping was a lane-worker concept; with one packet
per window per frame, packet identity is window-level diagnostic data, not
per-animation state. Apps that need it get it from the compositor's window-
level diagnostics, not from `AnimationHandle`.

### 3.6 Layout-affecting animations

Some properties (`LayoutWidth`, `LayoutHeight`, `LayoutX`, `LayoutY`)
change the result of the Layout phase, not just Paint. When the scheduler
ticks a layout-affecting property, it sets `DirtyBit::Layout | Paint` on
the target node, not just `Paint`. The next FrameBuilder run re-measures
and re-arranges the affected subtree.

This subsumes the old `animateLayerRect` / `animateViewResize`
distinction: both become `animateProperty(node, LayoutWidth, ...)` etc.,
and the scheduler tags them as layout-affecting via the property key.

The `View::applyLayoutDelta` / `UIView::applyLayoutDelta` callsites
(today's `ViewAnimator::resizeTransition` paths) become:

```cpp
scheduler.tweenProperty<float>(node, PropertyKey::LayoutX,
                               delta.fromRectPx.x(), delta.toRectPx.x(),
                               timing);
scheduler.tweenProperty<float>(node, PropertyKey::LayoutY, ...);
scheduler.tweenProperty<float>(node, PropertyKey::LayoutWidth, ...);
scheduler.tweenProperty<float>(node, PropertyKey::LayoutHeight, ...);
```

Or, if a packed form proves ergonomic, a `Rect` overload that registers
four sub-animations under one `AnimationHandle`.

### 3.7 Transitions (internal to `StyleResolver`)

`Transition` records live in the global `StyleSheet`
(Style plan §3.5). During Phase 2 (Style), `StyleResolver::resolve(node)`
computes the new `ComputedStyle` and compares it to the previous frame's
cached `ComputedStyle`. For each property that

1. changed, *and*
2. has a `Transition` declared in the cascade,

the resolver calls
`scheduler.transition(nodeId, propertyKey, prevValue, newValue, spec)`.

The scheduler:

- If no animation is active for `(node, key)`, starts a fresh property
  animation with `from = prevValue`, `to = newValue`, the transition's
  duration and curve. Seeds the side table with `prevValue` immediately
  so Paint reads the pre-transition value during this frame.
- If an animation *is* active for `(node, key)`, retargets it to the new
  `to` value, preserving the current sampled value as the new `from` and
  the current progress as `0` of the new transition. This is the standard
  "smooth re-targeting" behavior CSS transitions already provide.

`scheduler.transition(...)` is not on the public API. `StyleResolver` is a
`friend class` of `AnimationScheduler`. App code authoring transitions does
so by adding `Transition` records to the global stylesheet, exactly as the
Style plan §3.5 specifies — never by calling `transition` directly.

### 3.8 Phase ordering

The scheduler runs during Phase 1 (Tick). Phase 2 (Style) may register new
transitions via the friend hook. Phase 4 (Paint) reads the side table.
Phase enforcement (Lifecycle plan §3.2) catches cross-phase violations:

| Method | Legal during | Asserts during |
|---|---|---|
| `tick(now)` | Phase 1 (Tick) | Style, Layout, Paint, Commit |
| `transition(...)` (friend) | Phase 2 (Style) | Tick, Layout, Paint, Commit |
| `value<T>(...)` | Phase 4 (Paint) | Tick (rare — see below), Style, Layout, Commit |
| `animateProperty/tween/animate` (public) | Any phase, including outside the frame | — (always defers; bits set, work runs next Tick) |

The "any phase" rule for public registration is critical: an app handler
that fires from a non-frame thread (e.g. an input event) registers an
animation immediately without crossing a phase boundary. The scheduler
enqueues the registration and processes it at the start of the next Tick.

### 3.9 What happens to UIView's per-property tween engine

The current `UIView::Impl::startOrUpdateAnimation` / `advanceAnimations` /
`elementAnimations` / `pathNodeAnimations` machinery becomes unnecessary.
Each of those tweens becomes a property animation in the scheduler:

```cpp
// before
uiview->startOrUpdateAnimation(elementTag, key, from, to, duration, curve);

// after — issued by UIView's content-rebuild path or by StyleResolver
//        (depending on whether it's a code-driven anim or a transition)
scheduler.tweenProperty<float>(
    nodeIdFor(elementTag),  // every UIView element is a child SceneNode
    propertyKeyFor(key),    // e.g. ShadowOffsetX
    from, to, timing, curve);
```

The "every UIView element is a child SceneNode" mapping is the Style plan
§6 question 6 / Render Redesign §3.8 direction. Once it lands, no `(tag,
key)` indirection is needed — every animatable thing is a `(NodeId,
PropertyKey)` pair.

Path-node animations (`UIView::Impl::pathNodeAnimations`) become
`animatePropertyAt(node, PathNodeX, index, ...)` /
`animatePropertyAt(node, PathNodeY, index, ...)`. The sub-index addresses
the path-node within the path.

### 3.10 Diagnostics

Per-animation diagnostics shrink: state, progress, playback rate, failure
reason. Per-window diagnostics — frame count, dropped frames, total active
animations, time spent in Tick — live on the compositor or `FrameBuilder`,
not on the scheduler. UIView's `lastAnimationDiagnostics` aggregator is
deleted; its consumers either:

- read window-level diagnostics from the compositor (frames, drops), or
- read scheduler-level counters from a new `AnimationScheduler::stats()`
  snapshot (active count, ticks executed, applies fired).

Per-instance packet IDs are gone for the reason in §3.5.

---

## 4. Header sketch — `wtk/include/omegaWTK/Composition/Animation.h`

```cpp
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/MultiThreading.h"

#include "Geometry.h"
#include "Layer.h"   // for LayerEffect::DropShadowParams / TransformationParams

#include <atomic>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <variant>

#ifndef OMEGAWTK_COMPOSITION_ANIMATION_H
#define OMEGAWTK_COMPOSITION_ANIMATION_H

namespace OmegaWTK { class AppWindow; class StyleResolver; }

namespace OmegaWTK::Composition {

    // ---- Unchanged primitives ---------------------------------------
    class OMEGAWTK_EXPORT ScalarTraverse { /* unchanged */ };
    struct OMEGAWTK_EXPORT AnimationCurve { /* unchanged */ };
    template<typename T> struct KeyframeValue { /* unchanged */ };
    template<typename T> class KeyframeTrack { /* unchanged */ };
    namespace detail { /* clamp01, lerp, KeyframeLerp specializations */ }

    // ---- Time + identifiers -----------------------------------------
    using AnimationId = std::uint64_t;
    using NodeId      = std::uint64_t;

    struct FrameTime {
        std::uint64_t monotonicNs;     // window pacer monotonic
        std::uint32_t frameIndex;
    };

    // ---- Enums ------------------------------------------------------
    enum class AnimationState : std::uint8_t {
        Pending, Running, Paused, Completed, Cancelled, Failed
    };
    enum class FillMode  : std::uint8_t { None, Forwards, Backwards, Both };
    enum class Direction : std::uint8_t {
        Normal, Reverse, Alternate, AlternateReverse
    };

    enum class PropertyKey : std::uint16_t {
        BackgroundColor, BorderColor, BorderWidth, Opacity, FillBrush,
        ShadowOffsetX, ShadowOffsetY, ShadowBlur, ShadowColor,
        TransformX, TransformY,
        TransformScaleX, TransformScaleY, TransformRotation,
        TextColor, TextSize,
        LayoutWidth, LayoutHeight, LayoutX, LayoutY,
        PathNodeX, PathNodeY,
        UserDefined = 0x8000
    };

    // ---- TimingOptions ----------------------------------------------
    struct TimingOptions {
        std::uint32_t durationMs   = 300;
        std::uint32_t delayMs      = 0;
        float         playbackRate = 1.0f;
        float         iterations   = 1.0f;
        FillMode      fillMode     = FillMode::Forwards;
        Direction     direction    = Direction::Normal;
    };

    // ---- AnimationHandle --------------------------------------------
    class OMEGAWTK_EXPORT AnimationHandle {
        struct StateBlock;
        SharedHandle<StateBlock> stateBlock;
        explicit AnimationHandle(const SharedHandle<StateBlock> &);
        friend class AnimationScheduler;
        // internal mutators omitted
    public:
        AnimationHandle();
        AnimationId    id() const;
        AnimationState state() const;
        float          progress() const;
        float          playbackRate() const;
        Core::Optional<OmegaCommon::String> failureReason() const;
        bool           valid() const;

        void pause();
        void resume();
        void cancel();
        void seek(float normalized);
        void setPlaybackRate(float rate);
    };

    // ---- AnimatedValue (side-table cell) ----------------------------
    using AnimatedValue = std::variant<
        std::monostate,
        float, int, std::uint32_t,
        Color, Point2D, Rect,
        BrushPtr,
        LayerEffect::DropShadowParams,
        LayerEffect::TransformationParams
    >;

    // ---- AnimationScheduler -----------------------------------------

    /// @brief Per-window animation runtime.
    ///
    /// Owns the active animation set and the (NodeId, PropertyKey)
    /// side table the Paint phase reads from. Constructed by AppWindow.
    /// Ticked by FrameBuilder Phase 1.
    ///
    /// Two registration paths:
    ///
    /// * **Property animations** — `animateProperty<T>` /
    ///   `tweenProperty<T>`. The scheduler samples the track and writes
    ///   the result to the side table during Tick. Paint reads the
    ///   table when emitting DrawOps.
    /// * **Callback animations** — `animate<T>` / `tween<T>`. The
    ///   scheduler samples the track and fires the supplied `apply`
    ///   callback synchronously from Tick. For app state that is not
    ///   a node property (custom counters, audio levels, etc.).
    ///
    /// Transitions are not authored through this surface — they feed
    /// in from `StyleResolver` via the friend `transition(...)` hook
    /// when ComputedStyle deltas are detected for transitioned
    /// properties.
    class OMEGAWTK_EXPORT AnimationScheduler {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend class ::OmegaWTK::StyleResolver;

        AnimationHandle transition(NodeId node,
                                   PropertyKey key,
                                   AnimatedValue from,
                                   AnimatedValue to,
                                   const Transition & spec);
    public:
        explicit AnimationScheduler(AppWindow & window);
        ~AnimationScheduler();

        // Property animations
        template<typename T>
        AnimationHandle animateProperty(NodeId, PropertyKey,
                                        KeyframeTrack<T>,
                                        TimingOptions = {});
        template<typename T>
        AnimationHandle tweenProperty(NodeId, PropertyKey,
                                      T from, T to,
                                      TimingOptions = {},
                                      SharedHandle<AnimationCurve> = AnimationCurve::Linear());
        template<typename T>
        AnimationHandle animatePropertyAt(NodeId, PropertyKey,
                                          std::uint32_t subIndex,
                                          KeyframeTrack<T>,
                                          TimingOptions = {});

        // Callback animations
        template<typename T>
        AnimationHandle animate(KeyframeTrack<T>,
                                std::function<void(const T &)> apply,
                                TimingOptions = {});
        template<typename T>
        AnimationHandle tween(T from, T to,
                              std::function<void(const T &)> apply,
                              TimingOptions = {},
                              SharedHandle<AnimationCurve> = AnimationCurve::Linear());

        // Side-table reads (Paint)
        template<typename T>
        Core::Optional<T> value(NodeId, PropertyKey,
                                std::uint32_t subIndex = 0) const;
        bool hasAnyAnimationFor(NodeId) const;

        // Lifecycle
        void tick(FrameTime now);
        void cancelAllForNode(NodeId);
        void pauseAll();
        void resumeAll();
        void cancelAll();

        // Diagnostics
        struct Stats {
            std::uint32_t activeProperty = 0;
            std::uint32_t activeCallback = 0;
            std::uint32_t ticksThisFrame = 0;
            std::uint32_t appliesFired   = 0;
        };
        Stats stats() const;
    };

}  // namespace OmegaWTK::Composition

#endif
```

The two registration paths share an internal type-erased
`registerErased(std::function<void(float)> sample, TimingOptions)` so
`Impl` is not templated. The templated public entry points capture
`KeyframeTrack<T>` (and `apply`, where applicable) into the closure.

---

## 5. Migration plan

This plan layers onto the
[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md)
tiers and the
[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md) tiers. It
**must not ship before** Lifecycle Tier B (phase separation) and Render
Redesign Tier 3 (one FrameBuilder per window) — both are hard prerequisites
because the scheduler has no home before the FrameBuilder exists.

### Tier A — Land the new scheduler alongside the old animator

**Ship alongside Render Redesign Tier 3 / Lifecycle Tier C.**

- Create `AnimationScheduler` skeleton in
  `wtk/src/Composition/AnimationScheduler.cpp` with the public API from
  §4. `Impl` owns the active-animation map, the side table, and the
  type-erased registration helper.
- Wire `AppWindow` to construct one `AnimationScheduler` alongside its
  `FrameBuilder`. `FrameBuilder::buildFrame()` calls
  `scheduler.tick(frameTime)` at the start of Phase 1.
- Implement property and callback animations against keyframe tracks
  using the existing `KeyframeLerp` specializations.
- Implement layout-affecting properties: ticking sets `Layout | Paint`
  on the target node; otherwise just `Paint`.
- Old `LayerAnimator` / `ViewAnimator` / `LayerClip` / `ViewClip` /
  `AnimationRuntimeRegistry` stay in tree, untouched. Existing callsites
  still build.

**Risk:** Medium. New type, new ownership, new phase integration. Heavy
unit-test coverage on the side table and on transition retargeting is the
mitigation.

**Files touched:** new `wtk/src/Composition/AnimationScheduler.{h,cpp}`,
`wtk/include/omegaWTK/Composition/Animation.h` (additive), `AppWindow.h`,
`AppWindow.cpp`, `FrameBuilder.cpp`.

### Tier B — Rewire `View::applyLayoutDelta` and `UIView::applyLayoutDelta`

**Ship alongside Render Redesign Tier 3, after Tier A is in.**

- `View::applyLayoutDelta` ([View.Core.cpp:173](../src/UI/View.Core.cpp))
  replaces its `ViewAnimator::resizeTransition` call with four
  `scheduler.tweenProperty<float>` calls (one per `LayoutX/Y/Width/Height`).
- `UIView::applyLayoutDelta` ([UIView.Layout.cpp:32](../src/UI/UIView.Layout.cpp))
  replaces `ensureAnimationLayerAnimator(...)->resizeTransition(...)`
  with the same pattern, targeting the relevant element NodeId.
- Build green. Existing animation/layout tests should produce identical
  results.

**Risk:** Low. Mechanical substitution.

### Tier C — Rewire UIView's per-element tween engine

**Ship alongside Render Redesign Tier 4.**

- `UIView::Impl::startOrUpdateAnimation` becomes a thin shim that calls
  `scheduler.tweenProperty<T>` for the element. The
  `(tag, key)` short-circuit (`std::fabs(state.to - to) <= 0.0001f`) is
  preserved by checking the scheduler's current target before issuing a
  fresh registration.
- `UIView::Impl::advanceAnimations` is deleted. The scheduler's `tick`
  is the only animation pump.
- `UIView::Impl::beginCompositionClock` is deleted. The clock concept was
  vestigial — see the original plan's §Step 3 — and has no analog under
  per-window scheduling.
- Path-node animations (`UIView::Impl::pathNodeAnimations`) become
  `animatePropertyAt(node, PathNodeX, idx, ...)` /
  `(node, PathNodeY, idx, ...)` calls.
- Delete `UIView::Impl::ensureAnimationViewAnimator`,
  `UIView::Impl::ensureAnimationLayerAnimator`, the
  `animationViewAnimator` field, the `animationLayerAnimators` map, and
  the `lastAnimationDiagnostics` aggregator from `UIViewImpl.h`.
- UIView's `applyAnimated*` readers route to
  `scheduler.value<T>(nodeId, key)` with `ComputedStyle` fallback.

**Risk:** Medium. The `(tag, key)` short-circuit must be preserved
exactly — apps that re-issue animations on every layout pass rely on
"same target = no restart" behavior.

### Tier D — Wire transitions from `StyleResolver`

**Ship alongside Style plan Tier 3.**

- `StyleResolver::resolve(node)` caches the previous frame's
  `ComputedStyle`. When the new resolution differs on a transitioned
  property, the resolver calls `scheduler.transition(...)`.
- `Transition` value type lives with `StyleSheet`; the scheduler accepts
  it directly.
- The scheduler's transition path supports retargeting: if a transition
  is already active for `(node, key)`, the new `to` retargets in place
  preserving the current sampled value.
- The `transition` boolean and `duration` float on the old `Entry` /
  `StyleRule` are retired (Style plan Tier 1 already did most of this);
  this tier removes the last consumers in the animation system.

**Risk:** Medium. Retargeting is the standard CSS edge case; requires
focused tests for "interrupt mid-transition with a new target."

### Tier E — Delete the old surface

**Ship after Tier D.**

- Delete `LayerAnimator`, `ViewAnimator`, `LayerClip`, `ViewClip`,
  `AnimationRuntimeRegistry` from `Animation.h` and `Animation.cpp`.
- Remove `friend class Composition::ViewAnimator;` from
  [View.h:107](../include/omegaWTK/UI/View.h) and the forward decl on
  line 22.
- Update doc comments at View.h:158 and View.h:163 that name
  `LayerAnimator` / `ViewAnimator`; rephrase to `AnimationScheduler`.
- Update [API.rst](API.rst):
  - Replace the `LayerClip` / `ViewClip` (lines 1687–1709) and
    `LayerAnimator` / `ViewAnimator` (lines 1710–1797) sections with a
    single `AnimationScheduler` section.
  - Update cross-references at API.rst:1025 and API.rst:1035.
- Remove any remaining `clockMode` / packet-id references in tests and
  sample apps.
- Final build green.

---

## 6. Open questions

1. **Sub-index space.** `subIndex = 0` covers the common case; path-node
   animations and gradient-stop animations need positive indices. Is
   `std::uint32_t` enough, or should the side-table key be
   `(NodeId, PropertyKey, OmegaCommon::String)` for arbitrarily-keyed
   sub-properties? Recommendation: stay with `uint32_t` until a real use
   case forces a string. String keys cost a hash on every Paint read,
   which is the hot path.

   Agreed.

2. **`FillBrush` / `BrushPtr` interpolation.** The variant includes
   `BrushPtr`, but `KeyframeLerp<BrushPtr>` is not a meaningful
   specialization in general (you can't lerp arbitrary brushes). The Style
   plan punts on this. Recommendation: support brush animations only for
   solid-color brushes (lerp the color underneath), and reject other
   brush kinds with a `Failed` state on the handle. WML's gradient-stop
   animation is then a per-stop scalar animation, not a "brush" animation.

   This is a logcial approach. Each of the components of color brush should be animatable. For Gradient brushes, each stop would be animatble. For bitmap brushes, probably,

3. **Re-targeting outside transitions.** §3.7 specifies that a
   `transition()` call on an active `(node, key)` retargets in place.
   Should the public `tweenProperty` do the same? CSS transitions
   retarget; explicit JS animations do not (they queue or replace
   per-element policy). Recommendation: `tweenProperty` *replaces* (cancels
   the prior animation, starts fresh). Apps that want retargeting can
   pause + seek + retarget manually. Re-targeting is transition behavior,
   not generic-tween behavior.

4. **Removing callback animations entirely.** With the property side
   table covering every visual case, callback `animate<T>` is only useful
   for non-visual app state (custom counters, audio levels, GTE camera
   parameters that aren't node properties). Worth keeping the surface, but
   it's clearly secondary. Recommendation: keep it. Cost is low; deleting
   it removes the only code-driven animation path that doesn't go through
   property keys, which is occasionally useful.

5. **Animation lifetime when a node is destroyed.** §3.3 specifies
   `cancelAllForNode`. The question is when `SceneNode`'s dtor calls it —
   directly on destruction, or via a `FrameBuilder` deferred-cleanup pass
   so cancellation always happens during Tick? Recommendation: deferred
   cleanup pass. Direct cancellation from a node dtor that runs mid-paint
   would assert phase violation.

6. **Stats granularity.** §3.10 proposes `Stats` with active counts and
   tick stats. Add per-frame total time spent in Tick? Useful for
   profiling animation-heavy windows, cheap to collect. Recommendation:
   yes, add `tickElapsedNs` to `Stats`.

   Yes.

7. **`PropertyKey::UserDefined`.** Reserved for app-allocated keys.
   Should the scheduler track which user keys are in use, or trust the
   app? Recommendation: trust the app. The scheduler doesn't need to
   know what a user key means — it just needs to interpolate and store
   the typed value.

   Yes.

---

## 7. Relationship to existing plans

- **[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)** —
  This plan provides the `AnimationScheduler` that §3.6 of the render
  redesign refers to as the side-table writer. Tier A here aligns with
  Tier 3 there; Tiers B/C align with Tier 4 there. Without the
  per-window FrameBuilder this plan has no host.
- **[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md)** —
  Phase 1 (Tick) is exclusively this scheduler. The lifecycle plan's
  §3.8 ("animation reads happen during Paint") is implemented by
  `scheduler.value<T>(...)`. Phase enforcement assertions catch
  cross-phase misuse of the scheduler API.
- **[Style-StyleSheet-Refactor-Plan.md](Style-StyleSheet-Refactor-Plan.md)** —
  Tier 3 of that plan wires `Transition` from `StyleSheet` into this
  plan's `transition(...)` friend hook. Until then, transitions are
  recorded but not driven (Style Tier 2 says "the resolver records ... but
  the Animator isn't wired up to consume transition records until Tier
  3"). Tier D here is the consumer side of that handoff.
- **[Native-API-Completion-Proposal.md](Native-API-Completion-Proposal.md)** —
  The virtual-view model removes `Native::NativeItemPtr` from the
  scheduler's constructor. The scheduler is window-scoped because the
  compositor is window-scoped (one NativeItem per window).
- **[Direct-To-Drawable-And-SDF-Plan.md](Direct-To-Drawable-And-SDF-Plan.md)** —
  Unaffected. The scheduler writes typed values; Paint reads them and
  emits `DrawOp`s; the SDF backend renders them. Animation is invisible
  to the rasterizer.
- **[Frame-Pacing-Plan.md](Frame-Pacing-Plan.md)** — `FrameTime` comes
  from the pacer; the scheduler does not own a clock or a thread. The
  pacer-paced FrameBuilder calls `scheduler.tick(now)` at the start of
  every frame.

---

## 8. What gets deleted

At the end of Tier E:

- `LayerAnimator`, `ViewAnimator`, `LayerClip`, `ViewClip`,
  `AnimationRuntimeRegistry` — all five classes.
- `LaneContext`, `AnimationInstance` (today's per-lane bookkeeping),
  `runLane()`, `effectiveProgress()`, the global `lanes` map keyed by
  `syncLaneId`.
- The `friend class LayerAnimator` / `friend class ViewAnimator`
  declarations across `View.h` and `UIView.h`.
- `ClockMode` enum and all references to `WallClock` /
  `PresentedClock` / `Hybrid`.
- `AnimationHandle`'s packet telemetry fields and their
  `*Internal()` setters.
- `TimingOptions::clockMode`, `frameRateHint`, `maxCatchupSteps`,
  `preferResizeSafeBudget`.
- `UIView::Impl::elementAnimations`, `pathNodeAnimations`,
  `animationViewAnimator`, `animationLayerAnimators`,
  `lastResolvedElementColor`, `lastAnimationDiagnostics`.
- `UIView::Impl::beginCompositionClock`,
  `ensureAnimationViewAnimator`, `ensureAnimationLayerAnimator`,
  `advanceAnimations` (replaced by scheduler tick).
- The `LayerClip` / `ViewClip` / `LayerAnimator` / `ViewAnimator`
  sections of `wtk/docs/API.rst`.

**Estimated deletion:** ~1100 LOC across `Animation.cpp`,
`UIView.Animation.cpp`, and the per-view scaffolding in `UIView.Impl` /
`View.Impl`.
**Estimated addition:** ~700 LOC for `AnimationScheduler.{h,cpp}` plus
the side-table / variant infrastructure.
**Net reduction:** real, but the larger win is conceptual — one type, one
phase, one clock, one side table.

---

## 9. Honest uncertainty

I have not measured the cost of `std::variant<…>` + `std::unordered_map`
lookups on the Paint path. With N animated properties active and M paint
reads per frame, the side table is hit M times. For typical apps M is
small (low hundreds at most), but a heavily-styled list view could push M
into the thousands. Recommendation: ship Tier A with the simple variant +
hashmap; profile during Tier C; if the lookup is hot, swap to per-property
flat arrays indexed by `PropertyKey` enum. Premature here would design a
data structure for a problem that may not exist.

I am assuming retargeting (§3.7) is the right transition default. CSS does
this; iOS animations do this; Android does this. If WML's `transition:`
syntax ends up specifying "queue, don't retarget" semantics, Tier D needs
adjusting. Confirm with the Style plan owner before Tier D ships.

I am assuming the "one packet per window per frame" invariant from the
Render Redesign / Lifecycle plans is firm. If a future need re-introduces
out-of-band compositor submissions (e.g. for video presentation that has
to run faster than vsync), the scheduler's single-threaded design is
unaffected — video bypasses Paint entirely under the NativeViewHost plan
— but a new "compositor-driven" animation kind would need re-introducing
something like the lane-worker concept. That is a future plan, not this
one.

I have not surveyed every caller of the deleted classes. Tier E should be
preceded by a grep sweep for `LayerAnimator`, `ViewAnimator`, `LayerClip`,
`ViewClip`, `AnimationRuntimeRegistry`, `clockMode`,
`lastSubmittedPacketId`, `droppedPacketCount` across `wtk/src/`,
`wtk/include/`, and `wtk/docs/`. Any caller in `wtk/src/Widgets/` or in
sample apps must be migrated as part of Tier E, not after.
