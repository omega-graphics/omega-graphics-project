# Frame Pacing Plan

Replace the removed stale frame coalescing mechanism with a cooperative frame pacing system. Instead of silently dropping frames after they have been recorded and enqueued, the compositor communicates backpressure to producers *before* they record, allowing them to skip or defer invalidation at the source — before any GPU work is wasted.

---

## Motivation

Stale frame coalescing operated at the wrong point in the pipeline. By the time a frame reaches `scheduleCommand()`, the widget has already:

1. Executed its `onPaint()` callback
2. Issued all Canvas draw calls, building a `VisualCommand` list
3. Snapshot the `CanvasFrame` (including layer rect)
4. Pushed the frame through `CompositorClient` → `CompositorClientProxy`

Dropping the frame at enqueue time wastes all of that CPU work. Worse, during resize it drops the only frame that carries the correct post-resize geometry, causing a visible content gap.

Frame pacing moves the throttling decision upstream: the compositor exposes its load state, and widgets consult it before beginning a paint cycle. If the compositor is saturated, the widget defers its invalidation rather than recording a frame that will be redundant by the time it executes.

---

## Design

### Core concept: pace hints

The compositor maintains a per-lane **pace hint** — a lightweight signal that tells producers how quickly they should be submitting frames. The hint is derived from the lane's runtime state (in-flight count, queue depth, recent drop/fail rates) and changes dynamically as load fluctuates.

```cpp
enum class PaceHint : std::uint8_t {
    /// Lane is idle or well within budget. Produce frames freely.
    Normal,
    /// Lane is near capacity. Defer non-essential invalidations
    /// (e.g. hover effects, secondary animations). Resize and
    /// user-initiated paints should still proceed.
    Throttled,
    /// Lane is at capacity. Only produce frames for critical
    /// operations (initial paint, resize, explicit user action).
    /// Background repaints and animations should be skipped entirely.
    Saturated
};
```

The pace hint is advisory. Producers are never blocked — they can always submit if they judge the frame to be critical. The hint gives them the information to make that call themselves.

### Why not per-frame measurement

The original version of this plan computed the pace hint from instantaneous
per-frame state: `if inFlight >= kMaxFramesInFlightNormal → Saturated`. This
is the same pattern that made stale frame coalescing destructive — a single
snapshot of queue depth at the moment a widget is about to paint can trigger
throttling that causes visible content gaps, particularly during resize
where several frames arrive in rapid succession but each carries unique
geometry.

Research into production frame pacing systems confirms that every serious
implementation uses a **two-layer architecture**:

1. **Inner loop (per-frame hard gate):** A non-negotiable safety limit that
   prevents GPU queue overflow. This already exists in OmegaWTK as lane
   admission control (`waitForLaneAdmission`). It stays.

2. **Outer loop (time-windowed adaptive measurement):** A throughput
   measurement over a sliding window that determines the *pacing strategy*.
   This is what the pace hint should be derived from.

Per-frame snapshots are biased: fast-rendering sections produce more
samples per unit time than slow sections, so a moving average of raw
per-frame samples over-represents the good times and under-represents the
bad. Quantisation into fixed-duration time windows eliminates this bias.

### Pace hint computation: time-windowed frame time measurement

The compositor maintains a per-lane **FramePacingMonitor** — a lightweight
structure that records frame presentation times in quantised windows and
derives the pace hint from sustained throughput, not instantaneous state.

**Frame time, not FPS.** All measurement is in milliseconds. FPS is a
derived metric that distorts perception: the difference between 60 and
30 FPS is 16.67ms of frame time, while the difference between 30 and
15 FPS is also 16.67ms — equal cost, wildly different FPS delta.
Frame time is linear and directly comparable.

**100ms quantisation windows.** Each window accumulates frame times for
all packets presented during that 100ms interval. 100ms matches the
threshold of human visual perception for smoothness — Chromium's
1-second windows are too coarse for real-time adaptation (they use
them for retrospective quality metrics, not live pacing). At the end of
each window, the monitor computes the window's average frame time and
updates its rolling state.

