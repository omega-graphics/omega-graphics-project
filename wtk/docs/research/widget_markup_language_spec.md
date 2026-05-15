# Widget Markup Language

**Status:** Proposal. WML is the authoring front-end for the OmegaWTK
engine described in [`UIModel.rst`](../UIModel.rst). The engine — its
three authoring surfaces (`Layout` / `Style` / `StyleSheet`), its
five-phase frame lifecycle, and its per-window `AnimationScheduler` —
is authoritative. WML compiles down to those surfaces; it does not
introduce parallel concepts.

**Prerequisite reading:**

- [`UIModel.rst`](../UIModel.rst) — the engine model WML targets.
- [`Style-StyleSheet-Refactor-Plan.md`](../Style-StyleSheet-Refactor-Plan.md) —
  the three-surface authoring split and selector tiers.
- [`Animation-Scheduler-Plan.md`](../Animation-Scheduler-Plan.md) —
  the per-window animation runtime that consumes WML transitions and
  keyframes.

**WML**, the **Widget Markup Language**, is a lightweight HTML-inspired
markup with embedded CSS-like styling for authoring OmegaWTK widgets,
scene trees, and themes. It keeps HTML/CSS familiarity but maps every
construct onto an OmegaWTK concept: tags become `Widget` / `View` /
`UIView` declarations, `<style>` blocks become rules in the global
process-level `StyleSheet`, inline attributes become per-node `Style`
fields, layout properties become per-node `Layout` field assignments,
and animations become tracks on the `AnimationScheduler`.

---

## 0. How WML Maps to the Engine

WML is a compiler front-end, not a runtime. The compiler emits:

| WML construct | Engine output |
|---|---|
| `<widget name="X">` | A `Widget` subclass that builds a `View` (`UIView`) subtree at construction. |
| Child element tags (e.g. `<button>`, `<text>`) | Child `SceneNode`s in the widget's view subtree. |
| Inline attributes (`width="72"`, `color="red"`) | Per-node `Layout` (structural) or `Style` (visual) field assignments. |
| `<style>` block — visual rules | `StyleRule`s registered in the global `StyleSheet` stack. |
| `<style>` block — layout properties | Per-node `Layout` field assignments at instantiation; **never** `StyleRule`s. |
| `<style>` block — `transition: …` | `Transition` records on a `StyleRule` (consumed by `AnimationScheduler` via `StyleResolver`). |
| `<style>` block — `@keyframes` / `animation: …` | `AnimationScheduler` tracks emitted at element construction time. |
| `:hover` / `:pressed` / `:focused` / `:disabled` / `:checked` / `:selected` | Engine pseudo-class state bits on the node. |
| `:state(name)` | Custom state flags on the node. |
| `var(--name)` | Lookup against `Application::themeVars()` at cascade time. |
| `<theme>` block / `.wtheme` file | A `ThemeVars` map registered on `Application`. |
| `{binding}` | Property bindings; resolved by the runtime against the widget's model. |
| `on:event="handler"` | Calls into the widget's input `ViewDelegate` or a signal slot. |
| `<NativeViewHost>` / native embeds (`<Video>`, `<WebView>`) | A `NativeViewHost` view emitting a `DrawOp::NativeContent` carve-out. |

The engine has no awareness of WML. The compiler emits engine types;
the engine consumes them through the lifecycle described in
`UIModel.rst`. Anything WML appears to add (combinators, attribute
selectors, keyframes) is desugared into engine-native primitives at
compile time.

---

## 1. Basic File Structure

```html
<widget name="UserCard">
  <style>
    UserCard {
      width: 320px;
      padding: 16px;
      border-radius: 18px;
      background: #1e1e24;
      color: white;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);
    }

    .name {
      font-size: 22px;
      font-weight: 700;
    }

    .role {
      color: #a8a8b3;
      font-size: 14px;
    }

    Button.primary {
      margin-top: 12px;
      background: #4f7cff;
      color: white;
      border-radius: 12px;
    }

    Button.primary:hover {
      background: #6d91ff;
    }
  </style>

  <column gap="8px">
    <image class="avatar" src="{user.avatar}" width="72" height="72" />

    <text class="name">
      {user.name}
    </text>

    <text class="role">
      {user.role}
    </text>

    <button class="primary" on:click="sendMessage">
      Message
    </button>
  </column>
</widget>
```

