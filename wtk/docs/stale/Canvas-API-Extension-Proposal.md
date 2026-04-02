# Canvas API Extension and Completion Proposal

This document proposes API and feature extensions to complete and extend the Composition Canvas API in `omegaWTK/Composition/Canvas.h`. It is intended to align the public drawing surface with existing backend capabilities, close gaps for UI and vector rendering (e.g. SVGView), and add common 2D canvas features used in modern toolkits.

---

## 1. Current API Summary

### 1.1 Drawing primitives

| Method | Parameters | Notes |
|--------|------------|--------|
| `drawRect` | `Core::Rect&`, `SharedPtr<Brush>&` | Fill only; no border |
| `drawRoundedRect` | `Core::RoundedRect&`, `SharedPtr<Brush>&` | Fill only; no border |
| `drawEllipse` | `Core::Ellipse&`, `SharedPtr<Brush>&` | Fill only; no border |
| `drawPath` | `Path&` | Uses path’s internal brush/stroke |
| `drawText` | text, font, rect, color, optional `TextLayoutDescriptor` | Two overloads |
| `drawImage` | `BitmapImage&`, `Core::Rect` | Destination rect |
| `drawGETexture` | `GETexture&`, `Core::Rect`, optional `GEFence` | GPU texture to rect |

### 1.2 Effects and frame lifecycle

- **`applyEffect(SharedHandle<CanvasEffect>&)`** — frame-level effects (GaussianBlur, DirectionalBlur).
- **`applyLayerEffect(const SharedHandle<LayerEffect>&)`** — layer-level effects (e.g. DropShadow, Transformation).
- **`sendFrame()`** — submit current frame to the compositor.
- **`getCurrentFrame()`** / **`nextFrame()`** — inspect or consume the current frame.

### 1.3 Internal vs public gap

- **`VisualCommand`** and **`CanvasFrame`** already support:
  - **Border** (brush + width) for Rect, RoundedRect, and Ellipse.
  - Path params: stroke width, contour, fill, brush.
- The **public** `drawRect` / `drawRoundedRect` / `drawEllipse` do **not** accept an optional border; callers must emulate borders (e.g. draw inner/outer rects or use Path frame generators).

---

## 2. Proposed Extensions

### 2.1 Complete existing primitives (borders)

**Goal:** Expose the existing Border support in `VisualCommand` through the Canvas API so that rect, rounded rect, and ellipse can be drawn with an optional outline in one call.

**API addition:**

```cpp
// Optional border for rect/roundedRect/ellipse (Core::Optional<Border>)
void drawRect(Core::Rect & rect,
              Core::SharedPtr<Brush> & brush,
              Core::Optional<Border> border = Core::nullopt);

void drawRoundedRect(Core::RoundedRect & rect,
                     Core::SharedPtr<Brush> & brush,
                     Core::Optional<Border> border = Core::nullopt);

void drawEllipse(Core::Ellipse & ellipse,
                 Core::SharedPtr<Brush> & brush,
                 Core::Optional<Border> border = Core::nullopt);
```

- Implementations push the same `VisualCommand` data that already exists (rect/roundedRect/ellipse + brush + optional border).
- Existing call sites that only pass `(rect, brush)` remain valid if the new parameters default to `nullopt` (or overloads are added that delegate to the three-argument form).

**Alternative:** Add overloads that take `(geometry, brush, border)` and keep the current two-argument signatures as-is for backward compatibility.

---

### 2.2 Line and simple path strokes

**Goal:** Support drawing a single line or an open path (sequence of points) with a stroke only, without requiring construction of a full `Path` or `GVectorPath2D`.

**API addition:**

```cpp
/// Draw a line segment with the given brush and stroke width.
void drawLine(OmegaGTE::GPoint2D from,
              OmegaGTE::GPoint2D to,
              Core::SharedPtr<Brush> & brush,
              float strokeWidth = 1.f);

/// Draw a polyline (open) or polygon (closed) with stroke and optional fill.
void drawPolyline(OmegaCommon::Span<OmegaGTE::GPoint2D> points,
                  Core::SharedPtr<Brush> & strokeBrush,
                  float strokeWidth,
                  bool closed = false,
                  Core::Optional<Core::SharedPtr<Brush>> fillBrush = Core::nullopt);
```

- Backend can translate these into existing VectorPath commands (build `GVectorPath2D` or use a small temporary Path).
- Useful for SVG `<line>`, `<polyline>`, `<polygon>` and simple UI rules/dividers.

---

### 2.3 Arc and pie primitives

**Goal:** Draw ellipse arcs (and optionally wedges) without going through the full Path API.

**API addition:**

