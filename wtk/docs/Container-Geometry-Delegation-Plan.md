# Container Geometry Delegation Plan (Animation-Integrated)

## Goal
Provide a deterministic geometry/animation contract for widget trees where:

1. Containers own child geometry policy and clamping.
2. Child geometry proposals flow through `requestRect(...)`.
3. View/Layer animation classes from Composition API are used as playback engines.
4. Compositor receives only packet-coherent, lane-stamped commands.
5. Layout remains UI-owned; compositor remains execution/presentation-owned.

## Current Baseline
The following pieces are already present:

1. Geometry proposal API in `Widget`:
   - `GeometryProposal`, `GeometryChangeReason`.
   - `requestRect(...)`, `clampChildRect(...)`, `onChildRectCommitted(...)`.
2. `Container` and stack widgets (`HStack`/`VStack`) for parent-side child management.
3. Sync engine with lane/packet semantics:
   - `syncLaneId`, `syncPacketId`.
   - packet lifecycle and admission gating.
4. Composition animation API:
   - `ViewAnimator`, `LayerAnimator`.
   - `ViewClip`, `LayerClip`.
   - `animate(...)`, `animateOnLane(...)`.
   - transition helpers (`resizeTransition`, `shadowTransition`, `transformationTransition`).

## Problem Statement
Geometry and animation still need one explicit ownership chain at widget level.  
Without it, animated rect/effect updates can bypass container clamps or split across packets, which causes jitter, delayed first-frame visibility, and resize instability.

## Ownership Contract

### UI/Layout Ownership
1. Container computes legal geometry bounds.
2. Child-proposed geometry is accepted only through `requestRect(...)`.
3. Non-animation resize/layout is committed synchronously by widget code.

### Animation Ownership
1. Animation intent is authored at widget/container level.
2. Animation sampling can run continuously, but sampled geometry must still pass container clamp policy.
3. Composition animators (`ViewAnimator`/`LayerAnimator`) are used to materialize runtime commands.

### Compositor Ownership
1. Compositor never chooses layout.
2. Compositor executes the resulting view/layer/frame commands.
3. Sync engine enforces lane packet coherence and admission pacing.

## New Architectural Addition: Widget-Level Animation Orchestrator
Introduce a widget-layer orchestrator that bridges geometry delegation and composition animators.

Proposed class:

`WidgetAnimator` (one per widget or container child-binding)

Responsibilities:

1. Hold widget-facing animation tracks (rect/opacity/effects).
2. Resolve animation target lane (`WidgetTreeHost::laneId()`).
3. Sample tracks and route updates:
   - Geometry tracks -> `requestRect(..., GeometryChangeReason::Animation)`.
   - View visual tracks -> `ViewAnimator`.
   - Layer visual/effect tracks -> `LayerAnimator`.
4. Coalesce per-widget updates into one composition session boundary where possible.
5. Expose handle telemetry for diagnostics.

## Proposed Widget Animation API (Draft)

### Types
```cpp
enum class WidgetAnimationDomain : uint8_t {
    Geometry,
    ViewVisual,
    LayerVisual
};

struct WidgetClip {
    Core::Optional<Composition::KeyframeTrack<Core::Rect>> rect;
    Core::Optional<Composition::KeyframeTrack<float>> viewOpacity;
    Core::Optional<Composition::KeyframeTrack<Composition::LayerEffect::TransformationParams>> layerTransform;
    Core::Optional<Composition::KeyframeTrack<Composition::LayerEffect::DropShadowParams>> layerShadow;
};
```

### Orchestrator
```cpp
class WidgetAnimator {
public:
    explicit WidgetAnimator(Widget & widget);

    Composition::AnimationHandle animate(const WidgetClip & clip,
                                         const Composition::TimingOptions & timing = {});

    Composition::AnimationHandle animateOnLane(const WidgetClip & clip,
                                               const Composition::TimingOptions & timing,
                                               std::uint64_t syncLaneId);

    void pause();
    void resume();
    void cancelAll();
};
```

