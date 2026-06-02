# Style / StyleSheet / Layout Refactor Plan

**Status:** Proposal. Nothing below is implemented yet.
**Scope:** Split the current `StyleSheet` (per-`UIView`, locally scoped,
imperative) into two distinct concepts: a per-node **`Style`** (the
inline / authored style on a single `UIView` or, eventually, scene
node) and a process-global **`StyleSheet`** (a CSS-like rule set that
matches by selector across the entire application). Reorganize layout
authoring so it composes cleanly with this split and with the future
WML markup layer.
**Prerequisite reading:** [UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md),
[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md),
[research/widget_markup_language_spec.md](research/widget_markup_language_spec.md),
[Layout-API-Current-Use-Evaluation.md](Layout-API-Current-Use-Evaluation.md).
**Non-goals:** Implementing WML parsing. Changing the SDF / DrawOp
contract. Changing the animation API (assumes
[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md)). Defining a
full CSS subset — this plan defines the *engine* that a CSS-like or
WML-driven authoring layer feeds into.

---

## 0. Cross-plan alignment (2026-05-31)

This plan was authored before the AnimationScheduler existed. The
adjacent animation workstream has since shipped parts of the surface
this plan depends on, under a different filename and a different
class name. The summary below tells you how the sections that follow
should read against the current tree; the body has been kept stable
for diff readability.

- **`Animation-API-Simplification-Plan.md` → [Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md).**
  The animation refactor was renamed and re-scoped. The old filename
  lives in `docs/stale/`. Every reference below to
  "Animation-API-Simplification-Plan" should be read as
  "Animation-Scheduler-Plan."
- **`Animator` → `AnimationScheduler`.** The new type is a per-window
  runtime owned by `AppWindow::Impl`, ticked once per outermost
  `FrameBuilder::beginFrame`. It landed in
  [UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)
  Phases 4.3 (scheduler stood up alongside the legacy runtime) and
  4.4 (UIView animation surface migrated onto it; legacy
  `ViewAnimator` / `LayerAnimator` now dormant). Every reference below
  to `Animator` should be read as `AnimationScheduler`.
- **The transition handoff is `scheduler.transition(...)`, not a
  generic call.** Animation-Scheduler-Plan §3.7 nails the shape:
  `scheduler.transition(nodeId, propertyKey, prevValue, newValue, spec)`
  — a `friend class` hook on `AnimationScheduler` invoked by
  `StyleResolver` during Phase 2 (Style). The scheduler **seeds the
  side table with `prevValue` immediately** so Paint reads the
  pre-transition value during the same frame the transition begins,
  and handles re-targeting (a new `to` on an already-active
  `(node, key)` smooth-retargets) the way CSS transitions do. App
  code never calls `transition(...)` directly — it declares
  `Transition` records in the global `StyleSheet`, exactly as §3.5
  below specifies. Wiring this hook is Animation-Scheduler-Plan
  **Tier D**, the hard prerequisite for this plan's Tier 3.
- **Per-window scheduler vs per-`Application` StyleSheets.** §3.8
  puts `StyleSheet`s on the `Application` (§6 Q1 resolved
  "Application"). The AnimationScheduler is per-window.
  `StyleResolver::resolve(node)` reaches the scheduler through the
  node's owning AppWindow (`AppWindow::activeFrameBuilder()->animationScheduler()`
  in the current tree). The cascade rule database stays
  Application-scoped; the transition runtime that consumes its
  `Transition` records stays per-window.
- **`NodeId` / `PropertyKey` are already built.** The scheduler keys
  its side table on `(NodeId, PropertyKey, subIndex)`.
  `View::nodeId()` (Render Phase 4.4, public, plain `std::uint64_t`)
  returns the per-`View` id; `UIView::Impl::ensureElementNodeId(tag)`
  allocates one NodeId per `(UIView, UIElementTag)` pair lazily. The
  Style plan's `Property` enum (§3.3) maps directly onto the
  scheduler's `PropertyKey`; the same `UserDefined` half of the
  enum that UIView uses for its per-element channels (Render Phase
  4.4) is available for any property `PropertyKey`'s built-in slots
  do not cover.
- **Tier 1 below is `[PARTIAL]`, not `[DONE]`.** The rename
  (`StyleSheet` → `Style`) and the layout-out-of-Style move both
  landed. The animation-out-of-Style step **did not** — and is no
  longer the right move. The `AnimationScheduler` is a separate
  imperative API (not gated by `Style`), so the OLD
  `Style::elementAnimation` / `elementPathAnimation` /
  `elementBrushAnimation` entries are not "moved" anywhere; they are
  **deleted** by [UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)
  **Phase I — Dead-code sweep** (the post-Tier-4 follow-up logged
  during the 4.4 implementation). Style's role for animation is
  exclusively the declarative `Transition` records of §3.5 / Tier 3.
- **Phase ordering already settled (Animation-Scheduler-Plan §3.8).**
  The strict per-frame order is Tick → Style → Layout → Paint →
  Commit. `scheduler.tick(now)` runs in Tick; `scheduler.transition(...)`
  is legal only in Style (asserts elsewhere); `scheduler.value<T>(...)`
  is the Paint read; public `tweenProperty` / `animate` /
  `animateProperty` are legal in Tick, Style, Layout, or outside any
  frame, and **assert on Paint and Commit** — Paint is a pure read
  and Commit is the boundary out. (App code that wants to start an
  animation from a Paint code path should `requestFrame()` and issue
  the registration from the next frame's Style or Tick.) The
  first-frame ordering concern — "Style registers a transition this
  frame, Tick already ran" — is solved by §3.7's "seed the side
  table with prevValue immediately" rule above, not by reordering.

---

## 1. Why the current `StyleSheet` is the wrong shape

Reading [UIView.h:58-210](../include/omegaWTK/UI/UIView.h) and
[UIView.Style.cpp:15-305](../src/UI/UIView.Style.cpp):

### 1.1 `StyleSheet` is per-`UIView`, not per-application

`UIView::setStyleSheet(const StyleSheetPtr&)` attaches one style sheet
to one view. Each `Entry` is keyed by `viewTag` + `elementTag`. The
sheet is opaque to every other view in the window. There is no way to
say "all `Button.primary` instances in this app use this background
color" without copying the sheet onto every Button, or without writing
a side helper.

This is the inverse of how every UI toolkit on the planet handles
styling. CSS, Slate `FCoreStyle`, Unity USS, JavaFX CSS, Qt QSS, WPF
`ResourceDictionary` — all of them have a tree-wide rule set that
matches by selector and resolves per node. OmegaWTK's `StyleSheet`
matches the *inline-style* role (the equivalent of HTML `style="…"`),
not the stylesheet role.

### 1.2 The fluent builder mixes three distinct concerns

The `StyleSheetPtr` return-self builder
(`backgroundColor(tag, …).borderColor(tag, …).layoutWidth(tag, …)`)
collapses three different kinds of authoring into one bag of `Entry`
records:

- **Visual properties** (`background`, `border`, `dropShadow`,
  `elementBrush`, `text*`).
- **Layout properties** (`layoutWidth`, `layoutHeight`,
  `layoutMargin`, `layoutPadding`, `layoutClamp`,
  `layoutTransition`).
- **Animation tracks** (`elementAnimation`, `elementPathAnimation`,
  `elementBrushAnimation`).

The only shared property is "applies to a tagged element on this
view." The resolution paths (`UIView.Style.cpp` for visuals,
`mergeLayoutRulesIntoStyle` for layout, the animation system for
tracks) treat them as different objects internally — but the authoring
surface is one struct.

