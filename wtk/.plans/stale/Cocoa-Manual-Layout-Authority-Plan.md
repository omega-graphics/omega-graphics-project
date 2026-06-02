# Cocoa Manual Layout Authority Plan

## Goal
Prevent transient left-anchoring during live resize by making Cocoa a presentation host and keeping OmegaWTK as the sole authority for widget geometry.

## Problem Summary
During live resize, AppKit can emit intermediate geometry updates while NSView autoresizing behavior attempts to reposition child views from left/top anchors. Those temporary native placements can be presented before OmegaWTK's computed layout is applied.

## Design Direction
1. Disable Cocoa child autoresizing for widget-backed NSViews.
2. Route host content-view size/layout callbacks into the existing window resize event path.
3. Keep geometry writes explicit (from OmegaWTK layout) instead of implicit (from autoresizing masks/constraints).

## Slices

### Slice A (implemented)
- Add Cocoa host content-view callbacks for size/layout changes.
- Disable autoresizing behavior on widget child views at window-attachment time.
- Disable autoresizing behavior for child native views added under CocoaItem parent views.
- Keep current window resize event path; only improve callback timing and prevent implicit left-anchor placement.

### Slice B (implemented)
- Add generation IDs to host resize callbacks so stale geometry writes are ignored.
- Drop late frame applications older than latest committed layout generation.

### Slice C
- Batch frame/layer/drawable updates into a single transaction per resize tick.
- Ensure no partially-updated sibling geometry is presented.

### Slice D
- Add geometry validation gate (finite numbers + clamped drawable bounds) before native apply.
- Emit diagnostics when a geometry update is rejected.

### Slice E
- Add per-widget-tree authoritative commit barrier (only present if tree layout generation is complete).
- Integrate with sync engine frame lifecycle.

### Slice F
- Add instrumentation counters for dropped stale updates and host-autoresize suppression events.
- Add runtime toggle/logging for regression triage.

## Slice A Notes
- This slice intentionally does not change compositor scheduling.
- It only removes Cocoa implicit geometry authority and improves callback timing.

## Slice B Notes
- `WindowWillResize` now carries a `generation` token.
- Cocoa resize producer increments generation per queued host-bounds update.
- Async queued emissions are generation-gated and stale emissions are dropped.
- AppWindow delegate ignores stale generation updates before notifying widget tree hosts.
