# Native Theme Application Plan

**Status:** Proposal. Nothing below is implemented yet.
**Scope:** Close the cross-platform asymmetry where a `UIView` with no
explicit background color sometimes inherits the underlying OS window's
themed surface color and sometimes renders pitch black. Define the
priority chain that lets app code register a **custom theme** (with
light/dark variants) that overrides the native theme entirely, while
still consulting the OS for the active appearance bit.
**Prerequisite reading:** [Native-API-Completion-Proposal.md](Native-API-Completion-Proposal.md) §2.5
(the existing `NativeTheme.h` query API),
[Style-StyleSheet-Refactor-Plan.md](Style-StyleSheet-Refactor-Plan.md) §3.8
(theme variables on `Application`).
**Non-goals:** Defining the `Theme` data model itself (the Style plan
owns it). Re-specifying `ThemeDesc` (already done in `NativeTheme.h`).
Theming individual widget visuals (UA stylesheet, Style plan Tier 3).
Defining the `ColorScheme` / accent-color semantics beyond what
`ThemeDesc::Colors` already carries.

---

## 1. The asymmetry today

When an `AppWindow` displays a virtual view tree whose root `UIView` has
no explicit background color, what the user sees depends on the
platform:

| Platform | What backs the cleared content area | Themed by OS? |
|----------|--------------------------------------|---------------|
| macOS | The native `NSWindow` background paints behind the Metal `CAMetalLayer`. `CAMetalLayer.opaque = NO` lets the window color show through where Metal hasn't drawn. | **Yes — automatically.** |
| Windows | The D3D12 swapchain clears to whatever color the compositor was told. There is no `NSWindow`-like backing surface behind it. | **No.** The window paints whatever clear-color the engine picks; the OS theme never propagates. |
| GTK + Vulkan | `GtkWindow` paints its theme-coloured background to its `GdkWindow` — but the Vulkan swapchain owns the surface pixels once `vkAcquireNextImageKHR` returns, and the swapchain image is whatever `clearValue` we picked. GTK's painted background is never visible inside the rendered region. | **No.** GTK's theme is queried by widgets; the rendered surface is whatever Vulkan clears to (currently pitch black). |

The result is a visible cross-platform inconsistency. The same WTK app:
on macOS, a `UIView` with no background inherits the OS surface color
(white in light mode, dark in dark mode). On Windows it sits on a black
clear. On Linux it sits on a black clear *under a* themed GTK window
frame — making the seam between window chrome and content even more
jarring.

The `NativeTheme` query API (`queryCurrentTheme()` returning a
`ThemeDesc` with `Colors::background`) already exists on macOS and
Win32; GTK's is the gap noted in the Native-API proposal Current State
table. But the API is currently *informational* — nothing reads
`ThemeDesc::Colors::background` and pushes it into the render path's
clear color. macOS works by accident-of-architecture (the native
backing surface), not because anybody chose to apply the theme.

### 1.1 A second asymmetry: the window chrome itself

Everything above is about the *content* surface — the pixels the engine
renders into. There is a parallel and equally visible asymmetry in the
**non-client area** (the OS-drawn title bar and window frame), and here
Windows is the lone holdout:

| Platform | Does the window chrome follow OS light/dark automatically? |
|----------|------------------------------------------------------------|
| macOS | **Yes.** `NSWindow` redraws its title bar in the window's effective appearance with no app action. |
| GTK | **Yes.** The client-side-decoration / header-bar theme (or the WM for server-side decorations) repaints when the desktop color-scheme flips — the app calls nothing. |
| Windows | **No.** A bare `HWND`'s non-client area renders in **light mode regardless of the system setting** until the app explicitly opts in via `DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ...)`. |

This compounds the content bug rather than sitting beside it. Once
Tier 2 makes the *content* clear to a dark surface in dark mode, an
un-opted-in Windows window is *worse* than before: a dark canvas under
a stubbornly bright-white caption bar — a sharper seam than the old
all-black content was. Real parity with GTK/macOS therefore means
driving the **chrome appearance** from the same resolved
`ThemeAppearance` bit, not only the content clear color. That is §5.2's
job, and it is a required step — not optional polish.

---

## 2. Goal

A single rule that holds on every platform:

> A window with no app-specified background color clears to a defined
> "surface" color. By default that surface color is `NativeTheme.colors.background`
> (so the rendered region matches the OS chrome). If the application
> registers a **custom `Theme`**, the custom theme's surface color is
> used instead — but the active **`ThemeAppearance`** (Light or Dark)
> is still consulted from the OS so the custom theme can pick its own
> light/dark variant.

