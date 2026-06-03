# OmegaWTK Widget Markup Language (WML) — Specification

| Field | Value |
|---|---|
| Document | OmegaWTK Widget Markup Language Specification |
| Status | **Normative — Draft 1** |
| Version | 0.1.3 |
| Date | 2026-06-03 |
| Audience | Compiler implementers, engine maintainers, WML authors |

**Normative references** — implementers MUST be familiar with the
engine documents WML compiles against:

- [UIModel.rst](./UIModel.rst) — the engine model. Defines `Widget`,
  `View`, `UIView`, the five-phase frame lifecycle, and the
  per-window `AnimationScheduler`.
- [Style-StyleSheet-Refactor-Plan.md](./Style-StyleSheet-Refactor-Plan.md) —
  the three-surface authoring split (`Layout` / `Style` / `StyleSheet`)
  and selector tiers.
- [Animation-Scheduler-Plan.md](./Animation-Scheduler-Plan.md) — the
  per-window animation runtime.
- [Widget-View-Paint-Lifecycle-Plan.md](./Widget-View-Paint-Lifecycle-Plan.md) —
  the five-phase frame lifecycle.
- [UIView-Render-Redesign-Plan.md](./UIView-Render-Redesign-Plan.md) —
  `SceneNode`, `DisplayList`, `LayoutManager`.
- [NativeViewHost-Adoption-Plan.md](./NativeViewHost-Adoption-Plan.md) —
  the `<NativeViewHost>` contract.

**Informative references** — for background and rationale:

- [`research/widget_markup_language_spec.md`](./research/widget_markup_language_spec.md) —
  the proposal draft from which this spec is derived. Retained for
  history.

---

## Table of Contents

1. Introduction
2. Conformance and Document Conventions
3. Terminology
4. Engine Mapping
5. File Types and Project Structure
6. Built-In Elements
7. Declared Widget Surface
8. Styling Model
9. Themes and Variables
10. Data Binding
11. Events
12. State-Based Styling
13. Responsive Rules
14. Animation
15. Conditional Rendering
16. Widget Inheritance
17. Stylesheet Scope
18. Compiled Runtime Model *(informative)*
19. Conformance
20. Annex A — Bibliography *(informative)*
21. Annex B — Reserved Identifiers
22. Annex C — Document History *(informative)*

---

## 1. Introduction

### 1.1 Purpose

This document specifies the **OmegaWTK Widget Markup Language (WML)**:
an HTML-inspired markup with embedded CSS-like styling for authoring
OmegaWTK widgets, scene trees, and themes. It defines:

- The set of WML tags and their mapping to OmegaWTK C++ classes.
- The structure of a `<widget>` definition.
- The CSS subset that WML accepts.
- The data-binding syntax.
- The four-tier event surface and its mapping to engine delegate
  classes.
- The conformance bar a WML compiler MUST meet.

This document does NOT specify:

- The implementation of the WML compiler itself.
- The runtime behavior of the OmegaWTK engine (see the normative
  references in the front matter).
- The C++ widget classes WML targets (those are specified in the
  engine headers under `wtk/include/omegaWTK/`).

### 1.2 Scope

WML is the authoring front-end for the OmegaWTK engine. The engine —
its three authoring surfaces (`Layout` / `Style` / `StyleSheet`), its
five-phase frame lifecycle, and its per-window `AnimationScheduler` —
is authoritative. WML compiles down to those surfaces and MUST NOT
introduce parallel concepts.

A conforming WML compiler accepts WML source files (§5) and emits
C++ source code that, when compiled and linked against OmegaWTK,
constructs the widget tree, style data, and event wiring the WML
source describes. The engine has no awareness of WML; it consumes
only the OmegaWTK types listed in [UIModel.rst](./UIModel.rst).

### 1.3 What WML Is and Is Not

**WML is a look-only language that attaches to an existing C++
class.** It defines what a widget looks like, how it lays out, and
which events it surfaces — never *what to do* when an event fires,
*how to compute* a value, or *what state the widget holds*. The
widget itself — its properties, its state, its signals, its
handlers, its constructors, its member variables — is a hand-written
`OmegaWTK::Widget` subclass authored entirely in C++. WML does not
generate that class. WML compiles to a free function that, when
called on an instance of the class, builds the visual tree and wires
bindings, delegates, and observers against members the class
already exposes.

The C++ class declares its WML-visible surface explicitly with a
small family of macros (`OMEGAWTK_WML_WIDGET`,
`OMEGAWTK_WML_PROPERTY`, `OMEGAWTK_WML_STATE`,
`OMEGAWTK_WML_SIGNAL`, `OMEGAWTK_WML_HANDLER`). A build-time
**header extractor** tool scans the project's C++ headers for these
macros and emits a `.wmlh` declaration header per annotated class.
The WML compiler reads `.wmlh` files to validate tag references,
attribute usage, handler names, and binding targets. See §7 for the
macro surface and §5.7 for the build pipeline.

WML is, in summary:

```text
HTML-like markup whose tag names ARE the names of C++ Widget
subclasses authored by the engine or the project
+ CSS styling (Tier-1 selectors only)
+ four-tier events that compile to ViewDelegate /
  WidgetInteractionDelegate / WidgetObserver / NativeEventProcessor
  subclasses; the handler string is always the bare name of an
  OMEGAWTK_WML_HANDLER-annotated C++ method
+ dotted-path data bindings against OMEGAWTK_WML_PROPERTY-declared
  members (no expressions, no operators)
+ theme variables (ThemeVars)
+ state-driven styling (engine's InteractiveState pseudo-classes
  + :state(name) custom flags backed by OMEGAWTK_WML_STATE-declared
  bits on the C++ class)
+ animations (transitions + keyframes, both driven by
  the per-window AnimationScheduler)
```

WML is **not**:

- **A scripting language.** There is no `<script>` block, no
  function bodies, no statements, no assignments. The compiler
  MUST hard-error on any markup that tries to embed executable
  behavior.
- **An expression language.** Bindings are dotted-path reads of
  the C++ model (`{user.name}`, `{player.health}`). Arithmetic,
  ternaries, function calls, and operators MUST NOT be parsed
  inside `{}`. If a derived value is needed, it MUST be exposed
  as a property or computed accessor on the C++ widget class via
  the macros in §7.
- **An event-handler language.** `on:click="save"` names a C++
  method that has been annotated with `OMEGAWTK_WML_HANDLER`.
  The handler string MUST be a bare identifier. Call-form
  expressions such as `on:click="save(item.id)"` MUST be rejected
  by the compiler.
- **A class-generation language.** A `<widget name="X">` block
  does not synthesize a C++ class named `X`. The class `X` MUST
  already exist in the project's C++ sources. If no class `X`
  with `OMEGAWTK_WML_WIDGET(X)` is visible to the build, the
  WML compiler MUST report an `unbound widget` error.

---

## 2. Conformance and Document Conventions

### 2.1 RFC 2119 keywords

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**,
**SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY**,
and **OPTIONAL** in this document are to be interpreted as described
in BCP 14 (RFC 2119) when, and only when, they appear in all
capitals. These keywords apply to **WML compilers** unless the
context names a different actor (e.g. WML authors, the engine
runtime).

### 2.2 Normative vs informative

All sections in the body (1 through 19) are **normative** unless an
individual section is explicitly marked *(informative)*. Annexes are
normative when their conformance requirements are referenced from
the body (Annex B is referenced by §6.2 and §19) and informative
otherwise.

Example listings in normative sections are informative — they
illustrate normative rules but are not themselves rules.

### 2.3 Code-listing conventions

WML markup is shown in code blocks tagged `html`:

```html
<Button on:click="save"><Label text="Save" /></Button>
```

CSS-style style blocks are shown in code blocks tagged `css`:

```css
Button.primary { background: var(--accent); }
```

Generated C++ output is shown in code blocks tagged `cpp` and is
always informative.

---

## 3. Terminology

For the purposes of this document, the following terms apply.

**Application** — an instance of `OmegaWTK::AppInst`. Owns the
process-level `ThemeVars` registry and the global stylesheet stack.

**Author class** — the hand-written `OmegaWTK::Widget` (or
`OmegaWTK::App`) subclass that a `<widget>` / `<app>` block in
WML attaches its look to. The author class is the source of
truth for the widget's properties, states, signals, handlers,
member variables, and constructors. WML does not generate it.

**Behavior** — executable logic that mutates state, computes
derived values, or decides which signal to fire. Behavior MUST
NOT be expressed in WML; it MUST be expressed in C++ on the
author class.

**Binding** — a one-way or two-way reference from a WML attribute
to a property on the enclosing widget's C++ model. Syntax: `{path}`
(one-way) or `bind:attr="path"` (two-way). See §10.

**Class (CSS class)** — a name listed in a tag's `class` attribute
that participates in selector matching. WML classes share the
syntax and semantics of CSS classes. Classes are not unique;
multiple widgets MAY share a class. See §5.6.

**ID** — the value of a tag's `id` attribute. Identifies a single
widget instance application-globally. Matchable in selectors as
`#id`. IDs MUST be unique across the entire project's widget
tree; see §5.6.1.

**Compiler** — a conforming WML compiler. Reads `.wml`, `.wmlh`,
`.wmls`, and `.wtheme` source files and emits C++ source — a
**look function** per `<widget>` / `<app>` block, plus supporting
delegate / observer subclasses — that, linked against OmegaWTK and
against the project's hand-written author classes, builds the
widget visuals and wires bindings + events the source describes.

**Header Extractor** — a build-time tool, separate from the WML
compiler, that scans the project's C++ header files for
`OMEGAWTK_WML_*` macro invocations and emits one `.wmlh` file per
annotated author class. The extractor is the single source of
truth for `.wmlh` files in the build; WML authors and the WML
compiler MUST NOT hand-edit or hand-author `.wmlh` files. See
§5.7.

**ComputedStyle** — the per-`UIView` cache that holds the resolved
visual-property values for one frame. Populated by the
`StyleResolver` during the Style phase of the frame lifecycle.
Defined by the engine; see [UIModel.rst](./UIModel.rst).

**Custom state** — a named bit on the author class's state bitset
declared in C++ with `OMEGAWTK_WML_STATE(X)`. Matchable in
selectors as `:state(X)`. See §12.2.

**Declaration header (`.wmlh`)** — a build-artifact file that
declares an author class's WML-visible surface (properties,
states, signals, slots, parent kind) without defining its body.
Produced exclusively by the Header Extractor (§5.7) from C++
headers; authoritative for both built-in and user widgets;
read-only to authors. See §5.4.

**DirtyBit** — one of `Style`, `Layout`, `Content`, `Paint` on a
`View`. Set by binding updates, state changes, and explicit
`invalidate()` calls; consumed by the engine's `FrameBuilder` to
decide which lifecycle phases to run.

**Element tag** — a PascalCase identifier that names an OmegaWTK
C++ class. The complete set of built-in element tags is enumerated
in §6.

**Engine** — the OmegaWTK runtime. Defined by the normative
references in the front matter.

**Global stylesheet (`.wmls`)** — a standalone stylesheet file that
loads into `Application::stylesheetStack()` and applies its rules
cross-cuttingly against any widget kind that satisfies the
selector. See §5.5 and §17.

**Event** — a signal of user input, widget lifecycle change, or
window-level activity. WML organizes events into four tiers
(§11.2 through §11.5).

**Handler** — the C++ method named by an `on:<event>="name"`
attribute. The handler is called by the compiler-generated
delegate when the event fires.

**Kind** — a widget's class name as it appears in a selector.
`Button.primary` matches kind `Button` AND class `primary`. The
kind of a widget is the C++ class name of its `Widget` subclass.

**Look** — the visual and structural surface a `<widget>` block
produces: its element tree, its style rules, its layout, its
state-driven appearance, its animations. WML defines the look;
the author class consumes it via `OMEGAWTK_WML_WIDGET` (§7.1).

**Look extension** — the use of `extends-look="X"` on a
`<widget>` block to layer additional styles and slot fills on
top of an existing widget's look without restating its element
tree. See §16. Look extension is a WML construct; it is
independent of C++ inheritance.

**Look function** — the free function the WML compiler emits for
each `<widget>` / `<app>` block, conventionally named
`applyXLook(X &)`. Called by the author class (typically from
the `rebuildContent()` body generated by `OMEGAWTK_WML_WIDGET`)
to build the widget's visual tree and wire bindings + delegates.

**Layout properties** — the subset of CSS-like properties that
control structural geometry (`width`, `height`, `margin`,
`padding`, `gap`, `flex-*`, `layout`, `dock`, etc.). Compile to
per-node `LayoutStyle` field assignments, never to `StyleRule`s.
See §8.2.

**Property (widget property)** — a value declared on the author
class with `OMEGAWTK_WML_PROPERTY(name, T)`. The macro generates
a private member, a getter, and a setter; the Header Extractor
records the property in the class's `.wmlh`. Bindings (§10)
may read property values; two-way bindings (§10.3) write through
the generated setter. See §7.2.

**Selector** — a Tier-1 compound selector — kind, id, classes, and
pseudo-classes ANDed together. WML rejects Tier-2 combinators
(§8.3.2).