### Mapping Rules
1. `WidgetClip.rect`:
   - sampled rect -> `widget.requestRect(sampled, GeometryChangeReason::Animation)`.
   - container clamp applies before commit.
2. `WidgetClip.viewOpacity`:
   - route through `ViewAnimator::animateOnLane(ViewClip{opacity=...}, timing, lane)`.
3. `WidgetClip.layerTransform`/`layerShadow`:
   - route through `LayerAnimator::animateOnLane(LayerClip{...}, timing, lane)`.

## Feed Into Compositor (End-to-End Path)

1. App/widget schedules `WidgetAnimator::animate(...)`.
2. `WidgetAnimator` resolves lane id from host (or explicit lane override).
3. At each sample epoch:
   - geometry sample is clamped/committed via `requestRect(...)`.
   - visual/effect samples become `ViewClip`/`LayerClip` updates.
4. `ViewAnimator`/`LayerAnimator` submit via existing compositor client proxies.
5. Proxy stamps command packet metadata (`syncLaneId`, `syncPacketId`).
6. Compositor scheduler applies lane admission and executes packet in order.
7. Packet lifecycle telemetry updates animation handles (`submitted/presented/dropped`).

This keeps geometry policy in containers while still using compositor-backed animation playback.

## Integration Rules

1. Geometry safety:
   - all animation geometry writes must use `requestRect(..., Animation)`.
   - direct `setRect(...)` from animation paths is disallowed.
2. Clamp precedence:
   - container clamp runs before any downstream view/layer visual command emission.
3. Packet coherence:
   - animation updates for one widget tree lane should be grouped to avoid partial-frame interleaving.
4. Resize behavior:
   - under resize budget (`inFlight=1`), prefer newest sample only; stale packet replay is not allowed.

## Updated Slice Plan

### Slice A (Completed): Geometry Delegation API
1. Added proposal/change reason scaffolding and default parent hooks in `Widget`.
2. Added attach acceptance guard (`acceptsChildWidget`).

### Slice B (Completed): `Container` Base
1. Dynamic child management.
2. Parent clamp and child-commit callback handling.

### Slice C (Completed): Stack on Container
1. Stack layout routes child geometry through container-aware path.
2. Reflow remains in widget layer.

### Slice D (Completed): Clamp Policy Hardening
1. Promote explicit policy struct for bounds/overflow behavior.
2. Stabilize clamp decisions under resize pressure.

### Slice E (New): `WidgetAnimator` Scaffold
1. Add widget-level animation orchestrator class and lane resolution.
2. Add `WidgetClip` domain model.
3. Add mapping from widget tracks to `ViewAnimator`/`LayerAnimator`.

Acceptance:
1. Animations can be authored at widget level.
2. Geometry animation always passes container clamps.
3. No direct compositor layout authority is introduced.

### Slice F (New): Lane-Coherent Animation Submission
1. Ensure one widget animation epoch maps to lane-coherent packet submission.
2. Bind handle diagnostics to lane telemetry for jitter triage.
3. Enforce stale-step skipping during saturation/resize mode.

Acceptance:
1. No partial animated states across siblings in same widget tree.
2. Dropped packets do not replay stale geometry/effect states.
3. Frame-to-frame animation progression remains monotonic.

### Slice G: Regression and Diagnostics
1. Add tests for:
   - container clamp + animated child rect,
   - stack + animation + live resize,
   - mixed `UIView` shape/text plus layer effects.
2. Add proposal/clamp/commit trace hooks with lane + packet ids.

## Risks
1. Re-entrancy loops if animation-driven `requestRect` triggers immediate relayout recursion.
2. Divergence between geometry commit timing and visual effect timing.
3. Over-submission when many child tracks sample in same epoch.

## Mitigations
1. Container re-entrancy guards and coalesced relayout.
2. Per-epoch coalescing in `WidgetAnimator`.
3. Lane-aware pacing from sync telemetry with catch-up caps.

## Execution Order
1. Finish Slice D (clamp policy explicitness).
2. Implement Slice E (`WidgetAnimator` scaffold and mapping).
3. Implement Slice F (lane-coherent packet strategy).
4. Lock behavior with Slice G regression harness.
