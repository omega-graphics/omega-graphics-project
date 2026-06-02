# Animation Surface Expansion Plan

**Status:** Proposal. Nothing below is implemented yet.
**Scope:** Take the per-window `AnimationScheduler` built by
[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md) (landed via
[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md) Phases
4.3 / 4.4) and expand its **public** authoring surface so that app
code can drive animations against any `View` subclass — not just
`UIView`'s shadow channels — without poking through internals. Adds a
fluent builder, animation composition (sequence / parallel), per-
subclass entry points for `UIView` (full per-element coverage),
`SVGView` (per-element + whole-document), and `ScrollView` (smooth
scroll / fling / snap), plus a deterministic test surface.
**Prerequisite reading:** [Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md),
[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md),
[Style-StyleSheet-Refactor-Plan.md](Style-StyleSheet-Refactor-Plan.md),
[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md).
**Hard prerequisites (must land first):**

- **Render Plan Tier 4 complete**, including Phase 4.8 (legacy
  `ViewAnimator` / `LayerAnimator` deletion + per-view `LayerTree`
  removal) and Phase I (dead-code sweep that removes the orphan
  `Style::elementAnimation` / `elementPathAnimation` /
  `elementBrushAnimation` builders + the `ElementAnimationKey` enum
  + the orphan `applyAnimatedColor` / `applyAnimatedShape` readers).
- **Style Plan Tier 3 complete** — the `StyleResolver` ↔
  `AnimationScheduler` transition friend hook is live, so this plan
  can lean on the declarative-transition path for property changes
  driven by style cascades and only spec the imperative surface for
  app-authored animations.
- **`ComputedStyle` is the single source of truth for paint reads.**
  Paint reads `scheduler.value<T>(...)` first and falls through to
  `ComputedStyle` on miss; both layers are in place.

**Non-goals:** Redefining the scheduler's internal scheduling, the
`PropertyKey` enum, or the side-table key shape — those are owned by
Animation-Scheduler-Plan. Re-specifying the lifecycle phase model.
Adding new low-level animation primitives (springs, physics, parallax
deceleration math) beyond what the scheduler already supports —
those are follow-up proposals once this surface lands.

---

## 1. Why the current public surface is too narrow

After Render Phase 4.4, the only public way to drive an animation in
WTK is:

```cpp
uiView->animateElement(tag, UIView::AnimationChannel::ShadowOffsetY,
                       from, to, durationSec, curve);
```

This surface has five limitations that block real applications:

### 1.1 `UIView`-only

`animateElement` lives on `UIView`. `SVGView`, `ScrollView`,
`CanvasView`-replacement subclasses, and any custom `View` written by
an app cannot reach the scheduler at all without depending on
UI-private headers. A widget that wants to fade an SVG icon, smooth-
scroll its `ScrollView`, or pulse a custom `View`'s opacity has no
public path.

### 1.2 Five shadow channels only

`AnimationChannel` exposes `ShadowOffsetX/Y`, `ShadowRadius`,
`ShadowBlur`, `ShadowOpacity` — and that is the entire menu. Opacity,
transform, color, layout dimensions, custom app state — none of it
is reachable, even though the scheduler itself supports all of them
through the `PropertyKey` enum (including the `UserDefined` range).

### 1.3 One animation per call, no composition

Each `animateElement` call is one scalar tween. Building a
"fade-in-then-slide" animation requires the app to chain completion
callbacks manually. There is no sequential / parallel composition
primitive, and no way to address a group of animations atomically
(pause them, cancel them, observe their joint completion).

### 1.4 No introspection

Apps can issue a tween but cannot ask "is this animation still
running?", "what is its current sampled value?", or "wake me when
this animation completes." The scheduler returns an
`AnimationHandle`, but `animateElement` discards it.

### 1.5 No deterministic test surface

There is no public way for a test to advance the scheduler's clock
manually. Animation tests today either sleep + screenshot (flaky) or
poll private state (fragile). A clean test surface would unblock
property-based animation tests, golden-image diff tests across
multiple frames, and CI runs that need to assert "at t = 200 ms, the
sampled value is X."

### 1.6 Summary

The scheduler underneath is fully capable; the public bottleneck is
that only `UIView`'s element-scoped shadow channels are exposed, with
no composition, no introspection, and no test hooks. This plan
widens that bottleneck without re-spec'ing the scheduler.

---

## 2. What proven systems do

### 2.1 UIKit — `UIView.animate(withDuration:animations:completion:)`

Apple's surface is one static block-based call on the `UIView` base
class. Any property that the block mutates is captured by the
animation system, recorded as a tween, and replayed across the
animation's duration. Completion callbacks fire on the main thread.
Composition is via nested calls; cancellation is per-view
(`layer.removeAllAnimations()`) or per-animation key.

The pattern is convenient for ad-hoc animations and pathological for
introspection — there is no first-class `Animation` object the caller
holds. Newer code uses `UIViewPropertyAnimator`, which *does* return
a handle.

**What we take:** the "any property the caller names is animatable"
flavor of the entry point. The shape `view.animate().property(...)...
.start()` (a builder) is closer to `UIViewPropertyAnimator` than to
the block form, and gives the caller a handle.

