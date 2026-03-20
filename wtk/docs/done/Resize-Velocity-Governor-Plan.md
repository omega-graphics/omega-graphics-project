# Resize Velocity Governor Plan

## Goal
Eliminate resize jitter/corner-collapse by adding explicit resize dynamics tracking in the native/UI path and a GPU-capacity governor in the compositor.

This plan incorporates the proposal in `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/MY_PROPOSALS.md` and extends it with a reusable parent-child resize layout system:
1. Track resize behavior (velocity, acceleration/deceleration, and dead period).
2. Pace submission during active resize against measured GPU/compositor capacity for any widget tree.
3. Preserve epoch monotonicity and stale-packet rejection while pacing.
4. Ensure child widgets that are marked resizable resize with parents while staying inside parent bounds and child clamps.
5. Implement this child-resize behavior once in a `View`-associated coordinator so all view types can use it.

## Problem Model
1. Active resize generates high-frequency geometry/layout churn.
2. Widget/layout updates and render packet submission can outpace GPU present/complete cadence.
3. During this mismatch, stale/incomplete epochs can be presented transiently (jitter/corner collapse symptoms).
4. Child layout can diverge between containers/view types when each performs resize math independently.

## Design Principles
1. Resize state detection must be owned by native/UI layers, not inferred only from compositor packets.
2. Compositor should consume explicit resize state and lane pressure telemetry to make pacing decisions.
3. All trees must route through one governor path; animated trees may still consume quality ladders under pressure.
4. Keep last good frame visible while coalescing new state.
5. Child resize resolution belongs in UI layout (before compositor), not in render backend.
6. Parent-child constraints must be deterministic, clamp-safe, and shared across all `View` derivatives.

## Child Resize Contract
1. A child can opt into resize behavior (`resizable=true`) or remain fixed.
2. Resizable children are resolved against:
   - parent content bounds,
   - parent padding/insets,
   - child margins,
   - child min/max clamps.
3. Final child rectangle is always clamped within parent content bounds.
4. If constraints are unsatisfiable, fallback is:
   - clamp to nearest valid size,
   - preserve anchor/alignment,
   - mark overflow state for diagnostics.
5. Non-resizable children keep size, but position still clamps to parent bounds.

## Architecture

### 1) Resize Dynamics Tracker (Native + WidgetTree)
Add per-window tracker that records timestamped resize samples:
- `width`, `height`
- `dt`
- `velocity` (pixels/sec from size delta)
- `acceleration` (change in velocity)

Derive resize phase:
- `Idle`
- `Active`
- `Settling` (decelerating)
- `Completed` (dead period exceeded)

Completion criteria:
1. `abs(velocity) < velocityEpsilon`
2. `abs(acceleration) < accelEpsilon`
3. no significant size delta for `deadPeriodMs`

### 2) WidgetTree Resize Session State
Each `WidgetTreeHost` maintains a resize session:
- `sessionId`
- current phase
- current metrics snapshot
- policy mode (`StaticSuspend`, `AnimatedGoverned`)

Session is emitted to compositor as metadata on packet boundaries.

### 3) ViewResizeCoordinator (Reusable Layout Engine)
Introduce a `View`-associated class that performs parent-child resize resolution for all container-like views (`Stack`, `UIView`, future `ScrollView`, `SVGView`, `VideoView` host layouts):
1. Consume parent bounds + child resize specs.
2. Compute proposed child sizes by policy.
3. Apply clamp pass (`min/max`), then bounds pass (parent content rect).
4. Emit geometry batch for layout consumers.
5. Provide deterministic ordering and stable output during live resize.

This class is UI-layer only and runs before compositor command submission.

### 4) Unified Render Policy (Compositor Governor)
For all widget trees during resize:
1. Resolve parent-child geometry first through `ViewResizeCoordinator`.
2. Compute lane capacity from telemetry:
   - presented cadence
   - in-flight count
   - EWMA submit->present latency
   - optional GPU duration (if available)
3. Estimate incoming resize pressure from tracker velocity.
4. Admit/coalesce/drop packets by budget:
   - preserve newest epoch
   - drop superseded intermediate epochs
   - reduce effect quality first before dropping animation-critical packets

### 5) Capacity/Budget Model
Per lane, derive a dynamic budget:
- `targetInFlight` (already present in sync engine)
- `maxPacketRateHz` from observed present cadence
- `resizeVelocityBudget` from lane health

If `incomingRate > capacity`:
1. Coalesce geometry deltas.
2. Apply effect-quality degradation policy.
3. Drop stale intermediate packets (never drop newest authoritative epoch).

### 7) Integration with Existing Plans
This plan extends current docs and code paths:
- `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/WidgetTree-Sync-Engine-Plan.md`
- `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/LayerTree-Frontend-Backend-Sync-Plan.md`
- `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/Container-Geometry-Delegation-Plan.md`

No replacement of packet epoch gating; this is an upstream pacing and policy layer with a shared layout coordinator.

## Proposed API Surface

