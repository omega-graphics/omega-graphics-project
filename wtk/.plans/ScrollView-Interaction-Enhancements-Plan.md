# ScrollView Interaction Enhancements Plan

Two desktop-scroll niceties deferred as open questions in the
[ScrollableContainer plan §9.2 / §9.3](ScrollableContainer-Implementation-Plan.md#9-open-questions)
and flagged there as "ScrollView-side enhancements": **scroll-bar
hit-testing + thumb drag**, and **momentum (fling) scrolling**. Both build
on the now-complete
[ScrollView-4.7 integration](done/ScrollView-4.7-Integration-Plan.md)
(clip, wheel, bars, keyboard, event bubbling).

## 1. Existing infrastructure

| Piece | Where | Gives us |
|-------|-------|----------|
| Scroll-bar thumb rendering | `ScrollView::paintAfterChildren` | Thumb geometry (offset/content/viewport → thumb rect), currently computed inline for *drawing* only. |
| Event bubbling for clicks | `WidgetTreeHost::dispatchInputEvent` (V2.2) | `LMouseDown/Up` bubble from the deepest hit up to the ScrollView, so the ScrollView's handler can see a press on its bar. |
| `View::scheduleRepaint()` | `View.Core.cpp` (V2.1) | Offset change → one coalesced frame. Drag + momentum reuse it. |
| `AnimationScheduler::tween<T>(from,to,apply,curve)` | `AnimationScheduler.h` | Callback animation firing `apply(value)` each tick — the momentum decay vehicle. Lives on `AppWindow`; ticked once/frame by FrameBuilder. |
| `View::offsetFromRoot()` | `View.h` | The ScrollView's absolute window offset — needed to place the thumb rect in the same window-coord space as mouse-event positions. |

What is missing:

- **Thumb hit-testing.** The thumb is a draw-only `RoundedRect` `DrawOp`, not a `View`, so `hitTestWidget` never returns it. The ScrollView must test the mouse point against the thumb rect itself.
- **Mouse capture.** A thumb drag must keep receiving `CursorMove` / `LMouseUp` after the cursor leaves the thumb (or the viewport). `WidgetTreeHost` has no capture concept today — `CursorMove` is delivered to the deepest hit view, and V2.2 deliberately kept it deepest-only (hover). A drag needs events routed to the ScrollView regardless of hit-test until release.
- **Scheduler reach from a `View`.** `AnimationScheduler` is owned by `AppWindow` (`impl_->animationScheduler_`); FrameBuilder has an accessor but a `ScrollView` has no path to it. Momentum needs one (via `treeHost_` → owner window).

## 2. Key decisions (DECIDED 2026-06-26)

1. **Momentum scope = drag flings AND wheel.** `ScrollParams` carries no
   scroll *phase* today — just deltas. On macOS the trackpad's OS-level
   momentum already arrives as decaying `ScrollWheel` deltas the ScrollView
   applies directly, so naive app-side wheel momentum would double it. So
   wheel momentum requires plumbing a **scroll-phase** enum onto
   `ScrollParams` (`None` / `Began` / `Changed` / `Ended` /
   `MomentumBegan` / `Momentum` / `MomentumEnded`) so the ScrollView can
   tell an OS-momentum tick from a user tick:
   - **Trackpad (phase present):** let the OS momentum stream flow as-is —
     do NOT synthesize app momentum (no doubling).
   - **Discrete mouse wheel (`phase == None`):** synthesize an app-side
     fling on the scroll-end using the accumulated wheel velocity.
   - **Drag flings:** always app momentum (no OS involvement).
   macOS populates the phase from `NSEvent.phase` / `momentumPhase`;
   Win32 / GTK send `None` for now (mouse-wheel path → app momentum), with
   their trackpad-gesture phase a later native follow-up. This is phase
   **E5** below.
2. **Track click = jump thumb to the click position** (macOS overlay-bar
   behavior): a press on the bar track outside the thumb moves the offset so
   the thumb centers under the pointer, then (optionally) continues as a
   drag. Implemented in **E3**.
3. **Mouse capture is a general `WidgetTreeHost` primitive**, not a
   ScrollView-private hack — a future Slider / splitter / drag-drop needs
   the same. `captureMouse(View*)` / `releaseMouse()`.

## 3. Phases

| Phase | Description | Requires | Blocks |
|-------|-------------|----------|--------|
| **E1** | **Thumb geometry + hit-test.** Factor the thumb-rect math out of `paintAfterChildren` into a shared `thumbWindowRect(Axis)` helper (returns the thumb rect in absolute window coords, or empty when that axis has no range). `paintAfterChildren` calls it; a new `hitTestThumb(windowPoint)` returns which axis's thumb (if any) the point lands on. | None | E3 |
| **E2** | **Mouse capture in `WidgetTreeHost`.** Add `View * capturedView_` + `captureMouse(View*)` / `releaseMouse()`. In `dispatchInputEvent`, when a view is captured, route `LMouseDown/Up` + `CursorMove` straight to it (skip hit-test / hover synthesis), until released. General primitive. | None | E3 |
| **E3** | **Scroll-bar drag + track-jump.** In `DefaultScrollHandler`: `LMouseDown` whose window point hits a thumb → begin drag (store grab delta), `owner->captureMouse`, consume. A press inside the bar but off the thumb (track) first jumps the offset so the thumb centers under the pointer (decision #2), then begins a drag from there. `CursorMove` while dragging → map the new thumb position back to a scroll offset (inverse of E1's ratio math), clamp, `setScrollOffset`. `LMouseUp` → end drag, `releaseMouse`. | E1, E2 | E4 |
| **E4** | **Momentum (drag-fling).** Track thumb-drag velocity (offset delta per unit time across the last few `CursorMove`s). On `LMouseUp`, if speed > threshold, project a landing offset (velocity × factor), clamp to `[0,max]`, and `tween(current, landing, apply=setScrollOffset, decelerate curve, duration ∝ speed)`. Any new user input (wheel / drag / key) cancels the in-flight tween. Needs a `ScrollView`-reachable `AnimationScheduler` accessor (via `treeHost_`). | E3, scheduler accessor | E5 |
| **E5** | **Wheel momentum + native scroll-phase.** Add `ScrollParams::phase` (enum above); macOS populates it from `NSEvent.phase`/`momentumPhase`, Win32/GTK send `None`. ScrollView: on `phase == None` (discrete wheel) accumulate wheel velocity and fling on the trailing edge (a short idle or an explicit end); on any real phase, defer to the OS stream (no app momentum). Reuses E4's tween + cancel. | E4 | — |

E1–E3 deliver a draggable bar (the headline ask). E4 adds fling momentum on
drag release; E5 extends momentum to the mouse wheel with the native
scroll-phase guard against double-momentum on trackpads.

## 4. Design detail

### E1 — thumb geometry + hit-test
The vertical thumb (mirror for horizontal), in the ScrollView's *local*
space, is already:
```
trackH   = viewport.h - 2*margin
thumbH   = max(minThumb, trackH * viewport.h/content.h)
thumbY   = clamp(offset.y/(content.h-viewport.h),0,1) * (trackH - thumbH)
thumbRect(local) = { viewport.w - thickness - margin, margin + thumbY, thickness, thumbH }
```
`thumbWindowRect` adds `offsetFromRoot()` to lift it into window coords (the
space of `MouseEventParams::position`). `hitTestThumb(p)` returns
`Axis::Vertical` / `Horizontal` / none by point-in-rect. The inverse map for
E3: `offset.y = ((thumbY_new)/(trackH - thumbH)) * (content.h - viewport.h)`,
where `thumbY_new = clamp(pointerY - grabDelta - (windowTop+margin), 0, trackH-thumbH)`.

### E2 — capture routing
`dispatchInputEvent`, right after computing `pos`: if `capturedView_ != nullptr`
and the event is `LMouseDown/Up` or `CursorMove`, deliver via
`capturedView_->emit(event)` (capture is an explicit target, no bubbling) and
skip the hover-synthesis + hit-test path. `LMouseUp` still runs after the
capturer handles it so the capturer can `releaseMouse()` in its own
`LMouseUp`. Guard against a captured view detaching (clear on
`setTreeHostRecurse(nullptr)`).

### E4 — momentum vehicle
`AnimationScheduler::tween<Point2D>(current, landing, [sv](const Point2D& o){ sv->setScrollOffset(o); }, timing, decelerateCurve)`.
Velocity from the drag: keep the last `(offset, frameOr timestamp)` sample;
`v = Δoffset/Δt`. Landing = `current + v * kFlingFactor`, clamped. Store the
returned `AnimationHandle`; cancel it at the top of any wheel/key/drag
handler so user input always wins.

## 5. Verification
- **E1**: log/inspect `hitTestThumb` returns true only over the thumb pill, false on the track and content.
- **E2**: press inside a ScrollView thumb, drag the cursor *outside* the window bounds — `CursorMove` still reaches the ScrollView (capture holds); release ends it.
- **E3**: drag the thumb up/down — content tracks the thumb 1:1 (thumb at top → offset 0, thumb at bottom → offset max); no drift; releasing mid-track leaves it put.
- **E4**: a quick fling-and-release keeps scrolling and eases to a stop; it stops exactly at the clamped end when flung past it; a wheel tick mid-glide cancels the glide.
- All visual checks are screenshot / interaction hand-offs (screenshot tool not yet trusted).

## 6. Out of scope
- Win32 / GTK trackpad-gesture scroll-phase population (E5 ships macOS phase + `None` elsewhere; other backends' precise-gesture phase is a later native follow-up).
- Scroll-bar hover/press *styling* (the thumb color is static grey; theming is a separate Native-Theme pass, ScrollableContainer plan §9.1).
- Overscroll / rubber-band bounce.

## 7. Implementation increments
E1 (thumb refactor) and E2 (capture) have no standalone visual, so they land
together with E3 as the first verifiable increment — **a draggable bar**.
E4 (drag-fling momentum) and E5 (wheel momentum + native phase) each follow
as their own verifiable increment.

## 8. Status
- **E1 + E2 + E3 DONE + verified (2026-06-26).** Draggable thumb, track-jump,
  and cross-window drag all work on macOS. Required a **native fix**: macOS
  routes a button-held move to `mouseDragged:`, which `CocoaItem` did not
  forward — added the forwarder + mapped `NSEventTypeLeftMouseDragged` →
  `CursorMove` (CocoaEvent.mm). Backend audit: Win32 `WM_MOUSEMOVE` and GTK4
  `GDK_MOTION_NOTIFY` already fire during drags (no equivalent gap).
- **Known Win32 gap:** dragging *outside* the window drops motion — Windows
  needs an OS-level `SetCapture()`/`ReleaseCapture()` that the app-level E2
  capture does not trigger. macOS (implicit drag grab) and GTK4 (implicit
  button grab) continue delivering out-of-window motion. Wire
  `WidgetTreeHost::captureMouse` → native `SetCapture` when the Win32 build
  is next exercised.
- **E4 DONE + verified (2026-06-26).** Drag-fling momentum via
  `AnimationScheduler::tween<float>` (EaseOut) to a projected/clamped
  landing; cancelled on any new input + in `~ScrollView`; bootstrapped with
  `scheduleRepaint()` then sustained by FrameBuilder's D7.2 auto-pump.
  Tuning constants at the top of ScrollView.cpp.
- **E5 DONE (built, awaiting verify).** Added `Native::ScrollPhase` enum +
  `ScrollParams::phase`; macOS populates it from `NSEvent.momentumPhase` /
  `phase` (CocoaItem.mm), Win32/GTK send `None`. ScrollView wheel path:
  discrete mouse wheel (`phase == None`) synthesizes an app fling from
  per-tick velocity (rapid ticks re-project, last one coasts); a trackpad
  (any real phase) applies the OS's own decaying momentum deltas and adds
  NO app momentum (no double-glide). Reuses E4's `startFling` + `cancelFling`.
  **Win32/GTK trackpad-gesture phase population is a native follow-up** —
  they currently report `None`, so a precision touchpad there gets the
  discrete-wheel app-momentum path (acceptable; refine when those backends
  are exercised).