```cpp
struct FramePacingMonitor {
    static constexpr auto kWindowDuration = std::chrono::milliseconds(100);
    static constexpr unsigned kWindowHistorySize = 20;  // 2 seconds of history
    static constexpr unsigned kThrottleWindowThreshold = 2;  // 2 bad windows → Throttled
    static constexpr unsigned kSaturateWindowThreshold = 4;  // 4 bad windows → Saturated
    static constexpr unsigned kRecoveryWindowThreshold = 10; // 10 good windows → recover

    /// Target frame time for this lane (e.g. 16.67ms for 60 Hz).
    double targetFrameTimeMs = 16.667;

    /// Rolling window state.
    struct Window {
        double totalFrameTimeMs = 0.0;
        unsigned frameCount = 0;
        double averageFrameTimeMs() const {
            return frameCount > 0 ? totalFrameTimeMs / frameCount : 0.0;
        }
        bool overBudget(double target) const {
            return averageFrameTimeMs() > target;
        }
    };

    std::chrono::steady_clock::time_point windowStart {};
    Window currentWindow {};
    std::array<Window, kWindowHistorySize> history {};
    unsigned historyHead = 0;
    unsigned historyCount = 0;

    /// Consecutive over-budget windows (for throttle-up).
    unsigned consecutiveOverBudget = 0;
    /// Consecutive under-budget windows (for recovery).
    unsigned consecutiveUnderBudget = 0;

    /// Current computed hint (cached between window boundaries).
    PaceHint currentHint = PaceHint::Normal;
    bool startupStabilized = false;
};
```

**Recording a frame.** When a packet reaches the `Presented` lifecycle
phase, the compositor calls `monitor.recordFrameTime(frameTimeMs)` with
the measured frame time (present time minus submit time, in
milliseconds). If the current 100ms window has elapsed, the monitor
closes it, pushes it to history, and evaluates the hint.

**Asymmetric hysteresis.** The universal pattern across Chromium, Unreal,
and Unity is asymmetric response: throttle up quickly, recover slowly.

- **Throttle up (Normal → Throttled → Saturated):** Fast. If
  `kThrottleWindowThreshold` (2) consecutive windows are over budget,
  escalate to `Throttled`. If `kSaturateWindowThreshold` (4) consecutive
  windows are over budget, escalate to `Saturated`. At 100ms windows,
  this means throttling engages within 200–400ms of sustained overload.

- **Recovery (Saturated → Throttled → Normal):** Slow. Require
  `kRecoveryWindowThreshold` (10) consecutive under-budget windows
  before de-escalating one level. At 100ms windows, this is 1 second
  of sustained good performance. This prevents oscillation — the
  system does not immediately relax when one good window appears
  between bad ones.

- **History reset on discontinuity.** Following Unreal's dynamic
  resolution "panic reset" pattern: when a sudden load change is
  detected (e.g. frame time jumps by more than 3× the target in a
  single window), the monitor resets `consecutiveUnderBudget` to zero.
  This prevents stale "good" history from fighting adaptation to a
  genuinely new workload.

```
on window close:
    push currentWindow to history
    if currentWindow.overBudget(targetFrameTimeMs):
        consecutiveOverBudget += 1
        consecutiveUnderBudget = 0
        if consecutiveOverBudget >= kSaturateWindowThreshold:
            currentHint = Saturated
        else if consecutiveOverBudget >= kThrottleWindowThreshold:
            currentHint = max(currentHint, Throttled)
    else:
        consecutiveOverBudget = 0
        consecutiveUnderBudget += 1
        // Discontinuity detection: sudden spike resets recovery counter
        if previousWindow.averageFrameTimeMs > 3 * targetFrameTimeMs:
            consecutiveUnderBudget = 0
        if consecutiveUnderBudget >= kRecoveryWindowThreshold:
            currentHint = de-escalate one level
            consecutiveUnderBudget = 0  // require sustained proof again
```