### 2.2 SwiftUI — `withAnimation` + implicit transitions

SwiftUI conflates animation with state: declare an `@State` value,
wrap a mutation in `withAnimation`, and the runtime diffs the view
tree and animates every property whose value changed between renders.
Animation parameters (duration, curve) attach to the `withAnimation`
scope.

**What we take:** the implicit-transition idea is already the
direction Style Plan Tier 3 is heading (a `StyleSheet`-declared
`Transition` fires when `ComputedStyle` changes). This plan's
imperative surface is the complement — for animations app code
authors explicitly, not via Style.

### 2.3 Compose Multiplatform — `animate*AsState` + `AnimationSpec`

Compose has fine-grained `animateFloatAsState`, `animateColorAsState`,
etc., each backed by a per-state `Animatable<T>` holder. The caller
gets a `State<T>` that smoothly interpolates whenever its target
changes. `AnimationSpec<T>` is the parameter bag (tween, spring,
keyframes, repeated, infiniteRepeatable).

**What we take:** the typed property hook. `View::animate().property<float>(key, ...)`
should be a template, not a string-keyed call.

### 2.4 Flutter — `AnimationController` + `Tween<T>`

Flutter splits the role of "the clock" (`AnimationController` —
playback rate, duration, status callbacks) from "the value mapping"
(`Tween<T>` — `from → to` for a given clock position). A widget
listens to the controller and rebuilds itself when ticked.

**What we take:** the explicit `AnimationHandle` (already in the
scheduler) plays the controller role. Builder calls produce the
equivalent of a `Tween<T>` and bind it to a property. The
`AnimationGroup` composition primitive (§3.4) plays the role of
Flutter's `Animation` graph.

### 2.5 Web Animations API — `Animation` + `AnimationTimeline`

The W3C Web Animations API on `Element.animate(keyframes, options)`
returns an `Animation` object the caller can `pause()` / `play()` /
`reverse()` / `cancel()` / `finish()`, with `oncancel` / `onfinish`
callbacks and a `playState` getter. Multiple animations can target
the same element; the engine composes them.

**What we take:** the `Animation` object's full lifecycle API — our
`AnimationHandle` already covers most of this; this plan promotes
the missing bits (a `playState` accessor wrapping the scheduler's
state enum, an `awaitCompletion()` future-returning helper).

### 2.6 Lottie — animation groups + scene-graph application

Lottie applies an entire scene-graph of animations to a target tree
in lockstep, with a single `setProgress(t)` driving everything. The
group is the unit of composition, scrub, and reuse.

**What we take:** `AnimationGroup` (§3.4) — sequence and parallel
composition with a single handle on the outside. Scrubbing
(`AnimationGroup::seek(t)`) is a Tier-5 follow-up.

---

## 3. Proposed architecture

### 3.1 Three layers, two new

```
                 ┌──────────────────────────────────────────────┐
                 │  App / Widget code                           │
                 └──────────────────────────────────────────────┘
                              │
                  view->animate().property(...)
                  scrollView->animateScrollTo(...)
                  svgView->animatePath(...)
                  AnimationGroup::parallel(a, b).start()
                              │
                 ┌────────────▼─────────────────────────────────┐
                 │  Public animation surface                    │   <-- THIS PLAN
                 │  - View::animate() (base, virtual)           │
                 │  - per-subclass overrides (UIView/SVG/Scroll)│
                 │  - AnimationBuilder (fluent)                 │
                 │  - AnimationGroup (parallel/sequence)        │
                 │  - AnimationDirector (cross-view)            │
                 │  - AnimationTestController (deterministic)   │
                 └────────────┬─────────────────────────────────┘
                              │
                  scheduler.tweenProperty<T>(...)
                  scheduler.animatePropertyAt<T>(...)
                  handle.pause()/resume()/cancel()
                              │
                 ┌────────────▼─────────────────────────────────┐
                 │  AnimationScheduler (per-window)             │   <-- ALREADY BUILT
                 │  - tick() / value<T>() / side table          │       (Anim Plan
                 │  - register*/tween* primitives               │        Tiers A-D)
                 │  - transition() friend hook (StyleResolver)  │
                 └──────────────────────────────────────────────┘
```

The bottom layer is the scheduler. **This plan adds the middle layer
and nothing below it.** The scheduler stays as it was specified by
Animation-Scheduler-Plan; this plan only expands what reaches it from
outside.

### 3.2 `View::animate()` — the universal entry point

Every `View` subclass gains a virtual entry point that returns an
`AnimationBuilder` bound to the view. The base class implementation
covers properties every `View` carries:

```cpp
class View {
public:
    /// Returns a fluent builder for animations on this View.
    /// The builder targets THIS view's NodeId by default; subclasses
    /// can override to add element-scoped variants (UIView::animate
    /// returns a UIViewAnimationBuilder that lets you target elements
    /// by tag too).
    virtual AnimationBuilder animate();

    /// Cancel every animation the per-window scheduler is currently
    /// running against this view's NodeId. Returns the count cancelled.
    std::size_t cancelAnimations();

    /// True if any animation is active for this view's NodeId.
    bool hasActiveAnimations() const;
};
```

