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
| Child element tags (e.g. `<Button>`, `<Label>`) | Child `Widget` / `View` instances (PascalCase tag = real C++ class name). |
| Inline attributes (`width="72"`, `color="red"`) | Per-node `LayoutStyle` (structural) or `Style` (visual) field assignments. |
| `<style>` block — visual rules | `StyleRule`s registered in the widget's local style scope. |
| `<style>` block — layout properties | Per-node `LayoutStyle` field assignments at instantiation; **never** `StyleRule`s. |
| `<style>` block — `transition: …` | `LayoutTransitionSpec` / animated `Style::Entry` records consumed by the per-window `AnimationScheduler`. |
| `<style>` block — `@keyframes` / `animation: …` | `AnimationScheduler` tracks emitted at element construction time. |
| `:hover` / `:pressed` / `:focused` / `:disabled` | The four engine `InteractiveState` bits (see [WidgetTypes.h:11](../../include/omegaWTK/Widgets/WidgetTypes.h)) tracked by `WidgetInteractionDelegate`. |
| `:state(name)` | Custom state flags on the widget (compiler-generated bitset). |
| `var(--name)` | Lookup against `Application::themeVars()` at cascade time. |
| `<theme>` block / `.wtheme` file | A `ThemeVars` map registered on `Application`. |
| `{binding}` | Property bindings; resolved by the runtime against the widget's model. |
| `on:<event>="handler"` | Compiler-generated `ViewDelegate` (input) or `WidgetObserver` (lifecycle) subclass that calls the named C++ method on the owning widget. See §7. |
| `<NativeViewHost>` / `<VideoView>` | A `NativeViewHost`-backed view emitting a `DrawOp::NativeContent` carve-out. |

The engine has no awareness of WML. The compiler emits engine types;
the engine consumes them through the lifecycle described in
`UIModel.rst`. Anything WML appears to add (combinators, attribute
selectors, keyframes) is desugared into engine-native primitives at
compile time.

**Naming rule.** WML built-in tags use PascalCase that **exactly
matches the C++ class name in the engine**. `<Button>` is the
`OmegaWTK::Button` class, `<Label>` is `OmegaWTK::Label`, `<HStack>`
is `OmegaWTK::HStack`. There are no HTML-style lowercase aliases —
the tag is the type, and a typo is a compile error. This keeps the
generated C++ debuggable (an `<HStack>` in WML lands in a stack trace
as `HStack`, not as some intermediary) and removes a layer of
indirection the engine would otherwise have to document.

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

  <VStack gap="8px">
    <Image class="avatar" src="{user.avatar}" width="72" height="72" />

    <Label class="name">
      {user.name}
    </Label>

    <Label class="role">
      {user.role}
    </Label>

    <Button class="primary" on:click="sendMessage">
      Message
    </Button>
  </VStack>