During startup (before `startupStabilized`), the hint is always
`Normal` — initial paints must never be suppressed.

---

## Implementation

### Phase 1 — FramePacingMonitor and compositor pace hint query

**Files:** `Compositor.h`, `Compositor.cpp`

**1a.** Add the `FramePacingMonitor` struct to `Compositor.h` (private to
the `OmegaWTK::Composition` namespace). The full struct is specified in
the "Pace hint computation" section above.

**1b.** Add a per-lane pacing monitor map to `Compositor`:

```cpp
OmegaCommon::Map<std::uint64_t, FramePacingMonitor> lanePacingMonitors;
```

This lives alongside the existing `packetTelemetryState` and is
protected by the same mutex.

**1c.** Hook frame time recording into the packet lifecycle. When a
packet reaches the `Presented` phase (in the scheduler loop, lines
~424–450 of `Compositor.cpp`, where `entry.phase` is set to
`Presented`), compute the frame time and feed it to the monitor:

```cpp
if(entry.phase == PacketLifecyclePhase::Presented){
    const double frameTimeMs =
        std::chrono::duration<double, std::milli>(
            entry.presentTimeCpu - entry.submitTimeCpu).count();
    auto & monitor = lanePacingMonitors[command->syncLaneId];
    if(!monitor.startupStabilized && laneState.startupStabilized){
        monitor.startupStabilized = true;
    }
    monitor.recordFrameTime(frameTimeMs);
}
```

**1d.** Add the public query method:

```cpp
PaceHint getPaceHint(std::uint64_t syncLaneId) const;
```

Implementation reads the cached `currentHint` from the lane's
`FramePacingMonitor`. No computation happens here — the hint was already
updated when the last window closed in `recordFrameTime()`. This makes
the query a single map lookup and field read under the mutex.

```cpp
PaceHint Compositor::getPaceHint(std::uint64_t syncLaneId) const {
    if(syncLaneId == 0 || packetTelemetryState == nullptr){
        return PaceHint::Normal;
    }
    std::lock_guard<std::mutex> lk(packetTelemetryState->mutex);
    auto it = lanePacingMonitors.find(syncLaneId);
    if(it == lanePacingMonitors.end()){
        return PaceHint::Normal;
    }
    const auto & monitor = it->second;
    if(!monitor.startupStabilized){
        return PaceHint::Normal;
    }
    return monitor.currentHint;
}
```

### Phase 2 — Expose pace hint through CompositorClientProxy

**Files:** `CompositorClient.h`, `CompositorClient.cpp`

Add to `CompositorClientProxy`:

```cpp
PaceHint getPaceHint() const;
```

Implementation delegates to `frontend->getPaceHint(syncLaneId)`. Returns `PaceHint::Normal` if `frontend` is null (no compositor attached yet).

### Phase 3 — Expose pace hint through View

**Files:** `View.h`, `View.cpp`

Add to `View`:

```cpp
PaceHint compositorPaceHint() const;
```

Delegates to `compositorProxy().getPaceHint()`. This is the surface that Widget code calls.

### Phase 4 — Widget invalidation pacing

**Files:** `Widget.h`, `Widget.Paint.cpp`, `WidgetImpl.h`

Modify `Widget::executePaint()` to consult the pace hint before beginning a paint cycle. The decision depends on both the hint and the paint reason:

```cpp
void Widget::executePaint(PaintReason reason, bool immediate) {
    // ... existing mode/reentrancy checks ...

    // Frame pacing: check compositor load before recording.
    if(!immediate && reason != PaintReason::Initial) {
        auto hint = view->compositorPaceHint();
        if(hint == PaceHint::Saturated && !isPaceCritical(reason)) {
            // Compositor is at capacity. Defer this invalidation.
            impl_->hasPendingInvalidate = true;
            impl_->pendingPaintReason = reason;
            return;
        }
        if(hint == PaceHint::Throttled && isPaceDeferrable(reason)) {
            impl_->hasPendingInvalidate = true;
            impl_->pendingPaintReason = reason;
            return;
        }
    }

    // ... proceed with paint as before ...
}
```