This means:
- macOS keeps its current visual outcome but reaches it through the same
  code path as the other platforms (explicit `setClearColor`, not
  accidentally-transparent layer compositing).
- Windows: the swapchain clear is the native theme background instead
  of pitch black.
- GTK + Vulkan: same — the Vulkan clear is the native theme background.
- Dark-mode OS systems get a dark cleared surface on all three
  platforms without app code.
- Window **chrome** (title bar / frame) follows OS light/dark on every
  platform too — macOS and GTK already do this automatically, but a
  bare Windows `HWND` stays light-mode until explicitly opted in, so
  parity requires driving the `HWND` appearance from the same bit
  (§1.1, §5.2).
- Apps with their own theme system get a single `Application::setTheme(theme)`
  call that overrides the native color but inherits the native
  appearance bit so the theme picks Light or Dark in step with the OS.

---

## 3. Priority chain

A single resolved value — `WindowSurfaceColor` — drives the per-frame
clear. It is resolved in priority order each time `ComputedStyle` is
recomputed for the root node (Style plan §3.6 / Phase 2):

```
1. Inline Style on the root UIView (highest)
     └─ if `style().backgroundColor` is set, that wins.
2. Active custom Theme's surface color
     └─ if `application().theme() != nullptr`, look up the surface color
        in the variant matching the OS appearance bit
        (theme.light().surface / theme.dark().surface).
3. NativeTheme.colors.background  (default)
     └─ queried from the OS on window creation + on appearance change.
4. Hardcoded fallback
     └─ Color::White (Light) / Color::Black approximated for Dark.
        Only reached if NativeTheme is unavailable (e.g. headless test).
```

Same chain governs other theme-derived values that need to land on the
native surface: e.g. the window-level `setOpacity` interacts with the
chosen surface color the same way (alpha-premultiplied), but the
clear-color decision is the primary new contract here.

The `ThemeAppearance` bit (Light | Dark) is always sourced from
`NativeTheme.appearance`, regardless of which row of the priority chain
wins — that is the "follow OS dark mode" semantic. A custom theme that
does not want to follow the OS can override by calling
`application().setForcedAppearance(ThemeAppearance::Dark)`; the OS
observer then no longer drives appearance flips.

---

## 4. Architecture

### 4.1 Existing pieces (no change)

- `Native::ThemeDesc` (`wtk/include/omegaWTK/Native/NativeTheme.h`) —
  the data carrier: `appearance` + `Colors{accent, background,
  foreground, controlBackground, controlForeground, separator,
  selection}` + `Typography`. Reused as-is.
- `Native::queryCurrentTheme()` — the static fetch. Reused; GTK gap is
  filled as part of this plan's Tier 1.
- `Native::NativeThemeObserver::onThemeSet(ThemeDesc&)` — the observer
  interface. Reused; the platform plumbing that *fires* it is filled in
  per-platform in Tier 1.

### 4.2 New: `Application` ownership of `NativeTheme`

The `Application` (Style plan §3.8) gains a small ownership block:

```cpp
class Application {
public:
    // ... existing setTheme(ThemePtr) / themeVars() ...

    /// The most recently observed OS theme. Driven by the platform
    /// observer registered at Application construction; mutated only
    /// by the Application's own observer trampoline.
    const Native::ThemeDesc & nativeTheme() const;

    /// When true, app code does not want to follow OS appearance flips;
    /// the cascade uses application().forcedAppearance() in place of
    /// nativeTheme().appearance.
    void setForcedAppearance(Optional<Native::ThemeAppearance>);
    Optional<Native::ThemeAppearance> forcedAppearance() const;

    /// Resolved per the §3 priority chain. Read at clear-color setup
    /// time; the value is recomputed when any of {root style,
    /// active custom Theme, nativeTheme, forcedAppearance} changes.
    Composition::Color resolveWindowSurfaceColor(const AppWindow & win) const;
};
```

`Application` registers exactly one OS theme observer at construction.
On each `onThemeSet` callback, it (a) caches the new `ThemeDesc`, and
(b) marks every registered window's root node `DirtyBit::Style`,
forcing the resolver to re-derive `WindowSurfaceColor` and the rest of
the cascade. This is the same root-dirty propagation Style plan §3.8
uses for custom-theme swaps — the two flows converge.