### UI/Host
```cpp
namespace OmegaWTK {

struct ResizeDynamicsSample {
    double timestampMs;
    float width;
    float height;
    float velocityPxPerSec;
    float accelerationPxPerSec2;
};

enum class ResizePhase {
    Idle,
    Active,
    Settling,
    Completed
};

struct ResizeSessionState {
    uint64_t sessionId;
    ResizePhase phase;
    ResizeDynamicsSample sample;
    bool animatedTree;
};

class ResizeDynamicsTracker {
public:
    void begin(float width, float height, double tMs);
    ResizeSessionState update(float width, float height, double tMs);
    ResizeSessionState end(float width, float height, double tMs);
};

struct ResizeClamp {
    float minWidth;
    float minHeight;
    float maxWidth;
    float maxHeight;
};

enum class ChildResizePolicy {
    Fixed,
    Fill,
    FitContent,
    Proportional
};

struct ChildResizeSpec {
    bool resizable;
    ChildResizePolicy policy;
    ResizeClamp clamp;
    float growWeightX;
    float growWeightY;
};

class ViewResizeCoordinator {
public:
    void attachView(View * parentView);
    void registerChild(View * childView, const ChildResizeSpec & spec);
    void updateChildSpec(View * childView, const ChildResizeSpec & spec);
    void unregisterChild(View * childView);
    void beginResizeSession(uint64_t sessionId);
    void resolve(const GRect & parentContentRect);
};

}
```

### Compositor-facing Metadata
```cpp
namespace OmegaWTK::Composition {

struct ResizeGovernorMetadata {
    uint64_t sessionId;
    bool active;
    bool animatedTree;
    float velocityPxPerSec;
    float accelerationPxPerSec2;
    ResizePhase phase;
};

}
```

## Slices

### Slice A: Native Resize Signal + Tracker Skeleton
1. Add resize callbacks in Cocoa path to mark begin/update/end of live resize.
2. Add `ResizeDynamicsTracker` to `WidgetTreeHost`.
3. Emit resize session snapshot to logs/diagnostics.

Acceptance:
1. Every resize produces coherent begin/update/end session with monotonic timestamps.
2. Velocity/acceleration numbers are finite and stable.

### Slice B: ViewResizeCoordinator Foundation
1. Add `ViewResizeCoordinator` class associated with `View`.
2. Add child resize specs (`resizable`, `policy`, `clamp`, growth weights).
3. Integrate coordinator with container-style views (at minimum `StackWidget` and `UIView`).
4. Ensure resolved child rects stay inside parent content bounds.

Acceptance:
1. Resizable children resize with parent but never exceed parent bounds.
2. Child min/max clamps are respected in all resize phases.
3. Non-resizable children keep fixed size and valid clamped position.

### Slice D: Animated Governor Metadata Plumbing
1. Attach `ResizeGovernorMetadata` to packet submissions.
2. Extend lane telemetry snapshot with governor inputs.
3. Ensure packet epochs remain monotonic with governor decisions.
4. Include coordinator generation id in packet diagnostics to detect stale geometry.

Acceptance:
1. Metadata visible in diagnostics for each animated packet during resize.
2. No regressions in current epoch gating behavior.

### Slice E: Capacity Model + Admission Policy + Quality Ladder
1. Implement EWMA lane capacity model.
2. Map resize velocity to dynamic budget.
3. Apply admission decisions: submit/coalesce/drop.
4. Define effect quality levels under pressure and auto-recover after stability.

Acceptance:
1. In-flight lane pressure remains bounded during aggressive resize.
2. Intermediate stale frames drop; newest epoch preserved.
3. Visual continuity remains during resize with controlled quality degradation.

### Slice F: Validation + Tuning Harness
1. Add stress scenarios for:
   - static multi-layer trees,
   - animated/effect-heavy trees,
   - mixed text + primitives,
   - nested resizable children with clamps.
2. Record resize metrics and packet stats for pass/fail.
3. Tune thresholds (`velocityEpsilon`, `accelEpsilon`, `deadPeriodMs`, EWMA constants).

Acceptance:
1. No corner-collapse under target resize stress.
2. Jitter reduced to acceptable threshold in both tests.
3. Child clamping invariants hold under repeated resize sweeps.

## Initial Threshold Defaults (Tunable)
1. `deadPeriodMs = 120`
2. `velocityEpsilon = 20 px/s`
3. `accelEpsilon = 80 px/s^2`
4. EWMA alpha (latency) = `0.2`
5. resize pressure decay window = `200 ms`

## Risks
1. Over-suspension can make resize feel unresponsive for static trees.
2. Too-aggressive dropping can cause visible temporal stepping.
3. Quality degradation policy can hide content if not bounded.
4. Coordinator integration can drift if legacy containers bypass it.

## Mitigations
1. Keep fallback throttle mode behind runtime flag.
2. Always preserve most recent authoritative epoch.
3. Clamp degradation levels and enforce recovery timeout.
4. Add one shared geometry invariant checker in coordinator path.

## Recommended Execution Order
1. Slice A
2. Slice B
3. Slice D
4. Slice E
5. Slice F