**Signal** — a typed outbound event declared on the author class
with `OMEGAWTK_WML_SIGNAL(name)` or `OMEGAWTK_WML_SIGNAL_T(name,
T)`. The macro generates an `emit_name(...)` method and an
internal slot list; the Header Extractor records the signal in
the class's `.wmlh`. Parents subscribe via `on:name="handler"`.
The author class invokes `emit_name(...)` from C++; markup MUST
NOT emit signals. See §7.4 and §11.7.

**State (built-in)** — one of the four `InteractiveState` values
tracked by `WidgetInteractionDelegate`: `Hovered`, `Pressed`,
`Focused`, `Disabled`. (The implicit fifth value `Idle` corresponds
to no pseudo-class match.)

**Style block** — a `<style>` element inside a `<widget>` block,
or the contents of a `.wmls` file. Contains CSS-like rules.
See §8.

**Style entry** — an `OmegaWTK::Style::Entry`. The atomic unit of
the engine's per-view visual style surface.

**StyleRule** — a (selector, property, value) record registered in
a `StyleSheet`. WML `<style>` blocks compile to `StyleRule`s for
visual properties only (§8.2).

**StyleSheet** — an ordered set of `StyleRule`s. WML compilers MUST
emit one `StyleSheet` per widget kind (§17).

**Theme** — a named `ThemeVars` map registered on `Application`.
Selected via `Application::setTheme(name)`. See §9.

**Tier (selector tier)** — the selector subset a compiler accepts.
Tier 1 supports compound selectors only (kind + id + classes +
pseudo-classes). Tier 2 (combinators) is reserved; the compiler
MUST reject Tier-2 selectors with an error.

**Tier (event tier)** — one of the four engine surfaces an event
maps to. Tier 1 = `ViewDelegate`. Tier 2 = `WidgetInteractionDelegate`.
Tier 3 = `WidgetObserver`. Tier 4 = `NativeEventProcessor`.
See §11.

**View** — an `OmegaWTK::View`, the engine's rendering primitive.
Owns a `LayerTree` and a `ViewDelegate`. A widget's root view is
always a `View` (or a subclass).

**Widget** — an `OmegaWTK::Widget` subclass authored in C++. Each
widget kind that participates in WML uses `OMEGAWTK_WML_WIDGET`
(§7.1) to bind to a `.wml` file of the matching name. The author
class — never WML — is the unit of widget identity.

**Widget kind** — see *Kind*.

---

## 4. Engine Mapping

The following table is normative. Every WML construct maps to a
specific engine output. A conforming compiler MUST produce the
listed output and MUST NOT introduce parallel concepts.