What the compiler produces for the example above:

- A `UserCard` subclass of `Widget`. Its `rebuildContent()` populates a
  view subtree of five `SceneNode`s (root, image, two texts, button).
- `Layout` assignments on the root (`width: 320`, `padding: 16`) and on
  the column (`gap: 8`).
- `Style` assignments authored on the root (`backgroundColor`,
  `borderRadius`, `dropShadow`, inherited `textColor`).
- Three `StyleRule`s registered in the local `<style>` block: `.name`,
  `.role`, `Button.primary`, plus the `:hover` variant.

---

## 2. Core Goals

WML is designed to be:

- **HTML-like**, so it is easy to learn.
- **CSS-compatible**, so existing layout and styling knowledge carries over.
- **Engine-faithful**, so every WML construct maps onto a documented
  OmegaWTK type — `Widget`, `View`, `Layout`, `Style`, `StyleSheet`,
  `Transition`, `AnimationScheduler`. WML never invents runtime
  behavior the engine doesn't already provide.
- **Component-based**, so every widget can be reused.
- **Native-friendly**, so it compiles to OmegaWTK on every supported
  platform (the same DisplayList → compositor pipeline runs everywhere).
- **Data-driven**, with built-in bindings and state styles.

---

## 3. Built-In Elements

WML tags map directly to OmegaWTK view kinds. Three categories:

### 3.1 Leaf views

```html
<text>Hello</text>
<button>Click Me</button>
<input placeholder="Username" />
<checkbox checked="{settings.enabled}" />
<slider value="{volume}" min="0" max="100" />
<image src="profile.png" />
<icon name="settings" />
<svg src="logo.svg" />
```

These compile to `UIView` subtrees (or `SVGView` in the case of `<svg>`)
authored with `UIViewLayoutV2` elements at construction time.

### 3.2 Layout containers

```html
<row></row>      <!-- StackLayout, horizontal -->
<column></column><!-- StackLayout, vertical -->
<stack></stack>  <!-- AbsoluteLayout-style overlay -->
<grid></grid>    <!-- FlexLayout (multi-axis) -->
<scroll></scroll><!-- ScrollView -->
<panel></panel>  <!-- bare View with a configurable LayoutManager -->
```

Each container picks a `LayoutManager` for its children:

| Tag | LayoutManager |
|---|---|
| `<row>` / `<column>` | `StackLayout` |
| `<stack>` | `AbsoluteLayout` |
| `<grid>` | `FlexLayout` |
| `<scroll>` | `ScrollView` (built-in subclass; its own clip + offset) |
| `<panel>` | configurable via `layout:` property (`row`, `column`, `grid`, `fill`) |

The `<surface>` and `<canvas>` tags from earlier drafts are **removed**:
`CanvasView` is deleted from the engine (see `UIModel.rst`
*Specialized View Subclasses*). Imperative drawing is not part of WML.

### 3.3 Reusable widget elements

```html
<UserCard user="{currentUser}" />
<ProductTile product="{item}" />
<SettingsPanel />
```

These compile to instantiations of user-authored `Widget` subclasses
(defined elsewhere with `<widget name="…">`).

### 3.4 Native embeds

```html
<NativeViewHost host-id="video-1" />
<Video src="clip.mp4" />              <!-- VideoViewWidget over NativeViewHost -->
<WebView src="https://…" />           <!-- future, same shape -->
```

These compile to `NativeViewHost` views that emit
`DrawOp::NativeContent` carve-outs. Bounds-sync rides
`onLayoutResolved`; paint stays pure.

---

## 4. Styling Model

WML supports CSS in a `<style>` block. The compiler routes each rule to
one of two destinations:

- **Visual properties** (`background`, `border`, `color`, `font-size`,
  `box-shadow`, `transform`, `opacity`, `transition`, …) become
  `StyleRule`s registered in the widget's local `StyleSheet`.
- **Layout properties** (`width`, `height`, `margin`, `padding`, `gap`,
  `layout`, `flex-*`, `inset-*`, `clamp`) become per-node `Layout`
  field assignments at instantiation. They never become cascaded rules.

This is the Slate split, not the CSS conflation. The engine's
`Layout` lives on each `SceneNode` and is read by the Layout phase;
visual `Style` is layered on top of `StyleSheet`-cascaded rules to
produce a `ComputedStyle` the Paint phase reads.

### 4.1 Supported selectors (Tier 1)

The Tier-1 selector grammar is a **single compound** — kind, id, classes,
and pseudo-classes ANDed together:

```css
Button { }
.primary { }
#submitButton { }
Button.primary { }
Button.primary:disabled { }
Button:hover { }
Input:focused { }
Checkbox:checked { }
```

### 4.2 Tier-2 combinators (deferred)

```css
Panel > Button { }   /* child */
Panel  Button { }    /* descendant */
Button + Text { }    /* adjacent sibling */
Button ~ Text { }    /* general sibling */
UserCard .name { }   /* descendant */
```

These are not implemented in Tier 1 of the engine. WML compilers may
parse them, but they will reject or warn until the engine's selector
matcher gains combinator support (see `Style-StyleSheet-Refactor-Plan.md`
§4.2).

### 4.3 What is *not* supported

The engine declines to implement these and the WML compiler must not
emit them:

- Attribute selectors — `[type="text"]`, `[featured="true"]`. WML
  authors use a class instead. The compiler will desugar
  `featured="true"` to `.featured` at instantiation.
- Structural pseudo-classes — `:nth-child()`, `:first-of-type`.
- `@supports`, `@scope`, `@layer`.
- Arbitrary `calc()` arithmetic. Theme `var()` covers the common case.

### 4.4 Cascade

Standard CSS cascade: specificity → source order → `!important`.
Inline `Style` (authored via attributes on a tag) layers on top of any
non-`!important` rule. The `StyleResolver` (Phase 2 of the frame
lifecycle) writes the resolved value into a `ComputedStyle` cache on
each node; Paint reads only `ComputedStyle`.

---

## 5. CSS Toolkit Extensions

WML adds a small set of engine-aware properties:

```css
.card {
  layout: column;          /* picks StackLayout on the node's LayoutManager */
  gap: 12px;
  align-items: center;
  justify-content: center;

  transition: background 160ms ease, scale 120ms ease;
}
```

Extra properties:

```css
layout: row | column | grid | stack | fill;
gap: 12px;
dock: top | bottom | left | right | fill;
hit-test: visible | invisible | children;
pointer-events: auto | none;
cursor: default | pointer | text | …;       /* maps to View::setCursorShape */
theme-color: accent;                          /* shorthand for var(--accent) */
elevation: 3;                                 /* shorthand for a Style::dropShadow preset */
```

`native-effect` (e.g. `blur`) is reserved but not exposed in Tier 1 — it
would require coordination with the SDF effect pipeline and the
`NativeViewHost` airspace contract.

Reminder: `layout`, `gap`, `dock`, `width`, `height`, `padding`,
`margin`, `align-items`, `justify-content` compile to **`Layout`** field
assignments on the node, not to `StyleRule`s. The cascading behavior
CSS implies for layout properties does not apply.

---

## 6. Properties and Data Binding

WML uses `{}` for data binding.

```html
<text>{player.name}</text>
<progress value="{player.health}" max="100" />
<button disabled="{player.health <= 0}">
  Heal
</button>
```

Two-way binding uses `bind:`.

```html
<input bind:value="username" />
<slider bind:value="volume" />
<checkbox bind:checked="settings.darkMode" />
```

One-way binding:

```html
<text>{username}</text>
```

Computed binding:

```html
<text>{player.health + '/' + player.maxHealth}</text>
```