```cpp
/// Draw an arc (or full ellipse if startAngle/endAngle span 2π) with optional fill (pie slice).
/// bounds: axis-aligned bounding rect of the ellipse; angles in radians.
void drawArc(OmegaGTE::GRect bounds,
             float startAngle,
             float endAngle,
             Core::SharedPtr<Brush> & brush,
             bool pie = false,
             Core::Optional<Border> border = Core::nullopt);
```

- **pie = false**: stroke-only arc (centerline + stroke width could be expressed via a small Path or a new VisualCommand variant).
- **pie = true**: filled wedge from center to arc endpoints, plus optional border on the arc.
- Backend may add an `VisualCommand::Type::Arc` or map to Path/VectorPath.

---

### 2.4 Image drawing options

**Goal:** Control scaling and sampling when drawing images/textures into a rect (e.g. aspect-fit, aspect-fill, tiling, sampling mode).

**API addition:**

```cpp
enum class ImageScaleMode : int {
    Stretch,      // Current behavior: fill rect
    AspectFit,    // Letterbox; preserve aspect ratio
    AspectFill,   // Crop to fill rect; preserve aspect ratio
    Tile,         // Repeat to fill rect
    TileX,        // Repeat horizontally only
    TileY         // Repeat vertically only
};

/// Extend drawImage to accept scale mode (and optionally source rect for subregion).
void drawImage(SharedHandle<Media::BitmapImage> & img,
               const Core::Rect & destRect,
               ImageScaleMode scaleMode = ImageScaleMode::Stretch,
               Core::Optional<Core::Rect> sourceRect = Core::nullopt);

/// Optional: explicit sampling mode for GPU backends (linear/nearest).
void drawGETexture(SharedHandle<OmegaGTE::GETexture> & texture,
                   const Core::Rect & rect,
                   SharedHandle<OmegaGTE::GEFence> fence,
                   ImageScaleMode scaleMode = ImageScaleMode::Stretch);
```

- `sourceRect` allows drawing a subregion of the image (sprite sheets, atlases).
- Backend maps scale mode to UVs and/or shader behavior; tiling implies repeat wrap.

---

### 2.5 Per-draw opacity and blend mode

**Goal:** Apply opacity or blend mode to individual draw commands without creating extra layers.

**API addition:**

```cpp
enum class BlendMode : int {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    SoftLight,
    HardLight,
    Difference,
    Exclusion
};

/// Optional draw options applied to the next draw (or to a new DrawOptions struct).
struct DrawOptions {
    float opacity = 1.f;
    Core::Optional<BlendMode> blendMode = Core::nullopt;
};

void setDrawOptions(DrawOptions options);
void resetDrawOptions();  // Restore default opacity and blend
```

- **Alternative:** Add optional `DrawOptions` (or just `opacity`/`blendMode`) to each draw call instead of state.
- Backend: extend `VisualCommand` (or a shared command header) with optional opacity and blend enum; compositor applies them when rendering that command.

---

### 2.6 Canvas-level transform and clip

**Goal:** Support a transform matrix and a clip region so that all subsequent draws are transformed and clipped until state is restored (e.g. for zoom, pan, or rounded-clip content).

**API addition:**

```cpp
void save();   // Push current transform + clip (and optionally draw options) onto a stack
void restore(); // Pop and restore

void translate(float dx, float dy);
void scale(float sx, float sy);
void rotate(float radians);
void transform(float m00, float m01, float m10, float m11, float tx, float ty);
// Or: void setTransform(const float m[6]);  // 2D affine row-major

void clipRect(Core::Rect & rect);
void clipRoundedRect(Core::RoundedRect & rect);
void clipPath(Path & path);  // Intersect current clip with path (fill) region
```

- **save/restore** apply to transform, clip, and optionally `DrawOptions`.
- Backend: each `VisualCommand` is interpreted in the current transform; clip can be a separate stack applied at submit or in the compositor.
- **Scope:** Start with `save`/`restore` plus `translate`/`scale`/`rotate` and `clipRect`; add full affine and path clip as needed.

---

### 2.7 Clear and background

**Goal:** Make frame background explicit and allow clearing to a color or clearing the current frame without drawing a full-screen rect.

**API addition:**

```cpp
/// Set the background color for the current frame (used when no draw covers a pixel).
void setBackground(Color color);

/// Clear the current frame to transparent (or to current background) and optionally reset draw state.
void clear(Core::Optional<Color> color = Core::nullopt);
```

- `CanvasFrame` already has a `background` field; `setBackground` would set it.
- `clear` would set all pixels to the given color (or transparent) and optionally clear the visual command list for the current frame so the next draws are the only content.

---

### 2.8 Text extensions

**Goal:** Better control over text rendering (max lines, truncation, underline/strikethrough, and optional gradient/brush for text color).

**API addition:**