### 4.3 New: the surface-color sink on `AppWindow`

```cpp
class AppWindow {
public:
    /// Set explicitly by the resolver after running the §3 priority
    /// chain. Sourced from app code only via inline Style or
    /// Application::setTheme.
    void setSurfaceColor(Composition::Color);
    Composition::Color surfaceColor() const;
};
```

`setSurfaceColor` writes through to the per-window `FrameBuilder` /
compositor binding so the next `beginFrame` issues the per-frame render
pass with the new `clearValue`. The actual translation happens in the
backend-specific `BackendRenderTargetContext` (the existing
per-window render-target machinery — see Panels-And-Window-Customization-Plan
Part C for the larger queue/context picture). The clear-color contract
is a one-field change to the existing `GERenderPassDescriptor` setup
inside the compositor's per-window frame open.

### 4.4 Flow per frame

1. **OS event** (mac: `viewDidChangeEffectiveAppearance`, Win32:
   `WM_SETTINGCHANGE`/`WM_THEMECHANGED`, GTK:
   `notify::gtk-application-prefer-dark-theme` on `GtkSettings`) →
   `Application`'s observer callback fires.
2. **Application caches** the new `ThemeDesc`, dirties root style on
   every registered window.
3. **Next frame, Phase 2 (Style)** — `StyleResolver::resolve(rootNode)`
   runs. Cascade reaches the surface-color rule, calls
   `application().resolveWindowSurfaceColor(win)`, writes the result
   into `win.setSurfaceColor(...)`.
4. **Next frame, Phase 4 (Paint)** — the compositor opens its render
   pass with the resolved color as the `clearValue`. Vulkan / D3D12 /
   Metal each see the same one `Color` value; the platform asymmetry is
   gone.

---

## 5. Per-platform realization

### 5.1 macOS — keep the outcome, change the mechanism

Today macOS gets a themed clear "for free" because `CAMetalLayer.opaque
= NO` lets the `NSWindow` background show through. Under this plan:

- `CAMetalLayer.opaque = YES`. The window content is whatever we
  rendered; the native backing color is no longer relied on.
- The Metal render pass `clearColor` is `application().resolveWindowSurfaceColor(win)`.
- `viewDidChangeEffectiveAppearance` on the content view fires the
  observer (already wired by `queryCurrentTheme()`'s test surface
  — verify).

Visual outcome is unchanged in default usage; the difference is that
macOS now obeys the same priority chain as the other platforms and a
custom `Theme` color actually wins (today it does not — the native
background overpaints transparent regions).

### 5.2 Windows — pull the swapchain clear color from `NativeTheme`

- D3D12 render pass clears to `application().resolveWindowSurfaceColor(win)`
  instead of whatever the engine's hardcoded default is.
- Observer: subscribe to `WM_SETTINGCHANGE` with `lParam` pointing at
  `"ImmersiveColorSet"` (the dark-mode flip), and `WM_THEMECHANGED` for
  classic-theme changes. Each fires `Application`'s observer trampoline
  with a fresh `queryCurrentTheme()` result.
- **Chrome parity — required, not optional.** A bare `HWND` does not
  follow the OS light/dark setting; macOS and GTK windows do this for
  free (§1.1), Windows does not. On window creation and on every
  observer fire, call
  `DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))`
  with `dark` derived from the resolved appearance
  (`application().forcedAppearance().value_or(nativeTheme().appearance)`
  — the same bit §3 feeds the content clear). This themes the
  *non-client* area the engine never paints; it is orthogonal to the
  content clear color but part of the same "follow OS dark mode"
  contract, and it is what actually brings Windows to visual parity with
  the other two platforms. It is independent of the chrome-customization
  in
  [Panels-And-Window-Customization-Plan.md](Panels-And-Window-Customization-Plan.md):
  it applies under `WindowChrome::Native`; under the Panels plan's
  `Custom`/`CustomWithNativeControls` chrome the app draws its own
  caption and drives the color directly, so the DWM call is skipped
  there.
  - **Version gotchas (off-platform-unverified from this Linux host):**
    `DWMWA_USE_IMMERSIVE_DARK_MODE` requires Windows 10 build ≥ 17763,
    and its attribute index is `20` on 2004+/Windows 11 but `19` on
    1809–1903 — version-gate and no-op on older builds. For an
    *arbitrary* caption color (to match a custom `Theme`'s `surface`
    rather than a binary light/dark) Windows 11 build 22000+ adds
    `DWMWA_CAPTION_COLOR` / `DWMWA_TEXT_COLOR`; treat that as the row-2
    (custom-theme) extension of chrome theming, mirroring how the
    content surface gets its row-2 override (Open Q7).