Conditional attributes:

```html
<button disabled="{!canSubmit}">
  Submit
</button>
```

Bindings compile to property updates that — when the bound model
mutates — call into the widget's `rebuildContent()` or a targeted
property setter, set the appropriate `DirtyBits`, and let `FrameBuilder`
process the change on the next frame. Bindings never bypass the
five-phase lifecycle.

---

## 7. Events

Events use the `on:` prefix and route through the view's `ViewDelegate`:

```html
<button on:click="save">
  Save
</button>

<input on:change="validateUsername" />

<panel on:pointerenter="showTooltip" on:pointerleave="hideTooltip">
  Hover me
</panel>
```

Event handlers can pass arguments:

```html
<button on:click="deleteItem(item.id)">
  Delete
</button>
```

Custom widget signals declared via `<signal>` (see §13):

```html
<ColorPicker on:colorChanged="updateTheme" />
```

Pointer routing matches the engine's hover dispatcher: the OS delivers
events to the single per-window `NativeItem`; `WidgetTreeHost` walks
the virtual scene tree top-down using `View::containsPoint` to find the
target.

---

## 8. State-Based Styling

WML exposes the engine's pseudo-class bits directly:

```css
Button:hover    { background: #444; }
Button:pressed  { scale: 0.97; }
Button:focused  { border-color: var(--accent); }
Button:disabled { opacity: 0.5; }
```

These map to the same node state bits described in `UIModel.rst`
*Pseudo-classes and state*. Flipping a bit sets `DirtyBit::Style` for
that node only.

Custom states use `<state>` declarations on the widget and `:state(name)`
in the stylesheet:

```html
<widget name="DownloadButton">
  <state name="idle" />
  <state name="downloading" />
  <state name="complete" />

  <style>
    DownloadButton:state(idle) {
      background: #2f6fff;
    }

    DownloadButton:state(downloading) {
      background: #ffaa22;
    }

    DownloadButton:state(complete) {
      background: #2ecc71;
    }
  </style>

  <button on:click="startDownload">
    {state == "downloading" ? "Downloading..." : "Download"}
  </button>
</widget>
```

A custom state is an arbitrary string flag on a node. Flipping it sets
`DirtyBit::Style`; the resolver re-runs for the dirty node (and any
descendants whose inherited properties changed).

---

## 9. Slots

Slots allow reusable widgets to accept custom content.

```html
<widget name="Card">
  <style>
    Card {
      padding: 16px;
      border-radius: 16px;
      background: var(--surface);
    }
  </style>

  <column>
    <slot name="header" />
    <slot />
    <slot name="footer" />
  </column>
</widget>
```

Usage:

```html
<Card>
  <template slot="header">
    <text class="title">Account</text>
  </template>

  <text>Your account is active.</text>

  <template slot="footer">
    <button>Manage</button>
  </template>
</Card>
```

Slots compile to placeholder child `SceneNode`s replaced at widget
construction with the caller's provided subtree.

---

## 10. Themes and Variables

Theme variables map onto `ThemeVars` registered on `Application`:

```css
:theme {
  --background: #111;
  --surface: #1e1e24;
  --text: #ffffff;
  --accent: #4f7cff;
  --radius-lg: 18px;
}
```

Widgets reference them via `var()`:

```css
Panel {
  background: var(--surface);
  color: var(--text);
  border-radius: var(--radius-lg);
}

Button.primary {
  background: var(--accent);
}
```

`var(--name)` is resolved at cascade time by `StyleResolver`, looking
up the name in `Application::themeVars()`.

Theme variants compile to additional `ThemeVars` maps; the active map
is selected by `Application::setTheme(...)`. A theme swap dirties the
root with `DirtyBit::Style` and the resolver re-cascades:

```css
:theme(light) {
  --background: #f6f6f8;
  --surface: white;
  --text: #111;
}

:theme(dark) {
  --background: #101014;
  --surface: #1e1e24;
  --text: white;
}
```

---

## 11. Responsive Rules

WML supports CSS-like media queries:

