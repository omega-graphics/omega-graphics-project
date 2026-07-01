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

**Status:** Implemented 2026-07-01. See §2.6 for the as-built notes and
where reality diverged from the §2.1–2.2 sketch below.

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

### 2.6 As-built notes (2026-07-01)

Three deltas from the §2.1–2.2 sketch surfaced once the resolver was
read closely. All are recorded here because the plan is the source of
truth.

**1. The resolved-style table has NO separate alignment/wrapping
cell.** §2.2 sketches `label.textAlignment` / `label.textWrapping` as
distinct UA rules, but alignment + wrapping are fused into ONE
`PropertyKey::TextLayout` cell holding a full
`Composition::TextLayoutDescriptor` (setter `StyleRule::setTextLayout`).
The UA sheet therefore authors one `setTextLayout({alignment,
wrapping})` per tag plus a separate `setTextLineLimit`, not three
independent cells. As built:
- `label`: TextColor black, TextLayout `{LeftUpper, WrapByWord}`,
  TextLineLimit 0. (Exactly the pre-strip `LabelProps` defaults →
  behavior-preserving for a default `Label`.)
- `icon`: TextColor black, TextLayout `{MiddleCenter, None}`,
  TextLineLimit 0.

`TextLayoutDescriptor` also carries its own `lineLimit` member, but the
paint path (`UIView.Update.cpp` `ensureTextLayout`) reads the separate
`TextLineLimit` cell and injects it into the descriptor, so the
descriptor's own field is ignored — the `TextLineLimit` cell is
authoritative.

**2. The real work was making the inline text-write block presence-
aware — the sketch undersold this.** §2.1 says "unset cells fall
through to the UA sheet," but before this phase the inline write block
in `UIView::resolveStyles` (`UIView.Style.cpp`) wrote
`TextColor` / `TextLayout` / `TextLineLimit` UNCONDITIONALLY (filling
defaults for unset fields) whenever ANY inline `Style` existed, which
silently overwrote the sheet cascade that `StyleResolver::apply` had
just written. Merely making the props `Optional` would NOT have made
the sheet the source of truth — the block's default-black / default-
`{LeftUpper,None}` / default-0 writes still won. The strip therefore
required:
- `ResolvedTextStyle` gaining per-field presence flags (`hasColor`,
  `hasAlignment`, `hasWrapping`, `hasLineLimit`), set from the
  `*Specificity >= 0` signals `resolveTextStyle` already computes.
- The write block guarding each cell on its flag (same shape `TextFont`
  and `resolveElementBrush` already used). Font presence stays signalled
  by `font != nullptr`.
- The fused `TextLayout` cell merges the authored sub-field(s) OVER the
  sheet-resolved layout (via `resolvedOptional<TextLayoutDescriptor>`),
  so authoring only alignment inline leaves wrapping to fall through
  (and vice versa) instead of snapping the unauthored half to a default.

This block is on the hot path for EVERY `UIView`, not just Label/Icon —
verified behavior-preserving for `Button` (authors color + alignment +
wrapping for its label/icon inline → all written, unchanged) and for
app-authored views (unauthored text cells now fall to the sheet for
`label`/`icon` tags, else to paint's black / `{LeftUpper,None}` / 0
fallbacks — same values).

**3. `icon` alignment is an INTENTIONAL behavior change (developer-
approved).** §2.2 claimed `icon.textAlignment = MiddleCenter` was
behavior-preserving because "the Icon-widget default falls out
naturally." It does not: the Icon widget never authored alignment, so
pre-strip it inherited the resolver's `LeftUpper` default. Button's icon
sub-element authors `MiddleCenter` inline and is unaffected by the UA
sheet either way, so the UA `icon` alignment governs ONLY the Icon
widget. `MiddleCenter` (centering the glyph in its box) was chosen
deliberately over strictly-preserving `LeftUpper`. No in-tree app
instantiates the `Icon` widget directly, so there is no in-tree visual
regression surface; a future Icon-widget user gets the centered default.

**Other as-built facts:**
- `followThemeForeground` (added after this plan was written) stays an
  inline-authored color path: when set, `Label::rebuildContent` writes
  the live OS foreground inline every rebuild (it is dynamic per theme
  and cannot live in the static sheet); when unset + `textColor`
  nullopt, the color falls through to the sheet.
- **No construction-site edits needed.** `Core::Optional<T>` is
  `std::optional<T>`, so the three `LabelProps` sites
  (`BasicAppTest` ×2, `ImageRenderTest` ×1), which assign concrete
  values via the named-local idiom, implicit-convert unchanged. There
  are zero external `IconProps` sites.