</widget>
```

What the compiler produces for the example above:

- A `UserCard` subclass of `Widget`. Its `rebuildContent()` populates
  a child-widget subtree of five `Widget`s (root `UserCard`, one
  `Image`, two `Label`s, one `Button`), parented under a `VStack`.
- `LayoutStyle` assignments on the root (`width: 320`, `padding: 16`)
  and `StackOptions.spacing = 8` on the `VStack`.
- `Style` entries authored on the root `UIView` (`backgroundColor`,
  `borderRadius`, `dropShadow`, inherited `textColor`).
- Three `StyleRule`s registered in the local style scope: `.name`,
  `.role`, `Button.primary`, plus the `:hover` variant.
- One auto-generated `ViewDelegate` subclass for the `Button` that
  routes `LMouseDown → LMouseUp` (a click) to `UserCard::sendMessage`
  (see §7).

---

## 2. Core Goals

WML is designed to be:

- **A look-only language.** WML defines what a widget looks like, how
  it lays out, what state knobs it exposes, and which events it
  surfaces — never *what to do* when an event fires or *how to
  compute* a value. Behavior lives in the C++ widget subclass; WML
  names the C++ entry points but contains no executable bodies, no
  expressions beyond dotted-path state reads, and no scripting
  surface. This is the rule that makes everything else in the spec
  hold its shape.
- **HTML-like**, so it is easy to learn.
- **CSS-compatible**, so existing layout and styling knowledge carries over.
- **Engine-faithful**, so every WML construct maps onto a documented
  OmegaWTK type — `Widget`, `View`, `Layout`, `Style`, `StyleSheet`,
  `Transition`, `AnimationScheduler`. WML never invents runtime
  behavior the engine doesn't already provide.
- **Component-based**, so every widget can be reused.
- **Native-friendly**, so it compiles to OmegaWTK on every supported
  platform (the same DisplayList → compositor pipeline runs everywhere).
- **Data-driven**, with read-only bindings and declarative state styles.

### 2.1 What WML is not

- **Not a scripting language.** There is no `<script>` block, no
  function bodies, no statements, no assignments. The compiler will
  hard-error on any markup that tries to embed executable behavior.
- **Not an expression language.** Bindings are dotted-path reads of
  the C++ model (`{user.name}`, `{player.health}`). Arithmetic,
  ternaries, function calls, and operators are not parsed inside
  `{}` — if you need a derived value, expose it as a property or
  computed accessor on the C++ widget class.
- **Not an event-handler language.** `on:click="save"` names a C++
  method. It is never `on:click="save(item.id)"` or
  `on:click="count++"` — those are behavior. The C++ handler
  receives the event's typed payload (§7) and reads its own model
  for any extra context it needs.

---

## 3. Built-In Elements

WML built-in tags are the exact class names of the C++ types they
construct. Five categories, all rooted in the engine source.

### 3.1 Display primitives — [`Widgets/Primatives.h`](../../include/omegaWTK/Widgets/Primatives.h)

Pure-paint widgets that own a `UIView` subtree describing a single
shape, image, or text run.

```html
<Rectangle fill="{accentBrush}" stroke="{borderBrush}" stroke-width="1" />
<RoundedRectangle fill="{surfaceBrush}" top-left="12" top-right="12"
                   bottom-left="12" bottom-right="12" />
<Ellipse fill="{accentBrush}" />
<Path fill="{brush}" stroke-width="2" close-path="true" d="M0,0 L10,10 …" />
<Separator orientation="horizontal" thickness="1" inset="8" />

<Label text="{user.name}" font="{titleFont}" alignment="leftUpper"
       wrapping="byWord" line-limit="0" />

<Icon token="settings" size="16" tint="{iconColor}" />

<Image src="profile.png" fit="contain" />
```

Each compiles to the matching `XxxProps` struct and the corresponding
`Xxx::Create(rect, props)` (or `setProps(props)` for re-instantiation).
There is no `<Text>` element — text rendering belongs to `<Label>`.

### 3.2 User-input widgets — [`Widgets/UserInputs.h`](../../include/omegaWTK/Widgets/UserInputs.h)

All four are `Container` subclasses, which means they wrap a styleable
`UIView` and can take child widgets (e.g. a `<Label>` inside a
`<Button>`).

```html
<Button on:click="save">
  <Label text="Save" />
</Button>

<TextInput placeholder="Username" bind:value="username" />

<Dropdown bind:selection="region">
  <option value="us">United States</option>
  <option value="eu">European Union</option>
</Dropdown>

<Slider bind:value="volume" min="0" max="100" orientation="horizontal" />
```

Note: the underlying C++ classes are minimal stubs today (constructor
only) — the WML compiler is responsible for wiring `bind:value`,
`placeholder`, and `on:click` semantics through a generated
`WidgetInteractionDelegate` subclass (see §7).
`<Checkbox>` and `<RadioGroup>` are **not** present in the engine and
must not be emitted by the compiler until the corresponding C++
widgets exist.

### 3.3 Layout containers — [`Widgets/Containers.h`](../../include/omegaWTK/Widgets/Containers.h), [`Widgets/BasicWidgets.h`](../../include/omegaWTK/Widgets/BasicWidgets.h)

```html
<HStack spacing="8" main-align="start" cross-align="center" clip-overflow="false">
  ...
</HStack>

<VStack spacing="12" padding-top="8" padding-bottom="8">
  ...
</VStack>

<Container clamp-position-to-bounds="true" min-width="120" min-height="40">
  ...
</Container>

<ScrollableContainer>
  ...
