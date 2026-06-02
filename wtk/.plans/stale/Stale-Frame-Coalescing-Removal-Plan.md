# Stale Frame Coalescing Removal Plan

Remove `dropQueuedStaleForLaneLocked` and all supporting infrastructure. The mechanism was designed to prevent the compositor from rendering frames that a newer frame has already superseded, but in practice it causes visible content gaps during resize: the coalescing drops the only frame that carries the correct post-resize geometry, the compositor spends hundreds of milliseconds clearing the queue, and the user sees a frozen or blank surface until a fresh frame eventually arrives.

---

## Problem statement

When a View resizes, the resize path triggers `Widget::invalidate(PaintReason::Resize)`, which records a new `CanvasFrame` at the new dimensions and submits it through the `CompositorClientProxy`. At the same time, the resize may produce additional frames from child widgets or layout passes. `dropQueuedStaleForLaneLocked` detects that the new frame targets the same `(CompositionRenderTarget, Layer)` pair as an existing queued frame and drops the older one.

The observed failure sequence in BasicAppTest:

1. Window resize event fires.
2. Widget tree relayouts. Multiple widgets invalidate, producing frames.
3. `scheduleCommand` is called for each frame. On entry, `dropQueuedStaleForLaneLocked` scans the queue and drops any existing render command for the same target.
4. The dropped packet's `CommandStatus` is set to `Delayed`, but the frame's visual content is lost.
5. The compositor processes the surviving (newest) frame, but the queue scan and telemetry bookkeeping for the dropped packets introduce measurable delay (~500ms observed in BasicAppTest).
6. The display shows stale content from before the resize until the new frame is finally presented.

The coalescing logic was defensive — intended for high-frequency animation scenarios where the compositor falls behind. But for resize events, every frame matters because each frame carries geometry at a different size. Dropping any of them creates a visible gap.

---

## What to remove

### Phase 1 — Remove the coalescing call site

**File:** `wtk/src/Composition/Compositor.cpp`  
**Location:** `scheduleCommand()` (line ~540)

Remove the conditional block:

```cpp
if(renderLike && commandHasNonNoOpRender(command)){
    dropQueuedStaleForLaneLocked(command->syncLaneId,command);
}
```

This is the only call site. After removal, all frames are enqueued unconditionally.

### Phase 2 — Remove the coalescing implementation

**File:** `wtk/src/Composition/Compositor.cpp`

Delete `dropQueuedStaleForLaneLocked()` (lines ~760–793). This method:
- Collects render target epochs from the incoming command
- Filters the queue for same-lane, same-target commands
- Calls `markPacketDropped(..., StaleCoalesced)` for each match
- Sets `CommandStatus::Delayed` on the dropped command
- Calls `noteQueueDropLocked` for telemetry

All of this becomes dead code once Phase 1 removes the call site.

### Phase 3 — Remove supporting helpers

**File:** `wtk/src/Composition/Compositor.cpp` and `Compositor.h`

Remove:
- `collectRenderTargetsForCommand()` — only used by `dropQueuedStaleForLaneLocked` and `targetsOverlap`.
- `targetsOverlap()` — only used by `dropQueuedStaleForLaneLocked`.
- `RenderTargetEpoch` typedef (`std::pair<const CompositionRenderTarget *, const Layer *>`) — only used as the element type for the target vectors in the above functions.

### Phase 4 — Remove `StaleCoalesced` from telemetry

**Files:** `Compositor.h`, `Compositor.cpp`

- Remove `PacketDropReason::StaleCoalesced` from the enum (line ~135). Remaining values: `Generic`, `NoOpTransparent`, `EpochSuperseded`.
- Remove the `staleCoalescedCount` field from `LaneRuntimeState` (line ~167).
- Remove the `if(reason == PacketDropReason::StaleCoalesced)` branch in `markPacketDropped()` (line ~883) that increments `staleCoalescedCount`.
- Remove references to `staleCoalescedCount` in `dumpLaneDiagnostics()` output formatting.

### Phase 5 — Remove `commandHasNonNoOpRender` guard on coalescing path

**File:** `Compositor.cpp`, `Compositor.h`

