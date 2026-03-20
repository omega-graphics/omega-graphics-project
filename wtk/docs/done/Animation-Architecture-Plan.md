# OmegaWTK Animation Architecture Plan (Sync-Engine Aligned)

## Purpose
Adapt animation architecture to the current WidgetTree sync engine so animation updates are packet-coherent, paced by lane admission, and anchored to completion/present telemetry instead of CPU submit cadence alone.

## Re-evaluation Summary
1. The old plan assumed a compositor tick loop that could drive animations independently.
2. The current runtime is now lane/packet based with:
- `syncLaneId` per `WidgetTreeHost`.
- `syncPacketId` per composition submission.
- packet lifecycle (`Queued`, `Submitted`, `GPUCompleted`, `Presented`, `Dropped`, `Failed`).
- per-lane in-flight gating and resize-mode budget.
3. Animation APIs exist for curves, keyframe tracks, clips, and handles, but playback is still mostly queue-driven and wall-clock threshold based.
4. The architecture must move from "issue many timed commands" to "sample once per lane epoch and submit one coherent packet".

## Design Goals
1. Ensure each presented animation step is a coherent packet per lane.
2. Remove jitter from CPU/GPU temporal mismatch during resize/effects load.
3. Preserve current public animation API direction (`TimingOptions`, curves, tracks, clips, handles).
4. Keep compatibility with transition helpers (`resizeTransition`, effect transitions) by re-implementing them as clip wrappers.

## Sync-Engine Constraints
1. All updates for a WidgetTree should flow through a single lane (`syncLaneId`).
2. Admission of the next packet should respect lane in-flight budget (`normal=2`, `resize=1`).
3. Stale packet pruning may occur under pressure; animation must be resilient to dropped intermediate packets.
4. "Frame advanced" should be defined by completion/present ack, not by CPU submit.

## Runtime Model
1. Lane-bound animation runtime
- Maintain active animation instances per `syncLaneId`.
- Each lane has one animation sampling state and one pending "next packet payload".

2. Hybrid animation clock
- Base time source is monotonic CPU clock.
- During contention, clamp progression to lane-presented epochs.
- On dropped packets, skip stale intermediate visual states and resample at current effective time.

3. Packet-coherent sampling
- For each lane tick, sample all active clips for a single lane timestamp.
- Coalesce target writes so each target receives at most one effective value per property in that packet.
- Submit one packet carrying all resulting render/layer/view commands for that lane epoch.

4. Completion-driven advancement
- On packet `Presented` or `GPUCompleted` ack, advance lane epoch and update handle progress/state.
- On `Dropped`, mark epoch skipped and immediately resample current time for the next admitted packet.
- On `Failed`, transition affected handles to `Failed` and stop clip instance.

## API Adjustments (Planned)
1. `TimingOptions` additions
- `clockMode` with values: `WallClock`, `PresentedClock`, `Hybrid` (default `Hybrid`).
- `maxCatchupSteps` (default `1`) to limit burst replay.
- `preferResizeSafeBudget` (default `true`) so resize/drag forces one-step pacing.

2. `AnimationHandle` additions
- `lastSubmittedPacketId()`.
- `lastPresentedPacketId()`.
- `droppedPacketCount()`.
- `failureReason()` (optional diagnostic string/enum).

3. Animator entrypoints
- Keep existing `animate(clip, timing)` signatures.
- Add explicit lane-targeted overload for advanced usage:
  - `animateOnLane(clip, timing, syncLaneId)`.

4. Diagnostics API
- Add optional per-handle telemetry snapshot:
  - effective lane id
  - current in-flight depth
  - submitted/presented packet ids
  - dropped packet count

## Compositor Integration
1. No new public command type is required for first pass.
2. Animation runtime materializes sampled states into existing commands:
- `CompositorViewCommand` for view rect/opacity-affecting view changes.
- `CompositorLayerCommand` for transforms/shadows.
- `CompositionRenderCommand` for frame/canvas changes.
3. Runtime submits sampled commands through existing proxy submission so packet metadata is automatically stamped.
4. Runtime consumes existing backend completion telemetry to drive lane epoch progression.