</ScrollableContainer>
```

| Tag | C++ class | Role |
|---|---|---|
| `<HStack>` / `<VStack>` | `HStack` / `VStack` (subclasses of `StackWidget`) | Flex-style row / column with `StackOptions`. |
| `<Container>` | `Container` | Generic parent widget with `ContainerClampPolicy`. |
| `<ScrollableContainer>` | `ScrollableContainer` | Overflow-scrolling container. |

`<Grid>`, `<AbsoluteStack>`, and `<Panel>` from earlier drafts are
**removed**: the corresponding `LayoutBehavior`s exist
(`StackLayoutBehavior`, etc.) but no dedicated `Widget` wrappers
exist yet. Author multi-axis layouts as a `<Container>` with a
`layout-behavior` attribute pointing at one of the engine's
`LayoutBehaviorPtr` factories; the WML compiler will hard-error
otherwise.

The `<canvas>` / `<surface>` tags from older drafts are permanently
removed: `CanvasView` is deleted from the engine
(see `UIModel.rst` *Specialized View Subclasses*).

### 3.4 Specialized views — [`UI/UIView.h`](../../include/omegaWTK/UI/UIView.h), [`UI/SVGView.h`](../../include/omegaWTK/UI/SVGView.h), [`UI/VideoView.h`](../../include/omegaWTK/UI/VideoView.h), [`UI/NativeViewHost.h`](../../include/omegaWTK/UI/NativeViewHost.h), [`UI/ScrollView.h`](../../include/omegaWTK/UI/ScrollView.h)

```html
<UIView tag="custom-canvas">
  <!-- raw UIView elements authored via UIElementLayoutSpec -->
</UIView>

<SVGView src="logo.svg" scale-mode="aspectFit" />

<VideoView src="clip.mp4" scale-mode="aspectFill" />

<NativeViewHost host-id="webview-1" />

<ScrollView>
  ...
</ScrollView>
```

These are **views, not widgets** (with the exception of
`<ScrollView>`, which the WML compiler usually emits as the
`<ScrollableContainer>` widget wrapper — author `<ScrollView>` only
when you need direct access to the raw view).

| Tag | C++ class | Notes |
|---|---|---|
| `<UIView>` | `UIView` | Direct authoring surface for `UIElementLayoutSpec`. Rarely used in WML — most widgets compile to a `UIView` implicitly. |
| `<SVGView>` | `SVGView` | Loads an `SVGDocument`; `scale-mode` maps to `SVGScaleMode`. |
| `<VideoView>` | `VideoView` | Native media playback. `scale-mode` maps to `VideoScaleMode`, `source-mode` to `VideoSourceMode`. |
| `<NativeViewHost>` | `NativeViewHost` | Carves out a region for a host-owned native view. Emits `DrawOp::NativeContent`. |
| `<ScrollView>` | `ScrollView` | Raw scroll view. Most authors want `<ScrollableContainer>` instead. |

`<WebView>` is **not** present in the engine and is not a valid
WML tag.

### 3.5 Custom widgets

User-authored `Widget` subclasses declared elsewhere with
`<widget name="…">` are referenced by their declared name:

```html
<UserCard user="{currentUser}" />
<ProductTile product="{item}" />
<SettingsPanel />
```

These compile to a call to the generated `XxxCard::Create(rect,
props)` and the constructor parameters declared in
`<property>` blocks (see §13).

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

WML uses `{}` for data binding. **Bindings are dotted-path reads of
the C++ widget model — nothing more.** No arithmetic, no ternaries,
no operators, no function calls, no string concatenation. If you
need a computed value, expose it as a property or computed accessor
on the C++ widget class and bind to its name.

One-way binding (the default):

```html
<Label text="{player.name}" />
<Slider value="{player.health}" min="0" max="100" />
<Image src="{user.avatar}" />
```

Two-way binding uses `bind:` and is restricted to inputs whose C++
widget exposes a setter (`TextInput`, `Slider`, `Dropdown`):

```html
<TextInput bind:value="username" />
<Slider bind:value="volume" />
<Dropdown bind:selection="region" />
```

Computed values — *do not* embed expressions:

```html
<!-- WRONG — embeds an expression in the markup -->
<Label text="{player.health + '/' + player.maxHealth}" />

<!-- WRONG — boolean operator in the binding -->
<Button disabled="{!canSubmit}">

<!-- RIGHT — the C++ class exposes a computed property -->
<Label text="{player.healthDisplay}" />

<!-- RIGHT — the C++ class exposes a positive-sense property -->
<Button disabled="{submitBlocked}">
  <Label text="Submit" />
