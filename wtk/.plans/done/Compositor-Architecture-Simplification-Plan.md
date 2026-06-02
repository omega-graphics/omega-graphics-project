# Compositor Architecture Simplification Plan

## Problem

The compositor thread currently handles three concerns that should be separated:

1. **GPU command execution** (render, blit, present) — correct responsibility
2. **Backend mirror layer tree management** (`backendLayerMirror`, epoch tracking, `applyLayerTreePacketDeltasToBackendMirror`) — should be on the main thread
3. **Native view/layer resizing** (`RootVisual::resize`, `resizeVisualForSurface`, CAMetalLayer geometry) — should be on the main thread

Mixing these on the compositor thread creates:
- `dispatch_sync` deadlocks during live resize (main thread busy in tracking mode)
- Epoch-based frame gating (`waitForRequiredTreeEpoch`) that blocks GPU work
- Stale coalescing (`dropQueuedStaleForLaneLocked` with `liveResizeEpoch`) that drops frames carrying updated content
- A complex resize governor (budget, pacing, pressure, quality degradation) that was designed to manage contention between threads but ends up throttling the pipeline

## Guiding Principle

The compositor thread executes GPU commands. Nothing else. All native layer geometry lives on the main thread where `View::resize` already runs.

---

## Phase 1 — Main-thread native layer geometry

**Goal:** CAMetalLayer frame/bounds/drawableSize are updated on the main thread during `View::resize`, not on the compositor thread during `executeCurrentCommand`.

### 1.1 Add native layer handle to `Layer`

| File | Change |
|------|--------|
| `Layer.h` | Add `void *nativeLayerHandle = nullptr` to `Layer`. Add `setNativeLayer(void *)`, `void *getNativeLayer()`. |
| `Layer.cpp` | Implement trivial accessors. |

### 1.2 Wire native layer creation through `Layer`

| File | Change |
|------|--------|
| `CALayerTree.mm` `makeRootVisual` | After creating the `CAMetalLayer`, store it on the `Layer` via `layer->setNativeLayer(metalLayer)` (the root layer from the `LayerTree`). |
| `DCVisualTree.cpp` | Same pattern for `IDCompositionVisual`. |
| `VKLayerTree.cpp` | Same pattern for Vulkan surface (if applicable). |

### 1.3 Move CAMetalLayer geometry updates to `View::resize`

| File | Change |
|------|--------|
| `View.cpp` `View::resize` | After `renderTarget->getNativePtr()->resize(rect)` and `ownLayerTree->getRootLayer()->resize(rect)`, read the native layer handle from the root layer and update its geometry (frame, bounds, drawableSize, contentsScale). This is the logic currently in `RootVisual::resize` (CALayerTree.h). |
| `CALayerTree.h` `RootVisual::resize` | Remove CAMetalLayer geometry updates (frame, bounds, position, drawableSize). Keep only `renderTarget.setRenderTargetSize(newRect)`. |

### 1.4 Remove `resizeVisualForSurface` from execution path

| File | Change |
|------|--------|
| `Execution.cpp` `executeCurrentCommand` | Remove the `resizeVisualForSurface(*target, targetContext, layerRect)` call. The render target size is still set via `targetContext->setRenderTargetSize(layerRect)` using the frame's snapshot rect (`comm->frame->rect`). |
| `Execution.cpp` | Delete the `resizeVisualForSurface` function. |

### Verification
- Build all three tests (TextCompositorTest, EllipsePathCompositorTest, SVGViewRenderTest).
- Resize window — CAMetalLayer should update synchronously on the main thread.
- Content should render at the correct size (frame rect snapshot matches viewport).

---

## Phase 2 — Remove the backend mirror layer tree

**Goal:** Eliminate `backendLayerMirror`, `LayerTreeDelta`, `layerTreeSyncState`, epoch tracking, and `waitForRequiredTreeEpoch`. The compositor no longer tracks layer geometry — it just renders what the frame tells it.

### 2.1 Remove delta generation

| File | Change |
|------|--------|
| `Layer.cpp` `Layer::resize` | Remove `parentTree->notifyObserversOfResize(this)` call. Keep `surface_rect = newRect`. |
| `Layer.cpp` `Layer::setEnabled/setDisabled` | Remove `notifyObserversOfEnable/Disable` calls. |
| `Compositor.cpp` | Remove `layerHasResized`, `layerHasDisabled`, `layerHasEnabled` implementations (keep as empty overrides or remove the observer interface). |

### 2.2 Remove delta storage and binding

| File | Change |
|------|--------|
| `Compositor.h` | Remove `LayerTreeDeltaType`, `LayerTreeDelta`, `LayerTreeSyncState`, `BackendLayerMirrorLayerState`, `BackendLayerMirrorTreeState`. Remove `layerTreeSyncState`, `backendLayerMirror`, `layerTreePacketMetadata` maps. |
| `Compositor.cpp` | Remove `enqueueLayerTreeDeltaLocked`, `coalesceLayerTreeDeltasLocked`, `bindPendingLayerTreeDeltasToPacketLocked`, `applyLayerTreePacketDeltasToBackendMirror`. |

### 2.3 Remove epoch gating from command processing

| File | Change |
|------|--------|
| `Compositor.cpp` `processCommand` | Remove the `waitForRequiredTreeEpoch` call and its failure path. |
| `Compositor.cpp` | Remove `waitForRequiredTreeEpoch`, `arePacketEpochRequirementsSatisfiedLocked`, `isPacketEpochSupersededLocked`, `stampCommandRequiredEpochLocked`. |
| `CompositorClient.h` `CompositorCommand` | Remove `requiredTreeEpoch` field. |