## Property Write Rules
1. Last-writer-wins per packet for same target/property.
2. Deterministic property order in packet build:
- layout-affecting rect updates.
- transform updates.
- shadow/effect updates.
- render frame updates.
3. During resize-mode budget (`inFlight=1`), disallow multi-step catch-up; only newest sampled state is emitted.

## Transition Compatibility
1. `resizeTransition` becomes a thin wrapper over `ViewClip`/`LayerClip` rect tracks.
2. `shadowTransition` and `transformationTransition` map to typed keyframe tracks.
3. Legacy methods remain, but implementation routes through unified clip runtime.

## Slice Plan (Rebased To Sync Engine)

### Slice A (Completed)
1. Curve sampling API and easing presets.
2. Typed keyframe track interpolation traits.
3. Handle state/control shell.
4. Clip type shells and `animate(...)` entrypoint stubs.

### Slice B (Completed In Sync Engine)
1. Packet lifecycle telemetry plumbing from backend completion into compositor.
2. Per-lane packet state tracking (`Queued`..`Presented`).
3. Per-lane in-flight accounting and admission hooks.

### Slice C (Completed In Sync Engine)
1. Lane admission gating with resize-mode budget.
2. Target-aware stale packet dropping under saturation.
3. Lane runtime state (`inFlight`, resize hold window).

### Slice D (Completed: Animation Runtime Binding)
1. Added a lane-scoped animation runtime registry in `/wtk/src/Composition/Animation.cpp`.
2. Added lane tick workers that sample active clips and submit grouped command packets through the existing proxy path.
3. Bound handle state/progress and packet ids to compositor packet lifecycle telemetry snapshots.

### Slice E (Completed: Ack-Driven Clocking)
1. Added `ClockMode` (`WallClock`, `PresentedClock`, `Hybrid`) and timing knobs (`maxCatchupSteps`, `preferResizeSafeBudget`) to `TimingOptions`.
2. Added compositor lane telemetry snapshots (`getLaneTelemetrySnapshot`) and used them to drive presented/ack pacing.
3. Added stale-step skipping behavior by dropping pending per-handle packet ids when lane dropped counts advance.

### Slice F (Completed: Transition Rewire)
1. Re-routed `resizeTransition`, `shadowTransition`, and `transformationTransition` to produce typed clips and run through `animate(...)`.
2. Added lane-targeted animation overloads (`animateOnLane`) while preserving existing method signatures.

### Slice G (Next: Validation + Instrumentation)
1. Add stress tests for resize + effects + active animation on the same lane.
2. Add per-lane animation diagnostics (dropped steps, packet lag, jitter proxy).
3. Validate in `EllipsePathCompositorTest`, `TextCompositorTest`, and upcoming `UIView`/`SVGView`/`VideoView` tests.

## Explicit Non-Goals
1. No cross-lane global ordering guarantee beyond current compositor queue ordering.
2. No hard VSync lock in this phase.
3. No backend-specific animation paths; backend neutrality is preserved through existing telemetry contract.

## Risks And Mitigations
1. Risk: first visible frame may miss late effect updates.
- Mitigation: packet-coherent sampling for animation writes and lane startup warmup gate.
2. Risk: animation appears to "jump" under packet drops.
- Mitigation: explicit stale-step skipping with deterministic resample at latest effective time.
3. Risk: resize contention starves animation progression.
- Mitigation: automatic resize-safe pacing and post-resize catch-up cap (`maxCatchupSteps`).

## Acceptance Criteria
1. Active resize with effects no longer shows interleaved/partial animation states within one lane.
2. Animation progression remains monotonic relative to presented packet epochs.
3. Dropped packets do not replay stale intermediate states.
4. Transition helpers produce the same final values as direct clip animation.