```css
@media width < 600px {
  UserCard {
    width: 100%;
    padding: 12px;
  }

  .actions {
    layout: column;
  }
}

@media width >= 900px {
  .dashboard {
    layout: grid;
    grid-template-columns: 1fr 1fr 1fr;
  }
}
```

Media conditions reserved for engine-aware authoring:

```css
@media pointer == touch   { }
@media platform == desktop { }
@media platform == mobile  { }
@media input == gamepad    { }
@media theme == dark       { }
```

The compiler wires media queries to global stylesheet activation: when
the predicate flips, the affected rule subset is enabled or disabled
and the root is dirtied with `DirtyBit::Style`.

---

## 12. Script Section

A widget may include a small logic section:

```html
<script language="wmls">
  property user;

  state expanded = false;

  function toggleExpanded() {
    expanded = !expanded;
  }

  function sendMessage() {
    emit("messageRequested", user.id);
  }
</script>
```

`property` declarations compile to widget constructor parameters /
setters. `state` declarations compile to widget-local model fields that
participate in bindings. `emit()` compiles to `<signal>` slot fires.
Function bodies execute on the UI thread between frames — the engine's
phase enforcement (see `UIModel.rst`) requires that any mutation set
`DirtyBits` rather than synchronously re-running paint.

---

## 13. Full Example: Product Card

```html
<widget name="ProductCard">
  <property name="product" type="Product" />
  <property name="featured" type="bool" default="false" />

  <signal name="buy" payload="Product" />

  <style>
    ProductCard {
      width: 280px;
      layout: column;
      gap: 10px;
      padding: 16px;
      border-radius: 20px;
      background: var(--surface);
      color: var(--text);
      transition: transform 160ms ease, box-shadow 160ms ease;
    }

    ProductCard:hover {
      transform: translateY(-3px);
      box-shadow: 0 10px 28px rgba(0, 0, 0, 0.25);
    }

    ProductCard.featured {
      border: 2px solid var(--accent);
    }

    .image {
      width: 100%;
      height: 160px;
      object-fit: cover;
      border-radius: 14px;
    }

    .title {
      font-size: 18px;
      font-weight: 700;
    }

    .price {
      font-size: 16px;
      color: var(--accent);
    }

    .buy {
      margin-top: 8px;
      background: var(--accent);
      color: white;
      border-radius: 12px;
      padding: 10px 14px;
    }
  </style>

  <column>
    <image class="image" src="{product.image}" />

    <text class="title">
      {product.name}
    </text>

    <text class="price">
      ${product.price}
    </text>

    <button class="buy" on:click="emit('buy', product)">
      Buy Now
    </button>
  </column>
</widget>
```

Usage (note: `featured` desugars to `.featured` class on the instance,
matching the Tier-1 selector model):

```html
<ProductCard
  product="{selectedProduct}"
  featured="true"
  on:buy="addToCart"
/>
```

The compiler emits:

- A `ProductCard` `Widget` subclass with `product` and `featured`
  properties. `setProps` calls `rebuildContent()` and sets
  `DirtyBit::Content | Style | Paint`.
- The local `<style>` block becomes seven `StyleRule`s registered in
  the widget's `StyleSheet` (six visual + the `:hover` variant), plus
  one `Transition` record carrying `(transform, 160ms, ease)` and
  `(box-shadow, 160ms, ease)`. Layout properties (`width`, `padding`,
  `gap`, `margin-top`) compile to per-node `Layout` field assignments
  at instantiation.
- A column subtree of four `SceneNode`s.

---

## 14. Layout Syntax

Tag-based layout:

```html
<row gap="12px" align="center">
  <button>Cancel</button>
  <button class="primary">Save</button>
</row>
```

CSS-based layout:

```html
<panel class="actions">
  <button>Cancel</button>
  <button class="primary">Save</button>
</panel>
```

```css
.actions {
  layout: row;
  gap: 12px;
  align-items: center;
  justify-content: flex-end;
}
```