`AnimationBuilder` is the new public type:

```cpp
class AnimationBuilder {
public:
    // Property tracks — typed.
    template<typename T>
    AnimationBuilder & property(PropertyKey key, T from, T to);

    template<typename T>
    AnimationBuilder & propertyAt(PropertyKey key, std::uint32_t subIndex,
                                  T from, T to);

    // Convenience properties for the channels every View can drive.
    AnimationBuilder & opacity(float from, float to);
    AnimationBuilder & translate(Point2D from, Point2D to);
    AnimationBuilder & scale(Point2D from, Point2D to);
    AnimationBuilder & rotate(float fromRadians, float toRadians);

    // Layout-affecting tweens (LayoutX/Y/Width/Height; auto-flagged
    // layoutAffecting so the scheduler dirties Layout, not just Paint).
    AnimationBuilder & moveTo(Point2D from, Point2D to);
    AnimationBuilder & resize(Size from, Size to);

    // Timing.
    AnimationBuilder & duration(float seconds);
    AnimationBuilder & delay(float seconds);
    AnimationBuilder & curve(SharedHandle<AnimationCurve>);
    AnimationBuilder & iterations(float count);   // -1 = infinite
    AnimationBuilder & direction(Direction);
    AnimationBuilder & playbackRate(float rate);

    // Lifecycle.
    AnimationBuilder & onComplete(std::function<void()>);
    AnimationBuilder & onCancel(std::function<void()>);

    // Commit — registers the animation(s) with the per-window scheduler.
    // Returns one handle per property track in registration order.
    // The simple-case caller usually wants `.start().front()`.
    OmegaCommon::Vector<AnimationHandle> start();

    // Shorthand: builds a single-track animation and returns its handle.
    AnimationHandle startOne();
};
```

The builder is a thin layer over `scheduler.tweenProperty<T>`. Every
typed `property<T>(...)` call becomes one tween at `start()` time;
multiple `property*` calls on one builder become parallel tracks
sharing the same `duration` / `delay` / `curve` / etc.

The base `View::animate()` lives on `View.h` and forwards to the
scheduler. UIView, SVGView, and ScrollView each override to return a
specialized builder subclass with subclass-specific shorthands.

### 3.3 Per-subclass surfaces

#### 3.3.1 `UIView::animate()` — element-scoped channels

```cpp
class UIViewAnimationBuilder : public AnimationBuilder {
public:
    /// Re-target this builder at one of the UIView's element tags.
    /// Subsequent property calls register against the element NodeId
    /// (UIView::Impl::ensureElementNodeId(tag)) instead of the view's
    /// own NodeId. Mutually exclusive with the view-level builder
    /// shorthands above.
    UIViewAnimationBuilder & element(const UIElementTag & tag);

    /// Shadow channels — promoted out of UIView::AnimationChannel
    /// (the Phase-4.4 stop-gap enum, which retires here).
    UIViewAnimationBuilder & shadowOffset(Point2D from, Point2D to);
    UIViewAnimationBuilder & shadowBlur(float from, float to);
    UIViewAnimationBuilder & shadowOpacity(float from, float to);

    /// Element-level color / opacity / transform — paint-reachable
    /// once Render Plan Phase I + Style Plan Tier 3 close the path
    /// from animatedValue back into the paint walk for non-shadow
    /// channels. The shorthands fold into ComputedStyle channels:
    UIViewAnimationBuilder & elementFill(Color from, Color to);
    UIViewAnimationBuilder & elementOpacity(float from, float to);
    UIViewAnimationBuilder & elementTransform(Transform from, Transform to);

    /// Path-node animation — for path-shape elements only. The
    /// scheduler keys path-node tweens with `subIndex = nodeIndex`.
    /// (Tier-2 surface — gated on a Style-plan path-node addressing
    /// model that does not exist yet.)
    UIViewAnimationBuilder & pathNode(int nodeIndex, Point2D from, Point2D to);
};
```

This **retires** the Phase-4.4 stop-gap `UIView::animateElement` +
`AnimationChannel` enum, replaced by the unified builder. The
existing call site in `ContainerClampAnimationTest` migrates to:

```cpp
uiView->animate()
    .element("animated_rect")
    .shadowOffset({0.f, 6.f}, {0.f, 60.f})
    .duration(1.5f)
    .curve(AnimationCurve::EaseInOut())
    .startOne();
```

#### 3.3.2 `SVGView::animate()` — SVG element + whole-document

`SVGView` keeps a parsed document and a cached `DisplayList`. To
animate post-parse properties we need a *post-parse element index*
that maps SVG `id` attributes back to addressable nodes inside the
display list — a small structural addition spec'd here, implemented
in this plan's Tier 3.