</Button>
```

The C++ implementation owns `healthDisplay` and `submitBlocked` as
ordinary getters; the compiler wires them to a binding subscription
that re-renders on change.

Bindings compile to property updates that — when the bound model
mutates — call into the widget's `rebuildContent()` or a targeted
property setter, set the appropriate `DirtyBits`, and let
`FrameBuilder` process the change on the next frame. Bindings never
bypass the five-phase lifecycle.

---

## 7. Events

WML events use the `on:<name>="handler"` syntax. Every WML event maps
to a specific C++ entry point in the engine, and the compiler emits
the glue that calls back into the widget's C++ class. There are four
event tiers, each backed by a different engine surface:

| Tier | Engine surface | What it carries |
|---|---|---|
| **Pointer / keyboard** | `ViewDelegate` ([View.h:305](../../include/omegaWTK/UI/View.h)) | Raw input events from `Native::NativeEvent`. |
| **Interaction state** | `WidgetInteractionDelegate` ([WidgetTypes.h:28](../../include/omegaWTK/Widgets/WidgetTypes.h)) | High-level click / hover / press / focus transitions. |
| **Widget lifecycle** | `WidgetObserver` ([Widget.h:271](../../include/omegaWTK/UI/Widget.h)) | Attach / detach / show / hide / resize. |
| **Window / app / gesture** | `Native::NativeEvent` ([NativeEvent.h:107](../../include/omegaWTK/Native/NativeEvent.h)) | Scroll wheel, drag, gesture, focus, window-lifecycle, app-activate. |

### 7.1 The wiring

The handler string is a method name on the enclosing `<widget>`'s
generated C++ class. The WML compiler emits, per widget kind:

- One `WidgetInteractionDelegate` subclass per child that uses a
  Tier 2 event (`on:click`, `on:hover`, `on:press`, `on:focus`).
- One `ViewDelegate` subclass per child that uses a Tier 1 event
  (`on:leftmousedown`, `on:keydown`, etc.) — installed via
  `View::setDelegate`.
- One `WidgetObserver` subclass for the widget root if any Tier 3
  event is used — installed via `Widget::addObserver`.

For Tier 4 (gesture / window) the compiler installs a
`NativeEventProcessor` directly on the view's `NativeEventEmitter`
and dispatches by `NativeEvent::EventType`.

A click handler in WML:

```html
<widget name="Toolbar">
  <Button on:click="save" on:rightclick="showContextMenu">
    Save
  </Button>
</widget>
```

generates roughly:

```cpp
class Toolbar : public Widget {
public:
    void save();
    void showContextMenu(const Native::MouseEventParams &);
private:
    struct ToolbarButtonDelegate : public WidgetInteractionDelegate {
        Toolbar *parent;
        void onLeftMouseUp(Native::NativeEventPtr ev) override {
            WidgetInteractionDelegate::onLeftMouseUp(ev);   // update InteractiveState
            if (getState() == InteractiveState::Hovered)
                parent->save();                              // synthetic "click"
        }
        void onRightMouseDown(Native::NativeEventPtr ev) override {
            WidgetInteractionDelegate::onLeftMouseDown(ev);
            parent->showContextMenu(*static_cast<Native::MouseEventParams*>(ev->params));
        }
    };
    Core::UniquePtr<ToolbarButtonDelegate> _buttonDelegate;
};
```

The compiler is responsible for matching the handler's declared
parameter list against the event's payload type (see §7.5).

### 7.2 Tier 1 — pointer and keyboard

These compile to direct `ViewDelegate` overrides on the target view.
The handler is called once per matching `NativeEvent`.

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

```html
<UIView on:leftmousedown="beginStroke" on:keydown="onShortcut">
  ...