### 5.3 GTK + Vulkan — fill the query gap, then clear to that color

This is the platform where the user-visible bug bites hardest today.
Two pieces:

**5.3.1 — Fill the GTK `queryCurrentTheme` gap.** The Native-API
proposal Current State table still lists GTK NativeTheme as "missing"
even though the completion-status header marks §2.5 Done — verify and
fill if absent. Source the data from:

- `appearance`: `gtk_settings_get_default()` →
  `gtk-application-prefer-dark-theme` (bool). On GNOME the
  `org.gnome.desktop.interface color-scheme` GSettings key is the
  modern source; XSetting `Net/ThemeName` is the legacy fallback.
- `colors.background` / `foreground` / `accent`: pull from
  `GtkStyleContext` against the toplevel window's style class. The
  GTK CSS engine resolves `theme_bg_color`, `theme_fg_color`, and
  `theme_selected_bg_color` (accent) to concrete RGBA via
  `gtk_style_context_lookup_color(ctx, "theme_bg_color", &rgba)`.
- `colors.controlBackground` / `controlForeground`: same lookup against
  a temporary `GTK_STYLE_CLASS_BUTTON` context.

Observer: connect to
`g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme", …)`
and `"notify::gtk-theme-name"`. Both re-query the theme and emit
`NativeThemeObserver::onThemeSet`.

**5.3.2 — Wire `NativeTheme.colors.background` into the Vulkan
clear.** This is the actual user-visible fix:

- `GEVulkanCommandQueue` / `GEVulkanCommandBuffer::startRenderPass`
  already takes a `VkClearValue` from the `GERenderPassDescriptor`. The
  WTK compositor sets that descriptor each frame at
  `FrameBuilder::beginFrame`. Today its `clearValue` is hardcoded /
  derived from the root `Style`'s background.
- Under this plan the compositor's per-window frame open reads
  `appWindow.surfaceColor()` (the resolved §3 result) and writes it as
  the `clearValue`. The Style resolver populated that field during
  Phase 2 of the same frame's tick.

No GTK-side change is needed for the clear itself — the GTK window
background isn't competing with Vulkan when the cleared color *is* the
GTK theme background. The "GTK theme inherited" outcome falls out of
the priority chain naturally.

---

## 6. Custom-theme integration with the Style plan

The Style plan §3.8 specifies:

```cpp
class Application {
public:
    void              setTheme(ThemePtr);
    ThemePtr          theme() const;
    const ThemeVars & themeVars() const;
};
```

This plan extends that surface without changing it:

- A `Theme` is a typed bundle of `ThemeVars` plus an explicit `surface`
  color. The Style plan's existing `ThemeVars` map carries the rest
  (`accent`, `foreground`, control colors, ...).
- A `Theme` may carry **light** and **dark** variants. The active
  variant is picked by `application().forcedAppearance().value_or(
  application().nativeTheme().appearance)`. This is the rule that lets
  custom themes "ignore" `NativeTheme` while still tracking OS dark
  mode.
- `setTheme(nullptr)` reverts to the native-theme default — the priority
  chain falls through to row 3.
- `StyleSheet`s author themed widget visuals through `StyleValue::Var("accent")`
  / `StyleValue::Var("surface")` etc., exactly as the Style plan §3.8
  already specifies. The new bits — `surface` color and
  light/dark-variant selection — slot into the same `ThemeVars`
  resolution path; no new resolver mechanism is required.

In short: this plan adds the **default source** for the theme variables
(the OS) and the **policy** for how a custom theme interacts with that
default (override the colors, inherit the appearance bit). The
resolver, the cascade rules, and the property-to-value plumbing are
already specified in the Style plan.

---

## 7. Migration tiers

### Tier 1 — `NativeTheme` query parity + the observer wiring

- Fill the GTK `queryCurrentTheme()` gap (§5.3.1) if not present.
- Implement the per-platform observer plumbing so
  `NativeThemeObserver::onThemeSet` fires on OS appearance change on
  all three platforms.
- Add `Application::nativeTheme()` getter (returns the cached
  `ThemeDesc`) and the observer trampoline that updates it.
