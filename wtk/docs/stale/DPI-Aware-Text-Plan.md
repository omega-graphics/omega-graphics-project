# DPI-Aware Text Rendering Plan

## Context

WTK text rendering was producing glyphs that were visibly larger than requested
on high-DPI displays and, on some zoom levels, was clipped or misaligned. Root
cause: `renderScale` (logical-to-physical pixel ratio) was being applied in two
or three places simultaneously — the font size was pre-multiplied by DPI, the
offscreen texture was sized in logical pixels, the D2D device context had no
DPI set, and the compositor then applied its own renderScale when stamping the
resulting bitmap into the window. The net effect was DPI scaling compounding
on itself.

## Architectural invariant

All `Composition::Rect` values, font sizes, and layout geometry are in
**device-independent pixels (DIPs)**. A DIP is 1/96 inch. The
logical-to-physical scale factor is carried on `ViewRenderTarget::renderScale_`
and read via `View::getRenderScale()`.

The **only** place renderScale is consumed is the backend that is about to
write physical pixels to a GPU resource. That backend is responsible for:

1. Allocating the offscreen surface in physical pixels (`dip * renderScale`).
2. Configuring the device context / rasterization target so that draw calls
   expressed in DIPs produce correctly sized physical pixels (e.g.
   `ID2D1DeviceContext::SetDpi(96 * scale, 96 * scale)` on D2D).
3. Leaving font sizes, text layout rects, and glyph runs in DIPs end-to-end.

This means `FontDescriptor::size` is always DIPs. `TextLayoutDescriptor` is
always DIPs. `Composition::Rect` handed to `TextRect::Create` is always DIPs.

## Plumbing

```
Native window (HWND / NSWindow / Wayland surface)
   │  NativeWindow::scaleFactor()          ← uniform API (§2.2 Native API Completion Proposal)
   │  (backed per-platform by:
   │     Win32  → GetDpiForWindow
   │     macOS  → NSWindow.backingScaleFactor
   │     Linux  → wl_output scale / wp_fractional_scale_v1 / Xft.dpi)
   ▼
DCVisualTree / MtlVisualTree / VkVisualTree  ──► view->setRenderScale(nativeWindow->scaleFactor())
   │
   ▼
Composition::ViewRenderTarget::renderScale_
   │
   ▼
View::getRenderScale()
   │
   ▼
Canvas::drawText  ──► TextRect::Create(rect, layoutDesc, renderScale)
   │
   ▼
Backend TextRect implementation
   • allocates physical-sized offscreen texture
   • sets device-context DPI
   • rasterizes with DIP-valued inputs
```

> **Virtual view model note:** Under the virtual view model there is exactly one `NativeItem` per
> window. `renderScale_` is therefore a per-window quantity sourced from `NativeWindow::scaleFactor()`,
> not a per-NativeItem value. Visual tree constructors (`DCVisualTree`, `MtlVisualTree`,
> `VkVisualTree`) call `nativeWindow->scaleFactor()` once at creation and pass the result to
> `view->setRenderScale()`. Per-monitor DPI changes trigger a rebuild via the
> `WindowScaleFactorChanged` event — see "Per-monitor DPI updates" below.

## Backend status

### Direct2D / DirectWrite (Windows) — **DONE**

`wtk/src/Composition/backend/dx/DWriteFontEngine.cpp`:

- `DWriteTextRect` takes `renderScale` in its constructor.
- Offscreen `GETexture` is allocated at `rect.{w,h} * renderScale`.
- `ID2D1DeviceContext::SetDpi(96 * scale, 96 * scale)` is set after context
  creation, so `DrawTextLayout(Point2F(0,0), …)` rasterizes at physical size
  even though the layout is sized in DIPs.
- `CreateFont` / `CreateFontFromFile` no longer multiply `desc.size` by the
  window DPI. Font size is always DIPs.
- Debug red clear in `drawRun` was replaced with transparent clear.

### Core Text / Metal (macOS) — **TODO**

`wtk/src/Composition/backend/mtl/CTFontEngine.mm`:

- `TextRect::Create` accepts `renderScale` but currently ignores it (parity
  with the D2D backend is required for retina displays).