</UIView>
```

### 7.3 Tier 2 — interaction-state synthetic events

These layer on `WidgetInteractionDelegate`, which tracks the
five-valued `InteractiveState` enum (`Idle`, `Hovered`, `Pressed`,
`Focused`, `Disabled`). The compiler synthesises them from raw
mouse / keyboard transitions and only invokes the handler on the
**transition**, not on every event.

| WML event | Fires when | Payload |
|---|---|---|
| `on:click` | `LMouseUp` arrives while `state == Hovered`. | `Native::MouseEventParams` |
| `on:rightclick` | `RMouseDown` arrives while `state == Hovered` or `Focused`. | `Native::MouseEventParams` |
| `on:press` | `state` transitions to `Pressed`. | none |
| `on:release` | `state` transitions out of `Pressed`. | none |
| `on:hover` | `state` transitions to `Hovered`. | none |
| `on:unhover` | `state` transitions out of `Hovered`. | none |
| `on:focus` | `state` transitions to `Focused`. | none |
| `on:blur` | `state` transitions out of `Focused`. | none |

Use Tier 2 for everyday widget interaction. Drop to Tier 1 when you
genuinely need the raw event (capturing modifiers on every mouse-move
during a custom drag, for example).

### 7.4 Tier 3 — widget-lifecycle events

These compile to a `WidgetObserver` subclass installed via
`Widget::addObserver`. The handler runs from inside the engine's
lifecycle hooks — do not block.

| WML event | `WidgetObserver` hook | Payload |
|---|---|---|
| `on:mount` | `onWidgetAttach` | `WidgetPtr` (parent) |
| `on:unmount` | `onWidgetDetach` | `WidgetPtr` (former parent) |
| `on:resize` | `onWidgetChangeSize` | `Composition::Rect` (old, new) |
| `on:show` | `onWidgetDidShow` | none |
| `on:hide` | `onWidgetDidHide` | none |

```html
<widget name="ChartPanel">
  <Container on:mount="loadData" on:resize="reflow">
    ...
  </Container>
</widget>
```

### 7.5 Tier 4 — native gesture / scroll / window events

These dispatch from a `NativeEventProcessor` registered on the
view's `NativeEventEmitter`. The compiler matches on
`NativeEvent::EventType` and unwraps `event->params` to the typed
struct documented in [`NativeEvent.h`](../../include/omegaWTK/Native/NativeEvent.h).

| WML event | `EventType` | Payload |
|---|---|---|
| `on:scroll` | `ScrollWheel` | `Native::ScrollParams` |
| `on:scrollup` / `on:scrolldown` / `on:scrollleft` / `on:scrollright` | `ScrollUp` / `ScrollDown` / `ScrollLeft` / `ScrollRight` | `Native::ScrollParams` |
| `on:dragstart` | `DragBegin` | `Native::MouseEventParams` |
| `on:drag` | `DragMove` | `Native::MouseEventParams` |
| `on:dragend` | `DragEnd` | `Native::MouseEventParams` |
| `on:pinch` | `GesturePinch` | gesture payload (TBD struct) |
| `on:pan` | `GesturePan` | gesture payload |
| `on:rotate` | `GestureRotate` | gesture payload |
| `on:loaded` | `HasLoaded` | `Native::ViewHasLoaded` |
| `on:windowwillclose` | `WindowWillClose` | none |
| `on:windowresize` | `WindowHasResized` | `Native::ViewResize` |
| `on:appactivate` / `on:appdeactivate` | `AppActivate` / `AppDeactivate` | none |
| `on:scalechange` | `WindowScaleFactorChanged` | `Native::WindowScaleFactorChangedParams` |

Window- and app-tier events are only valid on `<app>` and the root
widget of an `AppWindow`; the compiler rejects them on child widgets.

### 7.6 Handler signatures

The handler string is **always a bare method name** on the enclosing
widget's generated C++ class. There is no call form, no argument
list, and no expression syntax in `on:<event>=` strings — those would
be behavior, and behavior lives in C++.

The compiler resolves the C++ signature from the event's tier table:

- If the C++ method takes no parameters, `widget->handler()` is called.
- If it takes one parameter and the type matches the event's payload
  struct, `widget->handler(const PayloadStruct &)` is called.
- If it takes one parameter but the type doesn't match, that is a
  compile error.

```html
<!-- Bare method name — the ONLY accepted form -->
<Button on:click="save" />

<!-- Receive the payload struct explicitly in C++ -->
<UIView on:leftmousedown="beginStroke" />
<!-- → void beginStroke(const Native::LMouseDownParams & params); -->
```

If the handler needs context beyond the event payload — say, the
item's ID in a delete button — the C++ method reads it from the
widget's own model (e.g. `this->item_->id`), not from a markup
argument list. Push the context into the widget's properties at
construction time; don't smuggle it through the handler string.

### 7.7 Custom widget signals

Widgets declare custom outbound events with `<signal>` (see §13).
A signal is a typed event consumed by the **parent** widget via the
same `on:<name>="handler"` syntax:

```html
<widget name="ColorPicker">
  <signal name="colorChanged" payload="Composition::Color" />
  ...
</widget>