Both forms compile to identical engine output: a `Panel` node with a
`StackLayout` (horizontal) installed as its `LayoutManager`, and child
buttons whose `Layout` fields encode gap / alignment hints.

---

## 15. Widget Inheritance

Widgets can extend other widgets. WML inheritance is **widget-level
composition**, not view-level subclassing — the engine's `View` is not
designed for inheritance.

```html
<widget name="PrimaryButton" extends="Button">
  <style>
    PrimaryButton {
      background: var(--accent);
      color: white;
      border-radius: 10px;
      padding: 10px 16px;
    }

    PrimaryButton:hover {
      background: var(--accent-hover);
    }
  </style>

  <slot />
</widget>
```

Compiles to a `PrimaryButton` widget that wraps a `Button` widget and
applies additional `StyleRule`s scoped to the `PrimaryButton` kind.

Usage:

```html
<PrimaryButton on:click="save">
  Save Project
</PrimaryButton>
```

---

## 16. Animation Support

WML supports two animation surfaces, both of which compile to
`AnimationScheduler` operations (see `Animation-Scheduler-Plan.md`).

### 16.1 Transitions — declarative property interpolation

`transition: property duration curve, …` in a `<style>` block declares
that when a transitioned property changes between frames, the engine
interpolates from the previous value to the new one.

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

The compiler emits `Transition` records on the `StyleRule`. At runtime,
when `StyleResolver` detects a delta on a transitioned property in
Phase 2 (Style), it calls `AnimationScheduler::transition(...)` through
the resolver's friend hook. The scheduler writes interpolated values
into its `(NodeId, PropertyKey)` side table during Phase 1 (Tick) of
each subsequent frame; Paint reads the side table.

If a transition is already running for `(node, property)`, the new
target retargets the in-flight animation — preserving the current
sampled value as the new `from` and progress as `0` of the new
transition. This is the standard CSS retargeting behavior.

### 16.2 Keyframes — imperative tracks

`@keyframes` and `animation: …` compile to `AnimationScheduler` tracks
issued at element construction (or on a state change):

```css
@keyframes popIn {
  from {
    opacity: 0;
    scale: 0.92;
  }

  to {
    opacity: 1;
    scale: 1;
  }
}

.modal {
  animation: popIn 180ms ease-out;
}
```

The compiler converts each `@keyframes` block into a `KeyframeTrack<T>`
per animated property and issues `scheduler.animateProperty(...)` when
the element receives the `.modal` class.

State-driven animation (`:enter` / `:exit`) compiles to scheduler
calls fired by the widget lifecycle hooks:

```css
Panel:enter { animation: fadeIn 120ms ease-out; }
Panel:exit  { animation: fadeOut 100ms ease-in; }
```

Unlike transitions, keyframes do **not** retarget — re-issuing an
animation cancels the prior track and starts fresh.

### 16.3 Property keys

Animation property names in WML map to `PropertyKey` values on the
scheduler (`BackgroundColor`, `Opacity`, `TransformX/Y`,
`TransformScaleX/Y`, `TransformRotation`, `LayoutX/Y/Width/Height`,
etc.). Animating a layout-affecting key dirties `Layout | Paint` on the
target node; animating a visual key dirties `Paint` only.

Brushes are animated component-wise: solid color brushes interpolate
the underlying `Color`; gradient brushes interpolate each stop; bitmap
brush animation is not supported at Tier 1.

---

## 17. Conditional Rendering

```html
<if condition="{user.isAdmin}">
  <button>Admin Panel</button>
</if>

<else>
  <text>Standard User</text>
</else>
```

Loops:

```html
<for each="item in inventory">
  <InventoryItem item="{item}" />
</for>

<for each="item, index in inventory">
  <InventoryItem item="{item}" index="{index}" />
</for>
```

Switching:

```html
<switch value="{screen}">
  <case value="home">
    <HomeScreen />
  </case>

  <case value="settings">
    <SettingsScreen />
  </case>

  <default>
    <NotFoundScreen />
  </default>
</switch>
```