| WML construct | Engine output |
|---|---|
| `<widget name="X">` | A free function `applyXLook(X &)` emitted into a generated `.cpp`, called from the author class's `rebuildContent()` (typically generated by `OMEGAWTK_WML_WIDGET(X)`) to build the visual subtree. |
| `<app name="X">` | A free function `applyXLook(X &)` of the same shape, where `X` is the author's `OmegaWTK::App` subclass. |
| `<widget name="X" extends-look="Y">` | A look function `applyXLook(X &)` whose first action is a call to `applyYLook(static_cast<Y&>(x))`, followed by the extension's additional styles + slot fills (§16). |
| Child element tag `<Y …>` where `Y` is a built-in tag (§6) | An instantiation of `OmegaWTK::Y`. |
| Child element tag `<Z …>` where `Z` is a user-authored widget | An instantiation of the user's `Z` author class. |
| Inline attributes (`width="72"`, `color="red"`) | Per-node `LayoutStyle` (structural) or `Style::Entry` (visual) field assignments. |
| Attribute matching an `OMEGAWTK_WML_PROPERTY` on the target class | A default-constructed instance, followed by a call to the generated setter (`target->setX(value)`). |
| `<style>` block — visual rules | `StyleRule`s registered in the widget kind's `StyleSheet` (§17). |
| `<style>` block — layout properties | Per-node `LayoutStyle` field assignments at instantiation; MUST NOT compile to `StyleRule`s. |
| `<style>` block — `transition: …` | `LayoutTransitionSpec` or animated `Style::Entry` records consumed by the per-window `AnimationScheduler` via `StyleResolver`. |
| `<style>` block — `@keyframes` / `animation: …` | `AnimationScheduler` tracks emitted at element construction or state-transition time. |
| `:hover` / `:pressed` / `:focused` / `:disabled` | The four `InteractiveState` bits tracked by `WidgetInteractionDelegate`. |
| `:state(name)` | A bit on the author class's custom-state bitset, declared in C++ via `OMEGAWTK_WML_STATE(name)`. |
| `var(--name)` | `ThemeVars` lookup at cascade time against `Application::themeVars()`. |
| `<theme>` block / `.wtheme` file | A `ThemeVars` map registered on `Application`. |
| `{path}` | A subscription on the named `OMEGAWTK_WML_PROPERTY` path; on change, set the appropriate `DirtyBit`s and update the bound attribute. |
| `bind:attr="path"` | The above, plus a writeback that invokes the property's C++ setter when the input widget reports a change. |
| `on:<event>="handler"` | A compiler-generated `ViewDelegate` / `WidgetInteractionDelegate` / `WidgetObserver` / `NativeEventProcessor` subclass that invokes `handler` (which MUST be an `OMEGAWTK_WML_HANDLER`-annotated method on the enclosing widget's author class) when the event fires. See §11. |
| `<slot name="X" />` placeholder | A hole in the look-function output that the look-function for an extending widget (§16) or a caller-site `<template slot="X">` fills. |

### 4.1 PascalCase tag rule

WML built-in tag names are the exact C++ class names of the types
they construct. A conforming compiler MUST treat tag identifiers
case-sensitively. `<Button>` is `OmegaWTK::Button`; `<button>` is
not a valid WML tag and MUST be rejected.

The PascalCase-equals-class-name rule serves two ends: it makes the
generated C++ debuggable (a WML `<HStack>` appears in stack traces
as `HStack`, not as an intermediate wrapper), and it removes a
naming layer the engine would otherwise need to document.

---

## 5. File Types and Project Structure

### 5.1 File extensions

The following file extensions are normative. A conforming compiler
MUST accept these extensions and MUST NOT process any other
extension as WML source.

| Extension | Contents | Role |
|---|---|---|
| `.wml` | Widget markup file. Contains one `<widget name="…">` definition or one `<app name="…">` definition. Defines the **look** that attaches to a same-named C++ author class. | Defines a widget's or app's visual surface. |
| `.wmlh` | Widget declaration header. Contains one `<widget name="…">` declaration with no body. **Produced exclusively by the Header Extractor (§5.7) from a C++ header that uses `OMEGAWTK_WML_*` macros.** Read by the WML compiler to validate tag references, attributes, handler names, and binding targets. | Build-artifact contract between the C++ author class and the WML compiler. See §5.4. |
| `.wmls` | Standalone stylesheet. Contains CSS rules at file scope. | Cross-cutting styling for existing widget kinds. Loads into `Application::stylesheetStack()`. See §5.5. |
| `.wtheme` | Theme definition file. Contains a single `@theme` block. | Compiles to a `ThemeVars` map registered on `Application`. |

There is no script-file extension. Behavior lives in the C++
author class (typically `UserCard.h` / `UserCard.cpp` next to
`UserCard.wml`). WML never embeds behavior, and there is no
WML-specific behavior file.

### 5.2 Example project layout

*Informative.* A typical project organizes WML sources alongside
the C++ author classes they attach to:

```text
ui/
  widgets/
    UserCard.h               # author class — declares the surface with OMEGAWTK_WML_*
    UserCard.cpp             # author class — handler bodies, signal emission, member methods
    UserCard.wml             # look (attaches to UserCard)
    UserCard.wmlh            # build artifact, emitted by the Header Extractor — DO NOT EDIT
    UserCard.wml.gen.cpp     # build artifact, emitted by the WML compiler — DO NOT EDIT

    ProductCard.h
    ProductCard.cpp
    ProductCard.wml
    ProductCard.wmlh         # extractor output
    ProductCard.wml.gen.cpp  # WML compiler output

    PrimaryButton.h
    PrimaryButton.cpp
    PrimaryButton.wml
    PrimaryButton.wmlh
    PrimaryButton.wml.gen.cpp

  themes/
    DarkTheme.wtheme
    LightTheme.wtheme

  styles/
    global.wmls              # cross-cutting rules for built-in + user widget kinds
    forms.wmls
```

Three tools collaborate to produce a buildable target:

- **The Header Extractor** scans `*.h` for `OMEGAWTK_WML_*` macro
  invocations and emits one `.wmlh` per annotated class. Inputs:
  C++ headers. Outputs: `.wmlh` files. See §5.7.
- **The WML compiler** reads `.wml` source plus the `.wmlh` files
  produced by the extractor (plus SDK-shipped `.wmlh` for built-in
  widgets) and emits one `<source>.wml.gen.cpp` per `.wml`
  containing the look function `applyXLook(X &)` and any
  associated delegate / observer subclasses.
- **The C++ compiler** compiles the author's `.cpp` files together
  with each `<source>.wml.gen.cpp` to produce the final target.

The WML compiler does NOT emit a `.h` or a class declaration. The
class is the author's. The WML compiler only emits the body of the
look function, plus the delegate types it needs as friend-accessed
helpers.

### 5.3 Top-level WML structure

A `.wml` file MUST contain exactly one top-level element, which
MUST be either `<widget name="…">` (a reusable widget look) or
`<app name="…">` (an application root look). The compiler MUST
reject files that contain neither, both, or multiple of either.

The `name` attribute is the binding to the C++ author class:
`<widget name="UserCard">` MUST attach to a class
`OmegaWTK::UserCard` (or the project's namespace equivalent) that
declares `OMEGAWTK_WML_WIDGET(UserCard)` in its body (§7.1). The
WML compiler MUST resolve the binding via the `.wmlh` produced by
the Header Extractor; if no matching `.wmlh` exists, the compiler
MUST report an `unbound widget` error.

```html
<widget name="UserCard">
  <style> … </style>

  <!-- body: a single root element -->
  <VStack> … </VStack>
</widget>
```

A `<widget>` block MUST contain:

- Zero or one `<style>` blocks (§8).
- Zero or more `<slot>` placeholders (§7.5), authored inline in
  the body where the slot should appear.
- Exactly one body root element (any built-in or custom widget tag).

A `<widget>` block MUST NOT contain `<property>`, `<state>`, or
`<signal>` elements; those are declared in C++ on the author class
via the macros in §7. The compiler MUST reject these elements with
a `surface declaration not allowed in WML — declare on the author
class` error.

### 5.4 Widget declaration headers (`.wmlh`)

A `.wmlh` file declares an author class's **WML-visible surface**
— its properties, states, signals, slots, and parent kind. It is
a **build artifact produced exclusively by the Header Extractor
(§5.7)** from the author's C++ header. `.wmlh` files MUST NOT be
hand-authored by WML authors and MUST NOT be emitted by the WML
compiler.

`.wmlh` exists to bridge two tools that read different languages:
the WML compiler reads markup but cannot parse C++; the Header
Extractor reads C++ headers via a narrow macro-recognition pass
and emits `.wmlh` as a stable, parseable interface. The WML
compiler reads `.wmlh` to validate tag references, attribute
usage, event handler signatures, and binding targets.

A conforming WML compiler MUST resolve every PascalCase tag
against a known `.wmlh` declaration before emitting code. The set
of known declarations is the union of:

1. SDK-shipped `.wmlh` files for built-in widgets (§5.4.2).
2. Extractor-emitted `.wmlh` files for the project's author
   classes (§5.4.3).

The `<widget name="…">` element in a `.wml` file is **not** a
declaration; it is a binding to a class declared elsewhere in
C++. A `.wml` file's own widget name MUST resolve to a `.wmlh`
produced by the extractor from the matching C++ author class.

#### 5.4.1 Format

A `.wmlh` file MUST contain exactly one top-level `<widget>`
element. The element MUST NOT contain a body root, a `<style>`
block, or any markup other than the declarations enumerated below.

A `<widget>` element inside a `.wmlh` file MAY contain:

| Element | Notes |
|---|---|
| `extends="X"` attribute | The C++ parent class (a widget kind or `Widget` itself). Optional; default is `Widget`. Reflects the author class's actual base class. |
| `<property name="X" type="T" default="…" />` | One per `OMEGAWTK_WML_PROPERTY` invocation on the author class. |
| `<state name="X" />` | One per `OMEGAWTK_WML_STATE` invocation. |
| `<signal name="X" payload="T" />` | One per `OMEGAWTK_WML_SIGNAL_T` (or `OMEGAWTK_WML_SIGNAL` without payload) invocation. |
| `<handler name="X" payload="T" />` | One per `OMEGAWTK_WML_HANDLER` annotation, recording the C++ method's name and parameter type. |
| `<slot name="X" />` | Reserved; slot placeholders live in the `.wml` (§7.5), not on the author class. Present in `.wmlh` only when an explicit slot-binding macro lands in a future revision. |

The WML compiler MUST reject any other content inside a `.wmlh`
`<widget>` element.

Example — `Label.wmlh` (extracted from the engine's
`include/omegaWTK/Widgets/Primatives.h`):

```html
<widget name="Label" extends="Widget">
  <property name="text" type="OmegaCommon::UString" default='""' />
  <property name="font" type="SharedHandle&lt;Composition::Font&gt;" default="nullptr" />
  <property name="textColor" type="Composition::Color" default="black" />
  <property name="alignment" type="Composition::TextLayoutDescriptor::Alignment" default="LeftUpper" />
  <property name="wrapping" type="Composition::TextLayoutDescriptor::Wrapping" default="WrapByWord" />
  <property name="lineLimit" type="unsigned" default="0" />
</widget>
```

Example — `Button.wmlh`:

```html
<widget name="Button" extends="Container">
  <!-- The current C++ Button is a Container subclass with no
       additional properties. The .wmlh grows as the C++ class
       grows. -->
</widget>
```

#### 5.4.2 SDK-shipped `.wmlh` for built-in widgets

The OmegaWTK SDK MUST ship one `.wmlh` per built-in widget kind
listed in §6. These are produced once at engine-build time by
running the Header Extractor over the engine's own headers (which
themselves use the `OMEGAWTK_WML_*` macros to declare their
WML-visible surface).

A conforming WML compiler MUST locate the SDK `.wmlh` directory
before compiling any user `.wml` file. The location is
implementation-defined but SHOULD be a directory named `wmlh/`
under the SDK install prefix.

If a built-in widget's C++ class gains or loses a property in a
later release of OmegaWTK, the next extractor run regenerates the
corresponding `.wmlh` automatically — the engine source is the
single source of truth.

#### 5.4.3 Extractor-emitted `.wmlh` for project author classes

The Header Extractor (§5.7) MUST emit one `.wmlh` per author class
in the project that declares `OMEGAWTK_WML_WIDGET(X)`. The emitted
`.wmlh`:

- MUST contain a single `<widget name="X" extends="…">` element
  whose `extends` reflects the author class's actual immediate
  base class.
- MUST list every `OMEGAWTK_WML_PROPERTY`, `OMEGAWTK_WML_STATE`,
  `OMEGAWTK_WML_SIGNAL`, and `OMEGAWTK_WML_HANDLER` invocation
  on the author class, with identical names, types, payloads,
  and defaults.
- MUST NOT contain a body root, a `<style>` block, or any other
  content.

Authors MUST NOT hand-edit extractor-emitted `.wmlh` files. The
source of truth for an author class's WML surface is the C++
header, not the `.wmlh`.

#### 5.4.4 Include path resolution

The WML compiler MUST resolve a tag `<X>` to a `.wmlh` declaration
by searching:

1. The set of project `.wmlh` files in the WML include path
   (extractor output for the current build).
2. The SDK `.wmlh` directory.

The first match wins. The compiler MUST report a
`reserved tag, C++ widget not yet available` error for tags listed
in Annex B and an `unknown widget tag` error for everything else
unresolved.

WML has no `<import>` directive. Tag resolution is implicit via
include path, mirroring the C++ compiler's handling of headers.

### 5.5 Standalone stylesheets (`.wmls`)

A `.wmls` file contains CSS rules at file scope. It compiles to a
`StyleSheet` registered on `Application::stylesheetStack()` at
application startup. Rules in a `.wmls` file are **not scoped** to
any particular widget kind — selectors match across the entire
widget tree.

The point of `.wmls` is to **style existing widgets cross-cuttingly**:
write `Button.danger { background: var(--danger); }` once in a
`.wmls`, and every `<Button class="danger">` in the application
picks up the styling regardless of which `<widget>` defines the
button's container.

#### 5.5.1 Format

A `.wmls` file's contents are exactly a `<style>` block's contents
(see §8) — top-level CSS rules, `@theme`-free, optionally with
`@media` and `@keyframes` blocks.

```css
/* global.wmls */

Button {
  padding: 8px 14px;
  border-radius: 6px;
}

Button.primary {
  background: var(--accent);
  color: white;
}

Button.danger {
  background: var(--danger);
  color: white;
}

Slider:focused {
  border-color: var(--accent);
}

@media platform == mobile {
  Button { padding: 12px 18px; }
}
```

#### 5.5.2 Registration order

When a `.wmls` file is loaded into the application, it MUST be
registered on `Application::stylesheetStack()`. The order of
registration is the order in which `.wmls` files are listed at
load time (typically by the application's entry point or by
`<style src="…">` directives in an `<app>` block).

Standard CSS cascade applies (§8.5). Later-registered sheets layer
on top of earlier ones at equal specificity.

#### 5.5.3 Loading from WML

A `.wmls` file MAY be loaded by reference from an `<app>` block:

```html
<app name="MyApp">
  <theme src="themes/DarkTheme.wtheme" />
  <style src="styles/global.wmls" />
  <style src="styles/forms.wmls" />
  …
</app>
```

The `<style src="…">` directive MUST be valid only inside an
`<app>` block. Inside a `<widget>` block, styling is authored as
an inline `<style>` element (kind-scoped per §17), not via
`<style src>`.

#### 5.5.4 Scoping rules

Rules in `.wmls` files are NOT silently rewritten to scope by
widget kind. The bare selector `.primary { … }` in a `.wmls` file
matches **every** widget with class `primary`, regardless of kind.
Authors who want per-kind rules MUST qualify the selector:
`Button.primary { … }`.

This is the distinction between `.wmls` (global, cross-cutting)
and an inline `<style>` block inside `<widget name="X">` (kind-
scoped per §17). The two surfaces exist for different jobs:

- **Inline `<style>` in `<widget>`** — co-located with the widget
  definition. The cascade automatically scopes to the widget's
  kind. Use this for styles that are intrinsic to *that one
  widget*.
- **`.wmls` files** — global. Use these for cross-cutting styling
  of built-in widgets (e.g. all `Button`s in the application get
  the same padding), for visual themes that are not theme variables
  (e.g. a `.danger` class definition that applies regardless of
  widget kind), and for reusable utility classes.

### 5.6 Universal widget attributes

Every WML tag in §6 (built-in or custom widget) accepts the
following universal attributes. They are not declared in `.wmlh`
headers — they are part of the language, applied uniformly to
every widget tag.

| Attribute | Type | Purpose |
|---|---|---|
| `id` | string identifier | Identifies one widget instance application-globally. Matchable as `#id` in selectors. |
| `class` | space-separated identifiers | One or more CSS class names. Matchable as `.class` in selectors. Multiple widgets MAY share a class. |

The `<app>` element does NOT accept `id` — its `name` attribute is
its identifier.

#### 5.6.1 The `id` attribute

The `id` attribute assigns a unique, stable identifier to a single
widget instance. IDs:

- MUST be unique across the entire project's widget tree. Two
  distinct widgets MUST NOT share an `id` value.
- MUST be a valid CSS identifier: a sequence of letters, digits,
  underscores, and hyphens, beginning with a letter or
  underscore.
- MAY be a literal string (`id="primary-action"`) or a dotted-path
  binding (`id="{user.id}"`). Literal IDs are validated for
  uniqueness at compile time; bound IDs are author-responsibility
  at runtime.

The compiler MUST report duplicate literal `id` values across all
`.wml` files in the project translation unit as a compile error.

```html
<!-- App-level usage: two Buttons distinguished by id -->
<app name="ConfirmDialog">
  <HStack>
    <Button id="confirm-action" on:click="confirm">
      <Label text="Confirm" />
    </Button>
    <Button id="cancel-action" on:click="cancel">
      <Label text="Cancel" />
    </Button>
  </HStack>
</app>
```

A `.wmls` file targets each one by ID:

```css
/* styles/dialog.wmls */

Button#confirm-action {
  background: var(--accent);
  color: white;
  border-radius: 8px;
}

Button#cancel-action {
  background: transparent;
  color: var(--text-muted);
  border: 1px solid var(--border);
}
```

The selector `Button#confirm-action` matches **only** the single
`<Button id="confirm-action">` instance. `Button#cancel-action`
matches **only** the cancel button. Other `<Button>` widgets in
the application — anywhere in any `.wml` file — are unaffected.

ID selectors have higher specificity than class selectors and class
selectors have higher specificity than kind selectors, per standard
CSS (see §8.5). A rule with `Button#confirm-action` will override a
rule with `Button.primary` even if `.primary` is listed later in
source order.

#### 5.6.2 The `class` attribute

The `class` attribute lists one or more space-separated CSS class
names. Classes are not required to be unique; multiple widgets MAY
share the same class.

```html
<Button class="primary large" on:click="save">Save</Button>
<Button class="primary small" on:click="quickSave">Quick Save</Button>
```

Selectors `.primary`, `.large`, `Button.primary`, etc. match every
widget whose class list contains the named class.

The boolean-attribute desugaring rule in §8.6 — `featured="true"` →
`.featured` class — adds to a widget's class list.

#### 5.6.3 IDs inside `<widget>` bodies

A `<widget name="X">` body MAY contain `id` attributes on internal
elements, but the compiler MUST treat duplicate literals across the
*expanded* widget tree as duplicate-ID errors. In practice this
means:

- If `X` is instantiated more than once in the project, an `id`
  literal inside `X`'s body will produce a duplicate-ID error on
  every instantiation after the first.
- Reusable widgets that may be instantiated more than once SHOULD
  use `class` instead of `id` for internal styling targets.
- A widget kind that is instantiated exactly once (e.g. an
  application-root `<LoginForm>` containing
  `<TextInput id="username">`) MAY use ID literals freely.

Authors who need per-instance identification of internal elements
in a multi-instance widget MUST expose the identifier as a property
on the author class via `OMEGAWTK_WML_PROPERTY(uniqueId, …)`
(§7.2) and bind it: `id="{uniqueId}"`. The runtime then sees
distinct IDs per instance.

#### 5.6.4 IDs on custom widget instantiations

When a custom widget is instantiated with an `id` attribute, the
ID applies to the **root view** of the widget instance:

```html
<UserCard id="featured-user" user="{currentUser}" />
```

```css
#featured-user {
  border: 2px solid var(--accent);
}
```

The selector `#featured-user` matches the root view of the
`UserCard` instance, not any of its internal child widgets.

### 5.7 Build pipeline

WML's authoring model involves three build-time tools, each with
a narrow, well-defined responsibility. A conforming OmegaWTK build
system MUST invoke them in this order:

```text
   project C++ headers (*.h)
   with OMEGAWTK_WML_* macros
            │
            ▼
   ┌──────────────────────┐
   │   Header Extractor   │   reads C++, recognizes the macros,
   │   (§5.7.1)           │   emits one .wmlh per annotated class
   └──────────────────────┘
            │
            ▼
   project *.wmlh (build artifacts)  +  SDK-shipped *.wmlh
            │                                    │
            └─────────────┬──────────────────────┘
                          ▼
        project *.wml (look definitions)
                          │
                          ▼
        ┌────────────────────────────────────┐
        │           WML Compiler             │   reads .wml + .wmlh,
        │                                    │   emits one .wml.gen.cpp
        └────────────────────────────────────┘   per .wml containing
                          │                      applyXLook(X &) and
                          ▼                      its delegate types.
        project *.wml.gen.cpp (build artifacts)
                          │
            ┌─────────────┴──────────────┐
            ▼                            ▼
   author *.cpp                  *.wml.gen.cpp
            │                            │
            └─────────────┬──────────────┘
                          ▼
                  ┌───────────────┐
                  │ C++ Compiler  │
                  └───────────────┘
                          │
                          ▼
                  linked target
```

#### 5.7.1 Header Extractor requirements

The Header Extractor is a separate executable from the WML
compiler. A conforming extractor MUST:

1. Accept a set of C++ header files as input.
2. Recognize the `OMEGAWTK_WML_WIDGET`, `OMEGAWTK_WML_PROPERTY`,
   `OMEGAWTK_WML_STATE`, `OMEGAWTK_WML_SIGNAL`,
   `OMEGAWTK_WML_SIGNAL_T`, and `OMEGAWTK_WML_HANDLER` macros
   (§7) without requiring a full C++ parse. Lexical recognition
   sufficient to identify these macros, their arguments, and the
   class containing them is sufficient.
3. Emit exactly one `.wmlh` per `OMEGAWTK_WML_WIDGET(X)`
   invocation found, named `X.wmlh` and placed in the build
   system's designated WML include path.
4. Report an error if two distinct classes in the project both
   declare `OMEGAWTK_WML_WIDGET` with the same `X`.
5. Report an error if `OMEGAWTK_WML_PROPERTY` / `_STATE` /
   `_SIGNAL` / `_HANDLER` invocations appear outside a class body
   that contains `OMEGAWTK_WML_WIDGET`.

The extractor MAY emit additional diagnostics. The extractor MUST
NOT emit C++ source code or modify the author's headers.

#### 5.7.2 WML compiler dependency order

The WML compiler MUST NOT read `.wml` files until the extractor
has emitted the project's `.wmlh` files. The WML compiler MAY
read SDK-shipped `.wmlh` files at any time.

If the C++ header for class `X` changes (a property added, a
handler renamed), the build system MUST re-run the extractor on
that header before re-running the WML compiler on any `.wml` that
references `X`.

---

## 6. Built-In Elements

The set of built-in WML tags is **closed**. A conforming compiler
MUST reject any PascalCase tag that does not appear in this section
unless it is a user-authored widget name (§6.5) or a reserved
identifier (Annex B).

### 6.1 Display primitives

Pure-paint widgets that own a `UIView` subtree describing a single
shape, image, or text run. Defined in
[`Widgets/Primatives.h`](../include/omegaWTK/Widgets/Primatives.h).

| WML tag | C++ class |
|---|---|
| `<Rectangle>` | `OmegaWTK::Rectangle` |
| `<RoundedRectangle>` | `OmegaWTK::RoundedRectangle` |
| `<Ellipse>` | `OmegaWTK::Ellipse` |
| `<Path>` | `OmegaWTK::Path` |
| `<Separator>` | `OmegaWTK::Separator` |
| `<Label>` | `OmegaWTK::Label` |
| `<Icon>` | `OmegaWTK::Icon` |
| `<Image>` | `OmegaWTK::Image` |

Each tag's attributes MUST map to the matching `XxxProps` struct
in `Primatives.h`. The compiler MUST emit
`Xxx::Create(rect, props)` at construction and
`xxx->setProps(props)` on attribute change.

There is no `<Text>` tag. Text rendering belongs to `<Label>`.

### 6.2 User-input widgets

Defined in
[`Widgets/UserInputs.h`](../include/omegaWTK/Widgets/UserInputs.h).
All four are `Container` subclasses and MAY take child widgets.

| WML tag | C++ class |
|---|---|
| `<Button>` | `OmegaWTK::Button` |
| `<TextInput>` | `OmegaWTK::TextInput` |
| `<Dropdown>` | `OmegaWTK::Dropdown` |
| `<Slider>` | `OmegaWTK::Slider` |

The tags `<Checkbox>`, `<RadioGroup>`, `<Toggle>`, and `<ProgressBar>`
are **reserved** for future C++ widgets (see Annex B). A conforming
compiler MUST reject these tags with a "reserved tag, C++ widget
not yet available" error until the corresponding C++ classes exist
in `OmegaWTK::`.

### 6.3 Layout containers

Defined in
[`Widgets/Containers.h`](../include/omegaWTK/Widgets/Containers.h)
and
[`Widgets/BasicWidgets.h`](../include/omegaWTK/Widgets/BasicWidgets.h).

| WML tag | C++ class | Role |
|---|---|---|
| `<HStack>` | `OmegaWTK::HStack` | Horizontal stack with `StackOptions`. |
| `<VStack>` | `OmegaWTK::VStack` | Vertical stack with `StackOptions`. |
| `<Container>` | `OmegaWTK::Container` | Generic parent widget with `ContainerClampPolicy`. |
| `<ScrollableContainer>` | `OmegaWTK::ScrollableContainer` | Overflow-scrolling container. |

WML SHOULD prefer `<HStack>` / `<VStack>` over a `<Container>` with
a stack layout behavior; the result is identical but the intent is
clearer.

`<Grid>`, `<AbsoluteStack>`, and `<Panel>` are NOT valid WML tags.
Multi-axis layouts MUST be authored as a `<Container>` with a
`layout-behavior` attribute pointing at one of the engine's
`LayoutBehaviorPtr` factories.

### 6.4 Specialized views

| WML tag | C++ class | Notes |
|---|---|---|
| `<UIView>` | `OmegaWTK::UIView` | Direct authoring surface for `UIElementLayoutSpec`. |
| `<SVGView>` | `OmegaWTK::SVGView` | Loads an `SVGDocument`. |
| `<VideoView>` | `OmegaWTK::VideoView` | Native media playback. |
| `<NativeViewHost>` | `OmegaWTK::NativeViewHost` | Carves out a region for a host-owned native view. |
| `<ScrollView>` | `OmegaWTK::ScrollView` | Raw scroll view. Most authors SHOULD use `<ScrollableContainer>` instead. |

`<WebView>` is reserved (Annex B) and MUST NOT be accepted as a
valid tag.

`<canvas>` and `<surface>` are not valid WML tags. Imperative
drawing MUST be authored via `<NativeViewHost>`; the engine has no
in-process imperative drawing surface (see
[UIModel.rst](./UIModel.rst) — *Specialized View Subclasses*).

### 6.5 Custom (user-authored) widgets

A user-authored widget is a widget declared elsewhere with `<widget
name="X">`. It is referenced by `<X>` from another WML file.

```html
<UserCard user="{currentUser}" />
<ProductTile product="{item}" />
<SettingsPanel />
```

The compiler MUST resolve the name against the set of `.wmlh`
declarations visible to the current translation unit — i.e. the
extractor-emitted `.wmlh` files for the project's author classes
(§5.4.3). Unresolved custom tags MUST be reported as compile
errors.

Attribute values on custom tags MUST correspond 1:1 to property
names declared on the target author class via
`OMEGAWTK_WML_PROPERTY` (§7.2), as recorded in its `.wmlh`.
Unknown attributes MUST be rejected.

---

## 7. Author-Class Binding

WML attaches a look to an `OmegaWTK::Widget` (or `OmegaWTK::App`)
subclass authored entirely in C++. The author class is the source
of truth for the widget's **identity, state, behavior, and
WML-visible surface**. WML does not generate the class, does not
add members to it, and does not provide a way to declare
properties, states, or signals in markup.

The C++ surface that WML may reference is declared via a small,
closed set of macros. The Header Extractor (§5.7.1) recognizes
these macros, the WML compiler validates `.wml` references against
them, and the C++ compiler turns them into ordinary member
declarations.

### 7.1 `OMEGAWTK_WML_WIDGET(ClassName)`

Marks a class as bindable from a `.wml` file of the matching name.

```cpp
class UserCard : public Widget {
public:
    OMEGAWTK_WML_WIDGET(UserCard)
    // … properties, states, signals, handlers, member methods …
};
```

The macro MUST expand to:

- A friend declaration permitting the WML compiler's emitted
  `applyUserCardLook(UserCard &)` to access non-public members
  it needs in order to wire bindings and delegates.
- A `rebuildContent()` override whose body calls
  `applyUserCardLook(*this)`. Author classes that need to do
  additional work in `rebuildContent` MAY override the override
  in the usual C++ way; subclasses MUST call the look function
  themselves if they bypass it.
- An entry in the Header Extractor's recognition table so that a
  corresponding `UserCard.wmlh` is emitted.

A class MUST NOT invoke `OMEGAWTK_WML_WIDGET` more than once. The
extractor MUST report duplicate invocations as an error.

A `.wml` file whose `<widget name="X">` does not match a class
declaring `OMEGAWTK_WML_WIDGET(X)` MUST be rejected by the WML
compiler with an `unbound widget` error.

### 7.2 `OMEGAWTK_WML_PROPERTY(name, Type)`

Declares a WML-visible property.

```cpp
OMEGAWTK_WML_PROPERTY(user, User)
OMEGAWTK_WML_PROPERTY(featured, bool)
OMEGAWTK_WML_PROPERTY(quantity, int)   // default-constructs to 0
```

A two-argument form declares a property with a default-constructed
initial value. A three-argument form supplies an explicit initial
expression:

```cpp
OMEGAWTK_WML_PROPERTY(featured, bool, false)
OMEGAWTK_WML_PROPERTY(scale, float, 1.0f)
```

Each invocation MUST expand to:

- A private member `Type _name;` (default-initialized when the
  three-argument form is not used).
- A getter `const Type & name() const noexcept { return _name; }`.
- A setter `void setName(const Type & v)` that stores the new
  value, marks `DirtyBit::Content | Style | Paint` on the
  widget's root view, and requests a `rebuildContent()` pass via
  the engine's standard mechanism. (Properties whose changes
  affect only paint or layout, never content, are not expressible
  through this macro in v0.2; the author may write the
  member/getter/setter by hand and decline to annotate it.)

