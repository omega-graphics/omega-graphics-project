# OmegaGTE Resource Tracking Plan

## Goal
Add engine-level GPU resource lifecycle telemetry to OmegaGTE that is independent from OmegaWTK tracing.

## Scope
Track resource lifecycles and submission timing inside OmegaGTE only:

1. Command queues
2. Command buffers
3. Native render targets
4. Texture render targets / textures
5. Pipeline state objects

## Non-Goals

1. No dependency on OmegaWTK types or layer tree metadata.
2. No compositor packet correlation in Slice 1.
3. No behavior changes to rendering flow in Slice 1.

## Event Schema
Each event carries:

1. `ts` (unix timestamp in milliseconds)
2. `backend` (`Common`, `Metal`, `D3D12`, `Vulkan`)
3. `event` (`Create`, `Destroy`, `Submit`, `Complete`, `Present`, `ResizeRebuild`, `Bind`, `Unbind`, `Marker`)
4. `resourceType` (string)
5. `resourceId` (monotonic uint64)
6. `nativeHandle` (pointer value as uint64)
7. `threadId` (hashed std::thread::id)
8. optional dimensions (`width`, `height`, `scale`)

## Runtime Toggle
Enable log output with:

`OMEGAGTE_RESOURCE_TRACE=1`

When disabled, tracker methods must be cheap no-ops.

## Slices

### Slice 1: Core Tracker Module
Deliver:

1. `GEResourceTracker` module in `gte/src/common`.
2. Monotonic resource id allocator.
3. Env-gated event logging.
4. In-memory recent-event ring buffer for debug pull.
5. No backend hooks required yet.

### Slice 2: Metal Hook-in
Add hooks to Metal create/destroy points for queues, targets, textures, buffers, and command buffers.

### Slice 3: DX/Vulkan Hook-in
Add same lifecycle hooks for D3D12 and Vulkan backends.

### Slice 4: Submission Timeline
Track submit/complete/present with queue/command-buffer correlation IDs.

### Slice 5: Churn Metrics
Compute per-type create/destroy counts, short lifetimes, and startup burst indicators.

### Slice 6: Debug Dump API
Expose snapshot and recent timeline dump helpers for diagnostics tooling.

## Slice 1 Implementation Notes
For this slice:

1. Keep the tracker internal to OmegaGTE source.
2. Keep APIs simple and backend-agnostic.
3. Avoid additional locks in hot path when tracing is disabled.
4. Avoid introducing compile-time dependencies outside standard library.