- Touched: `Widgets/Primatives.h` (props → `Core::Optional`),
  `Widgets/Primatives.cpp` (`Label`/`Icon::rebuildContent` presence
  guards), `UI/UIViewImpl.h` (`ResolvedTextStyle` flags),
  `UI/UIView.Style.cpp` (presence surfacing + presence-aware write +
  fused-layout merge), `UI/StyleSheet.cpp` (UA sheet expansion).

**Verification status:** COMPLETE. `OmegaWTK` framework + `BasicAppTest`
+ `ImageRenderTest` build and link clean on the macOS Metal target, and
the user-supplied visual A/B (§2.4) confirmed both apps render correctly
(2026-07-01) — `BasicAppTest`'s theme-following title + wrapped
description and `ImageRenderTest`'s white MiddleCenter title show no
regression. Phase L is done end-to-end. (Per §4, this plan stays at the
top level until Phase B also lands or is formally closed; §3/§6 leave
Phase B optional.)

## 3. Phase B — Button cascade rewrite

> **CLOSED — won't do (2026-07-01).** After reading the current Button
> against the cascade infrastructure, Phase B was closed without
> implementation. Reasoning (developer-approved):
>
> - **Approach (a) — UA sheet + `ThemeVars`/`Var` substitution (the §3.2
>   recommendation) — does not fit the current Button.** `Button::rebuildStyle`
>   computes its visuals as a per-instance function of (theme colors,
>   per-instance overrides) with runtime color MATH: hover bg =
>   `Color::lerp(controlBackground, accent, 0.10)`, pressed label =
>   `contrastOn(accent)` (luminance pick), disabled = `withAlpha(0.4)`,
>   plus per-instance `tintOverride` / `labelColorOverride` /
>   `hoverTransitionDuration` / `pressTransitionDuration` / `cornerRadius`.
>   D7.1's `Var` substitution does concrete-value substitution ONLY (no
>   computation, chains not followed), and `ThemeVars` is process-wide on
>   `AppInst` — it cannot hold per-instance values or compute blends. So
>   (a) would require precomputing + seeding many new theme vars per theme
>   AND still falling back to per-instance handling for overrides.
> - **Approach (b) — per-Button sheet — fits but is plumbing churn.** The
>   color math would stay byte-for-byte identical, just relocated from a
>   compact `switch(state_)` into a sheet builder emitting one rule per
>   pseudo-class. The `ButtonInteractionDelegate` stays regardless (its
>   `pendingClick_` / drag-off-cancel / click-confirm logic is model, not
>   visual). Net: churn + regression surface (transition timing, focus
>   ring, disabled alpha, drag-off semantics) for ~zero functional gain.
> - **The original motivation is already gone.** Phase B was born to fix
>   Button transitions; that was fixed surgically in D7.5b (§6) — solid-
>   color brush fills now interpolate and inline transitions fire, so
>   `BasicAppTest`'s buttons already animate without touching Button.
> - The current Button (`wtk/src/Widgets/UserInputs.Button.cpp`, ~370
>   lines) is stable, self-contained, and builds/animates correctly. It is
>   stable code, not decaying code — the refactor-cost/benefit does not
>   justify the risk today.
>
> Revisit only if a concrete need appears (e.g. a design system that
> genuinely requires app-swappable Button rule sets, or a FocusManager
> landing that wants `:focused` to cascade). The sketch below is retained
> as the record of the approach that was evaluated and set aside.

> **Note (2026-06-26):** Phase B is **no longer the driver for fixing
> Button transitions** — that bug was fixed surgically; see §6 below.
> The diagnosis that motivated "move Button to the cascade so it
> animates" turned out to be only half the story: the cascade path
> would have animated the *label* color but NOT the *background*,
> because the bg is a `FillBrush` cell and brush cells were
> deliberately non-transitionable (snap). §6 wires inline-`Style`
> transitions into the firing path AND makes solid-color brush fills
> interpolate, so `BasicAppTest`'s buttons now animate without
> touching `Button` at all. Phase B remains valid as a pure
> architectural cleanup (unify widgets on the cascade / strip inline
> defaults) but is now optional and no longer transition-motivated.

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

**Plan complete (2026-07-01).** Phase L shipped end-to-end (§2.6,
build + user visual A/B confirmed); Phase B was evaluated and CLOSED as
won't-do (§3 header). Both tracks are resolved, so this doc moves to
`wtk/.plans/done/`.

For the `label` / `icon` element tags, the UA sheet's seed rules are now
the source of truth for the in-tree `Label` / `Icon` widgets — they
author only app-overridden cells inline, and unset cells fall through to
the sheet. Button was left authoring its state-dependent visuals inline
(a deliberate decision, not a gap): its per-instance, theme-computed
color table does not fit the process-wide UA-sheet / `ThemeVars` model,
and D7.5b already made its inline transitions animate.