- No clear-color change yet — this tier is observational only.

**Risk:** Low. Pure native plumbing; no engine behavior change.

**Files touched:** `wtk/src/Native/gtk/GTKTheme.cpp` (new or extend),
`wtk/src/Native/win/WinTheme.cpp` (extend observer registration),
`wtk/src/Native/macos/CocoaTheme.mm` (extend observer registration),
`wtk/include/omegaWTK/UI/Application.h`,
`wtk/src/UI/Application.cpp`.

### Tier 2 — surface-color sink + priority chain

- Add `AppWindow::setSurfaceColor` / `surfaceColor` + the compositor
  wiring that consumes it for the per-frame `clearValue`.
- Implement `Application::resolveWindowSurfaceColor(win)` per the §3
  priority chain (rows 1, 3, 4 — row 2 lands in Tier 3).
- Hook the resolver: after Phase 2 (Style) runs for the root, the
  resolver calls `setSurfaceColor` on the owning `AppWindow`.
- On macOS, flip `CAMetalLayer.opaque` to `YES` and start clearing
  explicitly to the resolved color. Verify the visual outcome matches
  pre-change behavior in default usage.
- On Windows, apply the resolved appearance bit to the `HWND` chrome via
  `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` (§5.2) on window
  creation and on every observer fire — the title-bar half of parity,
  independent of and additional to the content `clearValue`.

**Risk:** Low on Vulkan / D3D12 (one field change in the per-frame
descriptor); Medium on macOS (the `opaque` flip changes layer
composition and could surface unexpected interactions with window
chrome, especially under `WindowChrome::Custom`/`CustomWithNativeControls`
from the Panels plan — verify with the BasicAppTest first, then any
panel-using test once Panels Part A lands).

**Files touched:** `wtk/include/omegaWTK/UI/AppWindow.h`,
`wtk/src/UI/AppWindow.cpp`, `wtk/src/UI/FrameBuilder.cpp`,
`wtk/src/Composition/backend/RenderTarget.h`,
`wtk/src/Native/macos/CocoaAppWindow.mm` (the `.opaque = YES` flip),
`wtk/src/Native/win/WinAppWindow.cpp` (the `DwmSetWindowAttribute`
chrome-appearance call),
the per-backend visual-tree builders.

### Tier 3 — custom theme override + appearance forcing

- Land row 2 of the §3 priority chain: if `application().theme() !=
  nullptr`, the custom theme's `surface` color (in the variant matching
  the active appearance) overrides `NativeTheme.colors.background`.
- Add `setForcedAppearance` + `forcedAppearance` and route them through
  the variant-pick.
- Custom `Theme` data type: a `ThemeVars` for each variant, an
  explicit `Composition::Color surface` per variant, and a name. Lives
  next to `ThemeVars` from the Style plan §3.8.
- A custom theme's variant flip on OS appearance change goes through
  the same root-dirty path as Tier 1 — no new propagation mechanism.

**Risk:** Low. Pure data-flow extension to the Application-level theme
machinery already proposed by the Style plan.

**Files touched:** `wtk/include/omegaWTK/UI/Application.h`,
`wtk/src/UI/Application.cpp`, `wtk/include/omegaWTK/UI/Theme.h` (new),
`wtk/src/UI/StyleResolver.cpp` (the cascade source for `surface`).

### Tier 4 — UA stylesheet uses `NativeTheme` defaults

Once the Style plan's user-agent stylesheet (Tier 3 of that plan)
lands, its `Button`/`Label`/`Icon` default styles bind their text color
to `var(--foreground)`, their control fills to `var(--control-background)`,
etc. — all of which resolve from the `ThemeVars` populated either by
the active custom `Theme` or by the cached `NativeTheme`. Widgets stop
hardcoding visual defaults; everything tracks the OS in default
configuration and the app theme in opted-in configuration.

This is more of a consequence than a step — it's the Style plan's Tier
3 with this plan as the data source. No new code lives here.

---

## 8. Open questions

1. **One global `NativeTheme` vs. per-window `NativeTheme`.** On
   macOS, the effective appearance can vary per-window (a window with
   `NSAppearance.appearanceNamed:NSAppearanceNameAqua` overrides the
   system default). On Win32, dark-mode hint via DWM is per-window
   too. On GTK, the theme is desktop-wide. Recommendation: keep
   `Application::nativeTheme()` as the *system* default, add an
   optional `AppWindow::nativeTheme()` override that defaults to
   `application().nativeTheme()` but can be replaced by app code to
   force one window into a specific appearance. Defer until somebody
   actually asks for per-window appearance.