```cpp
class SVGViewAnimationBuilder : public AnimationBuilder {
public:
    /// Whole-document shorthands — these route to the SVGView's own
    /// NodeId and apply to the entire rendered tree.
    //   (covered by base AnimationBuilder: opacity, translate, etc.)

    /// Re-target at one SVG element by its `id` attribute. The
    /// SVGView builds an `id` → NodeId map at parse time (Tier 3).
    SVGViewAnimationBuilder & svgElement(StringView id);

    /// Per-SVG-element property animations.
    SVGViewAnimationBuilder & fill(Color from, Color to);
    SVGViewAnimationBuilder & stroke(Color from, Color to);
    SVGViewAnimationBuilder & strokeWidth(float from, float to);
    SVGViewAnimationBuilder & strokeDashOffset(float from, float to);  // "draw on" effect
    SVGViewAnimationBuilder & opacity(float from, float to);
    SVGViewAnimationBuilder & transform(Transform from, Transform to);

    /// Path morphing: interpolate every control point of the
    /// addressed `<path>` element between two same-length point sets.
    /// Differently-shaped paths must be normalized by the caller (or
    /// by a follow-up plan that adds SMIL-style path-data alignment).
    /// Issues N parallel `animatePropertyAt(node, PathNodeX/Y, i, ...)`
    /// calls under one handle.
    SVGViewAnimationBuilder & morphPath(
            OmegaCommon::Vector<Point2D> from,
            OmegaCommon::Vector<Point2D> to);
};
```

Implementation note for Tier 3: `SVGView::rebuildDisplayList` walks
the parsed XML; the walk extends to allocate a stable `NodeId` per
element carrying an `id` attribute (or stash them on the
`DisplayList` in a side index). Animation reads probe the side table
at paint time, identically to UIView's element NodeIds.

#### 3.3.3 `ScrollView::animate()` — scroll position + fling + snap

`ScrollView`'s `scrollOffset` is the natural animation target. The
view already has `setScrollOffset(point)`; a property animation on a
new `PropertyKey::ScrollOffsetX/Y` slot drives it.

```cpp
class ScrollViewAnimationBuilder : public AnimationBuilder {
public:
    // Inherits the View-level shorthands (opacity etc).

    /// Smooth-scroll to an absolute offset over `durationSec`.
    /// Equivalent to two parallel tweens against ScrollOffsetX/Y
    /// from the current offset to `target`.
    AnimationHandle animateScrollTo(Point2D target,
                                    float durationSec,
                                    SharedHandle<AnimationCurve> curve = AnimationCurve::EaseOut());

    /// Inertia: start at the current offset with `velocity` (px/s)
    /// and decelerate at `decelerationRate` (per-second deceleration
    /// fraction, default ~0.95 — iOS-style flick). Internally:
    /// duration = log(epsilon) / log(rate); from→to computed
    /// closed-form; curve = ExponentialDecay.
    AnimationHandle flingTo(Point2D velocity,
                            float decelerationRate = 0.95f);

    /// Snap to the nearest of `snapPoints` from the current offset
    /// with a quick ease-out. Issued as a single animateScrollTo
    /// targeting whichever snap point is closest.
    AnimationHandle snapTo(OmegaCommon::Vector<Point2D> snapPoints,
                           float durationSec = 0.25f);

    /// Cancel any in-flight scroll animation (smooth / fling / snap).
    /// Returns true if anything was cancelled.
    bool cancelScrollAnimation();
};
```

ScrollView adds two new keys to the scheduler's `PropertyKey` enum:
`ScrollOffsetX`, `ScrollOffsetY`. Both are `layoutAffecting=false`
(scroll is a paint-time translate of children's window offsets, not a
layout invalidation — see Render Plan §3.6 ScrollView contentOffset
contract). Paint reads them in `ScrollView::contentOffset()` and
folds them into the existing returned `Point2D` before the
FrameBuilder accumulator integrates the result.

### 3.4 `AnimationGroup` — composition

```cpp
class AnimationGroup {
public:
    /// Compose animations to run simultaneously. The group's handle
    /// completes when ALL members complete; cancelling the group
    /// cancels every member.
    static AnimationGroup parallel(OmegaCommon::Vector<AnimationHandle>);

    /// Compose animations to run one after another. Stage N+1
    /// registers (and its `delay` starts ticking) when stage N
    /// completes. The group's handle completes when the LAST stage
    /// completes.
    static AnimationGroup sequence(OmegaCommon::Vector<AnimationBuilder>);

    // Lifecycle — mirrors AnimationHandle, but operates on the whole
    // group. Pausing a sequence pauses its current stage; pausing a
    // parallel pauses every member.
    void                pause();
    void                resume();
    void                cancel();
    AnimationState      state() const;
    AnimationGroup &    onComplete(std::function<void()>);
    AnimationGroup &    onCancel(std::function<void()>);

    /// Underlying handle — opaque, but returned so groups can compose
    /// (a group of groups). Internally each group is one synthetic
    /// callback animation on the scheduler whose `apply()` advances
    /// the stage counter or polls member completion.
    AnimationHandle handle() const;
};
```

Groups never extend the scheduler's data model — they are bookkeeping
on top of existing primitive registrations.

### 3.5 `AnimationDirector` — cross-View choreography (Tier 4)

