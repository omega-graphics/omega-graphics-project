# ScrollView 4.7-Pipeline Integration Plan

This plan re-integrates `View::ScrollView` into the current (Phase 4.7+)
rendering, layout, and event pipelines. It is a **prerequisite** for
[ScrollableContainer-Implementation-Plan](ScrollableContainer-Implementation-Plan.md)
S1: that plan's widget code is correct and builds, but it composes a
`ScrollView` that does not actually scroll, clip, draw bars, or receive
wheel input under the present pipeline.

## 0. Why this plan exists (the regression)

`ScrollView` was authored in the Tier-3 era. The
[ScrollableContainer plan §2](ScrollableContainer-Implementation-Plan.md#2-existing-infrastructure)
treats it as working ("Tier 3 Phase 3.6 made it a `PushClip` producer and
a `contentOffset()` consumer"). Since then, **Phase 4.7's paint rewrite and
the §2.3a focus/event changes silently orphaned three of its four
integration points.** A live capture of the S1 verification scene (a 200×200
viewport, content 200×600, three stacked 200×200 bands) shows a single blue
square, no scroll bars, and no response to the wheel. Tracing each symptom:

| # | Symptom | Root cause | Evidence |
|---|---------|------------|----------|
| 1 | Content collapses to the viewport; all bands stack at y=0 (blue, last-added, on top); scroll range is zero | `ScrollView` sets no `LayoutManager`, so it falls back to `AbsoluteLayout`, whose `arrange` FitContent-clamps the content child **down to the viewport**. The inner `ContainerLayout` then clamps every band's `maxY` to 0. | `View::layoutManager()` → `&AbsoluteLayout::instance()` ([View.Core.cpp:152](../src/UI/View.Core.cpp)); `AbsoluteLayout::arrange` uses `spec.resizable=true`+`FitContent` ([LayoutManager.cpp:153-160](../src/UI/LayoutManager.cpp)); `clampRectToParent` shrinks resizable children to the parent box ([LayoutManager.cpp:96-98](../src/UI/LayoutManager.cpp)) |
| 2 | Wheel input never reaches the ScrollView (and is unlogged) | Positional dispatch hit-tests to the **deepest** view (a band) and `emit`s there; `NativeEventEmitter::emit` dispatches **only to that view's own receivers — no bubbling**. The band has no scroll handler. | `dispatchInputEvent` → `hitTest(pos)` → `target->emit(event)` ([WidgetTreeHost.cpp:762,821](../src/UI/WidgetTreeHost.cpp)); `NativeEventEmitter::emit` ([NativeEvent.cpp:113-119](../src/Native/NativeEvent.cpp)) |
| 3 | Content is not clipped to the viewport | The 4.7 paint walker calls the **virtual** `View::paint(PaintContext&)`. `ScrollView` instead defines `paint(DisplayList&)` — a different signature that merely *hides* the virtual (compiler `-Woverloaded-virtual` warned). It is never called. | walker calls `node.paint(pc)` ([FrameBuilder.cpp:367](../src/UI/FrameBuilder.cpp)); the base comment names ScrollView as a virtual-`paint` overrider it no longer is ([View.Core.cpp:573](../src/UI/View.Core.cpp)) |
| 4 | No scroll bars | Same orphaned-paint cause: `paintOverlay` is referenced nowhere but its own file. Even a correct virtual override needs a **post-children hook** to emit the matching `PopClip` + bars, which the pre-order-only walker does not provide. | `grep paintOverlay` → only `ScrollView.cpp`; walker `paintSubtree` is pre-order with no after-children call ([FrameBuilder.cpp:366-381](../src/UI/FrameBuilder.cpp)) |

The one integration point that **survived** is `contentOffset()`: the walker
folds it into `pc.offset` per descent ([FrameBuilder.cpp:370-377](../src/UI/FrameBuilder.cpp)),
so once an offset *changes*, descendants do shift. That is the only reason
scrolling is recoverable at all.

## 1. Scope

In scope: make `ScrollView` clip, scroll, draw bars, and receive wheel +
keyboard input under the 4.7 pipeline. Out of scope: momentum/smooth scroll,
scroll-bar drag (both already deferred by the ScrollableContainer plan's
open questions §9.2/§9.3).

Design decisions already taken with the developer:
- **Event routing = generic ancestor bubbling** (not a scroll-specific walk,
  not focus-based). An unconsumed native event propagates up the `View`
  parent chain until a receiver consumes it.

## 2. Phases

| Phase | Description | Requires | Blocks |
|-------|-------------|----------|--------|
| **V1** | **Content-extent layout.** Give `ScrollView` a no-op `PassthroughLayout` so its content child keeps the extent the host assigns (never FitContent-shrunk to the viewport). Fixes symptom #1: bands land at 0/200/400 and scroll range becomes `content − viewport`. | None | S1 verification (positions); V3/V4 (bars need a real extent for thumb ratios) |
| **V2** | **Generic event bubbling.** Add a `handled` flag to `NativeEvent`; add `View::dispatchEvent` that emits on self then walks `parent_ptr` while unhandled; route positional events through it from `WidgetTreeHost`. Consuming handlers set `handled`. Fixes symptom #2. | None | S1+S4 verification (wheel + clicks); ScrollableContainer S5 `onScroll` |
| **V3** | **Clip via virtual paint + post-order hook.** Replace `ScrollView::paint(DisplayList&)` with an override of the virtual `View::paint(PaintContext&)` emitting `PushClip`; add a `View::paintAfterChildren(PaintContext&)` post-order hook to both FrameBuilder walkers; `ScrollView` overrides it to emit `PopClip`. Fixes symptom #3. | None (independent of V1/V2) | V4 |
| **V4** | **Scroll bars.** In `ScrollView::paintAfterChildren`, after `PopClip`, emit the two thumb `RoundedRect`s (port the existing `paintOverlay` math). Fixes symptom #4. | V1 (extent), V3 (the hook) | — |
| **V5** | **FocusManager + keyboard scroll.** Make `ScrollView` focusable (`FocusPolicy`) so the keyboard route ([WidgetTreeHost.cpp:749](../src/UI/WidgetTreeHost.cpp)) can deliver PageUp/Down/Home/End/arrows; handle them by adjusting the offset. Unblocks the ScrollableContainer plan's deferred keyboard-scroll (§9.4 / S6). | V2 (routing model) | ScrollableContainer S6 |

V1 + V2 + V3 + V4 together make the S1 verification scene fully functional
(scroll, clip, bars, wheel). V5 is the keyboard/focus follow-on the developer
flagged.

## 3. Phase detail

### V1 — Content-extent layout

`AbsoluteLayout` always passes `{resizable=true, FitContent}`, which shrinks
any oversized child to the parent box. No existing manager leaves a child's
rect untouched. Add a process-wide `PassthroughLayout` singleton whose
`arrange` is a no-op (and whose `measure` returns the node's own rect), and
set it on `ScrollView` in its constructor:

```cpp
// LayoutManager.h / .cpp
class OMEGAWTK_EXPORT PassthroughLayout final : public LayoutManager {
public:
    static PassthroughLayout & instance();
    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void arrange(View & node, const Composition::Rect & finalRectLocal) override; // no-op
};
```

`ScrollView` constructor: `setLayoutManager(&PassthroughLayout::instance());`

The content child's position clamp (it should sit at local `{0,0}`) is the
*host's* job via the content rect, not the ScrollView's — so a true no-op is
correct. ~25 LoC. Verify: with V1 alone (no clip yet) the three bands paint
as an overflowing 600px column at 0/200/400, and the §6.1 offset clamp's
`maxY` becomes 400.

### V2 — Generic event bubbling (highest blast radius)

The chosen model. Implementation shape:

1. `NativeEvent` gains `bool handled = false;`.
2. `NativeEventProcessor::onRecieveEvent` stays `void` (no signature ripple
   across the tree); a consuming handler sets `event->handled = true`.
3. New `View::dispatchEvent(NativeEventPtr)`: `emit(event)` on self; then
   `while(!event->handled && parent) { parent = parent->parent_ptr; parent->emit(event); }`.
   (`View` owns `parent_ptr`; `NativeEventEmitter` does not, so bubbling
   lives on `View`, not the emitter.)
4. `WidgetTreeHost::dispatchInputEvent` calls `target->dispatchEvent(event)`
   instead of `target->emit(event)` for the positional path (line 821).

#### Consumption semantics — the load-bearing invariants

Bubbling is only correct if consumption stops it at the *innermost capable*
handler. Two invariants make that hold (both surfaced by the developer's
button-in-scroll-view and nested-scroll-view cases, 2026-06-25):

- **Invariant A — type-scoped consumption.** A handler sets `handled` *only
  for the event types it actually consumes*, never blanket. A `Button`
  consumes the click types (`LMouseDown`/`LMouseUp`); it leaves `ScrollWheel`
  untouched. `ScrollView` consumes `ScrollWheel`; it leaves clicks untouched.
- **Invariant B — axis-aware scroll consumption.** `DefaultScrollHandler`
  sets `handled` when the wheel's axis is scrollable *on this ScrollView*
  (the axis is enabled and has range), even if the offset is already at the
  end. It does **not** consume an axis this ScrollView doesn't scroll, so the
  event bubbles to a parent that might. (v0 = no scroll-chaining at the
  limit, matching `NSScrollView`; chaining-at-limit is a deferred UX knob —
  it needs at-limit/rubber-band detection and a policy decision.)

Because dispatch **starts at the deepest hit view and walks up**, these two
invariants produce exactly the required behavior:

| Scenario | Hit (deepest) | Bubble path → consumer | Result |
|----------|---------------|------------------------|--------|
| Click a Button inside a ScrollView | Button | Button consumes click → stop | Button fires; ScrollView ignores it |
| Wheel while hovering that Button | Button | Button ignores wheel → bubbles → ScrollView consumes | List scrolls (hovering a button doesn't block the wheel) |
| Wheel inside a nested child ScrollView | leaf in inner SV | inner ScrollView consumes → stop | Inner scrolls; **outer does not** |
| Wheel over an inner SV that only scrolls X, with a Y-scrolling parent | leaf in inner SV | inner ignores Y (Invariant B) → bubbles → outer consumes Y | Inner handles X, outer handles Y |

The invariant that makes "the child scrollView scrolls but not its parent"
true is simply: the inner ScrollView is reached *first* on the way up and
consumes, so the outer never sees the event.

**Risk — this changes event semantics tree-wide.** Today a click/scroll fires
only on the deepest hit view; after V2 an *unconsumed* event climbs to the
root. Regression guard: audit every consuming handler and make it obey
Invariant A. The consumer surface is small — only `AppWindow`,
`ScrollView::DefaultScrollHandler`, and `ViewDelegate` subclasses (e.g.
`Button`'s) implement `onRecieveEvent` today. Staging to keep risk
contained:
- **V2.1**: add `handled` + `dispatchEvent`; route **only `ScrollWheel`**
  through bubbling; `DefaultScrollHandler` sets `handled` per Invariant B.
  Everything else keeps the current deepest-hit `emit`. This makes scrolling
  work with near-zero blast radius — and because clicks are *not* yet
  bubbled, the button cases already behave: a click on a button still fires
  only on the button (old path), while a wheel over that button bubbles past
  it (button ignores wheel) to the ScrollView. The nested-ScrollView case
  also works at V2.1: wheel bubbles from the inner leaf to the inner
  ScrollView, which consumes before the outer is reached.
- **V2.2**: migrate the remaining positional events (mouse down/up, move) to
  `dispatchEvent`. Gate this on auditing every click consumer to obey
  Invariant A — chiefly `Button`'s `ViewDelegate`, which must set `handled`
  on the click types so a click inside a ScrollView (or inside another
  click-handling ancestor) does not double-react up the chain. Hover
  enter/exit is synthesized by `WidgetTreeHost` itself (not via `emit`
  bubbling) — leave that path as-is or make it idempotent.

If V2.2 surfaces regressions, the generic mechanism still ships (V2.1) and
the migration can be paused without losing scroll.

### V3 — Clip via virtual paint + post-order hook

- Delete `ScrollView::paint(DisplayList&)` and `paintOverlay(DisplayList&)`.
- Override `void View::paint(Composition::PaintContext & pc)` in `ScrollView`
  to append `DrawOp::makePushClip(localBounds)` into `pc.displayList`.
- Add `virtual void View::paintAfterChildren(Composition::PaintContext & pc)`
  (default no-op) and call it at the **end** of both walkers
  (`paintSubtree` and `paintSubtreeWithCache`) after the child loop, before
  restoring `pc.offset`. `ScrollView` overrides it to append `makePopClip()`.
- This restores the balanced `PushClip`…children…`PopClip` bracket the
  Tier-3 design intended. Verify: content past the viewport is scissored.

Note: the cache walker (`paintSubtreeWithCache`) brackets each node's *own*
paint in capture markers; confirm the new after-children hook lands outside
that node's `EndCacheCapture` and that a `ScrollView` (which `wantsLayer()`)
interacts correctly with caching — likely make ScrollView cache-ineligible
while scrolling, mirroring the §G.5.4 drag-stretch precedent.

### V4 — Scroll bars

Port the thumb-ratio math from the current `paintOverlay` (ScrollView.cpp
lines ~121-167) into `paintAfterChildren`, emitting after the `PopClip` so
bars draw outside the clip. The math already reads `child->getRect()` (the
content extent) and the view rect — both correct once V1 lands. Verify:
vertical thumb visible, height ∝ viewport/content, position ∝ offset.

### V5 — FocusManager + keyboard scroll

Make `ScrollView` `isClickFocusable()` / give it a `FocusPolicy` so a click
inside focuses it (the M1 mouse-focus walk at
[WidgetTreeHost.cpp:811-818](../src/UI/WidgetTreeHost.cpp) already climbs to
the nearest focusable ancestor). Handle `KeyDown` (PageUp/Down/Home/End/
arrows) by adjusting `scrollOffset` through the same §6.1 clamp. This is the
enabler the ScrollableContainer plan deferred as S6.

## 4. Verification

- **V1**: S1 scene — bands paint at content y 0/200/400 (red/green/blue
  column), not a single blue square. Offset clamp `maxY == 400`.
- **V2.1**: wheel over the box scrolls; add a trace in `DefaultScrollHandler`
  to confirm delivery; over-scroll stops at both ends (§6.1 clamp). Place a
  `Button` inside the scroll content: wheel **over the button** still scrolls
  the list (button ignores wheel), and clicking the button fires its
  `onPress` without scrolling. Nest a second `ScrollView` inside the content:
  wheel over the inner box scrolls **only** the inner one.
- **V2.2**: clicking a band still routes correctly; clicking the inner
  button fires once and does not bubble to a click handler on an ancestor;
  no double-fired hover/click on ancestors.
- **V3**: content above/below the viewport is clipped at the viewport edges.
- **V4**: a vertical scroll bar renders; thumb tracks the offset.
- **V5**: focus the box, press PageDown/End — content scrolls; clamps hold.

All visual checks are screenshot hand-offs (per AGENTS.md the screenshot tool
is not yet trusted).

## 5. Relationship to the ScrollableContainer plan

This plan supplies the working `ScrollView` that
[ScrollableContainer S1](ScrollableContainer-Implementation-Plan.md#7-phases)
assumed already existed. Order of operations:

1. Land V1–V4 here → S1's already-merged widget code becomes functional.
2. ScrollableContainer S4 (`hitTestWidget` content-offset fold) composes with
   V2's bubbling — both walk `contentOffset()` / parent chains independently.
3. V5 here unblocks ScrollableContainer S6 (keyboard scroll), previously
   gated only on FocusManager.

No new dependency cycles: this plan depends on nothing from the
ScrollableContainer plan; that plan's runtime correctness depends on V1–V4
here.