`commandHasNonNoOpRender()` is also used by `markPacketQueued()` to set `entry.hasNonNoOpRender` on the `PacketLifecycleRecord`, which is read by `shouldDropNoOpTransparentFrame()`. **Do not remove this function** — it is still needed for the no-op transparent frame optimisation. Only remove its use in the coalescing guard.

---

## What to keep

- **`shouldDropNoOpTransparentFrame()`** — the no-op transparent frame optimisation is unrelated to coalescing. It drops frames that are entirely transparent with no visual commands, which is a valid bandwidth saving after startup stabilisation. Keep this.
- **`commandHasNonNoOpRender()`** — needed by `markPacketQueued` and `shouldDropNoOpTransparentFrame`. Keep.
- **Lane admission control** — `waitForLaneAdmission()` provides backpressure at the GPU level. It is the correct mechanism for preventing unbounded queue growth. Keep, and see the companion Frame Pacing Plan for extending it.
- **Packet lifecycle telemetry** — the `Queued`/`Submitted`/`GPUCompleted`/`Presented`/`Dropped`/`Failed` lifecycle tracking remains fully functional. Only the `StaleCoalesced` drop reason is removed.

---

## Risk assessment

**Increased queue depth under sustained high-frequency invalidation.** Without coalescing, the queue will grow if widgets invalidate faster than the compositor can process frames. This is the scenario coalescing was designed for. However:

- Lane admission already caps in-flight GPU work at 2 frames per lane.
- The priority queue naturally processes the most important work first.
- The companion Frame Pacing Plan introduces a feedback mechanism for this case: rather than silently dropping frames after they've been recorded and submitted, the compositor can request that producers slow their invalidation rate *before* recording.

**Redundant rendering.** Some frames that would have been dropped will now be rendered even though a newer frame for the same target exists in the queue. This is wasted GPU work, but the cost is bounded by the lane admission budget (at most 2 frames in flight). The frame pacing system will further reduce this waste by throttling production at the source.

---

## Verification

1. Build and run BasicAppTest.
2. Resize the window by dragging a corner. Content should update smoothly without freezing or blanking.
3. Verify that `dumpLaneDiagnostics()` output no longer references `staleCoalesced`.
4. Verify that the compositor queue does not grow unboundedly during sustained resize (monitor `queuedByType` in queue telemetry snapshots).
5. Build with ASan/TSan to confirm no new races are introduced by the removal.

---

## File change summary

| File | Changes |
|------|---------|
| `wtk/src/Composition/Compositor.h` | Remove `RenderTargetEpoch` typedef, `collectRenderTargetsForCommand` decl, `targetsOverlap` decl, `dropQueuedStaleForLaneLocked` decl, `PacketDropReason::StaleCoalesced`, `staleCoalescedCount` field |
| `wtk/src/Composition/Compositor.cpp` | Remove `dropQueuedStaleForLaneLocked` call in `scheduleCommand`, remove `dropQueuedStaleForLaneLocked` impl, remove `collectRenderTargetsForCommand` impl, remove `targetsOverlap` impl, remove `StaleCoalesced` branch in `markPacketDropped`, remove `staleCoalescedCount` from diagnostics output |

---

## References

- Coalescing call site: `wtk/src/Composition/Compositor.cpp:540`
- Coalescing implementation: `wtk/src/Composition/Compositor.cpp:760–793`
- Target collection: `wtk/src/Composition/Compositor.cpp:570–600`
- Target overlap check: `wtk/src/Composition/Compositor.cpp:593–600`
- Telemetry drop reason: `wtk/src/Composition/Compositor.cpp:883`
- Lane admission (kept): `wtk/src/Composition/Compositor.cpp:636–661`
- No-op frame dropping (kept): `wtk/src/Composition/Compositor.cpp:927–961`
- Resize-triggered invalidation: `wtk/src/UI/Widget.Geometry.cpp:39–42`, `wtk/src/UI/Widget.Layout.cpp:20–23`
- View resize path: `wtk/src/UI/View.Core.cpp:135–151`
- Companion plan: `wtk/docs/Frame-Pacing-Plan.md`
