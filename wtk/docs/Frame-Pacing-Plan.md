# Frame Pacing Plan

Replace the removed stale frame coalescing mechanism with a cooperative frame pacing system. Instead of silently dropping frames after they have been recorded and enqueued, the compositor communicates backpressure to producers *before* they record, allowing them to skip or defer invalidation at the source — before any GPU work is wasted.

---

## Status: reconciled with post-Tier-4

The compositor-side **mechanism** (the `FramePacingMonitor` + `PaceHint`,
Implementation Phases 1–2 + 6 below) is architecture-agnostic and stands
as written. The producer-side **integration** (Phases 3–5) was originally
written against the pre-Tier-4 per-view paint model and has been re-homed
onto the post-Tier-4 render architecture
([UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)), where
this plan is sequenced as **Phase H (follow-up)**. The substrates the
original draft named are gone or repurposed; the substitutions used
throughout:

| Pre-Tier-4 (original draft) | Post-Tier-4 (this plan now targets) |
|---|---|
| Widget records a `VisualCommand` list into a `CanvasFrame` | View `paint(PaintContext&)` appends `DrawOp`s into the window `DisplayList` |
| Per-view `CompositorClientProxy` push | One window proxy + lane; `FrameBuilder::submitDisplayList` |
| `Widget::executePaint` *drives* the paint | `executePaint` only marks `DirtyBits` + requests a frame; `FrameBuilder::buildFrame` drives Measure→Arrange→Paint |
| Defer via a `hasPendingInvalidate` flag | Leave `DirtyBits` set, skip this frame's build; the next admitted frame drains them |
| Per-view `LayerTree` + `LayerTreeObserver` | One window `LayerTree` + one window-level observer |
| `ViewAnimator` / `LayerAnimator` submit animation frames | `AnimationScheduler` ticks once per frame; Paint reads its side table |

Net effect: the pace check moves from *per-widget, at record time* to
*per-window, at the `FrameBuilder` frame-request gate*. The mechanism's
intent is unchanged — throttle non-critical production before a frame is
built, never block a critical one. One new integration the original draft
predates: **the pacer becomes the source of `AnimationScheduler::tick`'s
`FrameTime`** (see "Frame pacing as the `FrameTime` source" below),
replacing the interim `steady_clock` stand-in that redesign Phase 4.3 left.

---

## Motivation

Stale frame coalescing operated at the wrong point in the pipeline. By the time a frame reaches the compositor queue, the producer has already done all of its CPU work (post-Tier-4 terms):

1. Run each dirty node's `paint(PaintContext&)` over the dirty subtree
2. Appended all of its `DrawOp`s into the window `DisplayList`
3. Packed the `DisplayList` into the `CompositeFrame::WidgetSlice`
4. Submitted it through the window `CompositorClientProxy` (`submitDisplayList`)

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

Implementation delegates to `frontend->getPaceHint(syncLaneId)`. Returns `PaceHint::Normal` if `frontend` is null (no compositor attached yet). Post-Tier-4 there is one `CompositorClientProxy` per window and one sync lane per window (per-view proxies and lanes were collapsed in render-redesign Tiers 1/3), so this reads the window lane's hint.

### Phase 3 — Expose the pace hint at the window / FrameBuilder

**Files:** `FrameBuilder.{h,cpp}`, `AppWindow.cpp` (+ optionally `View.Core.cpp`)

The original draft put the accessor on `View` (delegating to a per-view
`compositorProxy()`). Post-Tier-4 there is no per-view proxy — there is one
proxy per window — and the consumer is the `FrameBuilder` frame-request
gate (Phase 4), not per-widget code. So the hint is read at the window
level:

```cpp
PaceHint FrameBuilder::compositorPaceHint() const;   // → window proxy getPaceHint()
```