Conditionals and loops compile to dynamic subtree management: when the
predicate flips or the bound collection changes, the widget's
`rebuildContent()` rewrites the affected branch of the scene tree and
sets `DirtyBit::Content | Style | Layout | Paint` on the parent. The
next `FrameBuilder` pass picks up the change.

---

## 18. Proposed File Types

```text
.wml      Widget markup file
.wcss     Widget CSS file (standalone StyleSheet)
.wtheme   Theme definition file (compiles to ThemeVars)
.wmls     Widget script file
```

Example project:

```text
ui/
  widgets/
    UserCard.wml
    ProductCard.wml
    PrimaryButton.wml

  themes/
    DarkTheme.wtheme
    LightTheme.wtheme

  styles/
    global.wcss
```

`.wcss` files compile to a `StyleSheet` registered on the
`Application` stylesheet stack. `.wtheme` files compile to a
`ThemeVars` map; the active theme is selected via
`Application::setTheme(...)`.

---

## 19. Example Theme File

```css
@theme DarkTheme {
  --background: #101014;
  --surface: #1c1c22;
  --surface-hover: #252532;
  --text: #ffffff;
  --text-muted: #aaaab5;
  --accent: #4f7cff;
  --accent-hover: #6d91ff;
  --danger: #ff4d4f;
  --success: #2ecc71;
  --radius-sm: 8px;
  --radius-md: 12px;
  --radius-lg: 20px;
}
```

Compiles to a `ThemeVars` map (`String → StyleValue`). Registered on
`Application` and made active via `Application::setTheme("DarkTheme")`.

---

## 20. Compiled Runtime Model

WML compiles to OmegaWTK engine types — not to a generic widget JSON.
A concrete example. This:

```html
<button class="primary" on:click="save">
  Save
</button>
```

Compiles to a `SceneNode` declaration roughly equivalent to:

```cpp
auto node = makeSceneNode<Button>();
node->addClass("primary");
node->setDelegate(...);                    // routes on:click → save
node->appendChild(makeTextNode("Save"));
```

Plus, in the enclosing `<style>` block, a `StyleRule`:

```cpp
sheet->rule(Selector::Kind("Button").Class("primary"),
            Property::BackgroundColor,
            StyleValue::Var("accent"));
sheet->rule(Selector::Kind("Button").Class("primary"),
            Property::BorderRadius,
            StyleValue::Px(12));
// …
```

And, at frame time, after Phase 2 runs, the `Button` node has a
`ComputedStyle` populated by the cascade:

```cpp
node->resolved() == ComputedStyle{
    .backgroundColor = Color::FromHex(0x4f7cff),
    .textColor       = Color::White,
    .borderRadius    = 12.f,
    // …
};
```

Phase 4 (Paint) reads `ComputedStyle` and emits one
`DrawOp::RoundedRect` to the per-window `DisplayList`. Phase 5 (Commit)
hands the list to the compositor.

There is no intermediate JSON representation in the engine. The "tree
+ stylesheet" form above *is* the runtime — `SceneNode` instances,
`StyleSheet` rule sets, and a per-node `ComputedStyle` cache.

---

## 21. Small Demo App

```html
<app name="DemoApp">
  <theme src="themes/DarkTheme.wtheme" />
  <style src="styles/global.wcss" />

  <state name="count" type="int" default="0" />

  <style>
    App {
      background: var(--background);
      color: var(--text);
      layout: column;
      align-items: center;
      justify-content: center;
      gap: 16px;
      width: 100%;
      height: 100%;
    }

    .counter {
      font-size: 48px;
      font-weight: 800;
    }

    .controls {
      layout: row;
      gap: 12px;
    }
  </style>

  <text class="counter">
    {count}
  </text>

  <row class="controls">
    <button on:click="count--">
      -
    </button>

    <button class="primary" on:click="count++">
      +
    </button>
  </row>
</app>
```

`<app>` compiles to an `Application` plus a single `AppWindow` carrying
the root widget. `<theme src="…">` registers a `ThemeVars` map and
makes it active. `<style src="…">` loads an additional `StyleSheet`
into the global stack.