<!-- Usage: -->
<ColorPicker on:colorChanged="updateTheme" />
```

The signal is fired from the widget's C++ class, not from the
markup. The compiler generates, for each `<signal name="X"
payload="T">`, a `void emit_X(const T &)` method on the widget's C++
class plus an `OmegaCommon::Vector<std::function<void(const T &)>>`
slot list. Parent-side `on:colorChanged="updateTheme"` registers
`[this](const Composition::Color & c){ this->updateTheme(c); }` into
that slot. The widget's C++ subclass calls `emit_colorChanged(color)`
whenever it wants to fire.

### 7.8 Pointer-routing model

Pointer events follow the engine's hover dispatcher: the OS delivers
the event to the single per-window `NativeItem`; `WidgetTreeHost`
walks the virtual scene tree top-down using `View::containsPoint` to
find the deepest hit, then dispatches through the matched view's
`ViewDelegate`. WML does not change this routing — it only generates
the delegate.

`hit-test: invisible` (a `LayoutStyle` field) skips the view during
the top-down walk; `pointer-events: none` (visual but unhittable)
is a planned alias.

---

## 8. State-Based Styling

WML exposes the engine's four built-in interaction states directly.
These are the values of `OmegaWTK::InteractiveState` (see
[WidgetTypes.h:11](../../include/omegaWTK/Widgets/WidgetTypes.h))
tracked by `WidgetInteractionDelegate`:

```css
Button:hover    { background: #444; }
Button:pressed  { scale: 0.97; }
Button:focused  { border-color: var(--accent); }
Button:disabled { opacity: 0.5; }
```

| WML pseudo-class | `InteractiveState` value |
|---|---|
| `:hover` | `Hovered` |
| `:pressed` | `Pressed` |
| `:focused` | `Focused` |
| `:disabled` | `Disabled` |

The default state is `Idle` (no pseudo-class matches).

Flipping the state sets `DirtyBit::Style` on the affected widget's
root view; the resolver re-cascades just that subtree.

There is intentionally no `:checked` or `:selected` pseudo-class —
neither maps to an existing engine state bit. Use `:state(checked)`
(§8.1) instead, and let the C++ widget class flip the flag via the
setter generated from the `<state name="checked" />` declaration.

### 8.1 Custom states

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

  <Button on:click="startDownload">
    <Label text="{labelText}" />
  </Button>
</widget>
```

The C++ `DownloadButton` class exposes a `labelText` computed
property whose value depends on the current state ("Download" when
idle, "Downloading…" when downloading, "Done" when complete) and
flips the state by calling `setDownloading(true)` etc. from inside
`startDownload()`. The markup only describes *what each state looks
like* and *which property to read for the label*; it never decides
what the label text should be.

A custom state is a bit on the widget's state bitset. Flipping it
(via the C++ setter generated from `<state name="…">`) sets
`DirtyBit::Style` on the widget's root view; the resolver re-runs
for the dirty subtree.

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

  <VStack>
    <slot name="header" />
    <slot />
    <slot name="footer" />
  </VStack>
</widget>
```

Usage:

```html
<Card>
  <template slot="header">
    <Label class="title" text="Account" />
  </template>

  <Label text="Your account is active." />

  <template slot="footer">
    <Button on:click="manageAccount">
      <Label text="Manage" />
    </Button>
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

## 12. Declared Widget Surface

WML does not have a script section. A widget's public surface — the
properties it accepts, the states it can be in, and the signals it
emits — is declared in the markup using top-level child tags inside
the `<widget>` block. The C++ subclass implements them.

```html
<widget name="UserCard">
  <property name="user" type="User" />
  <property name="expanded" type="bool" default="false" />

  <state name="idle" />
  <state name="expanded" />

  <signal name="messageRequested" payload="UserId" />

  <style> … </style>

  <!-- markup body -->
</widget>
```

What each declaration compiles to on the generated C++ class:

| WML declaration | C++ output |
|---|---|
| `<property name="X" type="T" default="…" />` | A constructor parameter, a `T getX() const`, and a `void setX(const T &)` setter that calls `rebuildContent()` and sets the matching `DirtyBit`s. |
| `<state name="X" />` | A bit in the widget's custom-state bitset, with `bool isX() const` and `void setX(bool)`; `setX(true)` flips the bit and sets `DirtyBit::Style`. |
| `<signal name="X" payload="T" />` | A `void emit_X(const T &)` method that fans out to every parent-side `on:X="…"` handler (see §7.7). |

All actual logic — what to do when the user clicks, when to flip a
state, when to call `emit_X` — lives in the C++ subclass. There is
no executable surface in the markup file.

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

  <VStack>
    <Image class="image" src="{product.image}" />

    <Label class="title" text="{product.name}" />

    <Label class="price" text="{product.priceDisplay}" />

    <Button class="buy" on:click="buyClicked">
      <Label text="Buy Now" />
    </Button>
  </VStack>
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
  properties (declared via `<property>`), a `buy` signal (`void
  emit_buy(const Product &)`), and a declared handler slot
  `buyClicked` (which the human-written C++ implements to call
  `emit_buy(product_)`). `setProduct` / `setFeatured` call
  `rebuildContent()` and set `DirtyBit::Content | Style | Paint`.
- The local `<style>` block becomes seven `StyleRule`s registered in
  the widget's local style scope (six visual + the `:hover` variant),
  plus one `LayoutTransitionSpec` carrying `(transform, 160ms, ease)`
  and `(box-shadow, 160ms, ease)`. Layout properties (`width`,
  `padding`, `gap`, `margin-top`) compile to per-node `LayoutStyle`
  field assignments at instantiation.
- A `VStack` subtree of four `Widget`s (`Image`, two `Label`s, one
  `Button`).

The C++ `ProductCard` class is the *only* place that decides what
`buyClicked` does (`emit_buy(product_)`) or what `priceDisplay`
returns (`"$" + std::to_string(product_.price)`) — the markup never
contains that logic.

---

## 14. Layout Syntax

Tag-based layout (preferred — the stack widget is explicit):

```html
<HStack spacing="12" cross-align="center">
  <Button on:click="cancel"><Label text="Cancel" /></Button>
  <Button class="primary" on:click="save"><Label text="Save" /></Button>
</HStack>
```

CSS-based layout (when you want one `<Container>` reused with
multiple stylesheet variants):

```html
<Container class="actions">
  <Button on:click="cancel"><Label text="Cancel" /></Button>
  <Button class="primary" on:click="save"><Label text="Save" /></Button>
</Container>
```

```css
.actions {
  layout: row;
  gap: 12px;
  align-items: center;
  justify-content: flex-end;
}
```

Both forms compile to identical engine output: a `Container` whose
root view has `StackLayoutBehavior` installed as its
`LayoutBehavior`, with `StackOptions{ spacing: 12,
mainAlign: End, crossAlign: Center }`. The HStack form is a
thin convenience wrapper that pre-installs the same behavior at
construction.

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
  <Button on:click="openAdminPanel"><Label text="Admin Panel" /></Button>
</if>

<else>
  <Label text="Standard User" />
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
```

Behavior — handler bodies, computed properties, signal emission —
lives in the widget's matching C++ source file (e.g.
`UserCard.cpp`), not in a separate WML-specific script file.

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
A concrete example inside `<widget name="Toolbar">`:

```html
<Button class="primary" on:click="save">
  <Label text="Save" />
</Button>
```

Compiles to a `Button` `Widget` instantiation in the parent's
`rebuildContent()` roughly equivalent to:

```cpp
auto button = Button::Create(buttonRect);
button->setClass("primary");

auto buttonDelegate = Core::makeUnique<ToolbarButtonDelegate>();
buttonDelegate->parent = this;
button->viewRef().setDelegate(buttonDelegate.get());     // §7 wiring
_buttonDelegate = std::move(buttonDelegate);

auto label = Label::Create(labelRect, LabelProps{
    .text = OmegaCommon::UString("Save"),
});
button->addChild(label);
addChild(button);
```

Plus, in the enclosing `<style>` block, two `Style::Entry` records on
the `Button`'s root `UIView`:

```cpp
auto buttonStyle = Style::Create();
buttonStyle->backgroundColor(button->viewAs<UIView>().tag(),
                             /*var(--accent) resolved →*/ Color::FromHex(0x4f7cff));
buttonStyle->elementRoundedCorner(button->viewAs<UIView>().tag(), 12.f);
button->viewAs<UIView>().setStyle(buttonStyle);
```

At frame time, after Phase 2 (Style) runs, the button's root view has
a `ComputedStyle` populated by the cascade (see
[UIViewImpl.h:55](../../src/UI/UIViewImpl.h)). Phase 4 (Paint) reads
`ComputedStyle` and emits one `DrawOp::RoundedRect` to the per-window
`DisplayList`. Phase 5 (Commit) hands the list to the compositor.

There is no intermediate JSON representation in the engine. The
**widget tree + per-view `Style` + global `StyleSheet` stack** above
*is* the runtime — real `Widget` / `View` / `UIView` instances, the
`Style` entries on each `UIView`, and the cascaded `ComputedStyle`
cache the resolver writes.

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

  <Label class="counter" text="{count}" />

  <HStack class="controls">
    <Button on:click="decrement">
      <Label text="-" />
    </Button>

    <Button class="primary" on:click="increment">
      <Label text="+" />
    </Button>
  </HStack>
</app>
```

`<app>` compiles to an `Application` plus a single `AppWindow` carrying
the root widget. `<theme src="…">` registers a `ThemeVars` map and
makes it active. `<style src="…">` loads an additional `StyleSheet`
into the global stack.

---

## 22. Language Summary

**WML is a look-only language.** It describes what a widget looks
like and what state knobs and event entry points it exposes — never
what to do or how to compute a value. Behavior lives in the C++
widget subclass.

```text
HTML-like markup whose tag names ARE the OmegaWTK C++ class names
  (Button, Label, HStack, VStack, Container, ScrollableContainer,
   Rectangle, RoundedRectangle, Ellipse, Path, Separator, Image,
   Icon, TextInput, Slider, Dropdown, UIView, SVGView, VideoView,
   NativeViewHost, ScrollView)
+ CSS styling
+ four-tier events that compile to ViewDelegate /
  WidgetInteractionDelegate / WidgetObserver / NativeEventProcessor
  subclasses; the handler string is always a bare C++ method name
+ dotted-path data bindings (no expressions, no operators)
+ <property> / <state> / <signal> declarations that compile to
  setters, state bits, and emit() methods on the C++ subclass
+ theme variables (ThemeVars)
+ state-driven styling (engine's InteractiveState pseudo-classes
  + :state(name) custom flags)
+ animations (transitions + keyframes, both driven by
  the per-window AnimationScheduler)
```

There is no `<script>` block, no expression language inside `{}`,
and no call-form handlers. Anything that would require those is by
definition behavior — author it on the C++ widget subclass.

Everything WML appears to add compiles to engine-native primitives —
`Widget`, `View`, `UIView`, `LayoutStyle`, `Style`, `StyleSheet`,
`LayoutTransitionSpec`, `KeyframeTrack`, `ViewDelegate`,
`WidgetObserver`. The engine has no special knowledge of WML; it sees
only the constructs documented in `UIModel.rst`.

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

4. ~~**`{}` binding language semantics.**~~ **Resolved** by the
   look-only rule in §2: bindings are dotted-path reads only.
   Computed values are exposed by the C++ widget as named
   properties; markup binds to the property name. See §6.

5. **`<canvas>` / imperative drawing for game-engine integration.**
   `CanvasView` is deleted from the engine, but OmegaGTE integration
   may still want imperative draw hooks. Recommendation: those use
   cases author a `<NativeViewHost>` (OmegaGTE provides a native
   layer) rather than an in-engine `<canvas>`. The engine's pure-Paint
   contract is worth preserving.

6. **Missing input widgets.** WML naturally invites `<Checkbox>`,
   `<RadioGroup>`, `<Toggle>`, and `<ProgressBar>` tags, but none of
   those C++ classes exist in `Widgets/UserInputs.h` today. Options:
   (a) the WML compiler hard-errors on the missing tags and we add the
   C++ widgets first; (b) WML emits synthesized compositions
   (`<Container>` + state flag + `<Label>`) under those names.
   Recommendation: (a) — add the C++ widgets first. The point of
   PascalCase = class name is that WML never invents widgets the
   engine doesn't have.

7. **Event handler return values and event consumption.** WML
   handlers in §7 are declared `void`. Should `on:leftmousedown`
   handlers be able to return `bool` to suppress the synthesized
   `on:click`, the way DOM `preventDefault()` works? Recommendation:
   yes, for Tier 1 only. Tier 2 / 3 / 4 stay void — they're
   notifications, not interceptors. The compiler treats a `bool`
   return as consumption and short-circuits the synthesized event.

8. ~~**Method-vs-call form for handlers.**~~ **Resolved** by the
   look-only rule in §2: handler strings are bare method names only.
   The compiler picks `widget->handler()` or
   `widget->handler(const PayloadStruct &)` from the event's tier
   table; ambiguity is a compile error. Call form is rejected.
   See §7.6.

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
  contract for `<NativeViewHost>` and `<VideoView>`.