**Pace criticality classification:**

```cpp
static bool isPaceCritical(PaintReason reason) {
    switch(reason) {
        case PaintReason::Initial:
        case PaintReason::Resize:
            return true;   // Always paint — geometry is changing.
        default:
            return false;
    }
}

static bool isPaceDeferrable(PaintReason reason) {
    switch(reason) {
        case PaintReason::StateChanged:
            return true;   // Can wait for the next normal cycle.
        case PaintReason::Resize:
        case PaintReason::Initial:
        case PaintReason::ThemeChanged:
            return false;  // User-visible structural changes.
        default:
            return false;
    }
}
```

Key invariants:
- `PaintReason::Initial` is never deferred (startup must be deterministic).
- `PaintReason::Resize` is never deferred (the user is actively dragging; stale geometry is immediately visible).
- `PaintReason::ThemeChanged` is not deferred under `Throttled` but is deferred under `Saturated`.
- `immediate == true` paints (from `invalidateNow()`) bypass pacing entirely — the caller explicitly requested immediate execution.

### Phase 5 — Deferred invalidation drain

When the compositor's load drops, deferred invalidations need to be picked up. There are two mechanisms:

**5a. Next `executePaint` call.** The existing `coalesceInvalidates` / `hasPendingInvalidate` loop in `executePaint` already handles this: the next non-deferred invalidation will drain the pending flag in the same paint cycle.

**5b. Compositor drain notification.** When the compositor's queue drains (`onQueueDrained()`), it can notify observed LayerTrees, which propagate to their Widgets. Add an optional callback on `LayerTreeObserver`:

```cpp
INTERFACE_METHOD void queueDidDrain() { }  // default no-op
```

The Compositor's `onQueueDrained()` already exists and is called by the scheduler after the queue empties. Extend it to notify observers. Widgets that have `hasPendingInvalidate == true` can call `invalidate(pendingPaintReason)` in response.

This ensures deferred frames are eventually produced even if no new user interaction triggers an invalidation.

### Phase 6 — Telemetry integration

**Files:** `Compositor.h`, `Compositor.cpp`

Add pacing-related counters to `LaneRuntimeState`:

```cpp
std::uint64_t paceHintThrottledCount = 0;   // Windows spent in Throttled
std::uint64_t paceHintSaturatedCount = 0;   // Windows spent in Saturated
std::uint64_t paceDiscontinuityResets = 0;   // History resets from load spikes
```

Increment in `FramePacingMonitor::recordFrameTime()` when windows close
and hint transitions occur. Include in `dumpLaneDiagnostics()` output.

Additionally, expose a `FramePacingSnapshot` through the diagnostics API:

```cpp
struct FramePacingSnapshot {
    PaceHint currentHint = PaceHint::Normal;
    double currentWindowAvgMs = 0.0;
    double recentAvgFrameTimeMs = 0.0;  // Average over last kWindowHistorySize windows
    unsigned consecutiveOverBudget = 0;
    unsigned consecutiveUnderBudget = 0;
    double targetFrameTimeMs = 16.667;
};
```

This lets developers inspect the monitor's state to verify that pacing
is activating when expected, that the target frame time is correct for
the display's refresh rate, and that recovery is progressing after a
load spike.

---

## Interaction with existing mechanisms

### Lane admission control (the inner loop)

Lane admission (`waitForLaneAdmission`) and frame pacing form the
**two-layer architecture** used by every production compositor:

- **Inner loop — lane admission.** A per-frame hard gate that blocks
  the scheduler thread when in-flight GPU work reaches the budget.
  This is the safety mechanism. It cannot be removed — without it,
  the GPU command queue can overflow. It operates after a command has
  already been enqueued and is about to execute. Every system has an
  equivalent: Chromium's `IsDrawThrottled()` (caps `pending_submit_frames_`
  at `kMaxPendingSubmitFrames`), Unreal's `FRenderCommandFence::Wait()`,
  Android Swappy's sync fence injection.