### 1.3 `Entry` already has a parallel `StyleRule` form

`Layout.h:250-304` defines `StyleRule` — the same data, but with
`selectorTag`, `specificity`, and `sourceOrder` fields, plus a
`beats()` operator. `convertEntriesToRules(sheet, viewTag)` exists.
This is the half-built CSS engine inside the codebase. It only
operates on a single sheet at a time and only on entries belonging to
one view tag. The selector is just a string match; specificity and
source order are populated but the matcher is trivial.

The work to make `StyleSheet` *be* a CSS-like rule set is mostly
already done. What is missing is: a real selector model, a global
registry, and a separation of what is authored inline (on one view)
versus what is authored globally.

### 1.4 Layout authoring is split across three places

- `LayoutStyle` (`Layout.h:100-128`) — the per-element resolved layout
  struct.
- `UIElementLayoutSpec::style` (`UIView.h:243-251`) — the per-element
  authored layout, baked into `UIViewLayoutV2`.
- `StyleSheet::layoutWidth/Height/Margin/...` — the per-element
  authored layout, baked into the same sheet as visuals.

These three views overlap. `UIElementLayoutSpec.style` and the
`StyleSheet` layout entries are two ways to set the same fields.
`mergeLayoutRulesIntoStyle` merges the sheet entries into a temporary
`LayoutStyle` per element. There is no single answer to "where is the
final layout for this element computed?" — it is the result of merging
two authoring surfaces and an enclosing `UIViewLayoutV2`.

### 1.5 Nothing in the model anticipates WML

[research/widget_markup_language_spec.md](research/widget_markup_language_spec.md)
describes a tree-wide stylesheet (`<style>` block, theme files,
`@theme`, `@media`, selector-based matching, state pseudo-classes
like `:hover` / `:pressed`, custom states, theme variables). None of
those concepts have a home in the current model. WML is not the only
driver here — the same gaps would be exposed by any global theming
attempt or by a designer tool — but WML makes the gaps concrete.

### 1.6 Summary

The current type called `StyleSheet` is doing the work of an
**inline style attribute**, but it is named, scoped, and built like
something you would expect to live at app scope. The result is that
both the inline-style use case and the global-stylesheet use case are
served badly: per-view authoring is awkward (you build a builder and
attach it), and global authoring is impossible.

---

## 2. What proven systems do

### 2.1 Chromium — `ComputedStyle`, `StyleResolver`, and document stylesheets

Blink keeps three layers strictly separate:

- **Author stylesheets** — `<style>` blocks, `<link rel=stylesheet>`,
  user-agent default sheets. Live on the `Document`. Match elements by
  CSS selector. One global rule database per document.
- **Inline style** — the `style=""` attribute on an element. Owned by
  the element. Highest specificity short of `!important`.
- **`ComputedStyle`** — the resolved, immutable result of applying all
  matching rules + inline style + inheritance to one element. Cached
  per element. The render tree consumes only `ComputedStyle`.

The flow is `StyleResolver::ResolveStyle(element)` → walks the rule
database, finds matching rules in cascade order, layers inline style on
top, applies inheritance, produces a `ComputedStyle`. Cascade is
specificity → source order → `!important`. `ComputedStyle` instances
are aggressively shared between elements with identical resolved
properties.

**What we take:** the three-layer separation. Per-node "inline style,"
process-global "rule database," and a per-node "resolved style
cache" that paint reads from. Selector-based matching with specificity
+ source order is a known-good algorithm; the
`StyleRule::beats()` skeleton in `Layout.h:303` is the same comparison.

### 2.2 Unreal Slate — `FCoreStyle` / `FSlateStyleSet`

Slate has no CSS. Instead, every widget reads a *style struct* (e.g.
`FButtonStyle`, `FTextBlockStyle`) describing its own visuals. A
`FSlateStyleSet` is a named registry of these structs keyed by
`FName`. Widgets look themselves up in the active style set via a key
that is part of their identity ("PrimaryButton.Pressed"). Themes are
swapped by replacing the active style set.

Layout in Slate is structurally separate — `SBoxPanel`,
`SHorizontalBox`, `SOverlay` — and never lives in the style struct.
Animation lives in `Animations.h` as curves and lerps the widget reads
during paint, not in the style.

**What we take:** layout, animation, and style are three orthogonal
authoring surfaces. The current `StyleSheet`'s habit of carrying
`layoutWidth`, `elementAnimation`, *and* `backgroundColor` together is
the failure of orthogonality Slate explicitly avoids. Strong typing of
named "style structs" per widget kind — `ButtonStyle`, `TextStyle`,
`ShadowStyle` — instead of one giant `Entry` discriminated union is
also worth stealing.

### 2.3 Unity UI Toolkit — USS + inline style + computed style

Unity copied Chromium's three-layer model almost exactly. Author USS
files (`.uss`) load into a `StyleSheet` resource registered on a
`PanelSettings` or attached to a `UIDocument`. `VisualElement` has
inline style via `style.<property>` (mutable, JS-like), inheritance
flows down. The runtime computes `ComputedStyle` per element on
dirty propagation.

The *one* thing Unity adds: USS is restricted to a fixed property set
(no arbitrary `var()` outside theme variables, no `calc()`, no
animations beyond `transition`). This makes the resolver fast and the
property set legible. The toolkit owns the property list; the
authoring layer is constrained to it.

**What we take:** a finite, enumerated property set authored on one
side and resolved on the other. Property keys are not strings at the
engine level — they are an enum.

### 2.4 JavaFX CSS — typed values and pseudo-classes