### 2.4 Remove epoch-based stale coalescing

| File | Change |
|------|--------|
| `Compositor.cpp` `scheduleCommand` | Remove `bindPendingLayerTreeDeltasToPacketLocked` call. Remove `coalesceForEpoch` and `liveResizeEpoch` from the stale-drop trigger condition. The trigger becomes: `renderLike && commandHasNonNoOpRender(command)`. |
| `Compositor.cpp` `dropQueuedStaleForLaneLocked` | Remove the `supersededByEpoch` check. Drop only based on target overlap. Remove `packetMetadataContainsResizeDeltaLocked`. |

### 2.5 Remove layer tree observer registration

| File | Change |
|------|--------|
| `Compositor.cpp` | Remove `observeLayerTree` / `unobserveLayerTree` layer-tree-delta-related logic (epoch tracking, lane binding). Keep the observer attachment for tree attach/detach lifecycle if needed by `ensureLayerSurfaceTarget`. |

### Verification
- Build and run all tests.
- Verify no epoch-related sync traces (`OMEGAWTK_SYNC_TRACE=1`).
- Resize should have zero `waitForRequiredTreeEpoch` delays.

---

## Phase 3 — Remove the resize governor

**Goal:** Eliminate all resize-specific throttling. Lane admission uses a flat in-flight budget. No pacing, no pressure, no quality degradation.

### 3.1 Remove resize governor metadata propagation

| File | Change |
|------|--------|
| `CompositorClient.h` | Remove `ResizeGovernorMetadata` struct. Remove `resizeGovernorMetadata` and `resizeCoordinatorGeneration` from `CompositorClientProxy`. Remove `resizeGovernor` and `resizeCoordinatorGeneration` from `CompositorCommand`. |
| `CompositorClient.cpp` | Remove `setResizeGovernorMetadata`. Remove governor/coordinator stamping in `submit()`. |
| `WidgetTreeHost.cpp` | Remove `applyResizeGovernorMetadata`, `makeResizeGovernorMetadata` calls from `notifyWindowResize`, `notifyWindowResizeBegin`, `notifyWindowResizeEnd`. |
| `View.cpp` | Remove `setResizeGovernorMetadataRecurse`. |

### 3.2 Simplify lane admission

| File | Change |
|------|--------|
| `Compositor.cpp` `laneBudgetForNow` | Return `kMaxFramesInFlightNormal` always. Remove `resizeBudgetActive`, velocity, pressure checks. |
| `Compositor.cpp` `laneMinSubmitSpacingForNow` | Return `0` always (no pacing). Or remove entirely and skip the `paceReady` check in `waitForLaneAdmission`. |
| `Compositor.cpp` `waitForLaneAdmission` | Simplify to: check `inFlight < budget`, if yes admit, if no sleep 1ms and retry. Remove pacing, saturation, startup hold logic. |

### 3.3 Remove resize governor state from telemetry

| File | Change |
|------|--------|
| `Compositor.h` | Remove `resizeModeUntil`, `latestResizeGovernor`, `latestResizeCoordinatorGeneration` from `LaneRuntimeState`. Remove `kMaxFramesInFlightResize`, `kResizeModeHoldWindow`. Remove `GovernorTuningConfig`. Remove resize-related fields from `LaneTelemetrySnapshot` and `LaneDiagnosticsSnapshot`. |
| `Compositor.cpp` | Remove `markLaneResizeActivity`, `resizeGovernorIndicatesActive`, `desiredLaneQualityForNow`, `updateLaneQualityForPresentedPacket`, `loadGovernorTuningConfig`, `isLaneSaturated`, `isLaneUnderPressure`, `adaptCanvasEffectForLane`. Remove `LaneEffectQuality` enum. |

### 3.4 Remove resize validation from WidgetTreeHost

| File | Change |
|------|--------|
| `WidgetTreeHost.h` | Remove `ResizeValidationSession`, `resizeValidationSession`, `resizeValidationTuning`, `resizeValidationScenario`. |
| `WidgetTreeHost.cpp` | Remove validation begin/end/sample logic from `notifyWindowResizeBegin`, `notifyWindowResize`, `notifyWindowResizeEnd`. |

### Verification
- Build and run all tests.
- Resize should produce no governor-related log messages.
- Lane admission should pass immediately when `inFlight < 2`.

---

## Phase 4 — Clean up dead code

**Goal:** Remove all code that is no longer referenced after Phases 1–3.

| Area | Candidates |
|------|-----------|
| `Compositor.h` / `.cpp` | Dead governor tuning env-var readers, dead sync trace emit calls, unused `commandContainsResizeActivity`, stale coordinator generation tracking. |
| `CompositorClient.h` / `.cpp` | Unused `ResizeGovernorPhase` enum, `resizeGovernorPhaseName`. |
| `MainThreadDispatch.h` | If no callers remain, remove the header. |
| `RenderTarget.h` | Remove `BackendSubmissionCompletionHandler` parameter from `commit()` if the callback is no longer used. |
| `WidgetTreeHost.h` / `.cpp` | Remove `ResizeTracker`, `ResizeSessionState`, `ResizePhase` if no longer used. |

### Verification
- Clean build with no warnings about unused functions/variables.
- Run all tests.