- **Outer loop — frame pacing.** A time-windowed adaptive signal that
  tells producers to slow down *before* they create commands. This is
  the efficiency mechanism. It prevents the queue from growing and
  avoids wasting CPU work on frames the inner loop would just block on.

Both are needed. Lane admission guarantees GPU safety. Frame pacing
reduces wasted CPU work and provides smooth, jitter-free throttling
based on sustained throughput rather than instantaneous state.

### No-op transparent frame dropping

`shouldDropNoOpTransparentFrame()` remains unchanged. It operates at execution time on frames that are already in the queue and contains no visual content. Frame pacing operates at production time on frames that would contain content. They are independent optimisations.

### Widget `coalesceInvalidates`

The existing `PaintOptions::coalesceInvalidates` flag causes `executePaint` to defer reentrant invalidations and drain them at the end of the current paint cycle. Frame pacing adds a second deferral path based on compositor load. Both set `hasPendingInvalidate` and reuse the same drain loop. They compose naturally.

### `PaintOptions::invalidateOnResize`

When `invalidateOnResize` is true (the default), a resize triggers `invalidate(PaintReason::Resize)`. Frame pacing classifies `Resize` as pace-critical, so it is never deferred. The resize path is unaffected by pacing.

---

## Pace hint lifecycle during resize

This is the critical scenario — the one that stale frame coalescing broke. Walk through the expected behaviour:

1. User drags window corner. Platform fires resize events.
2. `View::resize()` updates the native layer and root layer geometry.
3. `Widget::invalidate(PaintReason::Resize)` is called for affected widgets.
4. `executePaint` checks `isPaceCritical(Resize)` → `true`. Pacing is bypassed.
5. Widget records a `CanvasFrame` at the new dimensions and submits.
6. The frame enters the compositor queue. No coalescing occurs (removed).
7. The scheduler processes the frame. If the lane is at capacity, `waitForLaneAdmission` blocks until the prior GPU frame completes (bounded by `kMaxFramesInFlightNormal = 2`).
8. The frame is rendered at the correct size and presented.

Meanwhile, any background `StateChanged` invalidations from secondary widgets are deferred if the lane is saturated. They drain automatically when the queue clears, producing a single coalesced repaint at the final dimensions.

Result: resize frames are never dropped, background repaints are naturally batched, and the queue never grows unboundedly.

---

## Prior art and design rationale

The two-layer architecture and time-windowed measurement are drawn from
production systems. The research is documented here so future work can
reference the same sources.

### Chromium cc::Scheduler

Chromium's compositor uses `IsDrawThrottled()` — a per-frame hard gate
that checks `pending_submit_frames_ >= kMaxPendingSubmitFrames` — for
the inner loop. For quality measurement, it uses a
`DroppedFrameCounter` with a `SlidingWindowHistogram` (101 bins) over
**1-second sliding windows**, reporting average, worst-case, and 95th
percentile dropped frame rates. The 1-second window is for
retrospective metrics, not real-time adaptation. Chromium also uses
BeginFrame-aligned production (vsync-driven) rather than free-running
submission, and can enter a "high latency mode" that trades latency for
throughput based on historical main thread response times.

### Unreal Engine

Unreal's `r.GTSyncType` mode 2 uses frame flip statistics from the
display driver to **predict** the next vsync time, then aligns CPU
frame start to arrive just in time. The prediction is trained on frame
flip history, not instantaneous state. Unreal's dynamic resolution
system uses a "panic" mode: if N consecutive frames with available GPU
timings are over budget, it immediately drops resolution *and resets
its timing history*, preventing stale "good" data from fighting
adaptation. The history reset pattern directly informed the
discontinuity detection in this plan.

### Unity DynamicResolution

