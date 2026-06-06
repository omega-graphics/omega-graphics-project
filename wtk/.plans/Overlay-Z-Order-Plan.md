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
| **O2** | Paint walk extension — `paintSubtree` over the overlay slot after the main tree, tier ordering, per-tier FIFO.                                                  | O1; uses existing `FrameBuilder::buildFrame` paint pass                                                       | Visible overlays of any kind                                                       |
| **O3** | Hit-test + click-outside + Escape + anchor-destruction dismissal.                                                                                                | O1; uses existing `WidgetTreeHost` hit dispatcher                                                             | Real dropdown / context-menu behavior; Native-API §2.3a Tooltip activation         |
| **O4** | Focus restoration around present/dismiss (`pushRestorationPoint` / `popAndRestore`).                                                                              | O1 + **Native-API §2.3a Focus step 5** (`pushRestorationPoint` / `popAndRestore` on `FocusManager`)           | Widget-Stub Phase 6 `ContextMenu`, `Modal` (focus returns to opener)               |
| **O5** | Modal tab-trap (`Tab`/`Backtab` constrained to the Modal subtree while presented).                                                                                | O4 + **Native-API §2.3a Focus step 4** (`FocusManager::focusNext` / `focusPrevious`)                          | Widget-Stub Phase 6 `Modal` (keyboard cannot escape modal)                         |

O1–O3 land independently of FocusManager and unblock most of Phase 6. O4–O5 land alongside FocusManager (§2.3a). The two dependency chains (O1–O3 and O4–O5) are independent of each other.

`AppPanel` is intentionally absent from this dependency chain — see §7.

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

Yes

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
