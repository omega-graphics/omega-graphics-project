# Widget Markup Language

A concept **HTML-based widget language** for a UI toolkit, with built-in CSS-like styling support.

**Widget Markup Language**, or **WML**, is a lightweight HTML-inspired language for building native or web-style UI widgets. It keeps the familiarity of HTML and CSS, but adds UI-toolkit features like properties, bindings, slots, signals, and state-driven styling.

A WML file can define reusable widgets, layouts, controls, and visual themes.

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

  <layout type="column" gap="8px">
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
  </layout>
</widget>
```

This resembles HTML, but the tags map to toolkit-native UI elements.

---

## 2. Core Goals

WML is designed to be:

- **HTML-like**, so it is easy to learn.
- **CSS-compatible**, so existing layout and styling knowledge carries over.
- **Component-based**, so every widget can be reused.
- **Native-friendly**, so it can compile to a desktop, mobile, game, or web UI backend.
- **Data-driven**, with built-in bindings and state styles.

---

## 3. Built-In Elements

WML includes standard UI elements:

```html
<text>Hello</text>
<button>Click Me</button>
<input placeholder="Username" />
<checkbox checked="{settings.enabled}" />
<slider value="{volume}" min="0" max="100" />
<image src="profile.png" />
<icon name="settings" />
```

Layout elements:

```html
<row></row>
<column></column>
<grid></grid>
<stack></stack>
<scroll></scroll>
<panel></panel>
<surface></surface>
```

Reusable widget elements:

```html
<UserCard user="{currentUser}" />
<ProductTile product="{item}" />
<SettingsPanel />
```

---

## 4. Styling Model

WML supports CSS directly through a `<style>` block.

```css
Button {
  padding: 10px 16px;
  border-radius: 10px;
  background: #333;
  color: white;
}

Button:hover {
  background: #444;
}

Button:pressed {
  scale: 0.97;
}

Button:disabled {
  opacity: 0.5;
}
```

Supported selectors:

```css
Button { }
.primary { }
#submitButton { }
Panel > Button { }
UserCard .name { }
Button:hover { }
Button:pressed { }
Input:focused { }
Checkbox:checked { }
```

---

## 5. CSS Toolkit Extensions

WML adds a few UI-toolkit-specific properties.

```css
.card {
  layout: column;
  gap: 12px;
  align-items: center;
  justify-content: center;

  transition: background 160ms ease, scale 120ms ease;
  animate-in: fade 180ms ease-out;
}
```

Extra properties could include:

```css
layout: row | column | grid | stack;
gap: 12px;
dock: top | bottom | left | right | fill;
hit-test: visible | invisible | children;
pointer-events: auto | none;
theme-color: accent;
native-effect: blur;
elevation: 3;
```

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

---

## 7. Events

Events use the `on:` prefix.

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

Custom widget events:

```html
<ColorPicker on:colorChanged="updateTheme" />
```

---

## 8. State-Based Styling

Widgets can expose custom states.

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

This allows CSS to react to app-level state, not just pointer states.

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

  <panel>
    <slot name="header" />
    <slot />
    <slot name="footer" />
  </panel>
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

---

## 10. Themes and Variables

WML supports CSS variables.

```css
:theme {
  --background: #111;
  --surface: #1e1e24;
  --text: #ffffff;
  --accent: #4f7cff;
  --radius-lg: 18px;
}
```

Then widgets can use them:

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

Theme variants:

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

WML supports CSS-like media queries.

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

Toolkit-specific media conditions could include:

```css
@media pointer == touch { }
@media platform == desktop { }
@media platform == mobile { }
@media input == gamepad { }
@media theme == dark { }
```

---

## 12. Script Section

A widget may include a small logic section.

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

This script language could be JavaScript-like, Lua-like, or toolkit-specific.

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

    ProductCard[featured="true"] {
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

Usage:

```html
<ProductCard
  product="{selectedProduct}"
  featured="true"
  on:buy="addToCart"
/>
```

---

## 14. Layout Syntax

WML can use either tag-based layout:

```html
<row gap="12px" align="center">
  <button>Cancel</button>
  <button class="primary">Save</button>
</row>
```

Or CSS-based layout:

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

---

## 15. Widget Inheritance

Widgets can extend other widgets.

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

Usage:

```html
<PrimaryButton on:click="save">
  Save Project
</PrimaryButton>
```

---

## 16. Animation Support

Animations can be CSS-like.

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

State-driven animation:

```css
Panel:enter {
  animation: fadeIn 120ms ease-out;
}

Panel:exit {
  animation: fadeOut 100ms ease-in;
}
```

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

---

## 18. Proposed File Types

```text
.wml      Widget markup file
.wcss     Widget CSS file
.wtheme   Theme definition file
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

---

## 20. Compiled Runtime Model

A WML compiler could transform this:

```html
<button class="primary" on:click="save">
  Save
</button>
```

Into an internal widget tree:

```json
{
  "type": "Button",
  "classes": ["primary"],
  "events": {
    "click": "save"
  },
  "children": [
    {
      "type": "Text",
      "value": "Save"
    }
  ]
}
```

The style engine resolves this:

```css
Button.primary {
  background: var(--accent);
  color: white;
}
```

Into runtime properties:

```json
{
  "background": "#4f7cff",
  "color": "#ffffff",
  "padding": [10, 16],
  "borderRadius": 12
}
```

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

---

## 22. Language Summary

**WML** is essentially:

```text
HTML-like markup
+ CSS styling
+ toolkit-native widgets
+ data binding
+ component properties
+ signals and events
+ theme variables
+ responsive layout
+ state-driven styling
```

It gives UI developers something familiar like HTML/CSS, but structured enough to compile into a real custom UI toolkit.