JavaFX CSS resolves into typed values (`Color`, `Insets`, `Font`,
`Effect`) — not into strings. Selectors include pseudo-classes
(`:hover`, `:focused`, `:pressed`, `:disabled`) that are tied to node
state bits. The matcher checks pseudo-classes during cascade, not as
a pre-filter, so a node with `:hover` true sees a different cascade
result than the same node with `:hover` false — without rebuilding the
rule database.

**What we take:** typed property values (we already have these in
`StyleRule`); pseudo-class evaluation during resolution; state bits on
the node, not in the rule.

### 2.5 Qt QSS / WPF / Flutter — the points of agreement

Across these systems the agreements are:

| Property | Consensus |
|---|---|
| Stylesheets are tree-scoped, not node-scoped | Yes |
| Inline style is a separate authoring surface | Yes |
| Resolution produces a node-cached computed style | Yes |
| Cascade order: specificity → source order → !important | Yes |
| Pseudo-classes evaluated during resolve, not pre-filter | Yes |
| Layout is *not* part of the visual style system | Mostly yes (CSS conflates them; native toolkits separate) |
| Animation is *not* part of the visual style system | Yes (CSS `transition` is the lone exception, and it's a very thin shim) |
| Theme variables exist at the document/app level | Yes |

The current `StyleSheet` violates rows 1, 6, and 7.

---

## 3. Proposed architecture

### 3.1 Three orthogonal authoring surfaces, one resolver

```
┌─────────────────────────────────────────────────────────────┐
│  AUTHORED                                                    │
│  ┌──────────────┐   ┌──────────────┐   ┌────────────────┐   │
│  │  StyleSheet  │   │   Style      │   │   Layout       │   │
│  │  (global,    │   │   (per-node, │   │   (per-node,   │   │
│  │   selector-  │   │    inline)   │   │    structural) │   │
│  │   matched)   │   │              │   │                │   │
│  └──────┬───────┘   └──────┬───────┘   └────────┬───────┘   │
│         │                  │                    │           │
└─────────┼──────────────────┼────────────────────┼───────────┘
          │                  │                    │
          ▼                  ▼                    ▼
       ┌─────────────────────────────┐    ┌──────────────────┐
       │   StyleResolver             │    │  LayoutEngine    │
       │   - rule cascade            │    │  - measure       │
       │   - pseudo-classes          │    │  - arrange       │
       │   - theme vars              │    │                  │
       └────────────┬────────────────┘    └────────┬─────────┘
                    │                              │
                    ▼                              ▼
              ┌───────────────┐             ┌─────────────────┐
              │ ComputedStyle │             │ ResolvedRect    │
              │  (per node,   │             │  (per node,     │
              │   cached)     │             │   per frame)    │
              └───────┬───────┘             └────────┬────────┘
                      │                              │
                      └──────────────┬───────────────┘
                                     ▼
                          ┌─────────────────────┐
                          │   Paint phase       │
                          │   (DisplayList ops) │
                          └─────────────────────┘
```

The three boxes at the top are the **authoring surfaces**. The
resolvers below are the **engine**. The compute caches at the bottom
are the **paint inputs**.

The names map to the renames this plan proposes:

| Today | Tomorrow |
|---|---|
| `StyleSheet` (per-`UIView`, fluent builder) | `Style` (per-node, inline) |
| *(does not exist)* | `StyleSheet` (process-global, selector-matched) |
| `LayoutStyle` (per-element layout struct) | `Layout` (per-node layout struct, unchanged in shape) |
| `UIElementLayoutSpec.style` | `Layout` on the element node |
| `convertEntriesToRules` (private helper) | `StyleResolver::resolve(node)` (public) |
| `mergeLayoutRulesIntoStyle` | `StyleResolver::resolveLayout(node)` |
| *(implicit, in `UIView::update()`)* | `ComputedStyle` (cached on each node) |

### 3.2 `Style` — the per-node inline authoring surface

`Style` is what `StyleSheet` is today, minus the layout, minus the
animation, minus the global ambitions. It is the equivalent of
`element.style.*` in the DOM.

```cpp
struct Style {
    // visual
    Optional<Color>        backgroundColor;
    Optional<bool>         borderEnabled;
    Optional<Color>        borderColor;
    Optional<float>        borderWidth;
    Optional<DropShadow>   dropShadow;
    Optional<GaussianBlur> gaussianBlur;
    Optional<Directional>  directionalBlur;
    Optional<BrushPtr>     fillBrush;       // gradient, image, etc.

    // text
    Optional<FontPtr>      textFont;
    Optional<Color>        textColor;
    Optional<Alignment>    textAlignment;
    Optional<Wrapping>     textWrapping;
    Optional<unsigned>     textLineLimit;

    // (no layout here — see §3.4)
    // (no animation here — see §3.5)

    Style & background(Color);
    Style & border(Color, float width);
    Style & shadow(DropShadow);
    // ...etc, fluent only as a convenience, not as the canonical form.
};
```

Every property is `Optional<>`. `nullopt` means "inherit / fall back to
StyleSheet / fall back to default." This is the inline-style cascade
slot in CSS.

`Style` is owned by the node: `UIView::style()` returns a mutable
reference. There is no `Style::Create()` builder factory. The
fluent helpers exist only for ergonomics; `view.style().background = …`
is equally valid.

### 3.3 `StyleSheet` — the process-global rule set

`StyleSheet` becomes what its name has always implied: a database of
selector-matched rules that applies across the application.

```cpp
struct Selector {
    // Atoms ANDed together. Multiple atoms = compound selector.
    // (Combinators — descendant ` `, child `>`, sibling `+`, `~` —
    //  are a Tier-2 add-on; the Tier-1 selector is a single compound.)
    Optional<String>           tag;          // matches UIViewTag
    Optional<String>           kindName;     // "Button", "Label"
    OmegaCommon::Vector<String> classes;     // ".primary" ".large"
    Optional<String>           id;           // "#submit"
    OmegaCommon::Vector<PseudoClass> pseudo; // :hover :pressed :focused :disabled
    OmegaCommon::Vector<StateName>   states; // :state(downloading)
};

struct StyleRule {
    Selector selector;
    int      specificity = 0;     // computed from selector
    size_t   sourceOrder = 0;     // assigned at insert time
    bool     important   = false;

    Property property;
    StyleValue value;             // typed (Color | Length | Brush | …)
};

class StyleSheet {
public:
    static StyleSheetPtr Create();

    // Rule-set-style authoring (programmatic).
    StyleSheet & rule(Selector, Property, StyleValue);
    StyleSheet & rules(Selector, std::initializer_list<RuleKV>);

    // Theme variables.
    StyleSheet & var(String name, StyleValue);

    // Loading from external sources (Tier-3 / WML).
    static StyleSheetPtr LoadFromWML(StringView source);
    static StyleSheetPtr LoadFromFile(StringView path);

    // Composition: stylesheets stack like CSS @import.
    void   append(const StyleSheet & other);
    size_t ruleCount() const;
};
```

There is one process-global stylesheet **stack** — a vector of
`StyleSheetPtr` registered on the `Application` (or `AppWindow`, at the
implementer's discretion — see §6 question 1). Stacking gives us:

- a default "user agent" sheet at the bottom (sets `Button` defaults,
  etc.);
- one or more author sheets on top;
- an optional theme sheet at the very top (overrides accent colors).

`Application::styleSheets()` is the registration point. Mutating the
stack invalidates `DirtyBit::Style` on the root node.

### 3.4 `Layout` is removed from `Style` and `StyleSheet`

Layout properties (`width`, `height`, `margin`, `padding`, `clamp`,
`flex*`, `inset*`, `gap`) are **not** authored through `Style` or
`StyleSheet` in the engine model. They live on the `Layout` struct
attached to each scene node.

This is the Slate split, not the CSS one. CSS conflates layout and
visuals; Slate keeps them separate. For a native toolkit aiming at
mobile + desktop + game-engine integration, the Slate split is the
right one — it keeps the layout engine decoupled from the style
resolver, lets the layout engine evolve independently (e.g. swap to
Yoga or Taffy without touching style), and avoids the nested
specificity-vs-cascade question for layout properties.

The WML authoring layer is free to **expose** layout-as-CSS. The
compiler translates `width: 320px` in a WML `<style>` block into
`node.layout().width = LayoutLength::Px(320)` at compile time, not
into a `StyleRule`. Same for `padding`, `gap`, `flex-direction`. The
engine never sees these as cascaded rules.

The authoring surface for layout becomes:

```cpp
class SceneNode {           // or UIView, in the bridge phase
    Layout layout_;         // per-node, structural
    Style  style_;          // per-node, inline visual
    ComputedStyle resolved_;
public:
    Layout & layout();              // mutable — sets DirtyBit::Layout
    Style  & style();               // mutable — sets DirtyBit::Style|Paint
    const ComputedStyle & resolved() const;
};
```

`Layout` is the existing `LayoutStyle` renamed for symmetry. The
`UIElementLayoutSpec.style` field becomes
`UIElementLayoutSpec.layout` to remove the lexical confusion with
visual style. (In the interim, both names coexist; see §5.)

### 3.5 Animation is removed from `Style` and `StyleSheet`

> See §0 — the verbs in this section have shifted. `elementAnimation`,
> `elementPathAnimation`, `elementBrushAnimation` and the per-`Entry`
> `transition` / `duration` flags are **deleted** (Render Plan Phase I,
> follow-up) rather than "moved to the Animator," because the
> `AnimationScheduler` is a separate imperative API not gated by Style.
> What stays in Style is the **declarative transition record** below.

The imperative animation surface is the per-window
`AnimationScheduler` from
[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md) (built in
Render Phases 4.3 / 4.4); app code targets it directly via
`scheduler.tweenProperty<T>(nodeId, propertyKey, from, to, timing, curve)`
or `scheduler.animatePropertyAt<T>(nodeId, propertyKey, subIndex, track, timing)`.
Style sheets carry **transitions** only — declarative
"interpolate this property over N ms when it changes" rules — not
imperative animation tracks.

```cpp
struct Transition {
    Property property;
    float    durationSec = 0.f;
    SharedHandle<AnimationCurve> curve;
};

// In StyleSheet:
stylesheet->rule(Selector::Class("primary"),
                 Property::Transition,
                 StyleValue::Transitions({
                     {Property::BackgroundColor, 0.16f, easeInOut},
                     {Property::Transform,       0.12f, easeOut},
                 }));
```

When the resolver detects that a node's `ComputedStyle` differs from
the previous frame's `ComputedStyle` for a transitioned property, it
invokes the `friend class` hook on the per-window scheduler:
`scheduler.transition(nodeId, propertyKey, prevValue, newValue, spec)`
(Animation-Scheduler-Plan §3.7). The scheduler seeds the side table
with `prevValue` immediately so Paint reads the pre-transition value
during the same frame the transition begins, then interpolates each
subsequent Tick. If a transition is already active for the same
`(node, key)`, the new `to` smooth-retargets in place — the standard
CSS retargeting behaviour. Paint reads `scheduler.value<T>(nodeId, key)`.

This is exactly the CSS `transition` model. Wiring the friend hook is
Animation-Scheduler-Plan **Tier D** — the hard prerequisite for Style
plan Tier 3 (§5).

### 3.6 `ComputedStyle` — the per-node resolved cache

```cpp
struct ComputedStyle {
    // resolved (no Optional<>): every field has a concrete value.
    Color          backgroundColor   = Color::Transparent;
    bool           borderEnabled     = false;
    Color          borderColor       = Color::Transparent;
    float          borderWidth       = 0.f;
    Optional<DropShadow>   dropShadow;
    Optional<GaussianBlur> gaussianBlur;
    BrushPtr       fillBrush;
    FontPtr        textFont;
    Color          textColor         = Color::Black;
    Alignment      textAlignment     = Alignment::Start;
    Wrapping       textWrapping      = Wrapping::Word;
    unsigned       textLineLimit     = 0;
    // …
};
```

The paint phase reads only `ComputedStyle`. It never walks
`StyleSheet` rules and never inspects `Style`. This is the Chromium
move and the reason their paint walk is fast: cascade is solved before
paint runs.

The resolver runs in Phase 2 (Style) of the
[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md):

```cpp
void StyleResolver::resolve(SceneNode & node) {
    if (!(node.dirty() & DirtyBit::Style)) return;

    ComputedStyle out = ComputedStyle::Default();

    // 1. Inherit from parent for inheritable properties (textColor, font, …).
    if (auto * p = node.parent())
        inheritFrom(out, p->resolved());

    // 2. Cascade matching rules from the global StyleSheet stack.
    for (auto & sheet : application().styleSheets()) {
        for (auto & rule : sheet->matchingRules(node)) {
            apply(out, rule);   // honors specificity + source order
        }
    }

    // 3. Layer inline Style on top. Inline beats any non-!important rule.
    apply(out, node.style());

    // 4. Resolve theme var() references.
    resolveVars(out, application().themeVars());

    node.setResolved(std::move(out));
}
```

`matchingRules(node)` is a hash-bucketed lookup keyed by
tag/kind/class/id, with pseudo-class evaluation as a final filter
(JavaFX-style). We do not need a full Bloom-filter selector
optimizer for Tier-1; a flat scan over the rule list is acceptable
until profiling says otherwise. This is the kind of decision the
developer's intuition about "this codebase, this scale" should
override — start naive, measure, optimize.

**Frame ordering note.** The strict per-frame order is Tick → Style →
Layout → Paint → Commit (Animation-Scheduler-Plan §3.8). The scheduler
ticks *before* `StyleResolver::resolve(node)` runs, so a transition
the resolver fires this frame would otherwise have no sampled value
when Paint reads the side table later in the same frame. §3.7 of the
animation plan closes this by having `scheduler.transition(...)` seed
the side table with `prevValue` synchronously at registration — Paint
sees the pre-transition value during the registering frame, the next
frame's Tick advances. No reordering required.

### 3.7 Pseudo-classes and state

```cpp
enum class PseudoClass : uint8_t {
    Hover, Pressed, Focused, Disabled, Checked, Selected
};
```

These are bits on the node, set by the input layer. Custom states
(`:state(downloading)` from the WML spec) are arbitrary string flags
on the node, also bits/strings.

`DirtyBit::Style` is set when a pseudo-class flips, but only for the
node whose flip occurred. The resolver then re-runs *only* for that
node. Children inherit, so children of a node whose pseudo-class
changed and which has inheritable property changes also dirty — but
this is the resolver's call, not the input layer's.

### 3.8 Theme variables

Theme variables live on the `Application`:

```cpp
class Application {
public:
    void              setTheme(ThemePtr);
    ThemePtr          theme() const;
    OmegaCommon::Vector<StyleSheetPtr> & styleSheets();
    const ThemeVars & themeVars() const;
};
```

A `ThemeVars` is a `Map<String, StyleValue>`. Style values can
reference variables: `StyleValue::Var("accent")` resolves at cascade
time by looking up `accent` in `application().themeVars()`. Themes
swap by replacing the active `ThemeVars` and dirtying the root with
`DirtyBit::Style`.

This matches WML's `:theme(dark)` model and CSS custom properties.

### 3.9 What `Style` cannot do that `StyleSheet` can

| Capability | `Style` (inline) | `StyleSheet` (global) |
|---|---|---|
| Set a property on one node | ✅ | ✅ via selector |
| Set a property on every node matching a kind/class | ❌ | ✅ |
| Pseudo-class rules (`:hover`) | ❌ | ✅ |
| Theme variable references | ✅ via resolver | ✅ |
| Transitions | ❌ (declared once) | ✅ |
| Be loaded from WML / file | ❌ | ✅ |
| Be swapped at runtime for theming | partially (per node) | ✅ |
| Highest cascade priority | ✅ (beats non-!important rules) | ❌ unless `!important` |

This is the inline-vs-stylesheet split from the DOM, with no
surprises.

---

## 4. Selector model — Tier 1 vs Tier 2

### 4.1 Tier 1 — single compound selector

```
Selector := [Kind] [#Id] [.Class …] [:Pseudo …]
```

Examples: `Button`, `.primary`, `Button.primary`, `#submit`,
`Button:hover`, `Button.primary:disabled`.

This is enough to support the WML examples in
[research/widget_markup_language_spec.md](research/widget_markup_language_spec.md)
sections 1–8. It is also a clean superset of what
`convertEntriesToRules` does today (which is just `tag` matching).

Specificity, à la CSS:

```
specificity = id_count * 100 + class_or_pseudo_count * 10 + kind_count
```

`StyleRule::beats(other)` (`Layout.h:303`) becomes the canonical
cascade comparator: higher specificity wins; ties broken by
`sourceOrder`; `!important` outranks non-`!important`.

### 4.2 Tier 2 — combinators

Add when there is a real use case, not before:

```
Selector := SimpleSelector ( Combinator SimpleSelector )*
Combinator := ' ' | '>' | '+' | '~'
```

`Panel > Button` (child), `Panel Button` (descendant), `Button + Text`
(adjacent sibling), `Button ~ Text` (general sibling).

The matcher walks the ancestor chain at resolve time. For Tier 2,
adopt Bloom-filter ancestor fingerprints (Blink's
`SelectorChecker::FastReject`) only if profiling shows the linear
scan is hot.

### 4.3 What we explicitly do not implement

- Attribute selectors `[type="text"]` — defer indefinitely. WML can
  desugar `featured="true"` into a class.
- `:nth-child()` and structural pseudo-classes — defer.
- `@supports`, `@scope`, `@layer`, custom properties beyond simple
  `var()` — defer.
- The full `calc()` grammar — defer. Theme vars cover 90% of the use
  case. If we need arithmetic, add `calc()` later as a typed node.
- Animation keyframes (`@keyframes`) — these belong to the animation
  system, not the style sheet. The WML `animation: …` shorthand
  desugars to a `scheduler.animateProperty<T>(...)` /
  `animatePropertyAt<T>(...)` call against the `AnimationScheduler` at
  compile time.

These are judgment calls about scope. The principle: the engine
implements a finite, enumerated subset that covers the WML examples
and the in-tree widget kit. Anything beyond that is a follow-up
proposal.

---

## 5. Migration plan

This plan layers cleanly on the
[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md) /
[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md)
tiers. Each tier here is independently shippable.

### Tier 1 — split authoring surfaces, keep the resolver [PARTIAL]

**Ship alongside Render Redesign Tier 1 / Lifecycle Tier A.**

- Rename the existing `StyleSheet` → `Style`. The fluent builder
  becomes a per-node mutator. `UIView::setStyleSheet` becomes
  `UIView::setStyle`. The old `StyleSheetPtr` typedef stays as a
  `[[deprecated]] using StyleSheetPtr = StylePtr;` for one release.
  **[DONE]** — see `UIView::setStyle` + the `[[deprecated]]`
  `setStyleSheet` forwarder in `wtk/include/omegaWTK/UI/UIView.h`.
- Move the layout-related entries (`layoutWidth`, `layoutHeight`,
  `layoutMargin`, `layoutPadding`, `layoutClamp`,
  `layoutTransition`) out of `Style` and into the per-node `Layout`
  struct (the renamed `LayoutStyle`). `UIElementLayoutSpec.style`
  becomes `UIElementLayoutSpec.layout`. **[DONE]** — Render Tier B /
  B1; comment at `UIView.Update.cpp:240` documents the removal.
- ~~Move animation-related entries
  (`elementAnimation`, `elementPathAnimation`,
  `elementBrushAnimation`) out of `Style` and into the `Animator`,
  per the animation simplification plan.~~ **[SUPERSEDED — deferred
  to Render Plan Phase I].** The `AnimationScheduler` is a separate
  imperative API, not a destination Style entries can be "moved to";
  the OLD Style methods are orphan in the current tree (no caller in
  `UIView.Style.cpp`'s apply pass — only the scope-tracking switch in
  `collectStyleScope` mentions the entry kinds). They are **deleted**
  in [UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)
  Phase I (post-Tier-4 dead-code sweep). Style's animation role lives
  only in the declarative `Transition` records of §3.5, wired up in
  Tier 3 below.
- The internal resolver (`UIView.Style.cpp` +
  `convertEntriesToRules` + `mergeLayoutRulesIntoStyle`) is
  unchanged, but now operates on per-node `Style` only.
- `ComputedStyle` does not yet exist as a public type; resolution is
  still inline in the paint phase.

**Risk:** Low. This is a rename + categorization refactor. No new
matching, no global registry. The `[[deprecated]]` aliases keep
existing callers compiling.

**Files touched:** `UIView.h`, `UIView.Core.cpp`, `UIView.Style.cpp`,
`Layout.h`, `Layout.cpp`, every widget subclass that authors a sheet
today (~10 files in `wtk/src/Widgets/`).

### Tier 2 — add the global `StyleSheet` with Tier-1 selectors

**Ship alongside Render Redesign Tier 2 / Lifecycle Tier B.**

- Re-introduce the name `StyleSheet`, but as a *new* type — the
  selector-matched, process-global rule set.
- Implement `Selector` (Tier-1 single-compound form) and
  `StyleRule` based on the existing `Layout.h:250` skeleton.
  Reuse `StyleRule::beats()` as the cascade comparator.
- Add `Application::styleSheets()` (or `AppWindow::styleSheets()` —
  open question §6).
- Implement `StyleResolver::resolve(node)` — runs the cascade,
  layers inline `Style` on top, writes `ComputedStyle` onto the node.
  Lives in Phase 2 of the lifecycle.
- Paint phase switches from reading `Style` directly to reading
  `ComputedStyle`. This is a one-site change in `UIView.Update.cpp` /
  `UIViewNode::paint`.
- Pseudo-classes: only `:hover`, `:pressed`, `:focused`, `:disabled`
  for Tier 2. State bits live on the node; the input layer flips
  them and sets `DirtyBit::Style`.
- Transitions are declared in `StyleSheet` but not yet driven —
  the resolver records "this property changed and has a transition"
  but the `AnimationScheduler` friend hook (Animation-Scheduler-Plan
  §3.7 / Tier D) isn't called until Tier 3.

**Risk:** Medium. The selector matcher is new code; the cascade rules
are well-known but easy to get wrong on edge cases (specificity ties,
inherited properties). Heavy unit-test coverage on the resolver is
the mitigation.

**Files touched:** new `StyleSheet.{h,cpp}`, new
`StyleResolver.{h,cpp}`, `Application.h`, `AppWindow.h`,
`UIView.Style.cpp`, `UIView.Update.cpp`.

### Tier 3 — themes, transitions, custom states

**Ship alongside Render Redesign Tier 3 / Lifecycle Tier C.**

- Theme variables: `ThemeVars` on `Application`,
  `StyleValue::Var(name)` resolution at cascade time, theme swapping
  with root-level `DirtyBit::Style` propagation.
- Transitions: connect `StyleResolver` change detection to the
  per-window `AnimationScheduler` via the `friend class` hook
  `scheduler.transition(nodeId, propertyKey, prevValue, newValue, spec)`
  (Animation-Scheduler-Plan §3.7 / **Tier D** — the hard prerequisite
  for this tier). The scheduler seeds the side table with `prevValue`
  on registration and ticks each subsequent frame; Paint reads
  `scheduler.value<T>(...)`. The resolver finds the right scheduler
  through the node's owning AppWindow (`AppWindow::activeFrameBuilder()
  ->animationScheduler()` in the current tree).
- Custom states: `:state(name)` pseudo-class. Nodes carry a string
  set; setting/clearing a state dirties Style.
- Add the "user agent" default stylesheet — the OmegaWTK builtins
  for `Button`, `Label`, `Icon`, `Image`, etc. — at the bottom of the
  global stack. Widgets stop authoring inline visual `Style` for
  defaults and rely on the UA sheet.

**Risk:** Medium. Theme swap is a stress test for the resolver
(touches every node). Transitions are a stress test for the
resolver-to-scheduler handoff, especially retargeting (the
"interrupt mid-transition with a new target" case
Animation-Scheduler-Plan §3.7 calls out).

**Files touched:** `StyleSheet.cpp`, `StyleResolver.cpp`,
`AnimationScheduler.{h,cpp}` (the new `friend` hook),
`Application.cpp`, every widget subclass (to stop authoring defaults
in inline `Style`).

### Tier 4 — WML compiler front-end

**Ship after Render Redesign Tier 4 / Lifecycle Tier D.**

- Implement a WML parser that produces a tree of `SceneNode`
  declarations + a `StyleSheet` per `<style>` block.
- The parser is a separate library (`omegaWTK::WML`) that depends on
  the engine but not vice versa. The engine has no awareness of
  WML — it only consumes the `StyleSheet` and `SceneNode` outputs.
- Layout properties in WML `<style>` blocks (`width: 320px`, `gap`,
  `flex-direction`) compile to `Layout` field assignments on the
  declaring node *at instantiation*, not to `StyleRule`s. Visual
  properties compile to `StyleRule`s. This is the place where the
  CSS-conflation-of-layout-and-visuals is resolved at the
  authoring/engine boundary.
- Tier 2 selector combinators (`>`, ` `, `+`, `~`) are added if and
  only if WML examples exercise them.
- Theme files (`.wtheme`) compile to `ThemeVars`.
- Components, slots, bindings, events from the WML spec are out of
  scope for this plan — they belong to the WML compiler proposal.

**Risk:** High in absolute terms (it's a parser + new compiler), low
relative to the engine refactor (it's purely additive — the engine
doesn't depend on WML).

**Files touched:** new `wtk/wml/` subtree. No changes to the engine
beyond what Tier 3 already shipped.

---

## 6. Open questions

These are places where the developer's judgment about *this*
codebase should override anything in §3–5:

1. **`Application` vs. `AppWindow` as the stylesheet owner.** Chromium
   binds stylesheets to `Document` (one per browsing context, roughly
   one per window). Slate and Qt bind them to the application. Unity
   binds to `PanelSettings` (per UI document, roughly per window).
   For OmegaWTK the choice is between "one global stack" (simpler,
   matches Slate) and "one stack per `AppWindow`" (more flexible,
   matches Chromium). Recommendation: start with `Application`-level
   global, add `AppWindow`-level overrides in Tier 3 if a real use
   case appears. Multi-window apps with per-window theming are rare
   enough that we should not pay the API surface for them
   pre-emptively.

   ANWSER: To Application

2. **Layout-in-stylesheet for WML compatibility.** §3.4 says layout
   is *not* in `StyleSheet`. WML's `<style>` block puts `width`,
   `padding`, `flex-direction` alongside `background`. The Tier-4
   compiler resolves this by routing layout properties to per-node
   `Layout` and visual properties to `StyleRule`s. But this means
   WML cannot express "all `Button.primary` have `padding: 16px`"
   as a cascading rule — only as a per-node default applied at
   instantiation. Is that acceptable? Probably yes — Slate accepts
   this constraint and it has not bitten Slate in fifteen years —
   but it should be confirmed with whoever owns the WML proposal
   before Tier 4 lands.

3. **`!important`.** CSS has it. Slate, Qt, and JavaFX all have an
   equivalent. It is one boolean per rule. Recommendation: include it
   from Tier 2; it costs nothing and avoids the "we need to override
   the UA sheet for this one widget" debugging adventure.

4. **Inheritance scope.** Which properties inherit by default?
   CSS inherits text-related properties (`color`, `font-family`,
   `font-size`, `line-height`) and a small handful of others
   (`visibility`, `cursor`). It does *not* inherit visuals
   (`background`, `border`). Recommendation: copy the CSS list
   verbatim. Deviation here invites surprise.

5. **Specificity for nested compounds.** Should nested compound
   selectors (Tier 2) compute specificity additively across the
   chain (CSS) or only on the rightmost compound (some
   simplifications)? Recommendation: CSS-additive. It is the only
   model designers expect.

6. **Per-element vs per-node selector.** WML and CSS match on the
   element. OmegaWTK's `StyleRule` today matches on `selectorTag`
   which is the *view tag*, not the element tag. The new model
   matches on the *node*: a `SceneNode` (which corresponds to a
   widget instance). The element-within-a-`UIView` case (e.g. the
   `bg` element inside a `Rectangle`'s `UIViewLayoutV2`) becomes a
   child node, not a tagged sub-element. This is consistent with the
   Render Redesign plan's "every UIView element is a SceneNode"
   direction and removes the dual matching surface. Confirm before
   Tier 2 — it changes the migration cost for primitive widgets.

7. **What happens to `UIViewLayoutV2`?** If every element-inside-a-
   `UIView` becomes a child `SceneNode`, `UIViewLayoutV2` is no
   longer the authoring surface for elements — it becomes the
   *imperative* equivalent of WML, useful for code-driven UI. The
   render redesign plan §3.3 already says it stays as the authoring
   surface and feeds into the scene tree at build time. This plan
   inherits that decision.

---

## 7. Where the old concepts go

| Old | New | Tier |
|---|---|---|
| `StyleSheet` (per-`UIView`, fluent) | `Style` (per-node, inline) | T1 |
| `StyleSheetPtr` typedef | `[[deprecated]]` alias for `StylePtr` | T1 |
| `UIView::setStyleSheet` | `UIView::setStyle` (deprecated alias forwards) | T1 |
| `StyleSheet::Entry` | `Style` POD with `Optional<>` fields | T1 |
| `Entry::Kind` enum | Removed; field-per-property in `Style` | T1 |
| `StyleSheet::layoutWidth/Height/Margin/Padding/Clamp/Transition` | `Layout` field assignments / `StyleSheet` rules in WML | T1 |
| `StyleSheet::elementAnimation/elementPathAnimation/elementBrushAnimation` | **Deleted** — app code uses the per-window `AnimationScheduler` directly (`scheduler.tweenProperty<T>(...)` / `scheduler.animatePropertyAt<T>(...)`). Old Style methods are orphan today; Render Plan **Phase I** sweeps them. | T1 → Render Phase I |
| `StyleSheet::*` `transition` boolean per call | `StyleSheet` `Transition` declarations; consumed via `scheduler.transition(...)` friend hook (Animation-Scheduler-Plan **Tier D**) | T3 |
| `convertEntriesToRules` | Internal to `Style` → `ComputedStyle` cascade | T1 |
| `mergeLayoutRulesIntoStyle` | Removed; layout authoring is direct field assignment | T1 |
| `resolveLayoutTransition` | `AnimationScheduler` consumes `Transition` records from the global `StyleSheet` via the resolver friend hook (Animation-Scheduler-Plan Tier D) | T3 |
| `UIElementLayoutSpec.style` (a `LayoutStyle`) | `UIElementLayoutSpec.layout` (renamed) | T1 |
| `LayoutStyle` | `Layout` (renamed) | T1 |
| *(does not exist)* | `StyleSheet` (selector-matched, global) | T2 |
| *(does not exist)* | `StyleResolver` | T2 |
| *(does not exist)* | `ComputedStyle` (per-node cache) | T2 |
| *(does not exist)* | `ThemeVars` | T3 |
| *(does not exist)* | `Application::styleSheets()` | T2 |

---

## 8. Relationship to existing plans

- **[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md)** — `ComputedStyle` is the
  per-node cache that plan's Phase 2 (Style) writes and Phase 4
  (Paint) reads. The plan already defines a `ResolvedStyleCache`
  field on `SceneNode` (§3.6); this plan names it `ComputedStyle`
  and specifies the cascade rules that populate it. Tier 1 here
  aligns with Tier 1 there; Tier 2 here aligns with Tier 2 there;
  etc. Two specific touchpoints with later tiers of the render plan:
  (a) **Phase 4.4** built `View::nodeId()` + the per-element NodeId
  allocator on UIView that this plan's resolver feeds into the
  scheduler — no re-specification needed here; (b) **Phase I —
  Dead-code sweep** owns the deletion of the orphan
  `Style::elementAnimation`/`elementPathAnimation`/`elementBrushAnimation`
  builders + the `ElementAnimationKey` enum + the `Entry::Kind::*Animation`
  values, replacing this plan's Tier 1 "move animation entries out of
  Style" line item.
- **[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md)** — Phase 2 (Style) is
  where `StyleResolver::resolve(node)` runs. The phase guard from
  that plan is what enforces "style cannot be authored during
  paint." This plan's `Style` mutators set
  `DirtyBit::Style | DirtyBit::Paint`; `StyleSheet` mutations set
  `DirtyBit::Style` on the root.
- **[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md)** —
  Tier 3 transitions hand off to the per-window `AnimationScheduler`
  via the `friend class` hook `scheduler.transition(nodeId,
  propertyKey, prevValue, newValue, spec)` specified in §3.7 of that
  plan. Animation-Scheduler-Plan **Tier D** is the hard prerequisite
  for this plan's Tier 3 — Tiers A/B/C are already done (Render
  Phases 4.3 / 4.4) so the scheduler runtime, the side table, the
  per-window tick, the NodeId scheme, and the imperative public API
  all exist; only the resolver friend hook and the `Transition`
  record consumer are missing. Per-frame phase ordering for the
  resolver/scheduler interaction is fixed by §3.8 of the animation
  plan (Tick before Style; `transition` legal only in Style; the
  scheduler synchronously seeds the side table on registration so
  Paint reads the pre-transition value during the registering frame).
- **`Layout-API-Current-Use-Evaluation.md`** — describes the
  current per-element layout surface. This plan keeps that surface
  intact (only renames `LayoutStyle` → `Layout` and
  `UIElementLayoutSpec.style` → `.layout`), but moves the
  *cascading* of layout properties out of `StyleSheet` and onto
  per-node direct assignment.
- **`research/widget_markup_language_spec.md`** — the WML proposal.
  Tier 4 is the compiler front-end. This plan defines the engine
  surface that compiler emits into.
- **`Direct-To-Drawable-And-SDF-Plan.md`** — unaffected. The
  resolver writes `ComputedStyle`; paint reads `ComputedStyle` and
  emits `DrawOp`s. Whether the SDF backend or the triangulator
  consumes the ops is invisible to the style system.
- **[Animation-Surface-Expansion-Plan.md](Animation-Surface-Expansion-Plan.md)** —
  Sequenced AFTER this plan's Tier 3 (and the Render plan's Tier 4
  + Phase I). Style owns the *declarative* transition surface
  (cascaded `Transition` records fire automatically when
  `ComputedStyle` changes); the expansion plan owns the
  *imperative* surface (app code starting an animation explicitly
  via `view->animate().property(...)....start()`). The two
  complement each other — both target the same per-window
  `AnimationScheduler`, share its side table, and observe the same
  re-targeting rule (Anim §3.7). A property that is both
  Style-transitioned and imperatively animated by app code follows
  the scheduler's "most recent registration wins, sampling resumes
  from the current value" rule.

---

## 9. What gets deleted

At the end of Tier 4:

- `StyleSheet::Entry` (50+ lines) — replaced by `Style` POD
- `Entry::Kind` enum (~25 variants) — removed
- The fluent builder methods on `StyleSheet`
  (`backgroundColor`, `border`, `dropShadow`, …, ~40 methods,
  ~400 lines of forwarding code in `UIView.Core.cpp`) — replaced by
  direct field assignment on `Style` (one line each)
- `convertEntriesToRules` (~80 lines) — folded into the resolver
- `mergeLayoutRulesIntoStyle` (~60 lines) — gone; layout is direct
- The "transition" boolean and "duration" float on every `Entry`
  — gone; transitions live in the global `StyleSheet`
- `UIView.Style.cpp` view-tag-scoped resolver (~300 lines) —
  replaced by a process-global resolver against the new
  `StyleSheet` stack

**Estimated deletion:** ~900 LOC of per-view sheet plumbing.
**Estimated addition:** ~600 LOC (`StyleSheet`, `Selector`,
`StyleRule`, `StyleResolver`, `ComputedStyle`, `ThemeVars`).
**Net at Tier 3:** roughly even.
**Net at Tier 4 (with WML compiler):** large net add (the compiler is
new code), but additive — the engine is smaller and clearer.

---

## 10. Honest uncertainty

I have not surveyed every in-tree caller of `setStyleSheet` or every
widget subclass that authors a `StyleSheet` in `onPaint`. Tier 1's
"deprecated alias" promise needs that survey before it can be
believed. The grep sweep is a Tier-1 pre-flight checklist item; if any
widget authors animation or layout directly through the old
`StyleSheet`, it migrates first.

I am assuming `StyleRule::beats()` (`Layout.h:303`) implements
specificity + source order correctly today. I have read it but not
exercised it against a CSS conformance suite. Tier 2 should add a
small test fixture covering the standard cascade edge cases (tied
specificity, `!important`, inheritance) before the resolver becomes
the only path.

I am assuming the `UIElementLayoutSpec` → child-`SceneNode` move (§6
question 6) is the right call. It is consistent with the render
redesign plan, but it changes how primitive widgets author their
visuals — `Rectangle` no longer authors one `bg` element inside a
`UIViewLayoutV2`; it authors one child `SceneNode` with a `Rect`
draw op. Whoever migrates the primitive widgets in the render
redesign Tier 4 should be the same person who confirms this
direction here. If `UIViewLayoutV2` survives as the authoring
surface for sub-elements, the selector model needs an extra
"element tag within a node" axis (or a synthetic child-node-per-
element bridge in the resolver), which is doable but uglier.

I have not measured the cost of running the resolver every time a
node's `:hover` flips. For a window with N nodes and M global rules,
the worst-case scan is O(M) per dirtied node. With M in the low
hundreds (typical app sheet) and N in the tens, this is cheap. With M
in the thousands or N in the hundreds, it stops being cheap. Tier 2
should ship with a profiling note attached to the resolver and a
plan to add hash bucketing (by tag/kind/class) before Tier 3 if the
profile demands it. Premature optimization here would design a Bloom
filter for a problem that does not exist yet.

I am assuming WML can be retrofitted onto this engine without
revisiting Tier 2's selector model. The WML spec uses simple compound
selectors plus `:state(name)` plus a few combinators. All of these
are in scope for Tier 2 + Tier 4. If WML grows attribute selectors
or `:nth-child()` later, Tier 4 has to extend the matcher — but the
extension is local, not architectural.

I am taking the answer to Animation-Scheduler-Plan §6 Q2 ("how does
`fillBrush` interpolate?") as load-bearing for this plan's `Transition`
on the `FillBrush` property. The agreed answer is: **solid color
brushes interpolate component-wise; gradient brushes interpolate
per-stop; bitmap brushes do not interpolate (the transition is rejected
into a `Failed` handle state).** Tier 3 of this plan inherits that
contract without re-specifying it. If that answer shifts (e.g. bitmap
brushes gain a "cross-fade two textures" path), this plan does not
need to change — the scheduler's `transition` hook would simply accept
the new variant.

The `Tick → Style` ordering edge case worth re-reading at Tier 3 time
is: what happens when an inline `Style` mutation fires during an input
event, dirties the root with `DirtyBit::Style`, *and* a transition
declared in `StyleSheet` should kick in for that property? The
sequence I expect (per Animation-Scheduler-Plan §3.8): the next
frame's Tick runs first, the Style phase then resolves, sees prev vs
new differ, calls `scheduler.transition(...)`, which synchronously
seeds `prevValue` into the side table — Paint reads the seeded value
this frame, next frame's Tick starts the actual interpolation. Worth
a focused test fixture at Tier 3 time; the seeding-on-registration
rule is what keeps the first frame visually correct.