2. **`Theme` data model.** This plan refers to a custom `Theme` with
   light/dark variants, but the data model isn't fully spelled out
   here — does it own the `ThemeVars` outright, or is it a
   variant-selector that points at one of two `ThemeVars`? Probably
   the latter (less duplication when only the accent and surface
   differ between variants). Worth confirming when the Style plan
   moves Tier 3.
3. **CAMetalLayer `opaque = YES` regressions.** Flipping to opaque
   changes layer composition on macOS in ways that interact with
   window chrome corner rounding and the title-bar accessory area.
   Verify with `WindowChrome::CustomWithNativeControls` once Panels
   Part B lands — the traffic-light region might still expect a
   transparent backing. If it does, the `opaque` flip is conditional
   on `WindowChrome::Native` and falls back to the current transparent
   path under Custom modes. (Custom modes can clear to the resolved
   color regardless; only the *layer* opacity is affected.)
4. **Disabling OS-appearance follow per-app.** Some apps want to ship
   in a fixed appearance regardless of OS. `setForcedAppearance` covers
   the case where the app *picks* Light or Dark globally. The narrower
   case — "always Light, even on Dark OS, but still apply the custom
   theme's Light variant" — is also covered by
   `setForcedAppearance(Light)`. Confirm this is sufficient before
   adding a third axis.
5. **Linux desktop session detection beyond GTK.** Wayland sessions
   under GNOME read the color-scheme key the same way; KDE Plasma uses
   `kdeglobals` `ColorScheme` instead, and there is no GTK-side signal
   for that change. Out of scope for Tier 1 (we ride GTK's view of the
   world); flag for follow-up if KDE-host coverage becomes a
   requirement.
6. **Headless / test mode.** `queryCurrentTheme()` on a headless CI
   runner — what does it return? Currently undefined per
   `NativeTheme.h`. Recommendation: `ThemeDesc{}` defaulted values
   (Light appearance, white background) and `nativeTheme()` returns
   that. Style resolver falls through to row 4 (hardcoded) on the
   same data, so behavior is consistent.
7. **Chrome appearance: binary bit vs. arbitrary caption color.** The
   content surface runs the full §3 priority chain (a custom `Theme` can
   override the color outright). The Windows chrome, via
   `DWMWA_USE_IMMERSIVE_DARK_MODE`, is only a binary light/dark toggle —
   it consumes the *appearance bit*, not a color. Win11 22000+'s
   `DWMWA_CAPTION_COLOR` would let a custom `Theme` paint the caption to
   match its `surface`, giving chrome its own row-2 override; macOS/GTK
   expose no equivalent arbitrary-caption knob under native chrome.
   Recommendation: Tier 1–2 drive chrome from the appearance bit only
   (parity with mac/GTK), and defer arbitrary caption color to the
   custom-theme work (Tier 3), Win11-only, falling back to the binary
   toggle elsewhere. Confirm before building.

---

## 9. Relationship to existing plans

- **[Native-API-Completion-Proposal.md](Native-API-Completion-Proposal.md) §2.5** — the existing
  `NativeTheme.h` query API is the input to this plan. §2.5 will gain
  a forward-pointer note that the observer-wiring + application-side
  story lives here. Also: verify the GTK gap (§5.3.1) is real — the
  Current State table and completion-status header disagree.
- **[Style-StyleSheet-Refactor-Plan.md](Style-StyleSheet-Refactor-Plan.md) §3.8** — owns
  `Application::setTheme` and the `ThemeVars` cascade. This plan adds
  the *default source* for those variables (the OS) and the *custom
  override policy*. The Style plan's §3.8 gains a forward-pointer
  note. The cascade rules, property-to-value plumbing, and dirty-bit
  propagation are unchanged.
- **[Panels-And-Window-Customization-Plan.md](Panels-And-Window-Customization-Plan.md)** —
  Part B (`WindowChrome`) interacts with this plan in two places:
  (a) the title-bar caption color under `Custom` should track
  `NativeTheme.appearance` so the app-drawn caption matches the OS
  chrome the user expects; (b) the `CAMetalLayer.opaque` flip in
  Tier 2 §5.1 needs verification against the custom-chrome path
  (Open Q3). The `AppPanel` surface color follows the same priority
  chain as `AppWindow` — Application-scoped resolution, panel-owned
  sink — when the Panels plan lands.
