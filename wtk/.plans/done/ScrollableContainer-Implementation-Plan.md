# ScrollableContainer Implementation Plan

This document elaborates [Widget-Stub-Implementation-Plan §5A](Widget-Stub-Implementation-Plan.md#5a-scrollablecontainer) into a buildable phased plan. The parent plan's §5A is a one-page sketch (Options struct + class signature + "wraps a `ScrollView`"); this doc is what an implementer should consult before writing code.

The reason it warrants its own plan rather than inline expansion: the work touches three layers — the new widget itself, the existing `ScrollView` (offset-clamping bug, content-rect ownership), and `WidgetTreeHost::hitTestWidget` (must fold `contentOffset()` to dispatch clicks correctly into scrolled subtrees). Each layer needs its own phase boundary.

> **Prerequisite discovered during S1 (2026-06-25).** This plan's §2 assumes
> `ScrollView` is wired into the rendering/layout/event pipelines. It is not —
> Phase 4.7's paint rewrite and the §2.3a focus/event changes orphaned its
> layout, paint, and wheel-input integration after this plan was written. The
> S1 widget code (below) is correct and builds, but does not scroll/clip/draw
> bars/receive input until that is repaired. See
> [ScrollView-4.7-Integration-Plan](ScrollView-4.7-Integration-Plan.md) (phases
> V1–V5), which must land before S1 can be verified at runtime.

Related documents:

- [ScrollView-4.7-Integration-Plan](ScrollView-4.7-Integration-Plan.md) — **prerequisite**; re-integrates `ScrollView` into the 4.7 pipeline (layout passthrough, event bubbling, clip + bars, FocusManager).
- [Widget-Stub-Implementation-Plan §5A](Widget-Stub-Implementation-Plan.md#5a-scrollablecontainer) — parent catalog entry.
- [Widget-Type-Catalog-Proposal §3](Widget-Type-Catalog-Proposal.md) — `ScrollableContainer` is "Container with scroll viewport and content host; existing placeholder; integrate with `ScrollView` behavior."
- [Widget-Stub-Implementation-Plan Phase 5B–5F](Widget-Stub-Implementation-Plan.md#phase-5-scrollablecontainer-and-collection-widgets) — `ListView`, `TreeView`, `TableView`, `CollectionView`, `PropertyGrid` are downstream consumers of this work; their virtualization layers attach to whatever this plan ships.
- [`wtk/src/UI/ScrollView.cpp`](../src/UI/ScrollView.cpp) — the existing fully-virtual scroll viewport this widget wraps. Tier 3 Phase 3.6 made it a `PushClip` producer and a `contentOffset()` consumer.
- [`wtk/src/UI/WidgetTreeHost.cpp:563`](../src/UI/WidgetTreeHost.cpp) — `hitTestWidget`'s parent-relative descent (does not fold `contentOffset()` today).

---

## 1. Concept

A `ScrollableContainer` is a `Widget` whose content area can be larger than its visible bounds, with the overflow reachable via scroll input. Concretely:

- It exposes the usual container API (`addChild` / `removeChild` / `childWidgets()`) so that any widget can be added as scrolled content.
- It paints whatever children fit inside the viewport's clip; children fully or partially outside the viewport are clipped (already the `ScrollView` `PushClip` behavior).
- It absorbs scroll-wheel events and updates the scroll offset, shifting descendant rects so the viewport moves through the content (already the `ScrollView::contentOffset()` behavior).
- It computes a content extent (the union of child rects, or an explicitly-set size) and clamps the scroll offset to that extent.

What it is **not**:

- It is **not** a layout container of its own. The children are arranged inside the content host by whatever the inner host's `LayoutManager` does — by default `AbsoluteLayout` (children placed at their own rects), but the implementer can drop a `StackWidget` *inside* the `ScrollableContainer` for flex layout. The split keeps "what arranges children" decoupled from "what scrolls."
- It is **not** the place where virtualization lives. `ListView` / `TableView` / `CollectionView` (Phase 5B–5E) attach to the same content host and recycle widgets, but virtualization is *their* responsibility, not `ScrollableContainer`'s. This widget is the unvirtualized base case.

---

## 2. Existing infrastructure

| Piece | Where | What it gives us |
|-------|-------|------------------|
| `View::ScrollView` | `wtk/include/omegaWTK/UI/ScrollView.h`, `wtk/src/UI/ScrollView.cpp` | Single-child scroll viewport. Owns `scrollOffset`, emits `PushClip`/`PopClip` around the child's draw ops, and returns `-scrollOffset` from `contentOffset()` so `FrameBuilder` shifts descendant ops when it walks down. Has a `DefaultScrollHandler` for scroll-wheel input. |
| `ScrollViewDelegate` | same | Subclass-able event processor. We don't need to override it for v0 — the default handler is enough. |
| `Widget(ViewPtr view)` constructor | `wtk/include/omegaWTK/UI/Widget.h:246` | Lets us hand a Widget a pre-constructed `ScrollView` as its root view. |
| `Container(ViewPtr view)` constructor | `wtk/include/omegaWTK/Widgets/BasicWidgets.h:71` | Lets the *inner* content widget reuse the `ScrollView`'s child view — the load-bearing constructor for the architectural choice in §3. |
| `View::offsetFromRoot()` | `wtk/include/omegaWTK/UI/View.h:265` | Used by hit-test-related downstream code; reads `contentOffset()` for ancestors as it walks up, so it is already correct for nested scrolling. The bug is in `WidgetTreeHost::hitTestWidget`'s *downward* walk, not in this accessor. |
| `ContainerLayout` / `ContainerClampPolicy` | `wtk/include/omegaWTK/UI/LayoutManager.h` | Used by `Container` to clamp children to the parent rect. For a `ScrollableContainer`'s content host the clamp must be **off** along scrolling axes — children must be free to exceed the viewport. |

What is missing:

- A way to grow the content host's rect as children are added (so the union of child rects becomes the content extent).
- An upper-bound clamp on `ScrollView::scrollOffset`. Today the `DefaultScrollHandler` clamps to `≥0` but not to `≤ contentExtent - viewport`. This is a bug regardless of `ScrollableContainer`, but it materializes the moment we ship one.
- `WidgetTreeHost::hitTestWidget`'s downward walk must subtract the parent's `contentOffset()` before recursing, or clicks land on the un-scrolled coordinates.

These three are the only changes outside the new widget itself.

---

## 3. Architectural choice — three options

The thinnest viable widget is some combination of `ScrollView` (offset + clip + scroll-wheel routing) and a `Container` (child management). Three structures are plausible; this section enumerates the trade-offs and recommends one.

### Option A — Two-view composite (RECOMMENDED. DEVELOPER: YES)

```
ScrollableContainer (Widget)
├── view = ScrollView(viewport rect, child = contentView)
└── contentWidget_ : Container         // wraps `contentView`
    └── children live here, addressed in content space
```

Construction:

1. Build the inner `View` (`contentView`) sized to the viewport.
2. Build a `ScrollView` whose `child` is `contentView`.
3. Hand the `ScrollView` to `Widget(ViewPtr view)` so it is the widget's root view.
4. Build a `Container(contentView)` and store it as `contentWidget_`. This `Container` owns the children but does *not* introduce a third `View` — it reuses `contentView`.
5. `ScrollableContainer::addChild` / `removeChild` / `childWidgets()` forward to `contentWidget_`.

Why this works:

- `FrameBuilder`'s paint walker descends into `view->subviews()` — `contentView` is the only subview of `ScrollView`, and its own subviews are the children's views. The `PushClip` + `contentOffset()` shift on `ScrollView` applies to everything underneath as it already does.
- The hit-test fix in §6.2 makes `WidgetTreeHost::hitTestWidget`'s downward walk fold `ScrollView::contentOffset()` so the child rects are addressed in their actual (scrolled) screen positions.
- The Widget tree's `childWidgets()` returns `contentWidget_->childWidgets()`, so the rest of the framework (style cascade, layout walks, resize propagation) sees the children at the right level of nesting *as widgets* even though they live "under" a `ScrollView` *as views*.

Trade-offs:

- The widget owns two coupled objects (`ScrollView` and `Container`) and the user has to internalize that "the view tree is two-deep before the children start" if they ever drop down to the View layer. We handle this by never exposing `contentWidget_` publicly — it is an implementation detail.

### Option B — Inherit from `Container`

```
ScrollableContainer : public Container
└── view = ScrollView    // override Container's view
```

Tempting because `addChild` / `removeChild` come for free. Doesn't work:

- `Container::Container(rect)` calls `View::Create(rect)`, producing a plain `View` — not a `ScrollView`. We'd have to use `Container(ViewPtr)` and pass a `ScrollView`, but then the children added via `Container::addChild` get wired into the `ScrollView` itself (`addSubView` on the viewport), not into a content-host view. There is no separation between viewport and content extent — every child is laid out against the viewport's bounds, which is exactly the un-scrollable case.
- `Container`'s clamp policy (`ContainerLayout::arrange`) clamps children to the parent rect. For scrolling we want the clamp **off** on scrolling axes. We'd have to bypass `Container`'s default clamp, which means re-implementing most of `Container::onChildRectCommitted` — defeating the inheritance win.

### Option C — Widget owns only `ScrollView`, no inner Container

```
ScrollableContainer : public Widget
└── view = ScrollView
    ├── child = a single user-supplied View
```

This is basically `ScrollView` with a Widget wrapper. Drops the `addChild` contract from §5A entirely and requires the user to construct a `Container` (or a `StackWidget`) themselves, then hand it to `ScrollableContainer::setContent(widget)`. Cleaner in API but moves the burden onto callers — every test, every `ListView` impl, every documentation example would have to spell out the inner content widget. The catalog entry promises "container with scroll viewport and content host"; Option C makes the host an out-of-widget concern.

### Recommendation

**Option A.** The two-view composite is the only one that satisfies all four catalog requirements (addChild contract, content extent ≠ viewport, no inheritance-of-clamp issues, no caller-facing inner container).

---

## 4. Public API

```cpp
struct OMEGAWTK_EXPORT ScrollableContainerOptions {
    bool verticalScroll = true;
    bool horizontalScroll = false;

    /// Default is `true`: content rect grows to fit children automatically.
    /// Set to `false` and call `setContentSize(w, h)` to manage extent
    /// explicitly (e.g. when a `ListView` knows its total content height
    /// up front and does not want the host doing union-of-rects work).
    bool autoSizeContent = true;
};

class OMEGAWTK_EXPORT ScrollableContainer : public Widget {
    ScrollableContainerOptions options_;
    // contentWidget_ is the implementation-private inner Container —
    // not exposed because callers should not stack multiple containers
    // inside the same scroll viewport. addChild/etc. forward to it.
    SharedHandle<Container> contentWidget_;
public:
    explicit ScrollableContainer(
        Composition::Rect rect,
        const ScrollableContainerOptions & options = {});

    // Forwarded to contentWidget_.
    WidgetPtr addChild(const WidgetPtr & child);
    bool removeChild(const WidgetPtr & child);
    OmegaCommon::ArrayRef<WidgetPtr> childWidgets() override;

    // Explicit content sizing (S2 — `autoSizeContent = false` path).
    void setContentSize(float w, float h);
    Composition::Point2D contentSize() const;

    // Scroll API (S5).
    void scrollTo(float x, float y);
    Composition::Point2D scrollOffset() const;

    // Mutate options after construction (S5).
    void setOptions(const ScrollableContainerOptions & options);
    const ScrollableContainerOptions & options() const;

    ~ScrollableContainer() override;
};
```

`Create()` is omitted because `ScrollableContainer` inherits from `Widget` and the `WIDGET_CONSTRUCTOR()` factory does not accept a custom options parameter — direct construction matches the `StackWidget` precedent.

The `addChild` / `removeChild` overloads return what the inner `Container` returns; the type signatures match `Container`'s exactly so callers can swap a `Container` for a `ScrollableContainer` without code changes.

---

## 5. Internals

### 5.1 Content extent

- `autoSizeContent = true` (default): each `addChild` / `removeChild` recomputes `contentWidget_->rect()` as the bounding box of all child rects plus a small padding (configurable later — v0 uses zero). The non-scrolling axis is stretched to the viewport rect on that axis. The scrolling axis is the max(child extent, viewport).
- `autoSizeContent = false`: `setContentSize(w, h)` writes the content rect directly. `addChild` does not resize the content host.

The content rect always satisfies `content.w >= viewport.w` along the non-scrolling axis and `content.h >= viewport.h` along the non-scrolling axis — the `ScrollView`'s `paintOverlay` already assumes this when computing scroll-bar thumb ratios (line 121-167 of `ScrollView.cpp`).

### 5.2 Viewport resize

When the `ScrollableContainer`'s own rect changes (e.g. window resize), the viewport `ScrollView`'s rect tracks it, and the content rect is recomputed:

- `autoSizeContent = true`: re-stretch non-scrolling axis to the new viewport size; leave scrolling axis at max(union-of-children, new viewport).
- `autoSizeContent = false`: leave content rect at the caller's last `setContentSize`. If the new viewport is bigger than the content, clamp `scrollOffset` to zero.

### 5.3 Children don't get viewport-clamped

`Container`'s `ContainerLayout` clamps each child rect to the parent. For `contentWidget_`, the parent rect is the (possibly larger-than-viewport) content rect, so the clamp is against the content extent — which is exactly what we want. The `ContainerClampPolicy` default is fine; no override required.

The implication: `contentWidget_`'s rect must be set *before* `addChild` runs so the clamp sees the right bounds. The two-step is enforceable in `addChild`: recompute content rect → forward to `contentWidget_->addChild`.

### 5.4 Theme + dirty propagation

`Widget::onThemeSet` walks via `onThemeSetRecurse`. Because `childWidgets()` forwards to `contentWidget_->childWidgets()`, the theme walk reaches the scrolled children correctly. Same story for the dirty-bit propagation in `View::markDirty` — it walks parent pointers (set by `addSubView`) up to the root `View`, which on this widget is the `ScrollView`. The propagation works regardless of how many `View` layers sit between the leaf and the root.

---

## 6. Cross-cutting fixes (outside the widget itself)

### 6.1 `ScrollView` upper-bound clamp

`DefaultScrollHandler::onRecieveEvent` (line 22-41 of `ScrollView.cpp`) currently clamps `scrollOffset` to `≥ 0` but lets it grow without bound. Once content > viewport scrolling works, the user can scroll past the end and the bars draw outside their tracks.

Fix: clamp the new offset against `child->getRect()` and the `ScrollView`'s own rect:

```cpp
const auto & content = owner->child->getRect();
const auto & viewport = owner->getRect();
const float maxX = std::max(0.f, content.w - viewport.w);
const float maxY = std::max(0.f, content.h - viewport.h);
newOffset.x = std::clamp(newOffset.x, 0.f, maxX);
newOffset.y = std::clamp(newOffset.y, 0.f, maxY);
```

This lives in `ScrollView.cpp` and benefits any current or future user of `ScrollView` (not only `ScrollableContainer`). It is small enough to land in the same phase as the `ScrollableContainer` skeleton (§7 S1).

### 6.2 `WidgetTreeHost::hitTestWidget` content-offset fold

`hitTestWidget` (line 563-590 of `WidgetTreeHost.cpp`) descends children by subtracting `childView.getRect().pos` from the input point. It does **not** subtract `contentOffset()`. For non-scrolling views `contentOffset()` is `{0,0}` so this has been invisible — but `ScrollView::contentOffset()` returns `-scrollOffset`, so once a `ScrollView` is in a hit-test path, clicks land on the un-scrolled coordinates.

Fix: when entering a child whose own view has a non-zero `contentOffset()`, fold that into the local-point translation. The paint walker already does this via `PaintContext.offset`. The hit walker needs the same fold:

```cpp
Composition::Point2D localPoint {
    point.x - childView.getRect().pos.x,
    point.y - childView.getRect().pos.y
};
auto co = childView.contentOffset();
localPoint.x -= co.x;
localPoint.y -= co.y;
```

(Subtract, not add, because `contentOffset()` carries `-scrollOffset` and we want to shift the *point* in the same direction the *content* moved.)

The change is two lines plus a comment. It must land before `ScrollableContainer` users can click anything inside a scrolled subtree. This is §7 S4.

### 6.3 Resize clamping interaction

[Resize-Clamping-Plan](Resize-Clamping-Plan.md) Phase F flagged shape-primitive distortion on shrink. `ScrollableContainer`'s viewport rect is itself a shape (the visible region); its content rect is unrelated to that. The interaction is benign because content sizing is the inner host's concern, not the shape clamp's. No coupling work needed beyond §5.2's "shrink → clamp offset to zero" path.

---

## 7. Phases

| Phase | Description | Requires | Blocks |
|-------|-------------|----------|--------|
| **S1** | Two-view skeleton — `ScrollableContainer` constructor builds `ScrollView` + `contentView` + inner `Container`. `addChild` / `removeChild` forward. `setContentSize` and `contentSize()` ship. No auto-sizing yet (caller-supplied content size only). Includes §6.1's `ScrollView` upper-bound clamp because v0 users will hit it on the first scroll. | None. Uses existing `ScrollView`, `Container(ViewPtr)`, `Widget(ViewPtr)`. | S2; downstream Phase 5B–5E collection widgets that need any scrolling at all. | (THIS DONE)
| **S2** | Auto-sizing — `addChild` / `removeChild` / viewport-resize recompute content extent as `max(union-of-child-rects, viewport)`. Non-scrolling axis stretched to viewport. `options_.autoSizeContent` honored. | S1. | Use cases where the application does not know content size in advance (most). |
| **S3** | Cross-axis stretching for explicit-size mode — when `autoSizeContent = false` and viewport > content along the non-scrolling axis, stretch content. Keeps the clamp-policy story symmetric with S2. | S1 + S2 (the auto-sizing math is the reference for the stretch math). | Visual parity with native `NSScrollView` / GTK `ScrolledWindow` in the explicit-content case. |
| **S4** | `WidgetTreeHost::hitTestWidget` content-offset fold (§6.2). | None — independent of S1–S3, but only observable once a `ScrollableContainer` exists. Schedule alongside S1 so the first thing users do (click on scrolled content) works. | Phase 5B–5E collection widgets where every row is a button or text input. | (THIS DONE 2026-06-30) |
| **S5** | `scrollTo`, `scrollOffset()`, `setOptions`, and an `onScroll(callback)` hook for apps that need to react to user scrolling (e.g. for header pinning). | S1. | Phase 5B `ListView::scrollToItem`, Phase 5D `TableView` header pinning. |

S1 + S4 together is the **minimum viable shipping cut** — every catalog use case works to the point of usability. S2 + S3 are quality-of-life. S5 is API completeness.

Each phase is < 200 LoC. S1 is ~120 LoC of new widget + ~10 LoC of `ScrollView` fix. S4 is ~5 LoC.

---

## 8. Verification

- **S1**: Construct a `ScrollableContainer(rect=200×200)`, `setContentSize(200, 600)`, add a single tall `Rectangle` child. Verify scroll-wheel input shifts the rectangle up/down inside the viewport and stops at both ends (no over-scroll past 0 or past `content.h - viewport.h`).
- **S1+S4**: Add a `Button` at content y=400 inside the same scroll container, scroll it into view, click it. Verify the button's `onPress` fires. Without §6.2's fix this test fails — the hit lands wherever the un-scrolled button sits.
- **S2**: Construct `ScrollableContainer(rect=200×200)` with `autoSizeContent = true`, add three 200×300 children stacked vertically (set their rects manually to `y=0,300,600`). Verify the content rect grows to 200×900 without an explicit `setContentSize` call, and scrolling reaches the bottom child.
- **S3**: With `autoSizeContent = false`, `setContentSize(100, 600)` on a 200×200 viewport (content narrower than viewport along non-scrolling axis). Verify content stretches to 200×600 — i.e. children laid out at content-space `x = 50` appear centered visually rather than left-clipped.
- **S5**: `scrollTo(0, 200)` programmatically, then read `scrollOffset()` and confirm it returns `(0, 200)`. Subscribe to `onScroll`, scroll via wheel, confirm the callback fires with the delta.

The verification surface is small because the widget composes existing primitives. Each phase's test reuses BasicAppTest's existing widget-add scaffolding — no new test harness needed.

---

## 9. Open questions

1. **Scroll-bar styling.** The bars are hard-coded grey at `0.5, 0.5, 0.5, 0.6` (`ScrollView.cpp:19`). Should `ScrollableContainer` route a theme variable down (`themeVars.scrollBarColor` or similar)? Likely yes via a later "Native-Theme-Application" pass, not in this plan's phases. The widget itself does not consult themes today — it inherits whatever color `ScrollView` emits.

2. **Scroll-bar hit-test / drag.** The bars are currently draw-only (no mouse interaction). Click-drag-the-thumb is a real expectation for desktop UIs and is not in this plan. Track as a `ScrollView`-side enhancement, not a `ScrollableContainer` knob. If it lands, `ScrollableContainer` gets it for free.

3. **Momentum / smooth scroll.** Out of scope. The `DefaultScrollHandler` snaps to the new offset immediately. A future pass through `AnimationScheduler` could animate the offset; the API does not need to change to support that — `scrollTo` can grow an `(animate=true)` flag later without breaking source compat.

4. **Keyboard scroll** (PageUp/PageDown/Home/End/arrow keys). Requires focus to be inside the scrolled subtree, which requires [Native-API-Completion-Proposal §2.3a FocusManager](Native-API-Completion-Proposal.md#focus--virtual-focus-manager). Defer — this plan does not block on it, but a Phase S6 can be added when FocusManager lands.

5. **Nested scroll containers.** Two `ScrollableContainer`s, one inside the other. Each `ScrollView`'s `contentOffset()` should compose via the `View::offsetFromRoot()` chain (already does). The §6.2 hit-test fold composes too because it walks each child's `contentOffset()` independently. Verify in S4's tests by nesting a `ScrollableContainer` inside another and confirming clicks on the inner container's content land correctly.

6. **`StackWidget` as content.** A common pattern is `ScrollableContainer` → `VStack` → many children. Confirmed to work as long as the `VStack`'s rect is set to the content extent (i.e. its `flexLayout_` arranges children inside that rect). This is callsite responsibility — the `ScrollableContainer` cannot detect "the single child is a StackWidget, defer extent to it." If callers find this verbose, a follow-up convenience constructor (`ScrollableContainer::WithVStack(rect, opts)`) can land in S5 or later — not a v0 requirement.

7. **Viewport resize control (RESOLVED 2026-06-26).** A `ScrollableContainer` placed in a `crossAlign=Stretch` stack was widened to the full cross extent because `FlexLayout`'s explicit `Stretch` deliberately applies even to frozen leaves (so a `Separator` spans the axis). That is wrong for a viewport that owns its own width — e.g. a nested scroll container inside a scrollable page. Fix (a general Container capability, not ScrollableContainer-only, per developer direction): a child may now opt out of the cross-`Stretch` override via a new `Widget::layoutCrossStretchAllowed()` virtual (default `true`, so Separators keep stretching), plumbed through `FlexChildSpec.honorCrossStretch`. `Container::setResizeWithParent(bool)` (default `true`, preserves behavior) drives BOTH `isLayoutResizable()` (main-axis flex) and `layoutCrossStretchAllowed()` (cross stretch). `ScrollableContainerOptions::resizeWithParent` exposes the same on the viewport. ~70 LoC across `Widget`/`Container`/`FlexLayout`/`FlexChildSpec`/`StackWidget`/`ScrollableContainer`.

---

## 10. Cross-plan dependencies

### 10.1 What this plan needs from other plans

| Phase here | External dep | Why |
|------------|--------------|-----|
| S1–S5 | None at the plan-doc level. | The work is self-contained against the existing `ScrollView`, `Container`, `WidgetTreeHost` code. |
| S4 (`hitTestWidget` fold) | None (this plan ships the fix). | Listed here because the fix lives outside the widget. |

### 10.2 What other plans need from this plan

| Other plan / phase | Needs from here |
|--------------------|-----------------|
| [Widget-Stub-Implementation-Plan Phase 5B `ListView`](Widget-Stub-Implementation-Plan.md#5b-listview) | S1 (skeleton) + S2 (auto-size) + S4 (clicks). Virtualization is `ListView`'s own work but it cannot start until the scrolling primitive is real. |
| [Widget-Stub-Implementation-Plan Phase 5C `TreeView`](Widget-Stub-Implementation-Plan.md#5c-treeview) | Transitive via `ListView`. |
| [Widget-Stub-Implementation-Plan Phase 5D `TableView`](Widget-Stub-Implementation-Plan.md#5d-tableview) | Same as `ListView` + S5 (header-pinning needs the `onScroll` hook). |
| [Widget-Stub-Implementation-Plan Phase 5E `CollectionView`](Widget-Stub-Implementation-Plan.md#5e-collectionview) | Same as `ListView`. |
| [Widget-Stub-Implementation-Plan Phase 6 `Modal` backdrop](Widget-Stub-Implementation-Plan.md#phase-6-overlay-and-feedback-widgets) | Indirect — `Modal` content may itself be a `ScrollableContainer`. Not a build-order dep; just an integration note. |
| [WML-Specification.md `<ScrollableContainer>` / `<ScrollView>` mapping](WML-Specification.md) | S1 — the WML element `<ScrollableContainer>` maps to this class with the documented options. |

### 10.3 Dependency diagram

```
┌──────────────────────────────────────┐
│  ScrollableContainer-Implementation- │
│  Plan (this doc)                     │
│   ─ S1: skeleton + ScrollView clamp  │
│   ─ S2: auto-size                    │
│   ─ S3: explicit-size stretch        │
│   ─ S4: hitTestWidget content fold   │
│   ─ S5: scrollTo / options / onScroll│
└────────────────┬─────────────────────┘
                 │ feeds
   ┌─────────────┴───────────────────────────┐
   ▼                                         ▼
┌──────────────────────────────┐  ┌────────────────────────────────┐
│ Widget-Stub Phase 5B–5E      │  │ WML-Specification              │
│ (ListView, TreeView,         │  │ `<ScrollableContainer>` mapping │
│  TableView, CollectionView)  │  │                                 │
└──────────────────────────────┘  └────────────────────────────────┘
```

No cycles. The only fix that lives outside this plan's directory is the two-line `hitTestWidget` change in `WidgetTreeHost.cpp` — schedule it in S4 alongside the rest of this plan's land.