- `CTTextRect` needs to:
  - Allocate its `CGBitmapContext` / Metal texture at `rect.{w,h} * renderScale`.
  - Apply a scale transform on the CG context (`CGContextScaleCTM(ctx, scale,
    scale)`) or equivalent so `CTFrameDraw` output matches D2D behavior.
  - Leave `CTFontCreateWithName` size in DIPs.
- Source the scale via `NativeWindow::scaleFactor()` (§2.2) rather than calling
  `NSWindow::backingScaleFactor` directly; the macOS `AppWindow` implementation of
  `scaleFactor()` wraps `backingScaleFactor` internally.

### HarfBuzz / FreeType / Vulkan (Linux) — **PARTIAL**

`wtk/src/Composition/backend/vk/HarfbuzzFontEngine.cpp`:

- `TextRect::Create` accepts `renderScale` but currently ignores it.
- `HarfBuzzTextRect` needs to:
  - Size its rasterization target in physical pixels.
  - Pass a resolution to `FT_Set_Char_Size` derived from `renderScale`
    (typically `72 * scale` DPI when char size is in 1/64 points, leaving
    `FontDescriptor::size` interpreted as DIPs).
  - Apply scale when blitting the rasterized atlas into the offscreen texture.

**Done so far:** `GTKAppWindow::scaleFactor()` now returns a combined value —
`dpiScale × integerScale`, where `dpiScale = gdk_screen_get_resolution() / 96`
and `integerScale = gtk_widget_get_scale_factor()`. `Composition::Rect` is in
DIPs at the public boundary; `gtk_window_set_default_size`, `setRect`,
`getRect`, configure-event width/height, and geometry hints are all converted
through `dpiScale_` only (GTK applies `integerScale_` itself when allocating
the surface, so multiplying it again would double-apply). Runtime DPI changes
re-emit `WindowScaleFactorChanged` via the `gtk-xft-dpi` settings notify.

`HarfbuzzFontEngine.cpp::getScreenScaleFactor()` was updated to compute the
same combined formula so glyph rasterization matches window sizing today.
This is a stop-gap — see "Eventual scale source" below.

**Still pending:**

- `HarfBuzzTextRect` honoring `renderScale` instead of calling
  `getScreenScaleFactor()` from `drawRun`. Gated on the visual-tree wiring
  below; switching now would silently regress to 1.0× because the
  `renderScale` parameter is currently never populated on Linux.
- `VKLayerTree::Create` (or the per-window setup it delegates to) needs to
  call `view->setRenderScale(nativeWindow->scaleFactor())` — the same hook
  `DCVisualTree.cpp:52` already performs on Win32. Without this, every Linux
  `Canvas::drawText` reads `View::getRenderScale() == 1.0` regardless of what
  the native window reports, and the `renderScale` parameter to
  `TextRect::Create` is dead.
- FreeType resolution / Pango font-size scaling and offscreen surface sizing
  inside `HarfBuzzTextRect` need to switch from `getScreenScaleFactor()` to
  the constructor-passed `renderScale` once the previous two items land.

### Eventual scale source for Linux text (architectural note)

`HarfbuzzFontEngine.cpp` should not be a *source* of scale. The single source
of truth is `NativeWindow::scaleFactor()`, owned by the native window. The
flow into the font engine is:

```
NativeWindow::scaleFactor()
   │  (GTKAppWindow combined dpiScale × integerScale)
   ▼
Visual tree constructor (VKLayerTree / VKFallbackVisualTree)
   │  view->setRenderScale(nativeWindow->scaleFactor())
   ▼
ViewRenderTarget::renderScale_   ◄── per-window, mutated on
   │                                  WindowScaleFactorChanged
   ▼
View::getRenderScale()
   │
   ▼
Canvas::drawText reads ownerView_->getRenderScale()
   │
   ▼
TextRect::Create(rect, layoutDesc, renderScale)
   │
   ▼
HarfBuzzTextRect uses the constructor-passed scale — no GDK calls, no
static helpers, no second source of truth.
```

In other words: the **Compositor** (Canvas, ViewRenderTarget) carries the
scale that originated at the **NativeWindow**, and the FontEngine just reads
what it's handed. The current static `getScreenScaleFactor()` in
HarfbuzzFontEngine is a temporary patch to keep glyph density consistent
with `GTKAppWindow::scaleFactor()` until VKLayerTree wires the proper path
through.

