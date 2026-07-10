# Overlay Z-Order Plan

This document specifies the in-window overlay layer for OmegaWTK — the mechanism by which tooltips, popovers, dropdown menus, context menus, modal dialogs, snackbars, and drag-ghost visuals render **above** the main widget tree and **cover/clip** content underneath them within the same `AppWindow`.

It resolves the long-standing question of whether overlays are Widgets, Views, or some third construct, and how their stacking order is expressed in the `CompositeFrame` / `WidgetSlice` model.

Related documents:

- [Widget-Stub-Implementation-Plan.md](Widget-Stub-Implementation-Plan.md) — Phase 6 consumes this plan's `OverlayHost` to implement individual overlay widgets (`Tooltip`, `Popover`, `ContextMenu`, `Modal`, `Snackbar`, `Sheet`).
- [Panels-And-Window-Customization-Plan.md](Panels-And-Window-Customization-Plan.md) — Part A's `AppPanel` is a **separate top-level surface** for content that lives outside an `AppWindow` (tool palettes, tear-off inspectors). It is *not* an alternate render mode for this plan's overlays. See §7.
- [Native-API-Completion-Proposal.md §2.3a](Native-API-Completion-Proposal.md#23a-virtual-focus-cursor-and-tooltip-in-the-view-tree) — `Widget::setTooltip` is the public surface that drives a tooltip overlay through this layer.
- [UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md) — establishes intra-`UIView` `zIndex` for element-tag stacking. This plan adds **inter-Widget** stacking above that.

---

## 1. Concept

> **Overlays are Widgets.**

Specifically: an overlay is a `Widget` (or any subclass — `Container`, `Widget`-leaf, `StackWidget`, custom) whose root `View` lives in the `OverlayHost`'s overlay slot on `WidgetTreeHost` rather than in the main `View` subtree under `AppWindow::rootView`. It paints through the **same `View::paint(PaintContext &)`** hook every other Widget uses, emits the **same `DrawOp`s** into the **same window-wide `DisplayList`**, and lands in the **same `CompositeFrame::slices` vector** — just at the end, after every main-tree slice.

That choice (Widget, not a new entity) keeps the catalog uniform: `Popover` is a `Container` you can fill with arbitrary child widgets, `Tooltip` is a one-`Label` `Widget`, `ContextMenu` is a `VStack` of menu-item widgets. Everything that already works for in-tree widgets — themes, layout, animation, hit-testing — works for overlays without a parallel API.

What overlays **are not**:

| Alternative                              | Why we don't use it                                                                                                                                                                                                                                  |
|------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| A new `Overlay` base class               | Would force every overlay-eligible widget to derive from it. `Modal` wraps arbitrary content; `Popover` hosts arbitrary children. The "overlay-ness" is a *position in the tree*, not a *type*.                                                       |
| A separate `OverlayView : View`          | Would split the paint pipeline. `paintSubtree` would need two walks (main + overlay); animation, hit-testing, and theme inheritance would need duplicate code paths. We already separate the *slot* (main vs. overlay) without splitting the *type*. |
| A `zIndex` on every `View`               | The flat-zIndex model (CSS-style) needs a global sort across all siblings every frame and creates unintuitive precedence (a popover inside a deep subtree at `z-index: 9999` still sorts against root-level siblings). Tiered slots are simpler and match how every native UI system organizes window-level layers. |
| Native popup windows                     | Would force a second `NativeSurface` and compositor binding per overlay — far too heavy for the common case (tooltips, popovers, context menus inside the window). `AppPanel` exists for the separate use case of *out-of-window* UI (tool palettes, tear-offs), but is not a render path for overlays — overlays are window-bound by design. |

---

## 2. Z-tier model

Overlays paint in **tiers**, not by per-Widget z-index. Tiers are ordered:

```
0. Main tree           (root WidgetTreeHost subtree, painted first)
1. Floating            (popovers, dropdown menus, autocomplete lists)
2. Modal               (modal dialogs + their backdrop)
3. Tooltip             (hover popups; never absorb hits)
4. DragGhost           (drag-and-drop visual preview; never absorbs hits)
```

Higher number = paints later = sits visually on top. Within a tier, paint order is `OverlayHost::present` call order (FIFO), matching how every native UI library breaks ties.

**Why these tiers and no more.** Each tier corresponds to a distinct *dismissal precedence* — pressing Escape dismisses the topmost-tier overlay first, click-outside dismisses Floating but not Modal, and a Tooltip never blocks a Modal even though it paints above. The tiers are the *user-perceptible* layers; adding more (e.g. separate `MenuPopup` vs. `Popover`) would not change paint order or dismissal precedence, so they collapse to the same tier.

**Mapping to existing overlay widgets:**

| Widget                                                | Tier        |
|-------------------------------------------------------|-------------|
| `Popover`                                             | `Floating`  |
| `ContextMenu`, `PopupMenu`, `Select`/`Dropdown` popup | `Floating`  |
| `Modal`, `Sheet`                                      | `Modal`     |
| `Snackbar`                                            | `Floating` (when toast-style at a window corner) or `Modal` (when blocking) — per-instance choice |
| `Tooltip`                                             | `Tooltip`   |
| Drag-and-drop preview (future)                        | `DragGhost` |

---

## 3. `OverlayHost` API

`OverlayHost` is the per-window object that owns the overlay slot. It lives on `WidgetTreeHost`, and there is one per *virtual widget tree* — which is one per `AppWindow` (and, since an `AppPanel` also hosts a virtual widget tree, one per `AppPanel` too). The two `OverlayHost`s are independent; an overlay on a window's host has no relationship to overlays on an attached panel's host.

```cpp
// wtk/include/omegaWTK/UI/OverlayHost.h
namespace OmegaWTK {

enum class OverlayTier : uint8_t {
    Floating  = 1,
    Modal     = 2,
    Tooltip   = 3,
    DragGhost = 4
};

/// Where an overlay anchors itself when presented.
struct OverlayAnchor {
    enum class Mode : uint8_t {
        AtWidget,        // relative to a Widget's window-space rect + edge
        AtPoint,         // absolute window-space point (e.g. cursor for tooltips)
        CenterInWindow   // for Modal / Sheet
    };

    Mode mode = Mode::AtWidget;
    Widget * widget = nullptr;
    Composition::Point2D point {0.f, 0.f};

    enum class Edge : uint8_t { Top, Bottom, Left, Right };
    Edge edge = Edge::Bottom;
    float gap = 4.f;  // pixel gap between anchor and overlay edge
};

/// What dismisses this overlay (besides explicit OverlayHost::dismiss).
struct OverlayDismissPolicy {
    bool clickOutside       = true;  // mouseDown outside the overlay's bounds
    bool escapeKey          = true;  // global Escape, topmost overlay first
    bool windowDeactivate   = true;  // window loses key focus
    bool anchorDestroyed    = true;  // anchor Widget is removed from the tree
    bool absorbsHits        = true;  // mouse events inside go to overlay, not main tree
};

/// Opaque handle; OverlayHost can dismiss by handle or by Widget.
struct OverlayHandle {
    std::uint64_t id = 0;
    bool valid() const { return id != 0; }
};

class OMEGAWTK_EXPORT OverlayHost {
    struct Impl;
    Core::UniquePtr<Impl> impl_;
public:
    explicit OverlayHost(WidgetTreeHost & host);
    ~OverlayHost();

    /// Mount `overlay` into the overlay slot at the given tier. Computes
    /// the overlay's window-space rect from `anchor` (the overlay's own
    /// rect is treated as a desired size; the host edge-clamps it
    /// against the window). Returns a handle for later dismiss().
    OverlayHandle present(WidgetPtr overlay,
                          OverlayTier tier,
                          const OverlayAnchor & anchor,
                          const OverlayDismissPolicy & policy = {});

    /// Hide + unwire the overlay. Safe on a stale handle (no-op).
    void dismiss(OverlayHandle handle);
    void dismiss(Widget * overlay);      // by widget pointer
    void dismissAll(OverlayTier tier);
    void dismissAll();

    /// True iff at least one overlay in `tier` is currently presented.
    bool isPresenting(OverlayTier tier) const;

    /// Iterate (topmost first) for hit-testing and Escape dispatch.
    OmegaCommon::ArrayRef<Widget *> overlaysTopFirst() const;
};

}
```

`WidgetTreeHost::overlayHost()` returns a reference; widgets access it as `treeHost->overlayHost().present(...)`.

---

## 4. Paint-walk integration — how overlays land above `WidgetSlice`s

The current `FrameBuilder::buildFrame` walks `paintSubtree(root, pc)` once from the root, in pre-order, with each `View::paint(pc)` appending DrawOps to the same `DisplayList`. The end result is one window-wide `DisplayList` carried in a single `CompositeFrame::WidgetSlice` (today's slice list is effectively length-1; the type allows multiple but no producer uses that yet — see [CompositeFrame.h](../include/omegaWTK/Composition/CompositeFrame.h)).

This plan extends `buildFrame` to walk **two roots** in sequence:

```
buildFrame(root):
    style + layout passes — main tree only (overlay tree is laid out lazily on present)
    paint pass:
        paintSubtree(mainRoot, pc)             // emits the main DrawOps into dl
        for tier in [Floating, Modal, Tooltip, DragGhost]:
            for overlayWidget in overlayHost.overlaysIn(tier):       // FIFO within tier
                paintSubtree(overlayWidget.view, pc)                  // appends after the main DrawOps
```

Because the paint pass produces a single DrawOp stream in append order, overlay DrawOps land *after* every main-tree DrawOp. The compositor backend renders them last → they sit on top. No new `WidgetSlice` machinery is required for the base implementation; overlays ride the same slice the main tree produces.

> **Clipping.** Each `View::paint` already opens its own clip via `DrawOp::PushClip` (see `UIView::paint`). Overlays inherit this — a `Popover` with `rect = {200, 200, 150, 80}` clips its content to that rect. The "covers/clips content below" semantic falls out naturally: the overlay's opaque background DrawOp is emitted after the underlying main-tree DrawOps, and the backend draws in submission order. Transparent regions of the overlay show what's underneath (a `Popover` with a drop shadow and rounded corners shows the main tree's pixels in the shadow's gradient).

### 4.1 Why one slice and not one slice per overlay

The `CompositeFrame::WidgetSlice` type permits multiple slices with per-slice `targetLayer` pointers — useful when a future enhancement wants overlays composited into a different render layer (e.g. an OS-level transparency layer for tooltips). The base implementation does not need this: one DrawOp stream is simpler and the backend already handles ordering. When per-tier slices become useful (a Modal that wants OS-level blur behind its backdrop, for example), the `paintSubtree` call inside the tier loop becomes its own `WidgetSlice` with a distinct `targetLayer`. The slice-vector API is already in place — only the producer side changes.

### 4.2 Layout pass — overlays bypass the main-tree layout walker

Overlays are not part of `WidgetTreeHost`'s main `LayoutManager` walk. Each overlay computes its own window-space rect at `present` time from its `OverlayAnchor`, then becomes static for its lifetime. Reasons:

1. Overlays are short-lived and don't participate in flex/stack layout — running them through the main layout pass would force the manager to special-case "skip overlay subtree."
2. The anchor → rect math (edge + gap + edge-clamping against window bounds) is one-shot. If the anchor Widget moves, the overlay either dismisses (tooltips, dropdowns — almost always the right behavior) or follows (popovers — explicit opt-in via `OverlayDismissPolicy::anchorDestroyed = false` plus a `followAnchor` flag landed in a follow-up).
3. The main layout walker already knows how to early-return on a clean subtree; not visiting overlays at all is even cheaper.

If a specific overlay genuinely needs a `LayoutManager` (a `Popover` containing a `StackWidget` that flexes), the overlay's *children* still go through the normal layout machinery — the manager runs as part of `paintSubtree`'s pre-order descent the same way it does for the main tree. Only the overlay's *root rect* is anchor-computed.

### 4.3 Ornamentation — drop-shadow is the only baseline the host provides

The `OverlayHost` deliberately provides **one** piece of visual chrome on every overlay it presents: a soft drop shadow on the overlay's root view, applied via `Style::dropShadow` at present time. Everything else — the overlay's background fill, border, padding, corner radius, text styling — is the responsibility of the overlay widget itself, exactly the way every other Widget in the catalog is styled by its API user.

```cpp
struct OverlayOrnamentation {
    bool dropShadow = true;                                 // baseline; opt-out per present()
    Composition::LayerEffect::DropShadowParams shadowParams {
        /* offset */ {0.f, 2.f},
        /* radius */ 4.f,
        /* opacity */ 0.25f,
        /* color */ {0.f, 0.f, 0.f, 1.f}
    };
};
```

The shadow parameters track the macOS / GTK convention by default; per-overlay overrides come through `OverlayHost::present`:

```cpp
OverlayHandle present(WidgetPtr overlay,
                      OverlayTier tier,
                      const OverlayAnchor & anchor,
                      const OverlayDismissPolicy & policy = {},
                      const OverlayOrnamentation & ornament = {});
```

Rationale — three points:

1. **Tier-by-tier defaults would over-specify.** Every overlay tier wants the same shadow shape by default (the only "no shadow" cases are some snackbars and the drag-ghost, both of which set `dropShadow = false` explicitly). A single host-level default with opt-out is simpler than per-tier defaults.
2. **Content style belongs to the overlay widget, not the host.** A `Popover` is a `Container` with a `RoundedRectangle` background plus arbitrary children. The user already styles that `RoundedRectangle` (fill, border, corner radius) the same way they style any other Widget. The host adding chrome on top would either fight the user's style or force a parallel `OverlayStyle` API duplicating `Style`. Neither is worth it.
3. **Placement is per-Widget-type.** A `Popover` knows about its `PopoverEdge`. A `ContextMenu` knows about screen-edge clamping. A `Modal` centers itself in the window. A `Snackbar` anchors to a corner. The `OverlayAnchor` passed to `present` is the *placement contract* the widget's `present()` method already computes — the host just consumes the anchor and applies it. No additional placement API on `OverlayHost`.

Tooltips are the **only** exception. `Widget::setTooltip` is the public API for them, and the tooltip Widget is dispatcher-constructed (the API user provides a string, not a Widget tree), so the dispatcher needs explicit knobs for placement and chrome style. Those live on `TooltipDesc` — see [Native-API-Completion-Proposal §2.3a Tooltip Customization](Native-API-Completion-Proposal.md#customization--follow-up-not-v0).

---

## 5. Hit-testing and dismissal

### 5.1 Hit-test precedence

The hover / mouseDown dispatcher walks top-to-bottom:

1. `OverlayHost::overlaysTopFirst()` — in reverse tier order, then reverse insertion order within tier — testing each overlay's `View::containsPoint`.
2. Main tree, only if no overlay claimed the hit (or if the topmost claimant has `absorbsHits = false`).

Tooltips and drag-ghosts set `absorbsHits = false` (they paint over but don't block hits — you can click "through" a tooltip). Floating and Modal absorb by default.

### 5.2 Click-outside dismissal

Before delivering a `mouseDown` to the main tree, the dispatcher checks: for each currently-presented overlay with `clickOutside = true`, if the hit point is **outside** the overlay's bounds, the overlay is dismissed. Topmost first. This is the standard popover / dropdown behavior on every desktop platform.

Modal overlays use `clickOutside = false` plus a full-window backdrop child Widget; clicks land on the backdrop (which is part of the Modal overlay) and are absorbed, never reaching the main tree.

### 5.3 Escape key

`KeyDown(Escape)` is intercepted by `WidgetTreeHost` before routing to `FocusManager::focusedView`. If any overlay with `escapeKey = true` is presented, the topmost is dismissed and the key is consumed. Otherwise, Escape falls through to the focused view's delegate.

### 5.4 Anchor destruction

When a Widget is removed from the main tree, `WidgetTreeHost::notifyObservers(Detach)` fires. The OverlayHost subscribes and dismisses any overlay whose anchor (`OverlayAnchor::widget`) points at the departing Widget *if* its policy has `anchorDestroyed = true` (default). This prevents popovers from outliving the button that opened them.

---

## 6. Focus integration

Overlays may want to claim focus (a `ContextMenu`'s first item should be tab-focused; a `Modal`'s primary button should be focused on present). The `FocusManager` from [Native-API-Completion-Proposal §2.3a](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) supports this:

- `OverlayHost::present` calls `focusManager.pushRestorationPoint()` *before* mounting the overlay.
- The overlay's `onMount` (or its widget's `present()` method) can `view->focus(FocusReason::Popup)` to claim focus.
- `OverlayHost::dismiss` calls `focusManager.popAndRestore()` *after* unmounting, returning focus to whoever held it before the overlay opened — with `FocusReason::Popup`, so the focus ring re-appears if the prior holder was keyboard-focused.

This is the same flow Chromium's `views::Widget` uses for menus and dialogs.

Modal overlays additionally **trap** tab traversal: while presented, `FocusManager::focusNext`/`focusPrevious` is constrained to views inside the Modal's subtree. Constraint is per-tier (`OverlayTier::Modal`) — Floating and Tooltip do not trap.

---

## 7. Relationship to `AppPanel` — they are *separate*, not coupled

`AppPanel` ([Panels-And-Window-Customization-Plan Part A](Panels-And-Window-Customization-Plan.md#part-a--detached-panels)) is a second top-level *native surface*, separate from any `AppWindow`. It exists for content that lives **outside** an AppWindow — floating tool palettes, tear-off inspectors, anchored secondary windows. It hosts its own virtual widget tree, its own compositor binding, and its own `WidgetTreeHost` (and therefore its own `OverlayHost` too, governing overlays *within that panel*).

`OverlayHost` (this plan) is the in-window stacking layer. Its overlays are clipped to the `AppWindow` (or `AppPanel`) they were presented in. There is no opt-in or fallback path from `OverlayHost` into an `AppPanel` — they solve different problems:

| Use case                                                                 | Mechanism                                                                                                                                  |
|--------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| Tooltip / popover / context menu / modal inside an AppWindow             | `OverlayHost::present(...)` on the window's WidgetTreeHost. Clipped to window bounds.                                                       |
| Tooltip / popover / context menu inside a tool-palette `AppPanel`         | `OverlayHost::present(...)` on **the panel's** WidgetTreeHost. Clipped to panel bounds.                                                     |
| A floating tool palette, tear-off inspector, anchored secondary window    | Construct an `AppPanel` directly. It is not an "overlay" — it is its own UI surface.                                                        |

**Consequences for overlay design:**

- A long `ContextMenu` near the bottom-right of an `AppWindow` *clips* against the window edge. The menu either scrolls (a future `PopupMenu` enhancement) or repositions to fit (an existing dismiss-and-re-anchor option). It does **not** spawn an `AppPanel` to escape the window.
- A `Tooltip` near the right edge of an `AppWindow` clips. The dispatcher's edge-clamping math (see `OverlayAnchor`) tucks it inside the window; if the anchor is so close to the edge that the tooltip would have zero usable width, the tooltip is suppressed for that frame.
- An app that genuinely needs window-escaping floating UI — say, a color picker that should hover next to a button while the button's window is small — uses an `AppPanel` constructed by the application, not an overlay. Application code is responsible for the panel's lifecycle, positioning, and dismissal. There is no `OverlayHost` knob that turns an overlay into a panel.

This separation keeps two cleanly-typed concepts apart. `OverlayHost` is a stacking layer; `AppPanel` is a window. Conflating them (the previous draft of this plan) made the dependency graph noisier than it needed to be, and forced overlays to carry a "where am I hosted?" flag that nothing inside the overlay's own paint or hit-test code cared about.

---

## 8. Per-`UIView` `zIndex` vs. inter-Widget tier — they coexist

`UIElementLayoutSpec::zIndex` ([UIView.h:255](../include/omegaWTK/UI/UIView.h)) sorts elements *inside a single `UIView`*. Tiers sort *Widgets across the whole window*. They operate at different scopes:

```
window
├── main tree
│   └── UIView A
│       ├── element "bg"     zIndex 0
│       ├── element "border" zIndex 1
│       └── element "label"  zIndex 2
└── OverlayHost
    └── tier Floating
        └── Popover (a Container holding child Widgets, each with its own UIView)
            ├── UIView B (with its own element zIndex order)
            └── UIView C (    "       "       "       "   )
```

No interaction. A widget chooses its tier (or none, staying in the main tree); within its `UIView`, element `zIndex` continues to sort its tags as before.

---

## 9. Phases

| Phase | Description                                                                                                                                                       | Requires (cross-plan)                                                                                       | Blocks (downstream)                                                                |
|-------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------|
| **O1** [DONE] | `OverlayHost` skeleton — `present` / `dismiss` / `overlaysTopFirst`. Overlay slot on `WidgetTreeHost`. Anchor → rect math. No focus integration, no Modal trap. | — (uses existing `WidgetTreeHost`, `View::containsPoint`, `UIView`)                                          | Widget-Stub Phase 6 Tooltip / Popover / Snackbar MVPs                              |
| **O2** [DONE] | Paint walk extension — `paintSubtree` over the overlay slot after the main tree, tier ordering, per-tier FIFO.                                                  | O1; uses existing `FrameBuilder::buildFrame` paint pass                                                       | Visible overlays of any kind                                                       |
| **O3** [DONE] | Hit-test + click-outside + Escape + anchor-destruction dismissal.                                                                                                | O1; uses existing `WidgetTreeHost` hit dispatcher                                                             | Real dropdown / context-menu behavior; Native-API §2.3a Tooltip activation         |
| **O4** | Focus restoration around present/dismiss (`pushRestorationPoint` / `popAndRestore`).                                                                              | O1 + **Native-API §2.3a Focus step 5** (`pushRestorationPoint` / `popAndRestore` on `FocusManager`)           | Widget-Stub Phase 6 `ContextMenu`, `Modal` (focus returns to opener)               |
| **O5** | Modal tab-trap (`Tab`/`Backtab` constrained to the Modal subtree while presented).                                                                                | O4 + **Native-API §2.3a Focus step 4** (`FocusManager::focusNext` / `focusPrevious`)                          | Widget-Stub Phase 6 `Modal` (keyboard cannot escape modal)                         |

O1–O3 land independently of FocusManager and unblock most of Phase 6. O4–O5 land alongside FocusManager (§2.3a). The two dependency chains (O1–O3 and O4–O5) are independent of each other.

`AppPanel` is intentionally absent from this dependency chain — see §7.

### 9.1 O1 implementation notes (2026-06-05)

**Files shipped:**

- `wtk/include/omegaWTK/UI/OverlayHost.h` — public surface (`OverlayHost`, `OverlayTier`, `OverlayAnchor`, `OverlayDismissPolicy`, `OverlayOrnamentation`, `OverlayHandle`).
- `wtk/src/UI/OverlayHost.cpp` — pimpl skeleton with per-tier `std::vector<Entry>` storage, anchor → window-space math, edge-clamping, handle bookkeeping.
- `wtk/src/UI/WidgetTreeHost.h` + `WidgetTreeHost.cpp` — `Core::UniquePtr<OverlayHost> overlayHost_` member, public `overlayHost()` accessor (mut + const), forward decl, `friend class OverlayHost` so the impl reads `ownerWindow_` / `root` for `windowBounds()`.

**Surface beyond the §3 spec.** The plan §3 listed `present`/`dismiss`/`overlaysTopFirst`. O1 also ships:

- `overlaysIn(tier)` — bottom-up FIFO per tier; what §4's `paintSubtree` pseudo-code references for O2.
- `rectFor(handle)` / `tierFor(handle)` — O2's paint walker needs the resolved rect for positioning the overlay subtree; tier is what O3's Escape dispatcher routes against.
- `relayoutAll()` — public entry point for recomputing every entry's anchor → rect on demand. O1 does **not** auto-call it from `WidgetTreeHost::notifyWindowResize*` because the per-overlay policy ("dismiss on resize?" vs. "follow?") is a §11 open question; the call site is left for whichever phase commits to that policy.

Surfacing these in O1 means O2/O3 plug in via the iteration accessors without changing the API.

**Decisions that diverged from defaults** (per shepherd-developer-v14 §"make the invisible visible"):

1. Friend `OverlayHost` on `WidgetTreeHost` rather than adding public `ownerWindow()`/`rootWidget()` accessors — matches the existing tight-coupling pattern (`AppWindow`, `Widget`, `NativeViewHost` are all friends).
2. Used public `Widget::viewRef()` instead of friending `Widget` for the protected `view` field — kept Widget's coupling surface unchanged.
3. Pointer-typed host backref inside `OverlayHost::Impl` instead of reference, to satisfy the project's `cppcoreguidelines-avoid-const-or-ref-data-members` default.

**Deferred** (per §9 phase split):

- Paint walk in `FrameBuilder::buildFrame` — O2.
- Click-outside / Escape / anchor-destroyed dismissal triggers — O3.
- `FocusManager` push/pop around present/dismiss — O4.
- Modal tab-trap — O5.
- Wiring `relayoutAll()` into `WidgetTreeHost::notifyWindowResizeEnd` — decided per §11 open question (the policy "dismiss vs. follow on resize" is per-overlay and not in O1's contract).

**Verification status.** §10's O1+O2 entry requires the paint walk; pure-O1 verification is the mechanical surface: clean build, `WidgetTreeHost` ctor still constructs without regression, BasicAppTest still links. Visible-overlay tests start at O2.

### 9.2 O2 implementation notes (2026-06-05)

**Files touched:**

- `wtk/include/omegaWTK/UI/OverlayHost.h` — added `bool isPresentingAny() const` accessor used by the paintDirty gate.
- `wtk/src/UI/OverlayHost.cpp` — `present()` now marks the overlay's root view dirty across `Style | Layout | Paint` (so the first `buildFrame` runs all three passes) and calls `host->requestFrame()` (the overlay is outside the main tree, so `Widget::invalidate`'s usual `requestFrame` driver doesn't fire on first present).
- `wtk/src/UI/WidgetTreeHost.cpp` — `paintDirty()` extended to (a) force-Paint the main tree when overlays exist, then (b) walk every presented overlay in tier paint order (`Floating → Modal → Tooltip → DragGhost`), FIFO within tier, calling `fb->buildFrame(*overlay->view)` per overlay.

**Mechanics — how the slices land in the right order.** Each `FrameBuilder::buildFrame` call appends a `CompositeFrame::WidgetSlice` (see `CompositorClientProxy::submitDisplayList` at `wtk/src/Composition/CompositorClient.cpp:156`). Calling `buildFrame` N+1 times in one `beginFrame/endFrame` scope deposits N+1 slices in submission order. The backend renders slices in order, so the visual stack is exactly the §2 tier order: main tree first, then Floating, Modal, Tooltip, DragGhost.

**Why the main-tree force-Paint guard.** `FrameBuilder::buildFrame` early-returns when the root's dirty mask is zero ("nothing to do — the tree is clean"). If only an overlay subtree dirties (e.g. a tooltip animation tick), the main-tree `buildFrame` would skip submission and the deposited `CompositeFrame` would carry only overlay slices — the rest of the window would blank out. The `hasOverlays → markDirty(Paint)` guard ensures every frame the main slice is part of the composite. Cost: one extra pre-order paint walk per frame on the main tree when overlays exist — same shape of work pre-O2 `paintDirty` did anyway.

**Why the per-overlay force-Paint.** Same rationale — `buildFrame(overlay->view)` would early-return if the overlay's mask were zero on a follow-up frame (e.g. the user moves the mouse in the main tree and triggers a frame; the overlay didn't change but must remain visible). Force-Paint each overlay each frame keeps every overlay in every composited frame. Style and Layout are not force-marked because they ran on the present-time dirty bits — re-marking them every frame would re-style and re-layout unchanged overlays (NativeViewHost listens to `View::onLayoutResolved`, so spurious layout runs would generate downstream noise).

**Decisions surfaced beyond the plan §4 sketch:**

1. **Force-Paint instead of region-gated re-paint.** The §4 pseudocode shows the tier loop but says nothing about how to gate it across frames. The minimum correct shape is "force-Paint every frame an overlay is present"; the minimum optimal shape is "Tier-5 region-gated paint, which knows when nothing in the overlay's subtree changed." O2 ships the former; the latter is the Tier-5 work the FrameBuilder comment already calls out.
2. **One `buildFrame` call per overlay subtree** instead of extending `FrameBuilder::buildFrame` to accept multiple roots. Keeps the FrameBuilder signature unchanged, so future overlay-side changes don't ripple. The cost is N extra ScopedPhase brackets per overlay; immeasurable next to the actual paint walk.
3. **Drop shadow deferred to a small follow-up (call it O2.1).** §4.3 specifies "applied via `Style::dropShadow` at present time" — but `Style::dropShadow` is a UIView-only API, and not every overlay is UIView-backed (a `Container`-backed `Popover` has a plain `View`). The shadow can ship two ways: (a) Force every overlay's root view to be a UIView (constrains the catalog), or (b) emit a `DrawOp(DropShadowParams, shapeRect, ...)` directly from `paintDirty` into a small one-op submission *before* each overlay's `buildFrame`. (b) is cleaner but adds FrameBuilder→OverlayHost coupling. O2 stops at the paint walk; O2.1 picks one of (a)/(b). The `OverlayOrnamentation` struct from O1 carries the params verbatim until then. **[DONE 2026-06-05 — shipped option (b); see §9.3.]**

**Deferred** (still on the dependency chain):

- O3 — hit-test + click-outside + Escape + anchor-destroyed dismissal.
- O4 — focus restoration around present/dismiss.
- O5 — Modal tab-trap.

### 9.3 O2.1 implementation notes (2026-06-05) — drop shadow

§4.3 calls for the host to apply a soft drop shadow as the one piece of visual chrome on every presented overlay. O2 deferred this (§9.2 decision 3); O2.1 ships it via option (b) — emit a `DrawOp::Shadow` directly from the paint walk, rather than constraining every overlay to be UIView-backed.

**Files touched:**

- `wtk/src/UI/FrameBuilder.h` + `FrameBuilder.cpp` — added `submitOverlayShadow(const Composition::LayerEffect::DropShadowParams &, const Composition::Rect & shapeRect, float cornerRadius)`. Allocates a one-op `DisplayList` carrying a single `DrawOp::Shadow` and appends a `PendingSubmission` with `windowOffset == {0,0}` (the shadow's `shapeRect` is already in absolute window coordinates). No-op outside of `beginFrame`/`endFrame` brackets so stray callers don't silently accumulate orphaned ops in `pending_`.
- `wtk/include/omegaWTK/UI/OverlayHost.h`:
  - Added `OverlayOrnamentation::cornerRadius` (default `0.f`) so callers can match the overlay widget's visible silhouette.
  - Added `struct PresentedOverlay { Widget * widget; Composition::Rect rect; OverlayOrnamentation ornament; };` — the paint-time view of an entry.
  - Added `OmegaCommon::ArrayRef<PresentedOverlay> overlaysForPaintIn(OverlayTier tier) const;` — bottom-up FIFO iteration that hands the widget pointer, resolved rect, and ornament in one walk so `paintDirty` doesn't have to look up the rect/ornament separately.
- `wtk/src/UI/OverlayHost.cpp` — `overlaysForPaintIn` implementation backed by a new `tierPaintScratch` array (mirrors the existing `tierScratch` for widget-only iteration).
- `wtk/src/UI/WidgetTreeHost.cpp` — `paintDirty`'s overlay walk now uses `overlaysForPaintIn(tier)` and, for each entry with `ornament.dropShadow == true`, calls `fb->submitOverlayShadow(...)` *before* the per-overlay `buildFrame`. The shadow slice appends first, the overlay content slice appends second, the backend renders shadow underneath content.

**Submission order across multiple overlays.** For N overlays in one tier the deposited slices are `[shadow1, content1, shadow2, content2, ...]`. Overlay 2's shadow can therefore land on top of overlay 1's content if they overlap. Same effect as native UIs where a later popover's shadow falls across an earlier popover — the right behavior.

**Why a separate one-op submission rather than appending to the overlay's own DL.** `FrameBuilder::buildFrame` allocates a fresh `DisplayList` inside its body and submits exactly one slice at the end. Threading an extra pre-content shadow op into that DL would require either passing the shadow params into `buildFrame` (couples FrameBuilder to OverlayHost) or splitting buildFrame into smaller pieces (intrusive). The one-op submission keeps `buildFrame` untouched at the cost of one extra slice per shadow-bearing overlay — trivially cheap given the backend's per-slice cost is ~zero compared to the actual drawing inside it.

**Decisions surfaced:**

1. **`cornerRadius` lives on `OverlayOrnamentation`, not derived from the overlay widget's style.** The host has no way to ask a `View` "what corner radius do you paint with?" — that's an internal UIView/Style detail. Callers (the future `Popover`, `Tooltip`, `ContextMenu` widgets in Phase 6 of Widget-Stub-Implementation-Plan) know their own corner radius and pass it in the `ornament` arg at present time. The plan §4.3 said host-default ornamentation; the corner radius is part of that contract.
2. **Shadow `isEllipse` is hard-coded `false`.** Every overlay tier today renders against a rectangle (rounded or not). When a future tier needs an elliptical silhouette (drag ghost showing a circular avatar?), it would set `cornerRadius = w/2` to make the rectangle render as a pill, or we extend `OverlayOrnamentation` with an `isEllipse` flag. Defer until the case shows up.
3. **No-op when `dropShadow == false`.** The `Snackbar` and `DragGhost` cases in §4.3 explicitly opt out via this flag. The plumbing is a single `if` so there's no cost to the opt-out path.

**Verification status.** Mechanical: clean build, BasicAppTest still links. The shadow appears on screen behind any presented overlay whose `ornament.dropShadow` is true (default). On-screen verification — same caveat as O2 — needs a Phase 6 overlay widget to instantiate against.

**Verification status.** O2's §10 entries that are now buildable:

- *O1+O2 paint-above-buttons*: Present a `Popover` over a `Button` row → render order has Popover slice after main → Popover visually on top. Mechanically verified by the clean build + composite-frame slice ordering; visual verification needs the user to wire a `Popover` widget (Phase 6 of Widget-Stub-Implementation-Plan, which is itself gated on this phase + O3).
- *O2 stacking within tier*: Two `Popover`s presented in sequence — `overlaysIn(Floating)` returns them in insertion order, paintDirty walks them in that order, so the second's slice lands after the first's.
- *O2 stacking across tiers*: `Popover` → `Modal` → `Tooltip` — the `kPaintOrder` constant in `paintDirty` guarantees the §2 tier order.

The mechanics are correct; on-screen verification waits for a Phase 6 overlay widget to test against. Until then, the verification is mechanical (build + slice-order assertions).

### 9.4 O3 implementation notes (2026-07-09) — hit-test + dismissal

O3 wires the four §5 dismissal paths into the existing single input dispatcher. All four are policy-driven off the `OverlayDismissPolicy` O1 already stored on each entry; O3 is the first phase that *reads* those flags.

**Files touched:**

- `wtk/include/omegaWTK/UI/OverlayHost.h` — three dispatcher-facing methods:
  - `Widget * absorbingOverlayAt(Point2D)` — §5.1 precedence. Top-first walk (reverse tier, reverse insertion); returns the first overlay that *contains* the point **and** `absorbsHits`. A containing-but-transparent overlay (Tooltip / DragGhost) is skipped so the walk falls through to lower overlays / main tree.
  - `bool dismissClickOutside(Point2D)` — §5.2. Dismisses every `clickOutside` overlay whose rect excludes the point (topmost first); returns whether anything was dismissed so the caller can consume the gesture.
  - `bool dismissTopmostForEscape()` — §5.3. Dismisses the single topmost `escapeKey` overlay; returns whether one was consumed.
  - Anchor-destruction (§5.4) needs **no** public method — it is self-contained inside `present`/dismiss.
- `wtk/src/UI/OverlayHost.cpp`:
  - Per-entry `AnchorObserver` (nested in `Impl`, subclass of `WidgetObserver`) wired onto the anchor widget at `present` time when `mode == AtWidget && anchorDestroyed`. On the anchor's `onWidgetDetach` it dismisses the guarded overlay. The detach fires from `Container::unwireChild` (`BasicWidgets.cpp:173` → `notifyObservers(Widget::Detach)`).
  - Centralized `Impl::eraseById` / `detachAnchorObserver` / `requestDismissRepaint` so every dismiss path (public `dismiss*`, the three O3 paths, and the detach callback) un-wires the observer and forces a repaint uniformly.
  - `rectContainsPoint` free helper mirroring `View::containsPoint`'s half-open edge semantics.
- `wtk/src/UI/WidgetTreeHost.h` + `WidgetTreeHost.cpp`:
  - Private `View * hitTestOverlay(Point2D)` — asks the host for the absorbing overlay, translates the window-space point into the overlay's content space, and reuses `hitTestWidget` to find the deepest hit view.
  - `dispatchInputEvent`: positional events now compute `overlayTarget = hitTestOverlay(pos)` and prefer it over `hitTest(pos)`; mouse-downs run `dismissClickOutside` and consume the gesture when the click landed on no absorbing overlay but closed one; `KeyDown(Escape)` is intercepted (alongside the existing Tab hook) before delegate dispatch.

**Decisions surfaced beyond the §5 sketch:**

1. **Re-entrancy in anchor-destroy.** `onWidgetDetach` runs *inside* the anchor's `notifyObservers` loop, so the callback dismisses via `eraseById(id, skipDetachWidget = anchorWidget)`, which erases the OverlayHost entry but skips `removeObserver` on the widget being iterated — the widget's own teardown releases the observer. All other dismiss paths pass `nullptr` and un-wire eagerly. This is why the plan's "OverlayHost subscribes" is implemented as one observer *per anchored entry*, not one shared observer (`WidgetObserver::hasAssignment` binds an observer to a single widget anyway).
2. **Dismiss forces a main-tree repaint.** Erasing an overlay marks the root view `Paint`-dirty and calls `requestFrame` (`requestDismissRepaint`). Without the root mark, once the *last* overlay is gone `FrameBuilder::buildFrame` early-returns on the clean main tree and the deposited frame still shows the stale overlay pixels — the inverse of O2's force-paint guard. This also retroactively fixes O1's `dismiss` (which requested no frame).
3. **Click-outside applies to L and R mouse-down.** `OverlayDismissPolicy::clickOutside` is documented as "mouseDown outside the overlay's bounds" with no button qualifier; a right-click away from an open popover should close it too.
4. **Escape-up is not swallowed.** Unlike Tab (whose KeyUp is unconditionally eaten), Escape must reach the focused view when *no* overlay is present (to cancel a field), so only the consumed KeyDown is intercepted. A lone Escape-up is harmless (focused views don't act on key-release of Escape).

**Verification status.** Mechanical: `OmegaWTK_UI` library builds clean and `BasicAppTest` links (bundle re-signed). The four behavioral checks in §10 (O3 click-outside / click-through tooltip / Escape) remain gated on a Phase 6 overlay widget to instantiate against — no overlay is presented anywhere in-tree yet — same caveat as O1/O2. The logic is unit-testable in isolation once a test harness presents overlays through `OverlayHost`.

**Deferred** (still on the dependency chain):

- O4 — focus restoration around present/dismiss (`pushRestorationPoint` / `popAndRestore`).
- O5 — Modal tab-trap.

---

## 10. Verification

- **O1+O2**: Present a `Popover` over a `Button` row. Verify it paints above all buttons, including a `Button` whose rect overlaps the Popover's rect (the overlap area shows the Popover, not the underlying Button).
- **O2 stacking within tier**: Present two `Popover`s in the same tier in sequence. Verify the second paints above the first.
- **O2 stacking across tiers**: Present a `Popover` (Floating), then a `Modal`, then a `Tooltip`. Verify paint order: main tree < Popover < Modal < Tooltip.
- **O3 click-outside**: Present a `Popover` over a `Button`. Click the `Button` (outside the Popover). Verify the Popover dismisses and the click **does not** activate the Button on the same gesture (the click is consumed by the dismiss).
- **O3 click-through tooltip**: Present a `Tooltip` (`absorbsHits = false`) covering a `Button`. Click the area where the Tooltip overlaps the Button. Verify the Button activates.
- **O3 Escape**: Present a `Popover`, then a `Modal`. Press Escape twice. First press dismisses the Modal; second press dismisses the Popover.
- **O4 focus restore**: Tab to a `Button`, click it to open a `ContextMenu`. Dismiss the menu. Verify focus returned to the `Button` with the focus ring visible (`FocusReason::Popup` → keyboard reason).
- **O5 Modal trap**: Tab inside a Modal. Verify Tab cycles only through views inside the Modal; never reaches main-tree widgets behind it.
- **Window-edge clipping** (no O6 — verifies the separation from `AppPanel`): Present a `Popover` whose desired rect extends past the right edge of the `AppWindow`. Verify it is clipped (or repositioned by anchor edge-clamping) to within the window — *not* moved to an `AppPanel`.

---

## 11. Open questions

1. **Animated present / dismiss.** Should `OverlayHost::present` accept an `AnimationCurve` for fade-in / slide-in? Most production toolkits do; the trade-off is two extra frames of latency on first paint. Lean toward yes, opt-in via `OverlayPresentOptions::transition`.

Yes — **scheduled as [Native-API §2.3a T2.c](Native-API-Completion-Proposal.md#implementation-phasing)** (Tooltip v2). The `OverlayPresentOptions` transition (fade / fade-slide) lands on `OverlayHost` so every tier reuses it; the tooltip is the first consumer, Popover / Modal / ContextMenu follow. Dismissal becomes non-immediate when a transition is configured (a pending-removal flag on `Impl::Entry` + completion callback runs the reverse animation before erase).

2. **Pointer events on a moving anchor.** If a Popover's anchor Widget animates its rect, should the Popover follow? Default-dismiss-on-anchor-move matches dropdown semantics; default-follow matches inspector-panel semantics. Likely per-overlay — surface a `OverlayDismissPolicy::dismissOnAnchorMove` flag.

Yes. 

3. **Z-fighting across multiple windows.** If one `AppWindow`'s Modal needs to block another `AppWindow`'s input (app-modal vs. window-modal), the in-window overlay can't express that. App-modal is a Panels-Plan concern (parent window opacity + child Modal panel), not this plan's.

Yes.

4. **Cursor shape on overlay edges.** A Sheet resize handle wants a north-south cursor; a Popover arrow does not. `View::setCursorShape` already handles this — overlays just call it on their root view at `present` time.


---

## 12. Cross-plan dependencies

This plan does not live in isolation — three other plan docs reference it, and three other plan docs gate it. Spelled out:

### 12.1 What this plan needs from other plans

| Phase here | External dep                                                                                                                                            | Why                                                                                                                |
|------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| O1–O3      | None at the plan-doc level. Uses existing `WidgetTreeHost`, `FrameBuilder` paint walk, `View::containsPoint`, `UIView` + `Style`.                       | These are the in-tree infrastructure pieces already shipped.                                                       |
| O4         | [Native-API-Completion-Proposal §2.3a Focus](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) **step 5** (`pushRestorationPoint` / `popAndRestore`). | Focus must be saved when an overlay opens and restored when it closes; FocusManager owns the stored-focus history. |
| O5         | [Native-API-Completion-Proposal §2.3a Focus](Native-API-Completion-Proposal.md#focus--virtual-focus-manager) **step 4** (`focusNext` / `focusPrevious`). | Modal tab-trap intercepts these calls and constrains them to the Modal's subtree.                                  |

**Not a dependency:** [Panels-And-Window-Customization-Plan Part A](Panels-And-Window-Customization-Plan.md#part-a--detached-panels) (`AppPanel`). Earlier drafts modeled `AppPanel` as a detached-render-mode for overlays; that coupling has been removed (see §7). `AppPanel` is a separate top-level surface for out-of-window UI, and an overlay never spawns one.

### 12.2 What other plans need from this plan

| Other plan / phase                                                                                                                                                                                  | Needs from here                                                                                          |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| [Widget-Stub-Implementation-Plan Phase 6](Widget-Stub-Implementation-Plan.md#phase-6-overlay-and-feedback-widgets) — overlay widgets (`Tooltip`, `Popover`, `ContextMenu`, `Modal`, `Snackbar`, `Sheet`) | O1–O3 (hosting + paint + dismissal); O4 (ContextMenu / Modal focus return); O5 (Modal trap). |
| [Widget-Stub-Implementation-Plan Phase 6 Select/Dropdown popup](Widget-Stub-Implementation-Plan.md#phase-6-also-wires-selectdropdown-popup)                                                          | O1–O3 (popup hosting + click-outside dismissal). Long lists clip to window or reposition via anchor edge-clamping — no AppPanel fallback. |
| [Native-API-Completion-Proposal §2.3a Tooltip](Native-API-Completion-Proposal.md#tooltip--per-widget-virtual-popup) — `Widget::setTooltip` rendering surface                                          | O1–O3 + tier `Tooltip`. Window-edge clipping handled by the dispatcher's anchor math, not by escaping the window. |

### 12.3 Dependency diagram

```
┌──────────────────────────────┐         ┌──────────────────────────────────────┐
│  Native-API-Completion-      │         │  Overlay-Z-Order-Plan (this doc)     │
│  Proposal §2.3a Focus        │ feeds   │  ─ O1: OverlayHost skeleton          │
│  (FocusManager)              ├────────►│  ─ O2: paint-walk extension          │
│                              │         │  ─ O3: hit / dismiss                 │
│  step 4 ─► O5                │         │  ─ O4: focus restore (needs Focus 5) │
│  step 5 ─► O4                │         │  ─ O5: modal trap   (needs Focus 4) │
└──────────────────────────────┘         └──────────────┬───────────────────────┘
                                                        │ feeds
                            ┌───────────────────────────┴────────────────────┐
                            ▼                                                ▼
       ┌──────────────────────────────────────┐   ┌────────────────────────────────────┐
       │  Widget-Stub-Implementation-Plan     │   │  Native-API-Completion-Proposal    │
       │  Phase 6 (overlay widgets) +         │   │  §2.3a Tooltip                     │
       │  Phase 6 Select/Dropdown popup       │   │  (renders into tier `Tooltip`)     │
       └──────────────────────────────────────┘   └────────────────────────────────────┘

      ┌──────────────────────────────────────┐
      │  Panels-And-Window-Customization-    │  (separate concept — own dependency
      │  Plan Part A (AppPanel)              │   graph; *not* a dependency of this
      └──────────────────────────────────────┘   plan, and this plan is not one of
                                                 its dependencies. See §7.)
```

No cycles. Tooltip is the one place where the dependency from §2.3a back into this plan is conceptual rather than mechanical: `Widget::setTooltip` is an API on Widget that the dispatcher implements by constructing a one-Label overlay and calling `present(..., OverlayTier::Tooltip)` — the implementation lives in `WidgetTreeHost`, not in either plan, and only the API contract crosses.
