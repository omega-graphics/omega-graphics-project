# GPU Resource Tracking Plan

## Goal
Provide deterministic visibility into GPU resource lifecycle so startup/render churn can be measured, attributed, and reduced without guessing from log noise.

## Problem Summary
Current logs show repeated:

`Metal Command Queue Destroy`

This log line is emitted when a `GEMetalCommandQueue` is destructed, not when a frame is rendered. Because one native render target can own one command queue, repeated destroy logs imply target/queue churn, but do not directly indicate frame count.

## Observed Symptom (Current Evidence)
From `EllipsePathCompositorTest`, we have bursts of dozens of `Metal Command Queue Destroy` lines clustered at the same timestamp near startup/teardown windows (for example around `2026-02-27 23:44:37.139`).

This indicates one or both:

1. A render-target recreate storm during initialization/layout churn.
2. A teardown cascade where many short-lived targets created earlier are released together.

Without per-resource attribution, these logs cannot distinguish "too many frames" from "too many target lifecycles".

## Scope
Track lifecycle and usage of:

1. Native render targets (`GENativeRenderTarget` / `GEMetalNativeRenderTarget`)
2. Command queues (`GECommandQueue` / `GEMetalCommandQueue`)
3. Texture render targets and backing textures used by `BackendRenderTargetContext`
4. Compositor packets/frames that reference those resources

## Non-Goals
1. No renderer architecture rewrite in this plan.
2. No performance policy changes yet (only observability + guardrails).
3. No user-facing API changes in first phase.

## Key Outcomes
1. Distinguish frame count from resource create/destroy count.
2. Attribute each resource to lane/tree/layer and packet id.
3. Detect startup churn, short-lifetime resources, and recreate storms.
4. Produce actionable reports for optimization.

## Tracking Model

### Identity
Each tracked GPU resource gets:

1. `resourceId` (monotonic uint64)
2. `resourceType` (`NativeTarget`, `CommandQueue`, `TextureTarget`, `Texture`, etc.)
3. `ownerKind` (`RootVisual`, `ChildVisual`, `EffectPipeline`, `Transient`)
4. `ownerKey` (`syncLaneId`, `LayerTree*`, `Layer*`, `RenderTarget*`)
5. creation timestamp and destruction timestamp

### Frame/Packet Correlation
Each render packet captures:

1. `syncLaneId`, `syncPacketId`
2. render command id
3. resource ids touched by packet
4. submit/complete/present timestamps
5. packet result (`Presented`, `Dropped`, `NoOp`, `Failed`)

### Derived Metrics
1. Resources created per packet
2. Resources destroyed per packet
3. Short lifetime resources (destroyed within N packets or < X ms)
4. Churn ratio: `(creates + destroys) / presentedFrames`
5. Startup stabilization window (time until churn ratio < threshold)

## Slice Plan

### Slice A: Lifecycle Event Instrumentation
Add lightweight event hooks at create/destroy sites.

1. `GEMetalCommandQueue` ctor/dtor
2. `GEMetalNativeRenderTarget` ctor/dtor
3. `BackendRenderTargetContext::rebuildBackingTarget`
4. `BackendVisualTree` visual create/remove

Output event schema:

1. timestamp
2. event type (`Create`, `Destroy`, `ResizeRebuild`, `Bind`, `Unbind`)
3. resource id/type
4. owner fields
5. optional dimensions/scale

### Slice B: Compositor Correlation Layer
Record resource usage per compositor packet.

1. On command execution, attach active packet id to context
2. On resource event, include current packet context when available
3. Maintain per-lane rolling stats

### Slice C: Startup Churn Report
Add startup report emitted once after first stable present window.

Report fields:

1. packets queued/submitted/presented/dropped
2. unique resources created/destroyed
3. churn ratio
4. top churn contributors by owner kind/key
5. suspected recreate loops (same owner repeatedly recreated)

### Slice C.1: Startup vs Teardown Attribution
Extend report to split lifecycle events into:

1. `StartupWindow` (process launch to first stable-present window)
2. `SteadyStateWindow`
3. `ShutdownWindow` (scheduler/compositor shutdown)

For each window, report:

1. command queues created/destroyed
2. native targets created/destroyed
3. average lifetime of destroyed resources
4. top destroy bursts by timestamp bucket

This directly answers whether destroy bursts are startup waste or expected teardown.

### Slice D: Guardrails + Assertions (Debug)
Detect invalid churn patterns during development.

1. Warn on repeated create/destroy of same owner within small window
2. Warn when queue destroy count greatly exceeds presented packet count
3. Assert no resource destroy while still referenced by active packet context

### Slice E: On-Demand Dump + Timeline
Add developer dump command/env toggle.

1. Dump current resource graph
2. Dump last N packets with resource touches
3. Emit compact timeline for startup sequence

### Slice F: Optimization Readiness Hooks
Add counters needed before optimization patches.

1. How many native targets are reused vs recreated
2. How often `setRenderTargetSize` triggers backing rebuild
3. Queue reuse hit rate
4. Candidate pools for queue/target reuse

## Implementation Notes (Current Code Paths)

Primary hook points:

1. `/Users/alextopper/Documents/GitHub/omega-graphics-project/gte/src/metal/GEMetalCommandQueue.mm`
2. `/Users/alextopper/Documents/GitHub/omega-graphics-project/gte/src/metal/GEMetalRenderTarget.mm`
3. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/Composition/backend/RenderTarget.cpp`
4. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/Composition/backend/Execution.cpp`
5. `/Users/alextopper/Documents/GitHub/omega-graphics-project/wtk/src/Composition/Compositor.cpp`

Recommended runtime toggle:

1. `OMEGAWTK_GPU_RESOURCE_TRACE=1` for verbose event stream
2. `OMEGAWTK_GPU_RESOURCE_REPORT=1` for startup summary only

## Validation
Use two tests:

1. `EllipsePathCompositorTest`
2. `TextCompositorTest`

Expected after instrumentation:

1. Exact startup presented packet count is reported.
2. Destroy logs are attributable to specific resource ids/owners.
3. Recreate storms are identifiable within first run.

## Exit Criteria
Plan is complete when:

1. We can answer “how many startup frames were actually presented” from telemetry alone.
2. We can answer “why each command queue was destroyed” with owner and packet context.
3. We can isolate the top churn source before any optimization patch is attempted.
4. We can separate startup churn from shutdown destruction with windowed attribution.
