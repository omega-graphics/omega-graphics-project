# Widget Inline-Default Strip Plan

**Status:** Open. Spawned 2026-06-04 from D7.5's deferred follow-ups.
The D7.5 landing wired the user-agent stylesheet machinery and the
auto-install path; the remaining work is migrating the in-tree widgets
off their inline visual defaults so the UA sheet rules are no longer
purely informational.

**Parent plan:** `Widget-View-Paint-Lifecycle-Plan.md` — D7.5 entry.
This plan is the implementation track for the follow-ups recorded
there.

## 1. Context — why this was deferred

D7.5's narrow scope (UA sheet builder + auto-install at AppWindow
ctor) shipped without touching widget code so the cascade machinery
could land in isolation. Per the survey done during D7.5:

- `Rectangle` / `RoundedRectangle` / `Ellipse` / `Path` / `Separator`
  / `Image` already only author their inline `Style` when the prop
  pointer is non-null. They already match the "model-state-
  dependent overrides only" contract D7.5's spec asks for —
  no inline-default strip needed.
- `Label::rebuildContent` and `Icon::rebuildContent` unconditionally
  write `textColor` from a `Color` prop default. Stripping requires
  the prop type to become an optional cell so "unset" is
  distinguishable from "set to default-constructed black."
- `Button::rebuildStyle` authors a richly state-dependent table
  across Idle / Hovered / Pressed / Focused / Disabled. With D6.4's
  pseudo-class bits already shipped, this can be expressed as
  cascade rules.

Both follow-ups are breaking API changes for their construction
sites, so they wait until there's a real driver — either a code-base-
wide sweep slot or a feature that needs the cleaner cascade
shape.

## 2. Phase L — Label / Icon strip

**Goal:** Move `Label`'s and `Icon`'s unconditional inline writes for
default text-color cells onto the UA sheet. Inline writes survive
only for cells the app explicitly authored.

### 2.1 Surface change

`LabelProps` / `IconProps` shift from concrete-typed defaults to
optional cells:

| Field                        | Old type                                 | New type                                    |
|------------------------------|------------------------------------------|---------------------------------------------|
| `LabelProps::textColor`      | `Composition::Color {0,0,0,1}` (black)   | `Core::Optional<Composition::Color>`        |
| `LabelProps::font`           | `SharedHandle<Font>` (already optional)  | unchanged                                   |
| `LabelProps::alignment`      | `TextLayoutDescriptor::LeftUpper`        | `Core::Optional<...::Alignment>`            |
| `LabelProps::wrapping`       | `TextLayoutDescriptor::WrapByWord`       | `Core::Optional<...::Wrapping>`             |
| `LabelProps::lineLimit`      | `unsigned = 0` (0 = unlimited sentinel)  | `Core::Optional<unsigned>`                  |
| `IconProps::tintColor`       | `Composition::Color {0,0,0,1}` (black)   | `Core::Optional<Composition::Color>`        |
| `IconProps::size`            | `float = 16.f`                           | leave concrete (used in layout math, not a style cell) |

`Label::rebuildContent` / `Icon::rebuildContent` author each cell
only when its `Core::Optional<>` has a value. Unset cells fall
through to the UA sheet's defaults.

### 2.2 UA sheet expansion

`StyleSheets::BuildUserAgentStyleSheet()` already seeds
`label.textColor = black` and `icon.textColor = black`. Add:

- `label.textAlignment = LeftUpper`
- `label.textWrapping = WrapByWord`
- `label.textLineLimit = 0` (unlimited)
- `icon.textAlignment = MiddleCenter` (the Button's icon sub-
  element uses this; Icon-widget default falls out naturally)

Match the current widget-default values exactly so the strip is
behavior-preserving.

### 2.3 Construction-site sweep

Every `LabelProps {...}` and `IconProps {...}` literal in the tree
that sets a non-default value needs `= Color::create8Bit(...)` →
`= Core::Optional<Color>(Color::create8Bit(...))` (or whatever the
project's `Optional` factory pattern is — likely just `{...}` works
via implicit conversion).

Construction sites that rely on default values (`LabelProps{}` with
no fields) need no change — they get `nullopt` cells which the UA
sheet fills.

Expected sweep targets (from `grep -rn "LabelProps\|IconProps"
wtk`): a handful of test apps + any app code. To enumerate at start
of the phase.

### 2.4 Verification

- `Apps/ContainerClampAnimationTest.app` — has no labels / icons, so
  no behavioral change expected.
- `Apps/TextCompositorTest.app` — text-heavy; visual A/B against
  pre-strip screenshot.
- `Apps/BasicAppTest.app` — text + UI controls; visual A/B.
- Any in-tree app that uses `Label` / `Icon` directly.