Unity's sample implementation uses asymmetric counter-based hysteresis
with specific constants: scale-down is immediate, scale-up requires a
counter to reach 360 (approximately 6 seconds at 60 FPS) of sustained
good performance. The counter increments by 10 if GPU frame time is
decreasing, or by 3 if headroom exceeds 6% and delta is under 3.5% of
the frame budget. Any scale-down resets the counter to zero. This
strong asymmetry prevents oscillation and was the model for the
`kRecoveryWindowThreshold` in this plan.

### NVIDIA Reflex

Reflex maintains a **64-frame history window** and operates as a
"dynamic FPS limiter": it predicts how long the next frame will take
based on the previous N frames, then delays CPU submission to arrive
just before the GPU would otherwise idle. The documented weakness is
that rapid framerate changes cause the prediction to lag, producing
brief jitter until the history re-trains. This weakness validates the
discontinuity reset: when load changes suddenly, stale history should
be discarded rather than averaged through.

### Frame time vs FPS

Raw per-frame samples produce **biased averages**: fast-rendering
sections contribute more samples per unit time than slow sections, so a
moving average over-represents the good times. Quantisation into
fixed-duration windows (this plan uses 100ms) eliminates the bias.
100ms was chosen over 1-second because it matches human visual
perception thresholds for smoothness detection while still being long
enough to accumulate meaningful sample counts.

---

## Future extensions

### Adaptive lane admission budget

Currently `kMaxFramesInFlightNormal` is a compile-time constant (2).
The `FramePacingMonitor`'s history provides the data needed to make
this adaptive:

- If `recentAvgFrameTimeMs < targetFrameTimeMs * 0.5` (GPU is
  consistently fast) → increase budget to 3, allowing more pipelining.
- If `recentAvgFrameTimeMs > targetFrameTimeMs * 1.5` (GPU is
  consistently slow) → decrease budget to 1, reducing latency.

This would let the inner loop (lane admission) adapt alongside the
outer loop (frame pacing), absorbing burst submissions on fast GPUs
while self-protecting on slower hardware.

### Vsync-aligned frame production