- **[Widget-View-Paint-Lifecycle-Plan.md](Widget-View-Paint-Lifecycle-Plan.md)** —
  Phase 2 (Style) is the resolver site for `WindowSurfaceColor`;
  Phase 4 (Paint) is where the compositor consumes
  `appWindow.surfaceColor()` into the clear value. The phase guard
  enforces that no other site mutates the surface color mid-frame.
- **[Animation-Scheduler-Plan.md](Animation-Scheduler-Plan.md)** —
  Theme changes (OS or custom) could be animated as a cross-fade
  between resolved surface colors. Out of scope for this plan;
  `setSurfaceColor` is a hard write today. If theme transitions
  become a feature, the standard Style plan `Transition` record on
  `Property::Surface` (Style §3.5) hands off to the scheduler via the
  same `transition(...)` hook — no new mechanism required.

---

## 10. Honest uncertainty

~~I have not verified that GTK + Vulkan today literally renders pitch
black with no theme inheritance — the user reported it but I have not
isolated the swapchain `clearValue` in the WTK Vulkan render-target
context to confirm.~~ **Verified 2026-06-02.** The full chain is:
`Compositor::renderCompositeFrame` (`wtk/src/Composition/Compositor.cpp:293-312`)
initialises `clearR=clearG=clearB=clearA=0`, scans `frame->slices` for
the first slice with any RGBA component `> 0`, and adopts that slice's
`background` if it finds one. With no explicit root background (the
BasicAppTest case), every slice carries `(0,0,0,0)`, the loop never
fires, and `targetContext->beginFrame(0,0,0,0)` calls into
`FrameRenderPass::begin` (`wtk/src/Composition/backend/RenderPass.cpp:64-78`),
which builds a `GERenderPassDescriptor::ColorAttachment::Clear` with
clear color `(0,0,0,0)`. The Vulkan backend
(`GEVulkanCommandBuffer::startRenderPass` in
`gte/src/vulkan/GEVulkanCommandQueue.cpp:750-770`) copies that
straight into `VkClearValue.color.float32[0..3]`. The RGBA swapchain
treats `(0,0,0,0)` as opaque black. **That's the pitch black the user
sees, verbatim.** Tier 2 replaces that call site with
`targetContext->beginFrame(surface.r, surface.g, surface.b,
surface.a)` where `surface = application().resolveWindowSurfaceColor(win)`,
and the "first non-zero slice wins" heuristic in
`renderCompositeFrame` (which has its own latent fragility — a
non-root slice with a translucent background can steal the
whole-window clear) is deleted alongside.

I have assumed `CAMetalLayer.opaque` is `NO` today on the macOS
backend, which is what gives the implicit OS-themed clear. The
secondary fingerprints in the tree are consistent with that —
`wtk/src/Native/macos/CocoaItem.mm:22-24` (and the matching header at
`CocoaItem.h:119-121`) explicitly set `layer.backgroundColor` to a
`(0,0,0,0)` CGColor on the *root content view's* `CALayer`, which is
exactly what's needed for the metal layer above it to composite over
the `NSWindow` background. But I have NOT located the
`CAMetalLayer.opaque` assignment itself; verify on the macOS side
before flipping it in Tier 2. If `.opaque` is already `YES` and the
mac visual is being produced by an explicit `[NSColor
windowBackgroundColor]`-sourced `clearColor` somewhere, §5.1 simplifies
to "route the existing NSColor source through
`Application::resolveWindowSurfaceColor` instead of looking up
`NSColor` directly."

I have assumed the Application is the right owner for the OS theme
observer, mirroring the Style plan §6 Q1 resolution
("`Application`-scoped style sheets"). If theming needs to vary per
window (Open Q1) before Tier 1 ships, the observer moves to
`AppWindow` and the per-app cached `ThemeDesc` becomes a per-window
cached `ThemeDesc`. The §3 priority chain is unchanged; only the
ownership level shifts.

I have assumed that on Windows the existing D3D12 swapchain creation
path takes a clear color from a per-window context (it does — see
Panels plan Part C's discussion of `BackendRenderTargetContext`). If
the current code hardcodes the clear color at swapchain-creation time
rather than per-frame, Tier 2's "set it from the resolver each frame"
contract requires moving that source to the per-frame render-pass
descriptor first. Likely already correct on Vulkan + Metal; verify on
D3D12.