### 2.5 Estimate

~2 days incl. construction-site sweep + visual A/B on the text-
heavy tests.

## 3. Phase B — Button cascade rewrite

**Goal:** Express `Button`'s state-dependent visual table as cascade
rules driven by D6.4 pseudo-classes. `Button::rebuildStyle` shrinks
to "author only the model-state-dependent cells" (e.g. label text,
icon token, accent override).

### 3.1 Current shape (to retire)

`Button` holds an `InteractiveState` enum
(`Idle / Hovered / Pressed / Focused / Disabled`) and an
`onInteractionStateChanged` hook that runs `rebuildStyle(/*animate=*/true)`
on every transition. `rebuildStyle` switches on `state_` and writes
`bg`'s background brush, `label`'s text color, and a focus ring's
border via `Style::elementBrush(..., transition=true, duration=...)`.

### 3.2 New shape

D6.4 already maintains the per-view pseudo-class bits:
- `:hover` from cursor enter/exit.
- `:pressed` from left-mouse-down/up on the hover target.
- `:disabled` from `setEnabled(false)`.
- `:focused` — not wired today (FocusManager not yet shipped, per
  Native-API-Completion-Proposal §2.3a); the Button rewrite leaves
  the `:focused` rule in place but knows it never fires until
  FocusManager lands.

Button stops authoring per-state cells inline. Instead, the
button's owning code (or a `Button::installDefaultStyle()` helper)
publishes a sheet:

```cpp
auto sheet = StyleSheets::StyleSheet::Builder()
    // Default — Idle.
    .addRule(StyleRule{}
        .selector({"bg" /* element */})
        .setFillBrush(controlBackgroundBrush))
    .addRule(StyleRule{}
        .selector({"button", {"hover"} /* :hover */})
        .setFillBrush(controlBackgroundHoverBrush)
        .transition({PropertyKey::FillBrush,
                     {kHoverTransitionMs, AnimationCurve::EaseInOut()}}))
    .addRule(StyleRule{}
        .selector({"button", {"pressed"}})
        .setFillBrush(accentBrush))
    .addRule(StyleRule{}
        .selector({"button", {"disabled"}})
        .setFillBrush(controlBackgroundDisabledBrush))
    .build();
```

(Spelling is sketch; the real Selector struct already takes named
`pseudoClasses` per D6.4.)

Open design question: who owns the Button sheet?

- **(a)** UA sheet — install the Button rules in
  `BuildUserAgentStyleSheet()`. Theme-aware values (`accent`,
  `controlBackground`) are pulled via `Var` substitution at
  cascade time (D7.1's ThemeVars). Every Button in the process
  shares the same rule set; theme override = set new ThemeVars
  values.
- **(b)** Per-Button sheet — each Button publishes its own sheet
  at construction. Allows per-instance overrides but loses the
  "Buttons share rules" property.

Recommendation: **(a)**. The theme-vars path was built for
exactly this case (paint primitives that need theme-aware values
without hard-coding the lookup at every render site).

### 3.3 Retiring `InteractiveState` / `onInteractionStateChanged`

The state machine drove inline style writes. With the cascade
expressing the state visually, the Button's `state_` field becomes
informational only — read by hit-test paths, the click-confirmed
emission logic, etc.

Options:
- **Keep `state_` as a model field**, drop the inline-write path.
  `onInteractionStateChanged` shrinks to just emitting the
  click-confirmed signal when appropriate; the cascade picks up
  the visual change automatically because the pseudo-class bits
  were set by `WidgetTreeHost::dispatchInputEvent`.
- **Delete `state_` entirely** if no model code reads it. Reading
  `pseudoClassBits()` directly from the call sites that need
  state is equivalent.

The keep-or-delete decision is per-call-site. Plan: keep through
the refactor, delete in a follow-up if no readers survive.

### 3.4 Verification

`Apps/ContainerClampAnimationTest.app` doesn't use Button, so no
direct cover. Need a Button-bearing app to A/B against — likely
`BasicAppTest` or a small purpose-built test (Button hover /
press / disabled state sweep).

### 3.5 Estimate

~3-4 days incl. theme-vars rebinding for the accent / control
colors, the click-confirmed emission rewrite, and Button-focused
visual A/B.

## 4. Combined dependency note

Phase L and Phase B are independent — they touch different widgets
and different prop structs. They can land in either order or in
parallel. Phase B has a soft dependency on Phase L being
*conceptually* settled (the "widget authors only model-state-
dependent cells" pattern is shared), but no code dependency.

## 5. Closing note

When all of Phase L + Phase B land, this plan moves to
`wtk/.plans/done/`. Until then, the UA sheet's seed rules are a
safety net for app-authored views, not the source of truth for
the in-tree widgets.
