# Resize Velocity Governor Plan

## Goal
Eliminate resize jitter/corner-collapse by adding explicit resize dynamics tracking in the native/UI path and a GPU-capacity governor in the compositor.

This plan incorporates the proposal in `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/MY_PROPOSALS.md`:
1. Track resize behavior (velocity, acceleration/deceleration, and dead period).
2. Suspend static rendering during active resize and resume on completion.
3. For animated content, pace submission against measured GPU/compositor capacity.

## Problem Model
1. Active resize generates high-frequency geometry/layout churn.
2. Widget/layout updates and render packet submission can outpace GPU present/complete cadence.
3. During this mismatch, stale/incomplete epochs can be presented transiently (jitter/corner collapse symptoms).

## Design Principles
1. Resize state detection must be owned by native/UI layers, not inferred only from compositor packets.
2. Compositor should consume explicit resize state and lane pressure telemetry to make pacing decisions.
3. Static and animated trees need different policies.
4. Keep last good frame visible while coalescing new state.

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

### 3) Static Render Policy
For non-animated widget trees during `Active`/`Settling`:
1. Suspend immediate packet submission.
2. Continue local layout recomputation/coalescing, but do not submit each intermediate frame.
3. Keep last presented frame visible.
4. On `Completed`, submit one authoritative full-layout frame.

Optional safety valve: low-frequency fallback frame (e.g. 8-12 FPS) behind a debug flag.

### 4) Animated Render Policy (Compositor Governor)
For animated trees during resize:
1. Compute lane capacity from telemetry:
   - presented cadence
   - in-flight count
   - EWMA submit->present latency
   - optional GPU duration (if available)
2. Estimate incoming resize pressure from tracker velocity.
3. Admit/coalesce/drop packets by budget:
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

### 6) Integration with Existing Plans
This plan extends current docs and code paths:
- `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/WidgetTree-Sync-Engine-Plan.md`
- `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/docs/LayerTree-Frontend-Backend-Sync-Plan.md`

No replacement of packet epoch gating; this is an upstream pacing and policy layer.

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

### Slice B: Static Suspend Pipeline
1. Add `StaticSuspend` policy for non-animated trees.
2. During active resize, queue/coalesce invalidations without submitting each frame.
3. Submit one authoritative frame at completion.

Acceptance:
1. Static trees do not jitter during drag.
2. Final post-resize frame lands correctly within one cycle.

### Slice C: Animated Governor Metadata Plumbing
1. Attach `ResizeGovernorMetadata` to packet submissions.
2. Extend lane telemetry snapshot with governor inputs.
3. Ensure packet epochs remain monotonic with governor decisions.

Acceptance:
1. Metadata visible in diagnostics for each animated packet during resize.
2. No regressions in current epoch gating behavior.

### Slice D: Capacity Model + Admission Policy
1. Implement EWMA lane capacity model.
2. Map resize velocity to dynamic budget.
3. Apply admission decisions: submit/coalesce/drop.

Acceptance:
1. In-flight lane pressure remains bounded during aggressive resize.
2. Intermediate stale frames drop; newest epoch preserved.

### Slice E: Effect Degradation Ladder
1. Define effect quality levels under pressure.
2. Reduce expensive effects before dropping animation-critical packets.
3. Recover quality after stable period.

Acceptance:
1. Visual continuity remains during resize.
2. Effects recover automatically after resize completion.

### Slice F: Validation + Tuning Harness
1. Add stress scenarios for:
   - static multi-layer trees
   - animated/effect-heavy trees
   - mixed text + primitives
2. Record resize metrics and packet stats for pass/fail.
3. Tune thresholds (`velocityEpsilon`, `accelEpsilon`, `deadPeriodMs`, EWMA constants).

Acceptance:
1. No corner-collapse under target resize stress.
2. Jitter reduced to acceptable threshold in both tests.

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

## Mitigations
1. Keep fallback throttle mode behind runtime flag.
2. Always preserve most recent authoritative epoch.
3. Clamp degradation levels and enforce recovery timeout.

## Recommended Execution Order
1. Slice A
2. Slice B
3. Slice C
4. Slice D
5. Slice E
6. Slice F