For animations that span multiple Views (e.g. "fade out the old screen
and fade in the new one in parallel"), an optional `AnimationDirector`
captures the pattern:

```cpp
class AnimationDirector {
public:
    AnimationDirector & with(View *, AnimationBuilder);
    AnimationGroup      startParallel();
    AnimationGroup      startSequence();
};
```

This is sugar over `AnimationGroup::parallel(...)` / `sequence(...)`
that records which view each registered animation targets, so
introspection (`director.runningOn(view)`) is cheap. It is **not**
load-bearing — apps that prefer raw `AnimationGroup` get the same
behavior. The Director is Tier 4 only; Tiers 1-3 are usable without
it.

### 3.6 `AnimationTestController` — deterministic time

Tests need to advance the scheduler's clock without sleeping. The
controller is a debug-build-only friend of `AnimationScheduler` that
exposes:

```cpp
class AnimationTestController {
public:
    explicit AnimationTestController(AppWindow & window);

    /// Cancel the production frame-pacing clock and take over.
    /// While in test mode, the FrameBuilder's beginFrame uses the
    /// test controller's clock instead of steady_clock.
    void takeControl();
    void releaseControl();

    /// Advance the test clock by `dtMs` and pump one Tick. Repeat
    /// in a loop to step through an animation deterministically.
    void advance(unsigned dtMs);

    /// Run the scheduler forward until the given handle reaches
    /// the given state (or `maxSteps` ticks elapse). Each step is
    /// `dtMsPerStep`. Returns true on state reached, false on
    /// step-count exhaustion.
    bool runUntil(AnimationHandle, AnimationState,
                  unsigned dtMsPerStep, unsigned maxSteps);
};
```

Lives in a separate `wtk/test/` target so production binaries do not
ship it. The hook on `AnimationScheduler` is a single
`friend class AnimationTestController;` declaration.

### 3.7 What the scheduler grows (small, well-scoped)

This plan adds the *user-facing* surface, but it requires three
additive pokes to the scheduler / nearby types. They are small enough
to ship with Tier 1 here rather than amending the Animation-Scheduler-
Plan retroactively:

1. `PropertyKey::ScrollOffsetX` / `ScrollOffsetY` — new built-in
   slots (§3.3.3). `layoutAffecting=false`.
2. `PropertyKey::Opacity` already exists; `Transform*` already exists.
   No additions needed for the View-level base channels.
3. `AnimationHandle` gains `awaitCompletion()` — returns a
   `std::future<AnimationState>` resolved when the handle reaches a
   terminal state. Implementation: scheduler tracks per-handle
   completion callbacks (added in §3.4 anyway for groups), the
   `awaitCompletion` adapter wraps one into a future.

Everything else this plan adds is in a new module
(`wtk/include/omegaWTK/Animation/PublicAPI.h` or similar — name
finalized in Tier 1) that depends on the scheduler and on `View`.

---

## 4. Migration plan

Five tiers, each independently shippable. Tiers 1-2 cover every
`View` (including UIView's element-scoped surface); Tiers 3-4 add
the SVGView and ScrollView shorthands; Tier 5 is the test surface.

### Tier 1 — `View::animate()` + `AnimationBuilder` (base + UIView)

**Ships first.**

- Promote the per-window `AnimationScheduler` to public API: add
  `AppWindow::animationScheduler()` (currently only reachable via
  the private `FrameBuilder::animationScheduler()` accessor — Phase
  4.4 left this internal on purpose). Keep the friend hook for the
  StyleResolver path.
- New public header `wtk/include/omegaWTK/Animation/PublicAPI.h`:
  - `AnimationBuilder` (§3.2)
  - `View::animate()` virtual entry point + cancel/has-active helpers
  - `View::nodeId()` is already public (Render Phase 4.4)
- `UIView::animate()` override returning `UIViewAnimationBuilder`
  (§3.3.1) with the element-scope hook + the shadow-channel
  shorthands.
- Migrate `ContainerClampAnimationTest`'s `animateElement` call to
  the new builder. Delete the Phase-4.4 stop-gap
  `UIView::animateElement` method and the `UIView::AnimationChannel`
  enum.
- Validator: ContainerClampAnimationTest runs identically; new unit
  tests cover the base View opacity / transform / layout tweens
  against a `RootWidgetTest`-style scene.

**Risk:** Low. The scheduler is fully built; this tier is API shape +
forwarding code.

**Files touched:** new `wtk/include/omegaWTK/Animation/PublicAPI.h`,
new `wtk/src/Animation/AnimationBuilder.cpp`,
`wtk/include/omegaWTK/UI/View.h`, `wtk/src/UI/View.Core.cpp`,
`wtk/include/omegaWTK/UI/UIView.h`,
`wtk/src/UI/UIView.Animation.cpp`,
`wtk/include/omegaWTK/UI/AppWindow.h`,
`wtk/src/UI/AppWindow.cpp`,
`wtk/tests/ContainerClampAnimationTest/main.cpp`.

### Tier 2 — `AnimationGroup` composition + lifecycle introspection

**Ships after Tier 1.**

- `AnimationGroup::parallel` / `sequence` (§3.4).
- `AnimationHandle::awaitCompletion()` + per-handle completion
  callback (§3.7 #3).
- `AnimationGroup::pause/resume/cancel/state/onComplete/onCancel`.
- `View::cancelAnimations()` / `hasActiveAnimations()` implementation
  reaches the scheduler via `AppWindow::animationScheduler()`.
- New validator app `AnimationCompositionTest` — exercises three
  scenarios: parallel fade+slide, sequential fade-in-then-pulse,
  group cancel mid-flight.

**Risk:** Medium. `sequence` involves a synthetic callback animation
that re-registers the next stage on completion — needs careful
handling of "what if the prior stage was cancelled" and "what if a
nested group is paused mid-stage."

**Files touched:** `wtk/src/Animation/AnimationGroup.cpp`,
`wtk/include/omegaWTK/Composition/Animation.h` (the
`awaitCompletion` hook), `wtk/src/UI/AnimationScheduler.{h,cpp}`
(per-handle completion-callback storage), new
`wtk/tests/AnimationCompositionTest/main.cpp`.

### Tier 3 — `SVGView::animate()` surface

**Ships after Tier 2.**

- `SVGViewAnimationBuilder` (§3.3.2) with whole-document shorthands
  (inherited from `AnimationBuilder`) + per-element fill / stroke /
  opacity / transform / dash-offset / morphPath.
- `SVGView::rebuildDisplayList` extended to allocate a NodeId per
  SVG element carrying an `id` attribute, exposed via a private
  `idToNodeId_` map. Lookup is `SVGView::svgElementNodeId(id)`,
  consumed by the builder.
- `SVGView::paint` extended to read scheduler values for each
  addressed element and apply them to the cached DisplayList per
  frame (per-element opacity / transform multiplied into the
  existing ops; fill / stroke colors looked up before brush
  construction). Path morphing replaces the path's control points
  before paint emits the geometry.
- New validator `SVGViewAnimationTest` — animates a single
  `<rect>`'s fill from red to blue, animates a path's nodes through
  a known morph target, animates the document opacity.

**Risk:** Medium. The SVGView paint path was Tier-2 frozen for the
render redesign; reopening it for per-element animation reads needs
care that the cached-DisplayList model is not invalidated more often
than the animation requires. Mitigation: cache stays valid; only the
per-element overlay reads (opacity, fill, transform) re-evaluate per
frame, which is exactly the scheduler-side-table read pattern UIView
already uses for shadow channels.

**Files touched:** `wtk/include/omegaWTK/UI/SVGView.h`,
`wtk/src/UI/SVGView.cpp`,
`wtk/include/omegaWTK/Animation/PublicAPI.h` (the SVG builder),
new `wtk/tests/SVGViewAnimationTest/main.cpp`.

### Tier 4 — `ScrollView::animate()` + `AnimationDirector`

**Ships after Tier 3.**

- `ScrollViewAnimationBuilder` (§3.3.3) with `animateScrollTo` /
  `flingTo` / `snapTo` / `cancelScrollAnimation`.
- `PropertyKey::ScrollOffsetX` / `ScrollOffsetY` slots added to
  the scheduler enum (§3.7 #1). `ScrollView::contentOffset()`
  extended to fold scheduler-sampled scroll deltas into its
  returned `Point2D` (additive over the resting `scrollOffset_`
  field; the field stays authoritative for the rest position the
  animation eases toward).
- `AnimationDirector` (§3.5) — cross-View choreography sugar.
- ScrollView gains a `DefaultScrollHandler` enhancement: a fling
  starts automatically on touch release (or mouse wheel
  momentum); the existing handler routes to `flingTo` instead of
  setting `scrollOffset` directly.
- New validator `ScrollViewAnimationTest` — smooth-scroll an inner
  content view, fling the viewport, snap to a series of section
  offsets, cancel mid-fling.

**Risk:** Medium-high. Fling math (exponential deceleration model)
needs a real-feel curve; iOS-style decay rate is the documented
baseline. The animation-vs-user-input interaction (user starts
dragging during an in-flight smooth-scroll) needs to cancel the
animation cleanly without dropping a frame.

**Files touched:** `wtk/include/omegaWTK/UI/ScrollView.h`,
`wtk/src/UI/ScrollView.cpp`,
`wtk/src/UI/AnimationScheduler.h` (new PropertyKey slots),
`wtk/include/omegaWTK/Animation/PublicAPI.h` (ScrollView builder +
AnimationDirector), new `wtk/tests/ScrollViewAnimationTest/main.cpp`.

### Tier 5 — `AnimationTestController` + diagnostics

**Ships after Tier 4.**

- `AnimationTestController` (§3.6) in `wtk/test/`.
- `AnimationScheduler::Stats` extended with per-frame tick-elapsed
  histogram (the scheduler already tracks `tickElapsedNs`; add a
  ring buffer to expose the recent N samples for profiling
  overlays).
- A small "animation overlay" debug widget that renders running
  animations + their handles + sampled values, gated by an env flag
  (`OMEGAWTK_ANIM_DEBUG=1`). Tier-5-only; not load-bearing.
- Convert the four prior tier validators (Tiers 1-4) to use the
  test controller so they run deterministically in CI rather than
  needing a screenshot loop.

**Risk:** Low. Pure additive; the test controller is build-time
opt-in.

**Files touched:** new `wtk/test/AnimationTestController.{h,cpp}`,
`wtk/src/UI/AnimationScheduler.{h,cpp}` (friend declaration +
ring-buffer stats), CI config updates to run the converted
validators.

---

## 5. Open questions

1. **Where does the public header live?** Two reasonable homes:
   - `wtk/include/omegaWTK/Animation/PublicAPI.h` (new top-level
     subdirectory dedicated to animation surface)
   - `wtk/include/omegaWTK/UI/Animation.h` (folded under the existing
     UI surface)
   The new top-level reads cleaner once Tier 3-4 add SVGView /
   ScrollView builders, and matches the "scheduler is its own
   subsystem" structure the codebase is gravitating toward. Lean
   toward the new top-level, but confirm before Tier 1.

2. **`AnimationBuilder` ownership.** Returning by value (so chained
   methods can use `&`) is the C++ Builder pattern; returning a
   `unique_ptr` keeps the builder heap-allocated and chainable
   across function boundaries. Recommendation: by value, with one
   `start()` /`startOne()` consumer at the end. The pattern matches
   `std::ostringstream` / Fluent C++. Apps that need to carry the
   builder across scopes can wrap it.

3. **`onComplete` thread.** The scheduler ticks on the UI thread.
   Completion callbacks fire from `tick()` (i.e. from inside
   `FrameBuilder::beginFrame`). Apps that want to do post-completion
   work outside the Tick phase must defer to `requestFrame()`
   themselves. Worth documenting prominently.

4. **`AnimationGroup::sequence` re-targeting.** If stage 2 is a
   transition on the same `(node, key)` as stage 1, the scheduler's
   re-target rule applies (Anim §3.7). Sequence semantics expect a
   clean "stage 1 ends, stage 2 begins from stage 1's `to` value" —
   which is what re-targeting already gives. Verify with focused
   test at Tier 2.

5. **SVGView path morphing — alignment.** §3.3.2 requires the
   caller to supply same-length point sets. SMIL-style automatic
   point-count alignment (insert / drop / smooth interpolation
   between two structurally-different paths) is a deep topic; deferred
   to a follow-up "SVG Path Morphing Plan" rather than expanded here.
   Tier 3's `morphPath` documents the caller's responsibility.

6. **ScrollView fling — physics model.** §3.3.3 uses a single-decay
   exponential. A spring physics model (overshoot + settle) is a
   common alternative; iOS Scroll uses exponential, Android uses a
   power-of-velocity decay. Recommendation: ship exponential as the
   Tier-4 baseline (simplest, deterministic, matches iOS feel) and
   leave spring as a Tier-5+ opt-in via `ScrollViewAnimationBuilder
   ::useSpringFling(stiffness, damping)`.

7. **`UIView::AnimationChannel` deletion timing.** Tier 1 deletes
   the Phase-4.4 stop-gap. If out-of-tree code (unlikely; the surface
   is days old at plan-write time) calls `animateElement`, the
   deletion is a hard break. Recommendation: keep `animateElement`
   for one release as a `[[deprecated]]` forwarder to the builder.
   The Phase-I dead-code sweep applies the same "one cycle then
   delete" rule to other public surfaces, so this matches.

8. **Per-View pause/resume.** `View::cancelAnimations()` is Tier 1;
   `View::pauseAnimations()` / `resumeAnimations()` are obvious
   companions but not strictly required. Recommendation: ship the
   three together in Tier 1 — the scheduler already supports
   `handle.pause()` per-handle, the cross-handle "every handle on
   this NodeId" is trivial bookkeeping.

---

## 6. Where the old concepts go

| Old | New | Tier |
|---|---|---|
| `UIView::animateElement(tag, channel, ...)` | `uiView->animate().element(tag).shadowOffset(...)....startOne()` | T1 |
| `UIView::AnimationChannel` enum | `UIViewAnimationBuilder::shadowOffset/shadowBlur/shadowOpacity` | T1 |
| Apps reach scheduler only via private `FrameBuilder::animationScheduler()` | `AppWindow::animationScheduler()` (public) | T1 |
| No View-level animation entry point | `View::animate()` virtual | T1 |
| Manual completion-callback chaining | `AnimationGroup::sequence` + `onComplete` | T2 |
| No SVG-element animation surface | `SVGView::animate().svgElement(id).fill(...)` etc. | T3 |
| `ScrollView::setScrollOffset` direct mutation | `ScrollView::animate().animateScrollTo(...)` (manual setter still works for instant jumps) | T4 |
| No cross-View choreography | `AnimationDirector` | T4 |
| Tests sleep + screenshot for animation correctness | `AnimationTestController::advance` + `runUntil` | T5 |

---

## 7. Relationship to existing plans

- **[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md)** —
  The engine this plan builds on. No internals change; this plan
  consumes the existing `tweenProperty<T>` / `animatePropertyAt<T>` /
  `value<T>` / `tick(now)` surface, the per-window ownership model,
  the `(NodeId, PropertyKey, subIndex)` key shape, and the
  `AnimationHandle` lifecycle API. Three additive pokes are
  spec'd in §3.7: two new `PropertyKey` slots (`ScrollOffsetX/Y`),
  one new `AnimationHandle` accessor (`awaitCompletion()`). The Anim
  plan's §3.8 phase-enforcement table is the contract this plan's
  surface respects: registration is legal in Tick / Style / Layout /
  outside any frame; Paint and Commit assert.
- **[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)** —
  Hard prerequisite (Tier 4 + Phase I both complete). This plan
  inherits the per-`View` NodeId scheme (Phase 4.4), the per-
  `(UIView, UIElementTag)` NodeId allocation (Phase 4.4), and the
  centralized FrameBuilder phase pump (Phase 4.7) without re-spec.
  Phase I's deletion of the orphan `Style::elementAnimation` builders
  and the `applyAnimatedColor` / `applyAnimatedShape` readers is what
  frees `UIViewAnimationBuilder::elementFill` / `elementTransform` to
  read the scheduler side table directly without colliding with dead
  pre-existing paths.
- **[Style-StyleSheet-Refactor-Plan.md](Style-StyleSheet-Refactor-Plan.md)** —
  Hard prerequisite (Tier 3 complete). The Style plan owns the
  *declarative* transition surface (cascaded `Transition` records
  fired automatically when `ComputedStyle` changes); this plan owns
  the *imperative* surface (app code starting an animation
  explicitly). The two are complementary — they target the same
  scheduler, run through the same side table, and observe the same
  retargeting rules. A property that is both Style-transitioned and
  imperatively animated by app code follows the scheduler's
  re-target rule (Anim §3.7): the most recent registration replaces
  the prior animation, sampling resumes from the current value.
- **[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md)** —
  This plan respects the phase model. Public registration is legal
  in Tick / Style / Layout / outside any frame and asserts in Paint /
  Commit (Anim §3.8). `View::animate()` itself is not phase-scoped —
  it just returns a builder — but the builder's `start()` registers
  with the scheduler and inherits the assert.

---

## 8. What gets deleted

At the end of Tier 1:

- `UIView::AnimationChannel` enum (5 entries) — replaced by the
  builder's typed shadow shorthands.
- `UIView::animateElement` method — replaced by
  `uiView->animate().element(tag)...` (one release of
  `[[deprecated]]` forwarder before hard deletion).

Net change at Tier 5: ~50 LOC removed (the Phase-4.4 stop-gap),
~1500 LOC added (the new public surface across five files), ~600
LOC of new validators (`AnimationCompositionTest`,
`SVGViewAnimationTest`, `ScrollViewAnimationTest`, and the
test-controller-driven CI conversions).

---

## 9. Honest uncertainty

I have not measured how often a real app would want to compose more
than ~4 animations in one group. If most use cases are 1-2 tracks,
the group machinery is overkill and the bare `AnimationHandle`
suffices. If 10+ tracks are common (sprite animation, complex
transitions), the group will be the load-bearing primitive. Tier 2's
validator should exercise both ends so Tier 4-5 sizing decisions
have data.

I am assuming SVGView's `id` attribute is a reliable identity for
addressing per-element animations. SVG documents in the wild have
duplicate `id`s (technically invalid; commonly tolerated) and SVGs
with no `id`s at all (every-element-anonymous). Tier 3 needs a
fallback: probably "lookup-by-id returns the first match; lookup-by-
DOM-path is the formal-correct addressing." The DOM-path addressing
is not spec'd here; Tier 3 should produce a separate "SVG Addressing
Model" follow-up rather than litigating it inline.

I am assuming ScrollView's resting `scrollOffset_` field can remain
authoritative for "the position the animation is easing toward" with
the scheduler folding a transient delta in on top. The alternative is
to make `scrollOffset_` *be* the scheduler-driven value (writes the
side table at touch-end, the scheduler interpolates, reads come back
out). The first model is conservative and matches the existing field
semantics; the second is cleaner but reshapes ScrollView's data
model. Recommendation: ship the conservative model in Tier 4 and
revisit if a use case forces the cleaner one.

I have not surveyed every in-tree caller of the Phase-4.4
`UIView::animateElement`. The only known caller is
`ContainerClampAnimationTest`. The Tier-1 deprecation cycle catches
any I missed; the hard delete happens one release later.

I have not pinned down `AnimationBuilder`'s storage model when
`start()` returns multiple handles (one per property track). The
caller usually wants `startOne()` for the single-track case;
`start()` returning `Vector<AnimationHandle>` is a reasonable
default for the multi-track case but the order is "registration
order," which is implicit. Tier 2 should add a named-track variant
(`AnimationBuilder::property(name, ...)` keyed by string) if the
implicit-order rule turns out to be confusing in real use.

I am assuming the WML compiler (Style Plan Tier 4) will eventually
want to emit calls into this surface as well — for `<style>`-block
animations that can't be expressed as transitions. The shape of
those compiled calls is identical to the imperative app-code path,
so there is no extra design work needed; the compiler simply emits
`view->animate().property(...)....start()` at instantiation time.
Confirm with whoever owns the WML proposal before Tier 4 lands here.