The current design is free-running: widgets invalidate whenever state
changes, and the compositor processes frames as they arrive. A future
extension could align frame production to the display's vsync signal
(following Chromium's BeginFrame model), where the compositor issues a
"begin frame" signal at each vsync interval and widgets only paint in
response. This would eliminate over-production entirely but requires
deeper integration with the platform display link.

### Animation-aware pacing

Animation frames submitted by `ViewAnimator` or `LayerAnimator` could
carry a `PaceHint::Override` flag that bypasses throttling. This
prevents frame pacing from causing animation stutter. The animation
system already knows its target frame rate and can self-regulate —
frame pacing should not interfere with it. NVIDIA Reflex's approach of
using the animation's own timing to predict GPU load would be a natural
fit here.

### Per-widget pace sensitivity

Some widgets (e.g. a video surface) are inherently latency-sensitive
and should never be deferred. A `PaintOptions::paceSensitivity` field
could allow widgets to declare their tolerance:

```cpp
enum class PaceSensitivity : uint8_t {
    Normal,      // Subject to pacing (default)
    Responsive,  // Only defer under Saturated
    Critical     // Never defer
};
```

This is deferred because the current `PaintReason`-based classification
handles the common cases. Per-widget sensitivity would be needed if
custom widget types have pacing requirements that don't map cleanly to
paint reasons.

### Display refresh rate detection

The `FramePacingMonitor::targetFrameTimeMs` defaults to 16.667ms
(60 Hz). On high-refresh-rate displays (120 Hz, 144 Hz) this target is
wrong, causing the monitor to see every frame as under-budget and never
throttle. A future phase should query the platform display link for the
actual refresh interval (via `CVDisplayLink` on macOS, `DXGI_OUTPUT`
on Windows, `wl_output` on Linux) and set the target accordingly.

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/Composition/Compositor.h` | 1, 6 | `PaceHint` enum, `FramePacingMonitor` struct, `lanePacingMonitors` map, `getPaceHint()` decl, `FramePacingSnapshot`, pacing counters in `LaneRuntimeState` |
| `wtk/src/Composition/Compositor.cpp` | 1, 6 | `FramePacingMonitor::recordFrameTime()` impl, frame time recording in scheduler loop, `getPaceHint()` impl, counter increments, diagnostics output |
| `wtk/include/omegaWTK/Composition/CompositorClient.h` | 2 | `getPaceHint()` on `CompositorClientProxy` |
| `wtk/src/Composition/CompositorClient.cpp` | 2 | `getPaceHint()` impl |
| `wtk/include/omegaWTK/UI/View.h` | 3 | `compositorPaceHint()` decl |
| `wtk/src/UI/View.Core.cpp` | 3 | `compositorPaceHint()` impl |
| `wtk/include/omegaWTK/UI/Widget.h` | 4 | Forward-declare `PaceHint` |
| `wtk/src/UI/Widget.Paint.cpp` | 4 | Pacing check in `executePaint`, `isPaceCritical`, `isPaceDeferrable` |
| `wtk/include/omegaWTK/Composition/Layer.h` | 5 | `queueDidDrain()` on `LayerTreeObserver` |
| `wtk/src/Composition/Compositor.cpp` | 5 | Notify observers in `onQueueDrained()` |

---

## Dependency on coalescing removal

This plan assumes the stale frame coalescing mechanism has been removed per `Stale-Frame-Coalescing-Removal-Plan.md`. The two plans are complementary:

- **Coalescing removal** stops the compositor from dropping frames after they've been submitted, eliminating the resize content gap.
- **Frame pacing** prevents the queue from growing unboundedly in the absence of coalescing, by throttling non-critical production at the source.

The coalescing removal can be implemented independently and will immediately fix the resize gap. Frame pacing should follow to prevent queue growth under sustained high-frequency invalidation (e.g. rapid-fire state changes during a long animation while the compositor is processing a resize).

---

## References

### Codebase

- Compositor lane runtime state: `wtk/src/Composition/Compositor.h:154–170`
- Lane admission control: `wtk/src/Composition/Compositor.cpp:636–661`
- Packet presentation in scheduler loop: `wtk/src/Composition/Compositor.cpp:424–450`
- Widget paint cycle: `wtk/src/UI/Widget.Paint.cpp:8–57`
- Paint reason enum: `wtk/include/omegaWTK/UI/Widget.h`
- Paint options: `wtk/include/omegaWTK/UI/Widget.h:55–65`
- Queue drain callback: `wtk/src/Composition/Compositor.cpp` (`onQueueDrained`)
- View composition session: `wtk/include/omegaWTK/UI/View.h:168–177`
- Companion plan: `wtk/docs/Stale-Frame-Coalescing-Removal-Plan.md`

### External

- Chromium cc::Scheduler and BeginFrame: chromium.googlesource.com — "How cc Works", "Life of a Frame"
- Chromium DroppedFrameCounter / SlidingWindowHistogram: `cc/metrics/dropped_frame_counter.h`
- Chromium smoothness metric: web.dev/articles/smoothness
- Unreal Engine low-latency frame syncing: docs.unrealengine.com — "Low Latency Frame Syncing"
- Unreal Engine dynamic resolution panic/history reset: dev.epicgames.com — "Dynamic Resolution"
- Unity FrameTimingManager: docs.unity3d.com — "Frame Timing Manager"
- Unity DynamicResolution sample (counter-based hysteresis): github.com/Unity-Technologies/DynamicResolutionSample
- NVIDIA Reflex 64-frame history prediction: developer.nvidia.com — "Reflex Low Latency Platform"
- Android Swappy frame pacing library: source.android.com — "Frame Pacing"
- Frame time quantisation bias analysis: "The Highs & Lows of Frame Counting" (medium.com/@opinali)