The Header Extractor MUST emit one `<property name="name"
type="Type" default="…" />` per invocation into the class's
`.wmlh`.

WML attribute reads (`<UserCard user="{x}" />`) compile to
default-construction of `UserCard` followed by `userCard->setUser(x)`.
The author class MUST therefore be default-constructible.

### 7.3 `OMEGAWTK_WML_STATE(name)`

Declares a WML-visible custom state bit.

```cpp
OMEGAWTK_WML_STATE(expanded)
OMEGAWTK_WML_STATE(downloading)
```

Each invocation MUST expand to:

- A bit allocated in the widget's custom-state bitset (the engine
  surface introduced for `:state(X)` matching).
- A getter `bool isName() const noexcept`.
- A setter `void setName(bool)` that flips the bit and marks
  `DirtyBit::Style` on the widget's root view.

The Header Extractor MUST emit one `<state name="name" />` per
invocation into the class's `.wmlh`.

Custom states are matchable in selectors as `:state(name)` (§12.2).

### 7.4 `OMEGAWTK_WML_SIGNAL(name)` / `OMEGAWTK_WML_SIGNAL_T(name, T)`

Declares a typed outbound event.

```cpp
OMEGAWTK_WML_SIGNAL(ready)                                  // no payload
OMEGAWTK_WML_SIGNAL_T(messageRequested, UserId)             // payload type UserId
OMEGAWTK_WML_SIGNAL_T(colorChanged, Composition::Color)
```

Each invocation MUST expand to:

- A `void emit_name(const T &)` method (or `void emit_name()`
  when payload is absent) on the author class.
- A private slot list (`std::vector<std::function<void(const T
  &)>>`) that the look function appends to whenever a parent
  registers `on:name="handler"`.

The author class invokes `emit_name(...)` from C++ to fire the
signal. Markup MUST NOT contain `emit(...)` syntax; the WML
compiler MUST reject any such attempt.

The Header Extractor MUST emit one `<signal name="name"
payload="T" />` (or without `payload`) per invocation into the
class's `.wmlh`.

### 7.5 `OMEGAWTK_WML_HANDLER` (annotation)

Marks a member function as callable from a WML `on:*` attribute.
The macro is a prefix annotation that expands to nothing in C++;
its sole purpose is to be visible to the Header Extractor.

```cpp
class UserCard : public Widget {
public:
    OMEGAWTK_WML_WIDGET(UserCard)
    OMEGAWTK_WML_PROPERTY(user, User)

    OMEGAWTK_WML_HANDLER void save();
    OMEGAWTK_WML_HANDLER void onUserClicked(const Native::MouseEventParams &);
    OMEGAWTK_WML_HANDLER bool onKeyDown(const Native::KeyDownParams &);
};
```

The annotation MUST immediately precede an ordinary C++ member
function declaration. The Header Extractor MUST emit one
`<handler name="…" payload="…" />` per annotation into the class's
`.wmlh`, recording the C++ method's name and parameter type (and
return type for Tier-1 handlers, which MAY return `bool` per
§11.2).

The WML compiler MUST reject any `on:*` attribute whose handler
identifier does not appear in the target class's `.wmlh` handler
table. Calling a non-annotated public method from WML is a
compile-time error, even if the method exists in the C++ class.

### 7.6 Slot placeholders (`<slot>`)

A `<slot>` placeholder declares a hole in the widget's look that
is filled either by an extending widget's `.wml` (§16) or by a
caller-site `<template slot="name">`. Slots are a markup
construct only; they do not appear on the C++ author class and
they have no corresponding macro.

```html
<widget name="Card">
  <VStack>
    <slot name="header" />
    <slot />                  <!-- default slot -->
    <slot name="footer" />
  </VStack>
</widget>
```

A `<slot>` element has these attributes:

| Attribute | Required | Notes |
|---|---|---|
| `name` | No | If absent, this slot is the *default slot*. Each widget MUST have at most one default slot. |

Callers fill named slots with `<template slot="name">`:

```html
<Card>
  <template slot="header"><Label text="Title" /></template>
  <Label text="Body content" />               <!-- fills default slot -->
  <template slot="footer"><Button … /></template>
</Card>
```

The compiler MUST replace each `<slot>` placeholder with the
caller's matching subtree (or, in the look-extension case, with
the extending widget's `<template slot="…">` content) at look
function emission.

---

## 8. Styling Model

WML accepts CSS-like syntax inside `<style>` blocks (in `<widget>`
files) and in `.wmls` files. The compiler MUST route each property
declaration to one of two destinations.

### 8.1 Visual properties

Visual properties — `background`, `border`, `border-color`,
`border-width`, `color`, `font-size`, `font-weight`, `box-shadow`,
`transform`, `opacity`, `transition`, and the engine's blur /
drop-shadow effects — compile to `StyleRule`s registered in the
widget kind's `StyleSheet` (§17).

### 8.2 Layout properties

Layout properties — `width`, `height`, `min-width`, `max-width`,
`min-height`, `max-height`, `margin`, `margin-*`, `padding`,
`padding-*`, `gap`, `layout`, `dock`, `align-items`,
`justify-content`, `flex-*`, `inset-*`, `clamp` — compile to
per-node `LayoutStyle` field assignments at instantiation.

A conforming compiler MUST NOT emit `StyleRule`s for layout
properties. The cascade does not apply to layout in OmegaWTK.

### 8.3 Selectors

#### 8.3.1 Tier 1 — compound selectors

A WML compiler MUST accept selectors that are a single compound of
the following parts, ANDed together in any combination:

| Part | Form | Notes |
|---|---|---|
| Kind | `Button` | A built-in or custom widget kind name. At most one per compound. |
| ID | `#submitButton` | Matches the widget with that `id` (§5.6.1). At most one per compound. |
| Class | `.primary` | Matches widgets whose class list contains the name. Zero or more per compound. |
| Pseudo-class | `:hover` | One of the built-in interaction pseudo-classes (§12.1) or `:state(name)` (§12.2). Zero or more per compound. |

A compound selector with zero parts is invalid. All other
combinations are valid Tier-1 selectors:

```css
/* Kind-only */
Button { }

/* ID-only — matches one specific instance */
#submit-action { }

/* Class-only */
.primary { }

/* Compound combinations */
Button.primary { }
Button#submit-action { }
Button.primary:disabled { }
Button#submit-action:hover { }
#submit-action.large:focused { }
TextInput:focused { }
```

#### 8.3.2 Tier 2 — combinators (rejected)

The combinator forms `>`, ` ` (descendant), `+`, and `~` are NOT
permitted in conforming WML. The compiler MUST reject them with an
error.

```css
/* All four MUST be rejected: */
Panel > Button { }
Panel  Button { }
Button + Text { }
Button ~ Text { }
```

This is a hard error rather than a silent no-op so that future
selector support in the engine does not retroactively change the
behavior of WML files written today.

### 8.4 What is not supported

The compiler MUST reject:

- **Attribute selectors** — `[type="text"]`, `[featured="true"]`.
  Authors use a class instead. See §8.6 for the bool-attribute
  desugaring.
- **Structural pseudo-classes** — `:nth-child()`, `:first-of-type`,
  `:last-child`, etc.
- **`@supports`, `@scope`, `@layer`** at-rules.
- **`calc()` arithmetic.** `var()` (§9) covers the common case.

### 8.5 Cascade

Standard CSS cascade applies: specificity → source order →
`!important`. Inline `Style` (authored via attributes on a tag)
layers on top of any non-`!important` rule. The `StyleResolver`
(Phase 2 of the frame lifecycle) writes the resolved value into a
`ComputedStyle` cache on each node; Paint reads only
`ComputedStyle`.

#### 8.5.1 Specificity

Specificity of a Tier-1 compound selector is computed as the tuple
`(ids, classes-and-pseudo, kinds)` and compared lexicographically.
Higher values win.

| Selector | (id, class+pseudo, kind) |
|---|---|
| `Button` | (0, 0, 1) |
| `.primary` | (0, 1, 0) |
| `Button.primary` | (0, 1, 1) |
| `Button.primary:disabled` | (0, 2, 1) |
| `#submit` | (1, 0, 0) |
| `Button#submit` | (1, 0, 1) |
| `Button#submit.primary` | (1, 1, 1) |
| `Button#submit.primary:hover` | (1, 2, 1) |

An ID selector beats any combination of classes and kinds.
`#submit { background: red; }` overrides
`Button.primary.large { background: blue; }` regardless of source
order.

The `!important` flag promotes a declaration above all
non-`!important` declarations of any specificity. Two
`!important` declarations resolve by specificity → source order in
the usual way.

### 8.6 Attribute-to-class desugaring

The literal boolean attribute form `attr="true"` MUST desugar to a
single class `.attr` on the instance. Other forms (`priority="high"`,
numeric values, string values) MUST NOT desugar; they MUST be
treated as property bindings against `OMEGAWTK_WML_PROPERTY`
declarations on the target author class (§7.2).

```html
<!-- The "featured" boolean desugars to .featured -->
<ProductCard featured="true" product="{p}" />

<!-- "high" does NOT desugar; "priority" MUST be a declared property -->
<Task priority="high" />
```

### 8.7 CSS toolkit extensions

WML accepts the following engine-aware properties inside style
blocks. They compile to per-node `LayoutStyle` or `Style::Entry`
assignments as indicated.

| Property | Surface | Notes |
|---|---|---|
| `layout` | `LayoutStyle` | One of `row`, `column`, `grid`, `stack`, `fill`. |
| `gap` | `LayoutStyle` | Spacing between children. |
| `dock` | `LayoutStyle` | One of `top`, `bottom`, `left`, `right`, `fill`. |
| `hit-test` | `LayoutStyle` | One of `visible`, `invisible`, `children`. |
| `pointer-events` | `LayoutStyle` | One of `auto`, `none`. |
| `cursor` | `Style::Entry` | Maps to `View::setCursorShape`. |
| `theme-color` | `Style::Entry` | Shorthand for `var(--<name>)`. |
| `elevation` | `Style::Entry` | Shorthand for a `Style::dropShadow` preset. |

`native-effect` is reserved for future use (Annex B) and MUST be
rejected by Tier-1-conforming compilers.

---

## 9. Themes and Variables

### 9.1 Theme variables

Theme variables are declared in a `<theme>` block or `.wtheme` file:

```css
@theme DarkTheme {
  --background: #101014;
  --surface: #1c1c22;
  --text: #ffffff;
  --accent: #4f7cff;
  --radius-lg: 20px;
}
```

The compiler MUST emit, for each `@theme N { … }` block, a
`ThemeVars` map (`String → StyleValue`) registered on `Application`
under the name `N`.

### 9.2 `var()`

`var(--name)` references a theme variable. The compiler MUST emit a
`StyleValue::Var(name)` reference; the `StyleResolver` resolves the
reference at cascade time against `Application::themeVars()` for
the currently active theme.

```css
Panel {
  background: var(--surface);
  border-radius: var(--radius-lg);
}
```

### 9.3 Theme switching

Theme switching is performed at runtime by C++ code via
`Application::setTheme(name)`. The engine marks the root view with
`DirtyBit::Style`; the resolver re-cascades on the next frame.

WML provides no markup-level theme-switching primitive. The
application's C++ entry point is responsible for theme selection
(typically driven by user settings or system theme).

---

## 10. Data Binding

### 10.1 Syntax

WML uses `{}` for data binding. Bindings are **dotted-path reads of
the C++ widget model — nothing more**. The compiler MUST reject any
content inside `{}` that is not a chain of identifiers separated by
`.`.

```html
<!-- Accepted -->
<Label text="{player.name}" />
<Slider value="{player.health}" min="0" max="100" />
<Image src="{user.avatar}" />

<!-- All four MUST be rejected -->
<Label text="{player.health + '/' + player.maxHealth}" />
<Button disabled="{!canSubmit}" />
<Label text="{x ? y : z}" />
<Label text="{computeDisplay(x, y)}" />
```

Computed or derived values MUST be exposed as a property or
computed accessor on the C++ widget class; markup binds to the
property name.

### 10.2 One-way bindings

A single `{path}` inside an attribute value is a one-way binding.
The compiler MUST emit a subscription that, on change of the bound
property, sets the appropriate `DirtyBit`s and updates the bound
attribute on the next frame.

### 10.3 Two-way bindings

The `bind:` prefix declares a two-way binding. The compiler MUST
emit:

- A one-way read of the property (as in §10.2).
- A writeback that invokes the property's C++ setter when the input
  widget reports a change via the engine's `ViewDelegate`.

Two-way bindings are valid only on widgets whose C++ class exposes
the required setter. The current set is:

- `<TextInput bind:value="…">` — calls `TextInput::setValue`.
- `<Slider bind:value="…">` — calls `Slider::setValue`.
- `<Dropdown bind:selection="…">` — calls `Dropdown::setSelection`.

The compiler MUST reject `bind:` on any other widget or attribute.

### 10.4 Frame lifecycle integration

Bindings MUST NOT bypass the engine's five-phase frame lifecycle. A
binding update MUST set `DirtyBit`s and rely on `FrameBuilder` to
process the change on the next frame. Synchronous re-paint from
inside a binding update is forbidden.

---

## 11. Events

Events use `on:<name>="handler"`. The handler string MUST be a bare
C++ identifier naming an `OMEGAWTK_WML_HANDLER`-annotated method
(§7.5) on the enclosing `<widget>`'s author class. There are four
event tiers, each backed by a different engine surface.

### 11.1 Tier overview

| Tier | Engine surface | Use for |
|---|---|---|
| 1 | `ViewDelegate` ([View.h:305](../include/omegaWTK/UI/View.h)) | Raw pointer / keyboard input. |
| 2 | `WidgetInteractionDelegate` ([WidgetTypes.h:28](../include/omegaWTK/Widgets/WidgetTypes.h)) | High-level click / hover / press / focus transitions. |
| 3 | `WidgetObserver` ([Widget.h:271](../include/omegaWTK/UI/Widget.h)) | Widget attach / detach / show / hide / resize. |
| 4 | `NativeEventProcessor` ([NativeEvent.h:107](../include/omegaWTK/Native/NativeEvent.h)) | Scroll, drag, gesture, window/app lifecycle, focus. |

The compiler MUST emit, into the per-`.wml` `.wml.gen.cpp`
translation unit:

- One `WidgetInteractionDelegate` subclass per child element that
  uses a Tier-2 event. The look function instantiates and
  installs it during construction.
- One `ViewDelegate` subclass per child element that uses a Tier-1
  event (installed via `View::setDelegate`).
- One `WidgetObserver` subclass for the widget root if any Tier-3
  event is declared (installed via `Widget::addObserver`).
- For Tier 4: a `NativeEventProcessor` registered on the view's
  `NativeEventEmitter`, dispatching by
  `NativeEvent::EventType`.