---

## 22. Language Summary

**WML** is essentially:

```text
HTML-like markup
+ CSS styling
+ OmegaWTK widget kinds (Widget, View, UIView, LayoutManager,
  NativeViewHost)
+ data binding
+ component properties and signals
+ events routed through ViewDelegate
+ theme variables (ThemeVars)
+ state-driven styling (engine pseudo-classes + :state(name))
+ animations (transitions + keyframes, both driven by
  AnimationScheduler)
```

Everything WML appears to add compiles to engine-native primitives —
`Layout`, `Style`, `StyleSheet`, `Transition`, `KeyframeTrack`,
`SceneNode`. The engine has no special knowledge of WML; it sees only
the constructs documented in `UIModel.rst`.

---

## 23. Open Questions

These are the points where WML still needs developer input before the
compiler can ship:

1. **Per-widget local stylesheets vs. one global stack.** Each
   `<widget>` block defines a `<style>` that is, today, scoped to that
   widget instance's subtree. Should the compiler emit one global
   `StyleSheet` per widget kind (registered on the
   `Application` stack at first use, scoped by the widget's kind name
   in the selector), or per-widget-instance sheets attached to the
   subtree? The first matches CSS / Chromium; the second matches the
   pre-refactor `UIView::setStyleSheet`. `Style-StyleSheet-Refactor-Plan.md`
   §6 question 1 leans global. Recommendation: emit global per-kind
   and trust the cascade.

2. **Combinator timing.** Tier-2 combinators (`>`, ` `, `+`, `~`) are
   present in the WML grammar but deferred in the engine. Should the
   WML compiler reject combinators with a clear error, or accept and
   warn that they are no-ops until the engine ships them?
   Recommendation: hard error in Tier 1. A silent no-op invites bug
   reports against the engine for selectors the WML compiler emitted
   but the engine ignores.

3. **Attribute-to-class desugaring.** `featured="true"` desugaring to
   `.featured` is mechanical, but does it generalize?
   `priority="high"` → `.priority-high`? `state="loading"` →
   `:state(loading)`? Recommendation: only the literal `bool` form
   desugars to a single class; everything else stays as a property
   binding. Magic mappings here will surprise authors.

4. **`{}` binding language semantics.** WML uses `{}` for expressions,
   but the spec doesn't say which subset of expression syntax is
   supported (`||`, ternary, function calls, dotted property access).
   Recommendation: a fixed, documented subset — dotted access,
   comparison, ternary, basic arithmetic, no function calls (those go
   through `on:` handlers). Bindings that need logic call into the
   widget's script section.

5. **`<canvas>` / imperative drawing for game-engine integration.**
   `CanvasView` is deleted from the engine, but OmegaGTE integration
   may still want imperative draw hooks. Recommendation: those use
   cases author a `<NativeViewHost>` (OmegaGTE provides a native
   layer) rather than an in-engine `<canvas>`. The engine's pure-Paint
   contract is worth preserving.

---

## 24. References

- [`UIModel.rst`](../UIModel.rst) — engine surface WML compiles into.
- [`Style-StyleSheet-Refactor-Plan.md`](../Style-StyleSheet-Refactor-Plan.md) —
  the three-surface authoring split, selector tiers, cascade order,
  `ThemeVars`.
- [`Animation-Scheduler-Plan.md`](../Animation-Scheduler-Plan.md) —
  the runtime that consumes WML transitions and keyframes.
- [`Widget-View-Paint-Lifecycle-Plan.md`](../Widget-View-Paint-Lifecycle-Plan.md) —
  the five-phase frame lifecycle that processes WML-driven mutations.
- [`UIView-Render-Redesign-Plan.md`](../UIView-Render-Redesign-Plan.md) —
  `SceneNode`, `DisplayList`, `LayoutManager`.
- [`NativeViewHost-Adoption-Plan.md`](../NativeViewHost-Adoption-Plan.md) —
  contract for `<NativeViewHost>`, `<Video>`, future `<WebView>`.