A thin `View::compositorPaceHint()` convenience (reaching the window's
proxy via the view's window) can still be added if widget-level code needs
to consult the hint directly, but it is no longer the primary surface — the
gating decision is centralized in the `FrameBuilder`.

### Phase 4 — Frame-request gating (was: Widget invalidation pacing)

**Files:** `FrameBuilder.{h,cpp}`, `AnimationScheduler.{h,cpp}`, `AppWindow.cpp`

After render-redesign Phase 4.7, `Widget::executePaint(PaintReason, immediate)`
no longer drives a paint — it marks the subtree's `DirtyBits` and requests a
frame. So the pace check moves to the **frame-request gate**: before
`FrameBuilder` runs a Measure → Arrange → Paint pass for a non-critical
invalidation, it consults the window pace hint and may skip the build,
leaving the `DirtyBits` set for a later frame.

```cpp
// Consulted on the frame-request path (executePaint → request frame, or the
// pacer's buildFrame entry). `immediate` paints (invalidateNow) bypass pacing.
bool FrameBuilder::shouldBuildFrame(PaintReason reason, bool immediate) const {
    if(immediate || reason == PaintReason::Initial){
        return true;                       // never gate startup / explicit-now
    }
    if(scheduler_ != nullptr && scheduler_->stats().activeProperty > 0){
        return true;                       // a live animation is pace-critical
    }
    const PaceHint hint = compositorPaceHint();
    if(hint == PaceHint::Saturated && !isPaceCritical(reason)){
        return false;                      // defer: leave DirtyBits set, skip build
    }
    if(hint == PaceHint::Throttled && isPaceDeferrable(reason)){
        return false;
    }
    return true;
}
```

When `shouldBuildFrame` returns false, the invalidation's `DirtyBits` simply
stay set on the affected nodes — no separate `hasPendingInvalidate` flag,
because `DirtyBits` already coalesce across frames (redesign §3.4). The next
admitted frame (the drain notification of Phase 5, or any later non-deferred
invalidation) runs the pass and drains the accumulated bits in one paint.

**Pace criticality classification** (`PaintReason` survives Tier 4 unchanged —
`Initial` / `StateChanged` / `Resize` / `ThemeChanged`):

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
- `PaintReason::Resize` is never deferred (the user is actively dragging; stale geometry is immediately visible). Per render-redesign **Phase F**, resize also now forces a full-tree relayout + repaint with no per-widget opt-in, so the whole resize frame is pace-critical.
- A frame driven by a live `AnimationScheduler` animation is pace-critical (see "Animation-aware pacing"), so throttling never causes animation stutter.
- `PaintReason::ThemeChanged` is not deferred under `Throttled` but is deferred under `Saturated`.
- `immediate == true` paints (from `invalidateNow()`) bypass pacing entirely — the caller explicitly requested immediate execution.

### Phase 5 — Deferred frame drain

When the compositor's load drops, deferred frames need to be picked up. Two mechanisms, both re-homed to the window level:

**5a. Next admitted frame.** Because deferral just leaves `DirtyBits` set, the next non-deferred frame request (or the next pacer tick, once vsync-aligned production lands) runs `buildFrame`, which paints every node whose bits are still set. There is no per-widget pending-invalidate queue to drain.

**5b. Compositor drain notification.** When the compositor's queue drains (`onQueueDrained()`), it notifies the single window `LayerTreeObserver` (per-view observers were deleted in render-redesign 4.8), which asks the `FrameBuilder` to run a frame if any `DirtyBits` remain set. Add an optional callback on `LayerTreeObserver`:

```cpp
INTERFACE_METHOD void queueDidDrain() { }  // default no-op
```

The Compositor's `onQueueDrained()` already exists and is called by the scheduler after the queue empties. Extend it to notify the window observer.

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

### `DirtyBits` coalescing

Post-Tier-4 there is no per-widget `coalesceInvalidates` / `hasPendingInvalidate` drain loop — invalidations between frames union into each node's `DirtyBits` (redesign §3.4) and one `buildFrame` pass paints them all. Frame pacing adds a second reason to *not* run that pass yet (compositor load); it composes naturally because deferral is just "don't build this frame, the bits persist," which is the same state coalescing already relies on.

### Resize is always pace-critical

Render-redesign **Phase F** removes the per-widget resize opt-in (`anyWidgetOptsIntoResize` / `invalidateOnResize` as a relayout gate): a window resize unconditionally relayouts and force-repaints the whole tree at the new size (stretched content otherwise persists until redraw). Frame pacing classifies `Resize` as pace-critical, so that whole-tree resize frame is never deferred — the resize path is unaffected by pacing.

### Frame pacing as the `FrameTime` source (AnimationScheduler)

Render-redesign Phase 4.3 ticks the per-window `AnimationScheduler` once per frame with a `FrameTime{ monotonicNs, frameIndex }`, currently synthesized from a `steady_clock` plus a `FrameBuilder` frame counter — an interim stand-in. The pacer is the natural owner of that timestamp: a (eventually vsync-aligned) monotonic clock plus the frame index it already tracks. When this plan lands, `FrameBuilder` feeds the pacer's `FrameTime` into `scheduler.tick(...)`, replacing the stand-in, so animation timing and frame pacing run off one clock. This is the concrete seam flagged in render-redesign Phase H.

---

## Pace hint lifecycle during resize

This is the critical scenario — the one that stale frame coalescing broke. Walk through the expected behaviour:

1. User drags window corner. Platform fires resize events.
2. The window relayouts the whole tree (render-redesign Phase F): each container's `LayoutManager` re-arranges its children to the new rect.
3. The resize marks the whole tree for repaint and requests a frame with `PaintReason::Resize`.
4. `FrameBuilder::shouldBuildFrame` checks `isPaceCritical(Resize)` → `true`. Pacing is bypassed.
5. `buildFrame` runs Measure → Arrange → Paint; each node appends `DrawOp`s into the window `DisplayList` at the new dimensions, and `submitDisplayList` packs it into the `CompositeFrame`.
6. The frame enters the compositor queue. No coalescing occurs (removed — and the per-view `CanvasFrame` path it operated on is gone post-Tier-4).
7. The scheduler processes the frame. If the lane is at capacity, `waitForLaneAdmission` blocks until the prior GPU frame completes (bounded by `kMaxFramesInFlightNormal = 2`).
8. The frame is rendered at the correct size and presented.

Meanwhile, any background `StateChanged` invalidations from secondary nodes are deferred if the lane is saturated. Their `DirtyBits` stay set and drain automatically when the queue clears, producing a single coalesced repaint at the final dimensions.

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

Post-Tier-4 animation is the per-window `AnimationScheduler`
(render-redesign 4.3–4.4), not `ViewAnimator` / `LayerAnimator`, and
there is no per-animation packet to flag. Instead the `FrameBuilder`
frame-request gate (Phase 4) treats a frame as pace-critical whenever the
scheduler reports live animations (`scheduler.stats().activeProperty > 0`
/ `hasAnyAnimationFor`), so throttling never causes animation stutter —
this is already folded into `shouldBuildFrame`. The scheduler knows its
target timing and self-regulates; frame pacing should not interfere with
it. NVIDIA Reflex's approach of using the animation's own timing to
predict GPU load would be a natural further refinement.

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
| `wtk/src/UI/FrameBuilder.{h,cpp}` | 3, 4 | `compositorPaceHint()` (reads the window proxy), `shouldBuildFrame()` + `isPaceCritical` / `isPaceDeferrable`, frame-request gating |
| `wtk/src/UI/AppWindow.cpp` | 3, 4 | Wire the frame-request path through the FrameBuilder gate |
| `wtk/src/UI/AnimationScheduler.{h,cpp}` | 4 | Animation-aware override (`stats().activeProperty`) + consume the pacer's `FrameTime` |
| `wtk/include/omegaWTK/Composition/Layer.h` | 5 | `queueDidDrain()` on the window `LayerTreeObserver` |
| `wtk/src/Composition/Compositor.cpp` | 5 | Notify the window observer in `onQueueDrained()` |

---

## Relationship to coalescing removal

The stale frame-coalescing mechanism this plan replaces operated on the per-view `CanvasFrame` submission path — which Tier 4 deleted outright. So coalescing is moot in the post-Tier-4 world, and its removal plan is archived at `wtk/docs/stale/Stale-Frame-Coalescing-Removal-Plan.md`. The remaining need stands on its own: with no coalescing anywhere, frame pacing keeps the compositor queue from growing unboundedly under sustained high-frequency invalidation (e.g. rapid-fire state changes during a long animation while a resize is in flight) by throttling non-critical production at the source.

---

## References

### Codebase (file-level; any line numbers predate Tier 4)

- Compositor lane runtime state + lane admission control: `wtk/src/Composition/Compositor.{h,cpp}`
- Packet presentation / `onQueueDrained` in the scheduler loop: `wtk/src/Composition/Compositor.cpp`
- Frame driver (the new frame-request gate): `wtk/src/UI/FrameBuilder.{h,cpp}`
- Animation scheduler (`FrameTime` consumer + animation-aware override): `wtk/src/UI/AnimationScheduler.{h,cpp}`
- Widget invalidation entry (post-4.7: marks `DirtyBits` + requests a frame): `wtk/src/UI/Widget.Paint.cpp` (`executePaint`)
- Paint reason enum + paint options: `wtk/include/omegaWTK/UI/Widget.h`
- Window `LayerTree` observer / drain callback: `wtk/include/omegaWTK/Composition/Layer.h`
- Sequencing: `wtk/docs/UIView-Render-Redesign-Plan.md` Phase H
- Companion plan (archived): `wtk/docs/stale/Stale-Frame-Coalescing-Removal-Plan.md`

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