```cpp
struct TextDrawOptions {
    Core::Optional<int> maxLines = Core::nullopt;
    bool truncateTail = false;   // Ellipsize at tail if over maxLines or overflow
    bool underline = false;
    bool strikethrough = false;
};

void drawText(const UniString & text,
              Core::SharedPtr<Font> font,
              const Core::Rect & rect,
              const Color & color,
              const TextLayoutDescriptor & layoutDesc,
              TextDrawOptions textOptions = {});

/// Overload: use a Brush for text color (e.g. gradient text).
void drawText(const UniString & text,
              Core::SharedPtr<Font> font,
              const Core::Rect & rect,
              Core::SharedPtr<Brush> & brush,
              const TextLayoutDescriptor & layoutDesc,
              TextDrawOptions textOptions = {});
```

- Backend: extend text layout/rendering to respect `maxLines`, truncation, and decorations; gradient text may require a separate pass or shader.

---

### 2.9 Gradient and brush extensions for shapes

**Goal:** Ensure gradients can be defined in canvas space (e.g. linear from rect top to bottom) and are usable for all fill primitives.

- **Current:** `Gradient` has linear (angle) and radial (radius); `GradientBrush(gradient)` exists.
- **Proposal:** Document or extend gradient definition to support:
  - Linear: start/end points in canvas coordinates (or relative to a rect).
  - Radial: center + radius (and optional second radius for elliptical).
- No signature change to Canvas if Brush already carries a gradient; only Gradient construction and interpretation need to be defined (e.g. gradient bounds relative to the draw rect when not global).

---

### 2.10 CanvasEffect and LayerEffect extensions

**Goal:** Align effect API with usage and add common effects.

- **CanvasEffect:** Add optional effect **bounds** so blur (and similar) apply only to a subregion of the frame.
- **CanvasEffect types:** Consider adding ColorMatrix (tint, saturation, brightness), Opacity (global frame opacity), and a simple Shadow (offset + blur + color) that does not require a full layer.
- **LayerEffect:** Already has DropShadow and Transformation; document and keep as the way to get layer-level shadows and transforms. Optionally add Border (layer outline) as a dedicated effect if it simplifies UI.

---

## 3. Implementation priority (suggested)

| Priority | Feature | Rationale | Status |
|----------|---------|-----------|--------|
| ~~P0~~ | ~~2.1 Borders on rect/roundedRect/ellipse~~ | ~~Backend already supports it; closes the main API gap.~~ | Done |
| ~~P0~~ | ~~2.7 setBackground / clear~~ | ~~Makes frame setup explicit and avoids ad-hoc clear rects.~~ | Done |
| P1 | 2.2 drawLine / drawPolyline | Useful for SVG and UI; small backend surface. | |
| P1 | 2.4 ImageScaleMode (and sourceRect) | Needed for correct image/camera/video display. | |
| P2 | 2.6 save/restore, translate, scale, clipRect | Enables zoom, pan, and clipped content. | |
| P2 | 2.5 DrawOptions (opacity, blend) | Common for overlays and highlights. | |
| P3 | 2.3 drawArc | Convenience for charts and SVG arcs. | |
| P3 | 2.8 TextDrawOptions / gradient text | Improves text layout and styling. | |
| P3 | 2.9 Gradient in canvas space | Better control for fills. | |
| P3 | 2.10 Effect bounds and new effect types | Refinements; can follow after core drawing is complete. | |

---

## 4. File change summary (for implementation)

- **`wtk/include/omegaWTK/Composition/Canvas.h`**  
  Add declarations for: border overloads (2.1), drawLine/drawPolyline (2.2), drawArc (2.3), ImageScaleMode and drawImage/drawGETexture overloads (2.4), DrawOptions and setDrawOptions/resetDrawOptions (2.5), save/restore and transform/clip (2.6), setBackground/clear (2.7), TextDrawOptions and drawText overloads (2.8). New enums and structs as above.

- **`wtk/src/Composition/Canvas.cpp`**  
  Implement new methods by constructing the appropriate `VisualCommand` or by updating frame state (background, draw options, transform/clip stack). Backend-specific code may need to handle new command fields or new command types.

- **Compositor backends**  
  Extend handling of `VisualCommand` (e.g. Border, opacity, blend, transform, clip) and of image/texture drawing (scale mode, source rect) where not already supported.

- **Tests**  
  Add or extend tests (e.g. in EllipsePathCompositorTest or a dedicated Canvas test) to cover borders, clear, line/polyline, and image scale modes.

---

## 5. References

- Current Canvas API: `wtk/include/omegaWTK/Composition/Canvas.h`
- VisualCommand and CanvasFrame: same header
- Path and frame generators: `wtk/include/omegaWTK/Composition/Path.h`, `wtk/src/Composition/Path.cpp`
- Brush and Gradient: `wtk/include/omegaWTK/Composition/Brush.h`
- SVGView use of Canvas: `wtk/src/UI/SVGView.cpp` (drawRect, drawRoundedRect, drawEllipse, drawPath with frame generators)
