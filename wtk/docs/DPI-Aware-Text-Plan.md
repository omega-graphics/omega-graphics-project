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
> `view->setRenderScale()`; per-monitor DPI-change events will trigger a rebuild of the render
> target (see Non-goals).

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

### HarfBuzz / FreeType / Vulkan (Linux) — **TODO**

`wtk/src/Composition/backend/vk/HarfbuzzFontEngine.cpp`:

- `TextRect::Create` accepts `renderScale` but currently ignores it.
- `HarfBuzzTextRect` needs to:
  - Size its rasterization target in physical pixels.
  - Pass a resolution to `FT_Set_Char_Size` derived from `renderScale`
    (typically `72 * scale` DPI when char size is in 1/64 points, leaving
    `FontDescriptor::size` interpreted as DIPs).
  - Apply scale when blitting the rasterized atlas into the offscreen texture.
- Source the scale via `NativeWindow::scaleFactor()` (§2.2); the GTK/Vulkan
  `AppWindow` implementation is responsible for reading the Wayland
  (`wl_output` scale or `wp_fractional_scale_v1`) or X11 (`Xft.dpi` / XRandr)
  value and returning it from `scaleFactor()`. The Vulkan visual tree calls
  `nativeWindow->scaleFactor()` and passes the result to `view->setRenderScale()`.

## Non-goals (for now)

- Per-monitor DPI updates. `WM_DPICHANGED` on Windows (and equivalent signals
  on macOS / Linux) currently only resizes the window — the render target's
  `renderScale_` is frozen at creation. A later pass will handle the
  `WindowDpiChanged` event, call `nativeWindow->scaleFactor()` for the new
  value, and rebuild / rescale the render target. This is tracked as a
  follow-on to §2.2 (`NativeWindow` event emitter) in the Native API
  Completion Proposal.
- Mixed-DPI multi-window applications where Views are reparented between
  windows of different scales. The render target is per-window, so this will
  Just Work for Views that are always attached to a single window.
- Fractional scale factors below 1.0 (not a real configuration).

## Testing

`TextCompositorTest` is the reference case. Expected behavior after the D2D
fix: text appears at the size implied by `FontDescriptor::size` in DIPs,
crisply rasterized on a 150%/200% display, with no red background.
