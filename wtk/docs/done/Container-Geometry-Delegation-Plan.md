# Container Geometry Delegation Plan (View-Owned Animation)

## Goal
Provide a deterministic geometry/animation contract for widget trees where:

1. Containers own child geometry policy and clamping.
2. Child geometry proposals flow through `requestRect(...)`.
3. Composition animation classes are used only by `View`/`UIView`.
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
4. Composition animation API (view-side only):
   - `ViewAnimator`, `LayerAnimator`.
   - `ViewClip`, `LayerClip`.
   - `animate(...)`, `animateOnLane(...)`.
   - transition helpers (`resizeTransition`, `shadowTransition`, `transformationTransition`).

## Problem Statement
Geometry and animation ownership must be explicitly separated.
Without a hard boundary, widget-level rect updates and view-level animated updates can interleave and produce jitter, delayed first-frame visibility, and resize instability.

## Ownership Contract

### UI/Layout Ownership
1. Container computes legal geometry bounds.
2. Child-proposed geometry is accepted only through `requestRect(...)`.
3. Non-animation resize/layout is committed synchronously by widget code.

### Animation Ownership
1. Widgets/containers do not instantiate or own Composition animation handles.
2. `View`/`UIView` own animation intent, playback, and handle lifecycle (`ViewAnimator`/`LayerAnimator`).
3. Widget geometry remains clamp-driven (`requestRect(...)`/`setRect(...)`) and is not driven by Composition animation classes.

### Compositor Ownership
1. Compositor never chooses layout.
2. Compositor executes the resulting view/layer/frame commands.
3. Sync engine enforces lane packet coherence and admission pacing.

## Architectural Direction (Strict Layered Ownership)
Animation integration is constrained to view types (`View`, `UIView`) plus existing host/compositor sync plumbing.

Responsibilities:

1. `Widget`/`Container` own geometry policy and clamping only.
2. `WidgetTreeHost` resolves lane identity and provides submission timing context.
3. `View`/`UIView` bind visual/effect tracks directly to `ViewAnimator` and `LayerAnimator`.
4. Geometry writes from layout/resizing route through container clamp policy; no Composition animation path exists in `Widget`.
5. Per-frame coalescing happens in host/view update phases so one widget-tree epoch emits coherent packets.

## Proposed API Direction (Draft)

### Widget API Boundary
```cpp
class Widget {
public:
    void setRect(const Core::Rect & newRect);
    bool requestRect(const Core::Rect & requested,
                     GeometryChangeReason reason = GeometryChangeReason::ChildRequest);
    // No Composition animation API on Widget.
};
```

Rules:
1. Widget geometry APIs are synchronous and clamp-governed.
2. Widget code cannot directly own Composition animation handles.
3. Any animated visual/effect behavior is delegated to `View`/`UIView`.

### View/Layer Visual Animation Binding
Visual/effect animation remains in existing view types:

1. `View`/`UIView` acquire a `ViewAnimator` bound to their compositor proxy.
2. Per-layer visuals/effects use `LayerAnimator` from that `ViewAnimator`.
3. Lane-aware variants (`animateOnLane`) are preferred whenever host lane is available.

## Feed Into Compositor (End-to-End Path)

1. App/container updates widget geometry through clamp-governed APIs.
2. App/UIView schedules visual/effect animation via `ViewAnimator`/`LayerAnimator`.
3. Host resolves lane id from widget tree context.
4. `ViewAnimator`/`LayerAnimator` submit through existing compositor client proxies.
5. Proxy stamps command packet metadata (`syncLaneId`, `syncPacketId`).
6. Compositor scheduler applies lane admission and executes packet in order.
7. Packet lifecycle telemetry updates animation handles (`submitted/presented/dropped`).

This keeps geometry policy in containers and animation playback in views.

## Integration Rules

1. Geometry safety:
   - widget rect writes come from layout/resizing only and remain clamp-governed.
   - no Composition animation class usage inside `Widget`/`Container`.
2. Clamp precedence:
   - container clamp runs before downstream view submission.
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

### Slice E (Completed): Widget Animation Boundary Hardening
1. Remove/forbid widget-level Composition animation APIs.
2. Keep widget geometry path clamp-driven and synchronous.
3. Add guardrails/documentation so animation classes are used only by `View`/`UIView`.

Acceptance:
1. No widget type owns `Composition::AnimationHandle`.
2. Container clamping remains the only geometry authority.
3. No compositor layout authority is introduced.

### Slice F (Completed): Lane-Coherent Visual/Effect Animation Binding
1. Ensure `View`/`UIView` animation epochs map to lane-coherent packet submission.
2. Bind animation diagnostics to sync telemetry for jitter triage.
3. Enforce stale-step skipping during saturation/resize mode.

Acceptance:
1. No partial animated states across siblings in same widget tree.
2. Dropped packets do not replay stale geometry/effect states.
3. Frame-to-frame animation progression remains monotonic.

### Slice G (Completed): Regression and Diagnostics
1. Add tests for:
   - container clamp + animated child rect,
   - stack + animation + live resize,
   - mixed `UIView` shape/text plus layer effects.
2. Add proposal/clamp/commit trace hooks with lane + packet ids.

Acceptance:
1. Geometry path logs `proposal/clamp/commit` phases when `OMEGAWTK_GEOMETRY_TRACE` is enabled.
2. Test apps cover all three regression scenes (`ContainerClampAnimationTest`, `EllipsePathCompositorTest`, `TextCompositorTest`).
3. Trace output includes sync lane and predicted packet id so packet skew is diagnosable.

## Risks
1. Divergence between clamp-driven geometry commits and view-level visual effect timing.
2. Style/animation updates bypassing view ownership boundary.
3. Over-submission when many child tracks sample in same epoch.

## Mitigations
1. Container re-entrancy guards and coalesced relayout.
2. Host-side epoch coalescing before view submission.
3. Lane-aware pacing from sync telemetry with catch-up caps.

## Execution Order
1. Finish Slice D (clamp policy explicitness).
2. Implement Slice E (widget animation boundary hardening).
3. Implement Slice F (lane-coherent visual/effect binding in view types).
4. Lock behavior with Slice G regression harness.