## Per-monitor DPI updates

`renderScale_` was originally frozen at render-target construction. With the
`WindowScaleFactorChanged` event landing as part of `Native-API-Completion-Proposal.md`
§2.2, it is now a runtime-mutable quantity. The flow:

```
Platform DPI/backing-scale change
   │  (Win32 WM_DPICHANGED, macOS windowDidChangeBackingProperties,
   │   Wayland wp_fractional_scale_v1, GtkWindow notify::scale-factor)
   ▼
NativeWindow emits WindowScaleFactorChanged{oldScale, newScale, suggestedRect}
   │
   ▼
AppWindow default handler (Native API §2.2)
   │  • If suggestedRect (Win32 only): nativeWindow->setRect(*suggestedRect)
   │  • view->setRenderScale(newScale) recurses through the View tree
   │  • view->setNeedsDisplay() invalidates everything for repaint
   ▼
ViewRenderTarget::scaleChanged()  ◄── new, owned by this plan
   │  • re-runs the constructor's backingWidth_ / backingHeight_ derivation
   │  • reallocates the offscreen surface at rect.{w,h} * newScale
   ▼
Next paint
   │  • Canvas::drawText reads ownerView_->getRenderScale()  → newScale
   │  • TextRect::Create produces a properly-scaled offscreen surface
   │  • (Phase 6) cached TextLayout detects scale mismatch on resolve and
   │    rebuilds its TextRect + re-uploads its GETexture
   ▼
Glyphs are crisp on the new monitor
```

### What this plan owns

- **`ViewRenderTarget::scaleChanged()`** — new method on
  `wtk/src/Composition/CompositorClient.cpp`. Already-existing
  `setRenderScale(newScale)` only updates the float; `scaleChanged()`
  additionally re-derives `backingWidth_` / `backingHeight_` and rebuilds
  the GPU-side surface (same code path as the constructor and the resize
  handler at `RenderTarget.cpp:217`). Called by the AppWindow handler
  immediately after `setRenderScale`.

- **TextRect rebuild contract** — confirmed *not* a separate concern.
  Today's `Canvas::drawText` constructs a fresh `TextRect` per call, so it
  picks up the new scale on the next paint with zero added wiring. Phase 6's
  `TextLayout` detects the scale change via the per-draw scale re-read in
  `Composition-Extension-Plan.md` §6.3 — also no added wiring. This plan
  does **not** introduce a `TextRect::setScale` mutator; rebuild is the
  uniform answer.

### What the Native API plan owns

- The `WindowScaleFactorChanged` event type, params struct, and per-platform
  emission (see `Native-API-Completion-Proposal.md` §2.2).
- The default `AppWindow` handler that calls `view->setRenderScale` and
  `view->setNeedsDisplay`.

### Backend status (revisited)

The per-monitor flow only works if the platform `TextRect` backends honor
`renderScale` at every call. With the per-draw re-read in place, the
**TODO** items below become the gating work — without them, a held
`TextLayout` on macOS/Linux will rebuild but produce a same-sized bitmap on
the new monitor:

- macOS / Core Text — see "Core Text / Metal (macOS)" below. Same change as
  before; the priority bumps because Phase 6 + per-monitor DPI both depend
  on it.
- Linux / HarfBuzz — see "HarfBuzz / FreeType / Vulkan (Linux)" below.
  Same.

## Non-goals (for now)

- Mixed-DPI multi-window applications where Views are reparented between
  windows of different scales. The render target is per-window, so this will
  Just Work for Views that are always attached to a single window.
- Fractional scale factors below 1.0 (not a real configuration).
- ~~X11 `Xft.dpi` change-events. The Native API §2.2 implementation cuts the
  first GTK pass with integer `wl_output` / `notify::scale-factor` only;
  X11 fractional / on-the-fly DPI changes via `XSettings` are deferred.~~
  Landed: `GTKAppWindow` now subscribes to `GtkSettings::notify::gtk-xft-dpi`
  and re-emits `WindowScaleFactorChanged` when the combined value moves.

## Testing

`TextCompositorTest` is the reference case. Expected behavior after the D2D
fix: text appears at the size implied by `FontDescriptor::size` in DIPs,
crisply rasterized on a 150%/200% display, with no red background.