Each emitted delegate holds a back-pointer typed as the author
class (`Toolbar *parent;` in §18.1's example) so that it can
invoke the annotated handler directly.

### 11.2 Tier 1 — pointer and keyboard

| WML event | `ViewDelegate` hook | Payload struct |
|---|---|---|
| `on:mouseenter` | `onMouseEnter` | `Native::CursorEnterParams` |
| `on:mouseexit` | `onMouseExit` | `Native::CursorExitParams` |
| `on:leftmousedown` | `onLeftMouseDown` | `Native::LMouseDownParams` |
| `on:leftmouseup` | `onLeftMouseUp` | `Native::LMouseUpParams` |
| `on:rightmousedown` | `onRightMouseDown` | `Native::RMouseDownParams` |
| `on:rightmouseup` | `onRightMouseUp` | `Native::RMouseUpParams` |
| `on:keydown` | `onKeyDown` | `Native::KeyDownParams` |
| `on:keyup` | `onKeyUp` | `Native::KeyUpParams` |

Tier-1 handlers MAY return `bool`. When a Tier-1 handler returns
`true`, the compiler-generated dispatcher MUST suppress any
synthesized Tier-2 event that would otherwise fire for the same
input. (This is the WML equivalent of DOM `event.preventDefault()`,
limited to Tier-1.)

### 11.3 Tier 2 — interaction-state synthetic events

These events are synthesized by `WidgetInteractionDelegate` from
raw mouse and keyboard transitions. They are invoked on the
**state transition**, not on every underlying event.

| WML event | Fires when | Payload |
|---|---|---|
| `on:click` | `LMouseUp` arrives while state == `Hovered`. | `Native::MouseEventParams` |
| `on:rightclick` | `RMouseDown` arrives while state == `Hovered` or `Focused`. | `Native::MouseEventParams` |
| `on:press` | State transitions to `Pressed`. | none |
| `on:release` | State transitions out of `Pressed`. | none |
| `on:hover` | State transitions to `Hovered`. | none |
| `on:unhover` | State transitions out of `Hovered`. | none |
| `on:focus` | State transitions to `Focused`. | none |
| `on:blur` | State transitions out of `Focused`. | none |

Tier-2 handlers MUST return `void`.

### 11.4 Tier 3 — widget-lifecycle events

| WML event | `WidgetObserver` hook | Payload |
|---|---|---|
| `on:mount` | `onWidgetAttach` | `WidgetPtr` (parent) |
| `on:unmount` | `onWidgetDetach` | `WidgetPtr` (former parent) |
| `on:resize` | `onWidgetChangeSize` | old `Composition::Rect`, new `Composition::Rect` |
| `on:show` | `onWidgetDidShow` | none |
| `on:hide` | `onWidgetDidHide` | none |

Tier-3 handlers MUST return `void` and MUST NOT block. They are
invoked from inside the engine's lifecycle hooks.

### 11.5 Tier 4 — native gesture / scroll / window events

| WML event | `EventType` | Payload |
|---|---|---|
| `on:scroll` | `ScrollWheel` | `Native::ScrollParams` |
| `on:scrollup` / `on:scrolldown` / `on:scrollleft` / `on:scrollright` | `ScrollUp` / `ScrollDown` / `ScrollLeft` / `ScrollRight` | `Native::ScrollParams` |
| `on:dragstart` | `DragBegin` | `Native::MouseEventParams` |
| `on:drag` | `DragMove` | `Native::MouseEventParams` |
| `on:dragend` | `DragEnd` | `Native::MouseEventParams` |
| `on:pinch` | `GesturePinch` | gesture payload |
| `on:pan` | `GesturePan` | gesture payload |
| `on:rotate` | `GestureRotate` | gesture payload |
| `on:loaded` | `HasLoaded` | `Native::ViewHasLoaded` |
| `on:windowwillclose` | `WindowWillClose` | none |
| `on:windowresize` | `WindowHasResized` | `Native::ViewResize` |
| `on:appactivate` / `on:appdeactivate` | `AppActivate` / `AppDeactivate` | none |
| `on:scalechange` | `WindowScaleFactorChanged` | `Native::WindowScaleFactorChangedParams` |

Window- and app-tier events (`on:windowwillclose`,
`on:windowresize`, `on:appactivate`, `on:appdeactivate`,
`on:scalechange`) MUST be accepted only on `<app>` and on the root
widget of an `AppWindow`. The compiler MUST reject them on child
widgets.

Tier-4 handlers MUST return `void`.

### 11.6 Handler signature resolution

The handler string is **always a bare C++ identifier**. The
identifier MUST name a method on the enclosing widget's author
class that has been annotated with `OMEGAWTK_WML_HANDLER` (§7.5).
The compiler MUST consult the class's `.wmlh` handler table — not
the C++ header directly — and reject any handler reference that
does not appear there.

Given a matching handler entry, the compiler MUST resolve the
call shape from the event's tier table as follows:

- If the recorded method takes no parameters, the compiler MUST
  emit a call to `widget->handler()`.
- If the recorded method takes one parameter and the type matches
  the event's payload struct, the compiler MUST emit a call to
  `widget->handler(payload)`.
- If the recorded method takes one parameter but the type does
  not match, the compiler MUST report a type-mismatch error.
- If the recorded method takes more than one parameter, the
  compiler MUST report a signature error.

For Tier-1 events whose handler is declared `bool`-returning (per
§11.2), the compiler MUST emit code that respects the return
value's preventDefault semantics.

Call-form handler strings (`on:click="save(item.id)"`,
`on:click="count++"`) MUST be rejected by the compiler.

### 11.7 Custom signals

A custom signal is declared in C++ via `OMEGAWTK_WML_SIGNAL(name)`
or `OMEGAWTK_WML_SIGNAL_T(name, T)` on the author class (§7.4).
The Header Extractor records each signal in the `.wmlh`.

Parents subscribe via `on:name="handler"` on a child custom-widget
instantiation. The WML compiler MUST validate that `name` appears
in the target class's `.wmlh` signal table, and MUST emit a
subscription call against the generated slot list at look-function
construction.

The signal MUST fire from the author class via `emit_name(...)`,
which the macro provides. WML MUST NOT contain `emit(...)` syntax;
emission is behavior.

### 11.8 Pointer routing

Pointer events follow the engine's hover-dispatcher model: the OS
delivers the event to the single per-window `NativeItem`;
`WidgetTreeHost` walks the virtual scene tree top-down using
`View::containsPoint` to find the deepest hit, then dispatches
through the matched view's `ViewDelegate`.

WML does not alter this routing. It only generates the delegate.

`hit-test: invisible` (a `LayoutStyle` field) skips the view during
the top-down walk; `pointer-events: none` is an accepted alias.

---

## 12. State-Based Styling

### 12.1 Built-in pseudo-classes

WML exposes the engine's four built-in `InteractiveState` values
(`Hovered`, `Pressed`, `Focused`, `Disabled`) as CSS pseudo-classes.
The default state is `Idle` (no pseudo-class matches).

| WML pseudo-class | `InteractiveState` value |
|---|---|
| `:hover` | `Hovered` |
| `:pressed` | `Pressed` |
| `:focused` | `Focused` |
| `:disabled` | `Disabled` |

```css
Button:hover    { background: #444; }
Button:pressed  { scale: 0.97; }
Button:focused  { border-color: var(--accent); }
Button:disabled { opacity: 0.5; }
```

`:checked` and `:selected` MUST NOT be accepted; neither maps to an
engine state bit. Authors use `:state(checked)` (§12.2) instead.

A state transition sets `DirtyBit::Style` on the affected widget's
root view; the resolver re-cascades the dirty subtree.

### 12.2 Custom states

Custom states are declared in C++ on the author class with
`OMEGAWTK_WML_STATE(X)` (§7.3) and matched in WML selectors as
`:state(X)`.

```cpp
// DownloadButton.h
class DownloadButton : public Widget {
public:
    OMEGAWTK_WML_WIDGET(DownloadButton)

    OMEGAWTK_WML_STATE(downloading)
    OMEGAWTK_WML_STATE(complete)

    OMEGAWTK_WML_PROPERTY(labelText, OmegaCommon::UString)

    OMEGAWTK_WML_HANDLER void startDownload();
};
```

```html
<!-- DownloadButton.wml -->
<widget name="DownloadButton">
  <style>
    DownloadButton:state(downloading) { background: #ffaa22; }
    DownloadButton:state(complete)    { background: #2ecc71; }
  </style>

  <Button on:click="startDownload">
    <Label text="{labelText}" />
  </Button>
</widget>
```

The C++ `DownloadButton` class is responsible for:

- Calling `setDownloading(true)` / `setComplete(true)` (generated
  by `OMEGAWTK_WML_STATE`) to flip state bits.
- Setting `labelText` via `setLabelText(...)` (generated by
  `OMEGAWTK_WML_PROPERTY`) when the displayed string should change.
  An author preferring a computed-property pattern MAY override
  `labelText()` by hand and decline the macro; the markup binding
  `{labelText}` will then call the hand-written getter.

Flipping a custom state through the generated setter MUST set
`DirtyBit::Style` on the widget's root view, per §7.3.

---

## 13. Responsive Rules

WML supports CSS-like `@media` queries. The compiler MUST wire
media queries to global stylesheet activation: when the predicate
flips at runtime, the affected rule subset is enabled or disabled
and the root view is marked with `DirtyBit::Style`.

### 13.1 Size predicates

```css
@media width < 600px {
  UserCard { width: 100%; padding: 12px; }
  .actions { layout: column; }
}

@media width >= 900px {
  .dashboard { layout: grid; grid-template-columns: 1fr 1fr 1fr; }
}
```

### 13.2 Engine-aware predicates

The compiler MUST accept the following engine-aware media predicates:

```css
@media pointer == touch    { … }
@media platform == desktop { … }
@media platform == mobile  { … }
@media input == gamepad    { … }
@media theme == dark       { … }
```

The set of accepted predicate kinds is closed. The compiler MUST
reject unknown predicate kinds.

---

## 14. Animation

WML supports two animation surfaces. Both MUST compile to operations
on the per-window `AnimationScheduler`
(see [Animation-Scheduler-Plan.md](./Animation-Scheduler-Plan.md)).

### 14.1 Transitions

```css
Button.primary {
  background: var(--accent);
  transition: background 160ms ease, transform 120ms ease;
}

Button.primary:hover {
  background: var(--accent-hover);
  transform: translateY(-1px);
}
```

The compiler MUST emit a `LayoutTransitionSpec` (for layout
properties) or an animated `Style::Entry` (for visual properties)
on the matching `StyleRule`.

When `StyleResolver` detects a delta on a transitioned property in
Phase 2 (Style), it MUST call `AnimationScheduler::transition(...)`
through the resolver's friend hook. The scheduler writes
interpolated values into its `(NodeId, PropertyKey)` side table
during Phase 1 (Tick) of each subsequent frame; Paint reads the side
table.

#### 14.1.1 Retargeting

If a transition is already running for `(node, property)` when a
new target is set, the new target MUST retarget the in-flight
animation: the current sampled value becomes the new `from` value
and progress resets to `0` of the new transition. This is standard
CSS retargeting.

### 14.2 Keyframes

```css
@keyframes popIn {
  from { opacity: 0; scale: 0.92; }
  to   { opacity: 1; scale: 1; }
}

.modal { animation: popIn 180ms ease-out; }
```

The compiler MUST emit, for each `@keyframes` block, a
`KeyframeTrack<T>` per animated property and MUST issue
`scheduler.animateProperty(...)` when the element receives the
matching class.

`:enter` / `:exit` state-driven animations compile to scheduler
calls fired by the widget lifecycle hooks:

```css
Panel:enter { animation: fadeIn 120ms ease-out; }
Panel:exit  { animation: fadeOut 100ms ease-in; }
```

Unlike transitions, keyframes MUST NOT retarget — re-issuing an
animation cancels the prior track and starts a fresh one.

### 14.3 Property keys

Animation property names in WML MUST map to `PropertyKey` values on
the scheduler (`BackgroundColor`, `Opacity`, `TransformX/Y`,
`TransformScaleX/Y`, `TransformRotation`, `LayoutX/Y/Width/Height`,
etc.). Animating a layout-affecting key MUST dirty `Layout | Paint`
on the target node; animating a visual key MUST dirty `Paint` only.

Brush animation rules:

- Solid color brushes MUST interpolate the underlying `Color`.
- Gradient brushes MUST interpolate each stop.
- Bitmap brush animation MUST be rejected in Tier-1 conforming
  compilers.

---

## 15. Conditional Rendering

### 15.1 `<if>` / `<else>`

```html
<if condition="{user.isAdmin}">
  <Button on:click="openAdminPanel"><Label text="Admin Panel" /></Button>
</if>

<else>
  <Label text="Standard User" />
</else>
```

`condition` MUST be a dotted-path binding (§10.1) whose runtime
type is `bool`.

### 15.2 `<for>`

```html
<for each="item in inventory">
  <InventoryItem item="{item}" />
</for>

<for each="item, index in inventory">
  <InventoryItem item="{item}" index="{index}" />
</for>
```

`each` MUST be a binding of the form `name in path` or
`name, index in path`, where `path` is a dotted-path read whose
runtime type is an iterable collection.

### 15.3 `<switch>`

```html
<switch value="{screen}">
  <case value="home">     <HomeScreen /> </case>
  <case value="settings"> <SettingsScreen /> </case>
  <default>               <NotFoundScreen /> </default>
</switch>
```

`<switch value>` MUST be a dotted-path binding. `<case value>` MUST
be a literal of the same type.

### 15.4 Subtree-rebuild semantics

When a predicate flips or a bound collection changes, the compiler
MUST emit code that:

1. Calls `rebuildContent()` on the enclosing widget, which
   rewrites the affected branch of the scene tree.
2. Sets `DirtyBit::Content | Style | Layout | Paint` on the
   parent view.
3. Relies on the next `FrameBuilder` pass to render the change.

Conditionals and loops MUST NOT mutate the tree synchronously
outside the frame lifecycle.

---

## 16. Look Extension

WML has no concept of "widget inheritance" of its own. Widget
identity is a C++ concern: a subclass `FancyCard : public UserCard`
inherits the look the same way it inherits any other behavior —
the inherited `rebuildContent()` calls the inherited
`applyUserCardLook(*this)`, with `*this` upcasting to `UserCard &`.

What WML *does* offer, separately from C++ inheritance, is **look
extension**: the ability for one `<widget>` block to layer
additional styles and slot fills on top of another widget's look
without restating its element tree.

```cpp
// FancyCard.h
class FancyCard : public UserCard {
public:
    OMEGAWTK_WML_WIDGET(FancyCard)
    // FancyCard adds no new WML-visible surface beyond UserCard's.
};
```

```html
<!-- FancyCard.wml -->
<widget name="FancyCard" extends-look="UserCard">
  <style>
    FancyCard       { border: 2px solid var(--accent); }
    FancyCard:hover { transform: translateY(-1px); }
  </style>

  <template slot="header">
    <Badge label="VIP" />
  </template>
</widget>
```

### 16.1 Semantics

A widget block that declares `extends-look="X"` MUST compile to a
look function `applyFancyCardLook(FancyCard &)` whose body, in
order:

1. Calls `applyUserCardLook(static_cast<UserCard &>(fancyCard))`
   — building the parent's element tree and wiring its bindings
   and delegates.
2. Registers the extension's additional `<style>` rules into the
   widget kind's `StyleSheet` (§17.2), scoped to the extending
   kind name (`FancyCard`).
3. Fills any `<slot>` placeholders left open by the parent's look
   with the extension's `<template slot="…">` content.

The WML compiler MUST verify that `X` in `extends-look="X"`
resolves to a `.wmlh` declaration (§5.4). The C++ author class
for the extending widget is NOT required to inherit from `X` —
look extension and C++ inheritance are independent — but the
typical case is that it does, and the static_cast at step 1
relies on it.

### 16.2 What look extension may contain (v0.2)

For this revision of the spec, an `extends-look` widget's body
MUST be limited to:

- Zero or one `<style>` blocks. Rules cascade with the parent's
  per §17.
- Zero or more `<template slot="…">` blocks filling slots the
  parent declared.

A `<widget extends-look="X">` MUST NOT contain a body root
element other than `<style>` and `<template slot="…">`. In
particular, look extension MUST NOT modify, reorder, replace, or
remove elements from the parent's element tree; the only
tree-level surface is the parent's declared slots.

Element-level overrides (e.g. "target the parent's element by
class and replace its subtree") are reserved for a future
revision; see Annex B.

### 16.3 The `extends` attribute (removed)

The `extends="X"` attribute previously documented in this section
is removed from the language. Authors who want a widget to share
behavior with another widget MUST use C++ inheritance on the
author class; authors who want a widget to share look with
another widget MUST use `extends-look="X"`.

---

## 17. Stylesheet Scope

WML has three style surfaces, each with a different scope. The
compiler MUST emit each into the appropriate place on the
`Application::stylesheetStack()` and the cascade (§8.5) MUST
resolve them in the order specified below.

### 17.1 Surfaces

| Surface | Source | Scope |
|---|---|---|
| Inline widget styles | `<style>` block inside `<widget name="X">` | Kind-scoped: all selectors in the block are interpreted as if prefixed with `X`. |
| Global stylesheets | `.wmls` files (§5.5) | Unscoped: selectors match across the whole widget tree. |
| Theme variables | `.wtheme` files / `<theme>` blocks (§9) | Affect `var()` lookups; not stylesheets themselves. |

### 17.2 Kind-scoped inline `<style>` blocks

Every `<style>` block inside a `<widget name="X">` MUST compile to
**one `StyleSheet` per widget kind** `X`. The compiler MUST:

1. Emit the `StyleSheet` as a static (or once-initialised) member
   of the generated `X` class.
2. Register the `StyleSheet` on `Application::stylesheetStack()`
   the first time an instance of `X` is constructed.
3. Scope class selectors and bare pseudo-class selectors by the
   kind name `X`:
   - `X { … }`, `X.class { … }`, `X:hover { … }` — accepted
     as-is.
   - `.class { … }` — silently rewritten to `X.class { … }`.
   - `:hover { … }`, `:state(name) { … }` — silently rewritten
     to `X:hover { … }` / `X:state(name) { … }`.
4. **NOT** rewrite ID selectors. `#submit { … }` inside a
   `<widget X>` style block remains `#submit { … }` because IDs
   are application-globally unique (§5.6.1). An ID selector
   already names a unique widget; kind-scoping it would be
   redundant.

This is the CSS / Chromium model.

### 17.3 Global `.wmls` stylesheets

`.wmls` files compile to `StyleSheet`s registered on the
application stack at load time. Selectors are NOT scoped to any
widget kind. Authors who want per-kind rules MUST qualify the
selector (`Button.primary { … }`, not `.primary { … }`).

`.wmls` is the right place for:

- Cross-cutting rules applied to built-in widget kinds (`Button`,
  `Slider`, `Label`) without modifying their `.wml` definitions.
- Reusable utility classes (`.danger`, `.muted`, `.elevated`).
- Application-wide typographic and spacing defaults.

Inline widget `<style>` blocks are the right place for styles
intrinsic to one widget kind.

### 17.4 Cascade order across surfaces

The cascade resolves visual properties in this order (later entries
override earlier at equal specificity):

1. **Theme variables** — `var(--name)` references resolved against
   `Application::themeVars()` for the active theme.
2. **Global `.wmls` stylesheets** — in registration order.
3. **Kind-scoped inline `<style>` blocks** — for the widget's kind
   and every ancestor kind in the kind hierarchy.
4. **Inline attributes on the tag** — `<Button background="red">`
   layers on top of stylesheet rules (unless those rules are
   `!important`).
5. **State pseudo-class rules** — `:hover`, `:pressed`, `:focused`,
   `:disabled`, `:state(name)` rules apply within their respective
   level above; they do not form a separate cascade level.

Standard CSS specificity rules apply within each level (§8.5.1).
An ID selector (`#submit`) beats any class-and-kind combination
within the same level; a `.wmls` rule with an ID selector beats a
kind-scoped inline rule with only kind and class parts because ID
selectors out-specify them — and yet a kind-scoped inline rule
with an ID selector beats a `.wmls` rule with only kind and class
parts even if `.wmls` is later in registration order, because
specificity is checked before order.

---

## 18. Compiled Runtime Model *(informative)*

This section illustrates what a conforming compiler emits for
typical WML constructs. It is informative; the normative output
contract is defined by the per-construct rules in earlier sections.

### 18.1 A single button

The author writes a `Toolbar` class:

```cpp
// Toolbar.h
class Toolbar : public Widget {
public:
    OMEGAWTK_WML_WIDGET(Toolbar)

    OMEGAWTK_WML_HANDLER void save();
};
```

```cpp
// Toolbar.cpp
void Toolbar::save() { /* author body */ }
```

The Header Extractor emits `Toolbar.wmlh`:

```html
<widget name="Toolbar" extends="Widget">
  <handler name="save" />
</widget>
```

The author writes `Toolbar.wml`:

```html
<widget name="Toolbar">
  <Button class="primary" on:click="save">
    <Label text="Save" />
  </Button>
</widget>
```

The WML compiler emits `Toolbar.wml.gen.cpp`. Inside that file,
the look function body is:

```cpp
void applyToolbarLook(Toolbar &self) {
    auto button = Button::Create(buttonRect);
    button->setClass("primary");

    auto buttonDelegate = Core::makeUnique<ToolbarButtonDelegate>();
    buttonDelegate->parent = &self;
    button->viewRef().setDelegate(buttonDelegate.get());
    self._toolbar_wml_buttonDelegate = std::move(buttonDelegate);   // friend access

    auto label = Label::Create(labelRect, LabelProps{
        .text = OmegaCommon::UString("Save"),
    });
    button->addChild(label);
    self.addChild(button);
}
```

The WML compiler also emits, in the same generated translation
unit, the delegate type:

```cpp
struct ToolbarButtonDelegate : public WidgetInteractionDelegate {
    Toolbar *parent;
    void onLeftMouseUp(Native::NativeEventPtr ev) override {
        WidgetInteractionDelegate::onLeftMouseUp(ev);
        if (getState() == InteractiveState::Hovered)
            parent->save();
    }
};
```

And one `Style::Entry` plus one `StyleRule` for the `.primary`
class:

```cpp
auto buttonStyle = Style::Create();
buttonStyle->backgroundColor(button->viewAs<UIView>().tag(),
                             /* var(--accent) → */ Color::FromHex(0x4f7cff));
buttonStyle->elementRoundedCorner(button->viewAs<UIView>().tag(), 12.f);
button->viewAs<UIView>().setStyle(buttonStyle);
```

`OMEGAWTK_WML_WIDGET(Toolbar)` in the class body expands to (among
other things) an override:

```cpp
void rebuildContent() override { applyToolbarLook(*this); }
```

so the author never has to call the look function explicitly. After
Phase 2 (Style) of the frame lifecycle, the button's root view has
a `ComputedStyle` populated by the cascade. Phase 4 (Paint) emits
one `DrawOp::RoundedRect` to the per-window `DisplayList`.

### 18.2 Counter demo

The author writes a `DemoApp` class:

```cpp
// DemoApp.h
class DemoApp : public App {
public:
    OMEGAWTK_WML_WIDGET(DemoApp)

    OMEGAWTK_WML_PROPERTY(count, int, 0)

    OMEGAWTK_WML_HANDLER void increment();
    OMEGAWTK_WML_HANDLER void decrement();
};
```

```cpp
// DemoApp.cpp
void DemoApp::increment() { setCount(count() + 1); }
void DemoApp::decrement() { setCount(count() - 1); }
```

The author writes `DemoApp.wml`:

```html
<app name="DemoApp">
  <theme src="themes/DarkTheme.wtheme" />
  <style src="styles/global.wmls" />

  <style>
    DemoApp {
      background: var(--background);
      color: var(--text);
      layout: column;
      align-items: center;
      justify-content: center;
      gap: 16px;
      width: 100%;
      height: 100%;
    }

    .counter { font-size: 48px; font-weight: 800; }
    .controls { layout: row; gap: 12px; }
  </style>

  <Label class="counter" text="{count}" />

  <HStack class="controls">
    <Button on:click="decrement"><Label text="-" /></Button>
    <Button class="primary" on:click="increment"><Label text="+" /></Button>
  </HStack>
</app>
```

The Header Extractor emits `DemoApp.wmlh` (one `<property>`, two
`<handler>` entries). The WML compiler emits
`DemoApp.wml.gen.cpp` containing `applyDemoAppLook(DemoApp &)` and
the delegate types for the two buttons. The C++ compiler links
the author's `DemoApp.cpp` with the generated file to produce the
final binary.

No `count` field is declared in the markup — it's declared in C++
via `OMEGAWTK_WML_PROPERTY`. The `{count}` binding in the
`<Label>` resolves through the `.wmlh` to `self.count()`; on
change (via `setCount(...)`), the binding fires and the label
re-renders.

---

## 19. Conformance

### 19.1 Compiler requirements

A **conforming WML compiler** MUST:

1. Accept exactly the file extensions enumerated in §5.1.
2. Reject any tag not listed in §6 (built-in) or whose name does
   not resolve to a `.wmlh` produced by the Header Extractor for
   a project author class (§5.4) or in Annex B (reserved).
3. Treat WML tag identifiers case-sensitively and reject
   non-PascalCase forms (§4.1).
4. Route style properties to either `LayoutStyle` (layout) or
   `StyleRule` / `Style::Entry` (visual) per §8.1–§8.2 and never
   conflate the two.
5. Accept Tier-1 compound selectors (§8.3.1) and reject Tier-2
   combinators (§8.3.2) with an error.
6. Reject attribute selectors, structural pseudo-classes,
   `@supports`, `@scope`, `@layer`, and `calc()` (§8.4).
7. Implement the cascade per §8.5: specificity → source order →
   `!important`, with inline `Style` layered on top.
8. Desugar boolean attributes per §8.6 and reject non-boolean
   attribute-to-class desugaring.
9. Accept `var()` and theme variables per §9 and resolve them at
   cascade time via `Application::themeVars()`.
10. Accept binding syntax exactly as specified in §10.1 — dotted
    paths only — and reject all other expression forms inside `{}`.
11. Generate the delegate / observer / processor subclasses per
    §11 in the per-`.wml` `.wml.gen.cpp` output and dispatch
    events through them.
12. Accept handler strings only in bare-identifier form (§11.6),
    reject call-form handlers, and reject any handler identifier
    that does not appear in the target class's `.wmlh` handler
    table (§7.5).
13. Emit one `StyleSheet` per widget kind, registered on
    `Application::stylesheetStack()` per §17.
14. Emit per-property animation tracks through the per-window
    `AnimationScheduler` per §14.
15. Enforce frame-lifecycle integration on every mutation path:
    bindings, state changes, conditional re-renders, and animation
    triggers MUST set `DirtyBit`s and rely on `FrameBuilder` to
    process them.
16. Resolve every PascalCase tag against the union of SDK-shipped
    built-in `.wmlh` headers and extractor-emitted project `.wmlh`
    headers before emitting code (§5.4.4). The WML compiler MUST
    NOT itself emit `.wmlh` files.
17. Emit, for every `.wml` file compiled, exactly one
    `<source>.wml.gen.cpp` containing a free function
    `applyXLook(X &)` (where `X` is the bound author class) plus
    any delegate / observer / processor types it depends on. The
    WML compiler MUST NOT emit a class declaration, a header
    file, or a constructor for `X`.
18. Reject `<widget>` and `<app>` blocks that contain `<property>`,
    `<state>`, or `<signal>` child elements (§5.3). Surface
    declarations live on the author class via the macros in §7;
    declaring them in WML is a compile error.
19. Reject any `.wmlh` file whose `<widget>` element contains a
    body root, a `<style>` block, or `on:` attributes (§5.4.1).
20. Reject any `.wml` file whose `<widget name="X">` (or `<app
    name="X">`) does not resolve to a `.wmlh` declaring
    `OMEGAWTK_WML_WIDGET(X)` for an author class accessible to
    the build (`unbound widget` error, §5.3).
21. Load `.wmls` files into `Application::stylesheetStack()` at
    load time, unscoped (§5.5.4). Bare selectors in a `.wmls` file
    MUST NOT be silently rewritten to scope by kind.
22. Apply the cascade in the order specified in §17.4: theme vars
    → global `.wmls` rules → kind-scoped inline `<style>` rules →
    inline attributes. Specificity rules apply within each level.
23. Accept `<style src="…">` only inside `<app>` blocks. Reject
    `<style src="…">` inside `<widget>` blocks; widget-local
    styling MUST use an inline `<style>` element (§5.5.3).
24. Validate the uniqueness of every literal `id` value across all
    `.wml` files in the translation unit (§5.6.1). Duplicate
    literal IDs MUST be reported as compile errors, accounting
    for expansions of multi-instance widget bodies (§5.6.3).
    Bound IDs (`id="{path}"`) are out of scope for static
    uniqueness validation; the compiler MAY emit a runtime check.
25. Compute selector specificity as the tuple `(ids, class+pseudo,
    kinds)` and resolve same-level cascade by specificity → source
    order → `!important` (§8.5.1).
26. For `<widget extends-look="X">` (§16): emit a look function
    whose first action is a call to `applyXLook(static_cast<X
    &>(self))`. Reject any extension whose body contains anything
    other than a `<style>` block or `<template slot="…">` blocks
    in v0.2.

### 19.2 Header Extractor requirements

A **conforming Header Extractor** MUST:

1. Scan a configurable set of C++ header files and recognize the
   `OMEGAWTK_WML_WIDGET`, `OMEGAWTK_WML_PROPERTY`,
   `OMEGAWTK_WML_STATE`, `OMEGAWTK_WML_SIGNAL`,
   `OMEGAWTK_WML_SIGNAL_T`, and `OMEGAWTK_WML_HANDLER` macros
   without requiring a full C++ parse (§5.7.1).
2. Emit one `.wmlh` per `OMEGAWTK_WML_WIDGET(X)` invocation,
   named `X.wmlh`, listing every property / state / signal /
   handler declared on the same class with matching name, type,
   and payload (§5.4.3).
3. Reject duplicate `OMEGAWTK_WML_WIDGET(X)` across the project
   (§5.7.1).
4. Reject `OMEGAWTK_WML_PROPERTY` / `_STATE` / `_SIGNAL` /
   `_HANDLER` invocations outside a class that declares
   `OMEGAWTK_WML_WIDGET` (§5.7.1).
5. NOT emit C++ source code and NOT modify the author's headers.

### 19.3 Compiler MAY

A conforming WML compiler MAY:

- Emit additional diagnostics, warnings, or hints beyond the
  required errors.
- Cache parsed style sheets across translation units.
- Emit a combined translation unit per directory of `.wml` files
  instead of one `.wml.gen.cpp` per source, so long as the
  per-`.wml` look-function symbols remain available to the C++
  linker.
- Accept additional ergonomic file layouts (e.g. a single `.wml`
  declaring multiple sibling `<widget>` blocks) so long as the
  rest of this spec is honored.

### 19.4 Author requirements

A conforming WML author MUST:

- Provide a C++ author class with `OMEGAWTK_WML_WIDGET(X)` for
  every `.wml` file whose top-level element is `<widget name="X">`
  or `<app name="X">`, in a header reachable by the Header
  Extractor.
- Declare every WML-visible property, state, signal, and handler
  using the macros in §7. Public C++ methods that are not
  annotated with `OMEGAWTK_WML_HANDLER` MUST NOT be referenced
  from WML `on:*` attributes.
- Ensure that the C++ type of every `OMEGAWTK_WML_PROPERTY` and
  `OMEGAWTK_WML_SIGNAL_T` is reachable from the project's C++
  include path at compile time.
- Treat reserved identifiers (Annex B) as forbidden until their
  corresponding C++ widgets land.
- Treat extractor-emitted `.wmlh` files (§5.4.3) as build
  artifacts. Authors MUST NOT hand-edit them; the C++ header is
  the source of truth for an author class's WML surface.
- Treat SDK-shipped `.wmlh` files (§5.4.2) as read-only.

### 19.5 Engine requirements

The engine is not the target of WML conformance; the engine is the
target of the compiled output. The engine MUST continue to honor
the contracts in the normative references in the front matter for
WML's output to be valid, and MUST provide the
`OMEGAWTK_WML_WIDGET` / `_PROPERTY` / `_STATE` / `_SIGNAL` /
`_SIGNAL_T` / `_HANDLER` macros with the expansions described in
§7.

---

## 20. Annex A — Bibliography *(informative)*

**Engine documents** (normative for WML's output target):

- [UIModel.rst](./UIModel.rst)
- [Style-StyleSheet-Refactor-Plan.md](./Style-StyleSheet-Refactor-Plan.md)
- [Animation-Scheduler-Plan.md](./Animation-Scheduler-Plan.md)
- [Widget-View-Paint-Lifecycle-Plan.md](./Widget-View-Paint-Lifecycle-Plan.md)
- [UIView-Render-Redesign-Plan.md](./UIView-Render-Redesign-Plan.md)
- [NativeViewHost-Adoption-Plan.md](./NativeViewHost-Adoption-Plan.md)

**Engine headers** (the C++ surface WML compiles into):

- `wtk/include/omegaWTK/UI/Widget.h`
- `wtk/include/omegaWTK/UI/View.h`
- `wtk/include/omegaWTK/UI/UIView.h`
- `wtk/include/omegaWTK/UI/Layout.h`
- `wtk/include/omegaWTK/UI/ScrollView.h`
- `wtk/include/omegaWTK/UI/SVGView.h`
- `wtk/include/omegaWTK/UI/VideoView.h`
- `wtk/include/omegaWTK/UI/NativeViewHost.h`
- `wtk/include/omegaWTK/UI/AppWindow.h`
- `wtk/include/omegaWTK/UI/App.h`
- `wtk/include/omegaWTK/Native/NativeEvent.h`
- `wtk/include/omegaWTK/Widgets/Primatives.h`
- `wtk/include/omegaWTK/Widgets/UserInputs.h`
- `wtk/include/omegaWTK/Widgets/Containers.h`
- `wtk/include/omegaWTK/Widgets/BasicWidgets.h`
- `wtk/include/omegaWTK/Widgets/WidgetTypes.h`

**SDK artifacts** (referenced by §5.4.3):

- `wmlh/<WidgetKind>.wmlh` — one declaration header per built-in
  widget enumerated in §6. Shipped with OmegaWTK; the
  authoritative source of truth for the WML compiler's view of
  built-in widget surfaces.

**External**:

- RFC 2119 — "Key words for use in RFCs to Indicate Requirement
  Levels."

---

## 21. Annex B — Reserved Identifiers

The following tags are **reserved**. A conforming compiler MUST
reject them with a "reserved identifier; not yet available" error.
They will become valid tags only when the corresponding C++ class
exists in `OmegaWTK::` and an addendum to this specification adds
them to §6.

| Reserved tag | Intended C++ class | Status |
|---|---|---|
| `<Checkbox>` | `OmegaWTK::Checkbox` | Engine widget not yet implemented. |
| `<RadioGroup>` | `OmegaWTK::RadioGroup` | Engine widget not yet implemented. |
| `<Toggle>` | `OmegaWTK::Toggle` | Engine widget not yet implemented. |
| `<ProgressBar>` | `OmegaWTK::ProgressBar` | Engine widget not yet implemented. |
| `<WebView>` | `OmegaWTK::WebView` | Engine widget not yet planned. |
| `<Grid>` | (none) | Layout behavior, not a dedicated widget; use `<Container layout-behavior="grid">`. |
| `<AbsoluteStack>` | (none) | Layout behavior; use `<Container layout-behavior="absolute">`. |
| `<Panel>` | (none) | Use `<Container>`. |
| `<canvas>` / `<surface>` | (none) | `CanvasView` is deleted from the engine. Use `<NativeViewHost>`. |
| `<Text>` | (none) | Use `<Label>`. |

The following CSS-toolkit property is reserved:

| Property | Status |
|---|---|
| `native-effect` | Reserved for the SDF effect pipeline + `NativeViewHost` airspace contract. MUST be rejected in Tier-1. |

The following selector pseudo-classes are reserved:

| Pseudo-class | Status |
|---|---|
| `:checked` | Use `:state(checked)`. |
| `:selected` | Use `:state(selected)`. |
| `:nth-child()`, `:first-of-type`, etc. | Structural pseudo-classes (§8.4) are not part of WML. |

---

## 22. Annex C — Document History *(informative)*

| Version | Date | Notes |
|---|---|---|
| 0.1.0 (Normative — Draft 1) | 2026-05-31 | First formal version of WML. Derived from the proposal draft at [`research/widget_markup_language_spec.md`](./research/widget_markup_language_spec.md). All eight Open Questions from §23 of the proposal are resolved as normative rules in this document or as reserved identifiers in Annex B. |
| 0.1.1 | 2026-05-31 | Added widget declaration headers (`.wmlh`, §5.4) and renamed the standalone-stylesheet extension from `.wcss` to `.wmls` (§5.5). Expanded §17 to specify the cascade order across the three style surfaces (theme vars, global `.wmls`, kind-scoped inline `<style>`, inline attributes). Conformance items 16–21 added. |
| 0.1.2 | 2026-05-31 | Formalized universal widget attributes `id` and `class` (§5.6). The `id` attribute identifies a single widget instance application-globally; IDs MUST be unique. ID selectors (`#id`, `Button#id`) are now first-class in the Tier-1 grammar (§8.3.1). Selector specificity computed as `(ids, class+pseudo, kinds)` per §8.5.1. ID selectors inside kind-scoped inline `<style>` blocks are NOT kind-rewritten (§17.2). Conformance items 22–23 added. |
| 0.1.3 | 2026-06-03 | **Author-class binding model.** WML no longer generates widget classes; it attaches a *look* to a hand-written C++ author class. `<widget name="X">` binds to a class `X` that declares `OMEGAWTK_WML_WIDGET(X)`; the WML compiler emits a free function `applyXLook(X &)` instead of a class. The `<property>` / `<state>` / `<signal>` markup elements are removed; properties, states, signals, and handlers are declared in C++ via the macros `OMEGAWTK_WML_PROPERTY`, `OMEGAWTK_WML_STATE`, `OMEGAWTK_WML_SIGNAL` / `_SIGNAL_T`, and `OMEGAWTK_WML_HANDLER` (§7). A new build-time **Header Extractor** tool (§5.7.1) scans C++ headers for these macros and emits `.wmlh` files, which become pure build artifacts; the WML compiler no longer emits them. Widget inheritance is removed; in its place, §16 introduces *look extension* (`extends-look="X"`), restricted to styles + slot fills for v0.2. C++ subclassing of author classes is the supported path for sharing behavior. New §5.7 documents the three-tool build pipeline (Header Extractor → WML compiler → C++ compiler). Conformance restructured: §19.1 (compiler), new §19.2 (Header Extractor), §19.3 (MAY), §19.4 (author), §19.5 (engine). |

### Rationale highlights

For implementers wanting context on why specific rules were chosen,
the proposal draft at
[`research/widget_markup_language_spec.md`](./research/widget_markup_language_spec.md)
preserves the discussion record. The following resolutions are
called out because they shaped the design substantially:

- **Look-only stance (§1.3).** WML defines look; C++ defines
  behavior. The rule rules out scripts, expression bindings,
  and call-form handlers. Every other rule flows from this one.
- **Author-class binding, not class generation (§7).** The C++
  class is the noun; WML is a decoration applied to it. The
  alternative — generating the class from WML — was rejected
  because it would force authors to use a partial-class
  workaround (C++ has no real partial classes) and would put
  member layout, constructors, and base-class choice outside
  the author's hands. Free-function look + macro-declared
  surface keeps WML opt-in and the C++ class first-class.
- **Header Extractor as a separate tool (§5.7).** The WML
  compiler cannot parse C++; the C++ compiler cannot parse WML.
  A small extractor that recognizes only the `OMEGAWTK_WML_*`
  macros bridges the two without putting either through a full
  parse of the other's grammar. Inspired by Qt's MOC and
  Unreal's UnrealHeaderTool.
- **Annotated handlers (§7.5, §11.6).** Requiring
  `OMEGAWTK_WML_HANDLER` annotation lets the WML compiler give
  useful errors for typos and lets refactoring tools see the
  surface. The boilerplate cost is small compared to the cost of
  silent breakage when a method is renamed and a `.wml`
  reference goes stale.
- **Look extension scoped to styles + slots (§16).** Element
  override is genuinely useful but adds spec surface and a real
  design question about identifying parent elements (by id?
  class? path?). Limiting v0.2 to styles + slots lets the
  feature ship while leaving the override design open.
- **PascalCase tags equal C++ class names (§4.1).** Generated
  code is debuggable without translation tables; the markup
  cannot invent widgets the engine doesn't have.
- **Tier-1 selectors only (§8.3).** Combinators are deferred in
  the engine. Accepting them in WML would invite bug reports
  against the engine for selectors that the compiler emitted but
  the engine ignored.
- **One `StyleSheet` per widget kind (§17).** Matches CSS /
  Chromium and lets the cascade do its job. Per-instance sheets
  were considered and rejected as more bookkeeping for no clear
  authoring benefit.
- **Four-tier events (§11).** Engine surfaces are heterogeneous
  (`ViewDelegate` for raw input, `WidgetInteractionDelegate` for
  click synthesis, `WidgetObserver` for lifecycle,
  `NativeEventProcessor` for gestures/window). Pretending they
  are uniform would either underexpose or overcomplicate; tiers
  make the surface explicit.
- **`.wmlh` declaration headers (§5.4).** Built-in widgets are
  defined in C++; without a declaration header the WML compiler
  would have no way to validate `<Button text="…">` against the
  actual `Button` class surface. The `.h` / `.cpp` split is the
  established pattern; `.wmlh` / `.wml` mirrors it. User widgets
  get their `.wmlh` auto-emitted so authors don't pay the
  boilerplate cost.
- **`.wmls` global stylesheets (§5.5, §17).** Per-widget inline
  `<style>` blocks are co-located with their widget and
  kind-scoped automatically — perfect for styles intrinsic to one
  widget. Cross-cutting rules ("every `Button` gets this padding";
  "the `.danger` class applies to any widget kind") need an
  unscoped, application-level surface. `.wmls` provides exactly
  that, registered on `Application::stylesheetStack()` like any
  other style sheet.
- **App-globally unique IDs (§5.6.1).** HTML / CSS treats IDs as
  document-globally unique; WML mirrors that with the project's
  widget tree as the document. The rule makes `.wmls` selectors
  predictable — `Button#confirm-action` always means one specific
  Button — and makes the compiler's static validation simple
  (one literal-ID set across all sources). The cost is a
  restriction on reusable widgets: a `<widget>` body that uses ID
  literals cannot be instantiated more than once. The escape
  hatch is `id="{props.uniqueId}"`, which delegates uniqueness to
  the runtime. Classes remain the right answer for widget-internal
  styling targets that don't need per-instance addressability.
