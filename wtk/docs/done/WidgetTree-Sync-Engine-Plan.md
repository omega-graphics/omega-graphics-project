# WidgetTree Sync Engine Plan (Re-evaluated)

## Goal
Eliminate partial-frame interleaving and resize jitter by keeping each `WidgetTree` lane packet-coherent, admission-gated, and paced against compositor lifecycle telemetry.

## Re-evaluation Summary
The original plan is now partially obsolete. Core sync-engine features are implemented and live:

1. Global compositor runtime is active through `WidgetTreeHost`.
2. Per-tree sync lanes are assigned and propagated.
3. Proxy submission batches commands into lane packets with `syncPacketId`.
4. Compositor packet lifecycle and lane in-flight gating are implemented.
5. Animation runtime now consumes lane telemetry for ack-driven pacing.

The remaining work is no longer "build sync engine from scratch"; it is stabilization, adaptive quality, and diagnostics hardening.

## Implemented Features (Current State)

### Slice A (Completed): Packetized Lane Submission
1. `WidgetTreeHost` uses a process-global compositor.
2. Each host gets a stable `syncLaneId`.
3. Commands are stamped with `syncLaneId` and `syncPacketId`.
4. `CompositorClientProxy::submit()` emits `CompositorPacketCommand` for multi-command submissions.
5. Scheduler executes packet members in recorded order.

### Slice B (Completed): Completion Plumbing
1. Packet lifecycle phases exist (`Queued`, `Submitted`, `GPUCompleted`, `Presented`, `Dropped`, `Failed`).
2. Backend submission completion feeds packet telemetry state.
3. Packet release is tied to completion/present lifecycle transitions.

### Slice C (Completed): Lane Gating and Backpressure
1. Per-lane `inFlight` counters are tracked.
2. Lane budget enforcement is active:
   - normal budget: `2`
   - resize budget: `1`
3. Resize activity opens a temporary resize budget window.
4. Target-aware stale packet dropping exists for saturated lanes.

### Slice D (Completed): Animation Clock Integration
1. Animation runtime is lane-scoped and grouped by compositor proxy.
2. `TimingOptions` supports `ClockMode` (`WallClock`, `PresentedClock`, `Hybrid`).
3. Runtime consumes `getLaneTelemetrySnapshot(...)` for presented/dropped pacing.
4. Handle diagnostics now track submitted/presented packet ids and dropped counts.

## Current Runtime Contract

1. One packet = one lane submission epoch.
2. All packet members share:
   - `syncLaneId`
   - `syncPacketId`
3. Admission is lane-budget constrained before execution.
4. Completion telemetry updates lane state and drives animation progression.

## Known Gaps

1. First-frame consistency is not fully deterministic for effect-heavy scenes.
2. Telemetry is functional but still minimal for tuning:
   - lane snapshots expose packet ids/inFlight/drop counts
   - rolling quality metrics (EWMA latency budgeting) are not yet wired for policy control
3. Diagnostics are still developer-light for rapid jitter triage.

## Remaining Slices

### Slice E: Adaptive Quality + First-Frame Stabilization
1. Add first-frame stabilization gate for effect-enabled trees:
   - ensure initial packet includes resolved effect state before first visible present
   - prevent first-frame effect dropout
2. Add lane pressure metrics suitable for policy:
   - moving average submit->present latency
   - moving average GPU duration (best effort when available)
3. Add resize-mode adaptive policies:
   - clamp expensive effect parameters under sustained pressure
   - restore full quality when lane health recovers

### Slice F: Instrumentation and Debug Surfaces
1. Add per-lane counters:
   - packets queued/submitted/presented/dropped/failed
   - saturation occurrences
   - coalesced stale packet count
2. Add concise trace hooks for packet epochs and lane admission waits.
3. Add optional per-tree debug dump utility to capture lane state snapshots on demand.

### Slice G: Validation and Regression Hardening
1. Expand stress scenarios:
   - active resize + layer effects + animation
   - mixed UIView shape/text workloads on one lane
2. Add regression checks for:
   - first-frame effect presence
   - no delayed primitive reveal after first resize
   - monotonic animation progress under packet drops
3. Gate future compositor/UI changes against this suite.

## Acceptance Criteria (Updated)

1. No partial interleaving within a WidgetTree lane packet epoch.
2. First visible frame contains expected effect state for effect-enabled tests.
3. Active resize keeps lane in-flight within configured budget and avoids stale visual replay.
4. Animation progression remains monotonic relative to presented packet epochs.
5. Diagnostics are sufficient to explain jitter regressions without GPU trace dependency.

## Suggested Execution Order

1. Slice E first (highest user-visible impact: first-frame/effect and resize pressure).
2. Slice F second (improves iteration speed and regression triage).
3. Slice G last (lock behavior with repeatable validation).