## 6. Inline-`Style` transition firing + solid-color brush lerp (D7.5b)

**Status:** Implemented 2026-06-26. Small-feature note (well under the
~300-line ceiling — three resolver/scheduler touch points, no new
phase breakdown). This is a defect fix to the D7.2 transition
machinery in `Widget-View-Paint-Lifecycle-Plan.md` (now in
`.plans/done/`); it is recorded here because it recontextualizes
Phase B above.

### 6.1 The bug

`BasicAppTest`'s buttons snapped between Idle/Hover/Press instead of
animating, even though `Button::rebuildStyle` authors
`elementBrush("bg", …, transition=true, duration=…)` and
`textColor("label", …, transition=true, …)`. Two independent root
causes, both grounded in the source:

1. **Inline transitions never fired.** The sole call to
   `scheduler->transition<T>(…)` lives in
   `StyleResolver::applyTransitions` (`StyleResolver.cpp`), which
   early-returns when `impl.sheetBindings_.transitions` is empty —
   and that vector was populated *only* from winning **sheet rules**
   (`rule->transitions`). A widget styled purely inline (every
   in-tree widget today) recorded no transition specs, so the pass
   bailed and every cell snapped. The inline `transition`/`duration`
   flags on `Style::Entry` were read for effects (drop-shadow/blur)
   but dropped for `ElementBrush` and `TextColor`.

2. **Brush cells were non-transitionable by design.** Even via the
   sheet path, the button bg is a `FillBrush` cell
   (`SharedHandle<Composition::Brush>`). `isTransitionable_v`
   excluded brush handles ("a brush swap is a discrete change.
   Snap is the correct CSS-like behavior") and
   `KeyframeLerp<…Brush>` snapped. So the *background* fade — the
   most visible part of a button hover — could never animate without
   a change here. (`TextColor` is a `Composition::Color` cell and was
   always transitionable; it was blocked only by cause 1.)

### 6.2 The fix (three touch points)

- **`AnimationScheduler.h`** — add `KeyframeLerp<SharedHandle<Brush>>`
  (and a shared `lerpBrush` helper, also used by the existing
  `KeyframeLerp<AnimatedValue>` brush branch so sheet-driven keyframe
  brush tracks stay consistent). When both endpoints are
  `Brush::Type::Color`, interpolate the underlying `Color` and return
  a fresh `ColorBrush`; any other combination (gradient, texture,
  null) snaps on the `t>=1` boundary as before. **Limitation:** only
  *solid-color* fills fade; gradient/texture brushes still snap.
  Paint already reads the interpolated handle for free —
  `Impl::resolved<SharedHandle<Brush>>` queries the scheduler side
  table first, and `SharedPtr<Brush>` is an `AnimatedValue`
  alternative.

- **`StyleResolver.cpp`** — add `SharedHandle<Brush>` to
  `isTransitionable_v`, and a `valuesEqual<SharedHandle<Brush>>`
  specialization that compares two solid-color brushes by RGBA
  (falling back to handle identity for other kinds). The by-color
  compare is essential: widgets author a *new* `ColorBrush` handle on
  every `rebuildStyle`, so an identity compare would retrigger a
  transition every Style pass even when the color is unchanged.

- **`UIView.Style.cpp` / `UIViewImpl.h`** — surface the inline
  transition metadata for `ElementBrush` (out-param on
  `resolveElementBrush`) and `TextColor` (new `colorTransition` field
  on `ResolvedTextStyle`, mirroring the existing
  `ResolvedEffectTransition` pattern). In `resolveStyles`, after the
  FillBrush / TextColor cells are written, push a
  `ResolvedSheetBindings::TransitionRecord` (key + `durationMs` =
  `duration*1000` + curve) into `impl_->sheetBindings_.transitions`
  so `applyTransitions` (which runs right after) compares prev vs.
  current and fires. The inline records append to whatever the sheet
  cascade already recorded and share its per-Style-pass lifecycle.

### 6.3 Why this is `Button`-free

`Button::rebuildStyle` is unchanged — it already authored the
`transition=true`/`duration` flags; they were simply being ignored.
The fix makes the *framework* honor inline-authored transitions, so
every inline-styled widget (not just `Button`) animates. This is the
"keep the widget as-is, make solid-color brush fills transitionable"
direction chosen over the heavier Phase B cascade rewrite.

### 6.4 Verification

Build the Metal target on macOS; visual A/B `BasicAppTest`'s button
row (hover/press/disabled). The four buttons exercise the
150 ms-hover (`clickMe`), slow-hover (`slowHover`), instant-press
(`snapBtn`), and disabled (`disabledBtn`) configs. Per AGENTS Visual
Debugging, the screenshot is user-supplied (the `omega-debugviz`
tool is not yet trusted).
