# AppWindow Resize Plumbing — Outstanding Cleanups

Companion to the Tier 3 `UIView-Render-Redesign-Plan` work and the
`PaintOptions::invalidateOnResize` default flip. Lists redundant
resize-time work that survives the
`WidgetTreeHost::notifyWindowResize*` short-circuit (which now skips
the per-widget `handleHostResize` walk when no widget opts in via
`invalidateOnResize = true`).

After the short-circuit, the load-bearing native-side resize path is
`AppWindowDelegate::syncNativePresentLayer` (called unconditionally by
`dispatchResize{,Begin,End}ToHosts`). That single chokepoint resizes:

- the root `NativeItem` (CAMetalLayer / HWND child layer),
- the compositor backend's `rootVisual`,
- the window-scoped `LayerTree`'s root layer.

Items below are everything that still duplicates, conflicts with, or
double-dispatches that path. Triage labels match the original macOS
review: **CLEARLY REDUNDANT**, **LIKELY REDUNDANT (needs human eyes)**,
**KEEP**.

---

## CLEARLY REDUNDANT

### 1. macOS — `rootItem->resizeNativeLayer(rect, scale)` duplicates Cocoa autoresizing

**Where:** `wtk/src/UI/AppWindow.cpp:254` (inside `syncNativePresentLayer`).

The ContentView already has
`NSViewWidthSizable | NSViewHeightSizable` set in
`wtk/src/Native/macos/CocoaAppWindow.mm:56`. Cocoa's layout system
auto-resizes the contained CAMetalLayer's frame on every window
resize event — without WTK doing anything.

`resizeNativeLayer` then re-sets the same frame plus
`drawableSize`. The drawable size update is the only piece Cocoa
doesn't already do; everything else is no-op work.

**Suggested cleanup:** split `resizeNativeLayer` so the macOS impl
updates only the `CAMetalLayer.drawableSize` (and only when the
`scale * size` actually changed). Drop the frame re-assignment.
Windows path is unaffected — there's no equivalent autoresize.

---

## LIKELY REDUNDANT (needs human eyes)

### 2. macOS live-resize double-dispatch at session start

**Where:** `wtk/src/UI/AppWindow.cpp:350–365` (the `case
NativeEvent::WindowWillResize:` block).

At the start of a live-resize session (`!liveResizeActive`), the
handler fires `dispatchResizeBeginToHosts(rect)` AND
`dispatchResizeToHosts(pendingLiveResizeRect)` back-to-back. Each
runs `syncNativePresentLayer` (lines 279, 290, 299), which means
the CAMetalLayer's drawableSize is recomputed twice in the same
event tick at the start of every live-resize.

**Suggested cleanup:** when starting a live-resize session, the
"begin" dispatch alone is enough — the resize event that opened the
session is the first sample. Drop the back-to-back
`dispatchResizeToHosts` and let the next `WindowResize` event (or
the closing `WindowResizeEnded`) carry the next sample.

Watch out for: scenes that subscribe to `dispatchResizeToHosts` for
mid-resize layout updates and would notice the missing first
sample. The widget short-circuit makes that a non-event for the
default-config case, but custom delegate subclasses could care.

### 3. `queueResizeBounds` generation-number dedup

**Where:** `wtk/src/Native/macos/CocoaAppWindow.mm:334–340`
(dedup logic), `424–440` (`windowWillResize:toSize:` queues),
`455–473` (`windowDidResize:` queues).

`windowWillResize:` and `windowDidResize:` both end up in
`queueResizeBounds`. Dedup relies on generation numbers monotonically
distinguishing the two paths. If a session ever drops or reuses a
generation, the same bounds get dispatched twice. Hasn't been seen
in practice but the invariant isn't enforced anywhere — it's load-
bearing for not double-resizing the surface.

**Suggested cleanup:** add a single-event-loop-tick coalescer
in `queueResizeBounds` (drop a queued bounds when it equals the most
recent queued bounds, regardless of generation). Cheap, defensive,
makes the generation logic non-load-bearing.

---

## KEEP (mentioned for completeness)

### 4. `WindowWillStartResize` dispatch path

**Where:** `wtk/src/UI/AppWindow.cpp:312–322`.

Emits `WindowWillStartResize` and calls
`dispatchResizeBeginToHosts`. Necessary to open the resize session
so any widget that *does* opt in to `invalidateOnResize` gets a
clean session boundary for its layout. Don't touch.

### 5. `windowDidChangeBackingProperties`

**Where:** `wtk/src/Native/macos/CocoaAppWindow.mm:476–488`.

Only emits `WindowScaleFactorChanged`; no resize work. Necessary
for Retina display transitions. Don't touch.

### 6. `syncNativePresentLayer` itself

**Where:** `wtk/src/UI/AppWindow.cpp:244–275`.

The whole point of the short-circuit is that this method becomes
*the* native resize path. Its `rootVisual->resize` and
`windowLayerTree root layer resize` (lines 261, 273) used to look
redundant with the per-view walk — they are no longer redundant
after the short-circuit; they're now the only source of geometry
truth for the window-scoped tree. The `rootItem->resizeNativeLayer`
call on line 254 is the one piece worth trimming (item 1 above).

---

## Suggested ordering

1. **Item 1** — small, macOS-only, behavior-equivalent. Land first.
2. **Item 3** — adds a tiny safety net without changing behavior in
   the happy path. Land before touching item 2.
3. **Item 2** — actual behavior change (drops a resize sample at
   live-resize start). Land last, after items 1 and 3 reduce the
   noise floor, so any regression that surfaces is clearly
   attributable to this change.

---

## Open question (not a cleanup, but related)

Now that `PaintOptions::invalidateOnResize` defaults to false and
the widget walk is gated on it, audit other entry points that call
`View::resize` outside of the resize path — touch / mouse / hit-
testing / animation completion. Anything that walks the View tree
to nudge a rect should be checked for whether the rect change is
still meaningful when no widget repaints.
