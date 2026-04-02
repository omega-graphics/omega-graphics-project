# Composition Extension Plan

Consolidated plan for extending the Canvas, Brush, Color, and Gradient APIs in `omegaWTK/Composition`. Supersedes the separate Brush and Canvas extension proposals.

---

## Current state snapshot

### What works

- `Brush::Type` enum (`Color`, `Gradient`, `None`) exists in the header alongside legacy `isColor`/`isGradient` booleans.
- `GradientBrush()` header/source signature mismatch is fixed.
- `create32Bit` typo is fixed.
- Canvas exposes border overloads on `drawRect`, `drawRoundedRect`, `drawEllipse`.
- `setBackground()` and `clear()` exist on Canvas.
- Color solid-fill rendering works for all shape primitives and paths.
- `Path` supports `addArc`, `addLine`, `goTo`, `close`, stroke width, and a per-path brush.

### What doesn't work or is incomplete

| Issue | Location | Impact |
|-------|----------|--------|
| Backend still dispatches on `brush->isColor` / `!brush->isColor`, not `brush->type` | `RenderTarget.cpp:785,832,858,919,928` | `Brush::Type::None` is never handled; adding new brush types will silently fall through |
| Gradient compute shader is commented out | `RenderTarget.cpp:127-131` (shader), `698-702` (const buffer write) | Gradient brushes enter the texture pipeline but produce no texture — effectively broken |
| Gradient has only `float arg` (angle or radius) | `Brush.h:62` | No start/end points for linear, no center/focus for radial, no elliptical radii |
| No spread mode on gradients | — | Out-of-range stops clamp implicitly |
| No per-draw opacity on Canvas | — | SVGView hacks opacity into `Color.a`; won't extend to gradient fills |
| No `drawLine` / `drawPolyline` on Canvas | — | SVGView builds `Path` objects for `<line>` and `<polyline>` as a workaround |
| No canvas transform stack | — | No zoom, pan, or rotated content |
| `SharedPtr<Brush>&` parameter style | All Canvas draw methods | Can't pass temporaries; forces callers to name every brush variable |

---

## Phase 0 — Foundation cleanup

**Goal:** Make `Brush::Type` the single source of truth. Remove ambiguity before adding anything new.

### 0.1 Migrate backend to `switch(brush->type)`

Every site in `RenderTarget.cpp` that reads `brush->isColor` or `!brush->isColor` becomes:

```cpp
switch (brush->type) {
    case Brush::Type::Color:    /* solid fill path */  break;
    case Brush::Type::Gradient: /* texture path   */  break;
    case Brush::Type::None:     /* skip draw      */  return;
}
```

Affected locations: Rect (line ~785), RoundedRect (~832), Ellipse (~858), VectorPath stroke (~919) and fill (~928).

### 0.2 Remove legacy booleans

After 0.1, remove `isColor` and `isGradient` from `Brush`. Any remaining references outside the backend (unlikely — UIView uses brushes opaquely) get `brush->type == Brush::Type::Color` instead.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — remove `isColor`, `isGradient` fields
- `wtk/src/Composition/Brush.cpp` — remove boolean assignments from constructors
- `wtk/src/Composition/backend/RenderTarget.cpp` — switch-based dispatch

---

## Phase 1 — Gradient pipeline

**Goal:** Make `GradientBrush(...)` actually produce visible output. This is the single largest prerequisite for everything gradient-related.

### 1.1 Implement the gradient compute shader

The shader source in `RenderTarget.cpp` already defines `GradientTextureConstParams` and `LinearGradientStop`, but the `linearGradient` compute function is commented out. Implement:

```
// Linear: sample along the line defined by angle, 
// writing interpolated stop colors into outputTex.
[in input, in stops, out outputTex]
compute(x=1, y=1, z=1)
void linearGradient(uint3 thread_id : GlobalThreadID) {
    // Compute normalized position along gradient axis
    // Interpolate between stops
    // Write to outputTex at thread_id.xy
}
```

And similarly for a `radialGradient` compute function that samples by distance from center.

### 1.2 Wire up the const buffer write

In `createGradientTexture` (line ~698), the const buffer write is commented out. Uncomment and complete:

```cpp
bufferWriter->setOutputBuffer(constBuffer);
bufferWriter->structBegin();
bufferWriter->writeFloat(gradient.arg);
bufferWriter->structEnd();
bufferWriter->sendToBuffer();
bufferWriter->flush();
```

### 1.3 Connect gradient texture to shape rendering

The texture pipeline path (`useTextureRenderPipeline = true` when brush is not Color) needs to actually call `createGradientTexture` to produce `texturePaint` before the draw. Currently the gradient case sets `useTextureRenderPipeline = true` but never populates the texture.

### 1.4 Test

Verify gradient brushes render visibly on `drawRect`, `drawRoundedRect`, `drawEllipse`, and `drawPath` (fill).

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — shader source, `createGradientTexture`, `renderToTarget` gradient path

---

## Phase 2 — Gradient API extensions

**Goal:** Replace the single `float arg` with proper gradient geometry. Unblocks SVG gradient import and CSS-style gradients.

### 2.1 Gradient geometry structs

```cpp
struct Gradient {
    // ... existing Type enum, stops ...

    struct LinearDef {
        float startX = 0.f, startY = 0.f;
        float endX   = 0.f, endY   = 1.f;   // default: top-to-bottom
    };

    struct RadialDef {
        float centerX = 0.5f, centerY = 0.5f;
        float radiusX = 0.5f, radiusY = 0.5f;  // elliptical
        float focusX  = 0.5f, focusY  = 0.5f;  // optional focal point
    };

    union {
        float     arg;         // legacy
        LinearDef linearDef;
        RadialDef radialDef;
    };
};
```

Coordinates are shape-relative: `[0,0]` = top-left, `[1,1]` = bottom-right of the shape's bounding box.

### 2.2 New factory methods

```cpp
static Gradient Linear(std::initializer_list<GradientStop> stops,
                        float startX, float startY,
                        float endX, float endY);

static Gradient Radial(std::initializer_list<GradientStop> stops,
                        float centerX, float centerY,
                        float radiusX, float radiusY);

// With explicit focal point:
static Gradient Radial(std::initializer_list<GradientStop> stops,
                        float centerX, float centerY,
                        float radiusX, float radiusY,
                        float focusX, float focusY);
```

The existing `Linear(stops, angle)` and `Radial(stops, radius)` remain and internally convert to `LinearDef` / `RadialDef`.

### 2.3 Spread mode

```cpp
enum class GradientSpread : uint8_t {
    Pad,       // extend first/last stop color (current implicit behavior)
    Reflect,   // mirror gradient back and forth
    Repeat     // tile the gradient
};

struct Gradient {
    GradientSpread spread = GradientSpread::Pad;
};
```

### 2.4 Shader updates

Extend `GradientTextureConstParams` to carry `startX, startY, endX, endY` (linear) or `centerX, centerY, radiusX, radiusY, focusX, focusY` (radial) plus `uint spreadMode`. Update the compute shader to:

- Interpolate along the start→end line segment (linear) or by elliptical distance (radial)
- Remap out-of-range `t` values based on spread mode (clamp / fmod / mirror)

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — `LinearDef`, `RadialDef`, `GradientSpread`, union, new factories
- `wtk/src/Composition/Brush.cpp` — implement new factories, legacy factory conversion
- `wtk/src/Composition/backend/RenderTarget.cpp` — shader source, `createGradientTexture` buffer layout
- `wtk/src/Composition/backend/RenderTarget.h` — update `createGradientTexture` signature

---

## Phase 3 — Canvas drawing extensions

**Goal:** Fill the gaps in the Canvas drawing surface for general UI work.

### 3.1 `drawLine`

```cpp
void drawLine(OmegaGTE::GPoint2D from,
              OmegaGTE::GPoint2D to,
              Core::SharedPtr<Brush> & brush,
              float strokeWidth = 1.f);
```

Implementation: construct a temporary `Path` from `from` to `to`, set brush and stroke, delegate to `drawPath`.

### 3.2 `drawPolyline`

```cpp
void drawPolyline(const OmegaCommon::Vector<OmegaGTE::GPoint2D> & points,
                  Core::SharedPtr<Brush> & strokeBrush,
                  float strokeWidth,
                  bool closed = false,
                  Core::Optional<Core::SharedPtr<Brush>> fillBrush = std::nullopt);
```

Implementation: build a `Path` from the point list, optionally `close()`, set brushes, delegate to `drawPath`.

### 3.3 Per-draw opacity

Add opacity to individual draw commands instead of requiring layer effects or baking it into the color.

```cpp
struct DrawOptions {
    float opacity = 1.f;
};

void setDrawOptions(const DrawOptions & options);
void resetDrawOptions();
```

When `opacity < 1.0`, multiply it into the alpha channel during vertex color write (solid fills) or into the fragment output (textured/gradient fills). `VisualCommand` gains an `opacity` field.

**Not** on `Brush` — brushes are shared objects and per-draw opacity on a shared brush is confusing. Canvas owns the draw state.

### 3.4 `drawArc` (convenience)

```cpp
void drawArc(OmegaGTE::GRect bounds,
             float startAngle, float endAngle,
             Core::SharedPtr<Brush> & brush,
             bool pie = false,
             Core::Optional<Border> border = std::nullopt);
```

Implementation: `Path` already has `addArc(bounds, startAngle, endAngle)`. Build a Path, optionally add center-to-endpoint lines for pie, delegate to `drawPath`.

### 3.5 Brush parameter ergonomics

Change Canvas draw methods to accept `const Core::SharedPtr<Brush> &` instead of `Core::SharedPtr<Brush> &` so that temporaries and rvalues work:

```cpp
// Before:
void drawRect(Core::Rect & rect, Core::SharedPtr<Brush> & brush, ...);

// After:
void drawRect(Core::Rect & rect, const Core::SharedPtr<Brush> & brush, ...);
```

This lets callers write `canvas->drawRect(r, ColorBrush(Color::create8Bit(0xFF0000)))` without naming the brush.

### Files touched

- `wtk/include/omegaWTK/Composition/Canvas.h` — new methods, `DrawOptions`, const-ref brush params
- `wtk/src/Composition/Canvas.cpp` — implement new methods
- `wtk/src/Composition/backend/RenderTarget.cpp` — apply opacity multiplier in vertex/fragment output

---

## Phase 4 — Color improvements

**Goal:** Ergonomic color construction and manipulation for UI code.

### 4.1 Named color constants

```cpp
struct Color {
    static const Color Black;
    static const Color White;
    static const Color Red;
    static const Color Green;
    static const Color Blue;
    static const Color Yellow;
    static const Color Orange;
    static const Color Purple;
    // Transparent already exists
};
```

Defined in `Brush.cpp` via `create8Bit(Eight::...)`. Lets callers write `Color::Red` instead of `Color::create8Bit(Color::Eight::Red8)`.

### 4.2 HSL/HSV construction

```cpp
static Color fromHSL(float h, float s, float l, float a = 1.0f);
static Color fromHSV(float h, float s, float v, float a = 1.0f);
```

Pure CPU code. Useful for themes, data visualization, programmatic color generation.

### 4.3 Arithmetic helpers

```cpp
static Color lerp(const Color & a, const Color & b, float t);
Color withAlpha(float a) const;
Color lighter(float amount = 0.2f) const;
Color darker(float amount = 0.2f) const;
```

`lerp` is needed for animation and gradient evaluation. `withAlpha` avoids reconstructing colors for opacity. `lighter`/`darker` are standard UI helpers.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — new statics and methods on `Color`
- `wtk/src/Composition/Brush.cpp` — implement constants, factories, helpers

---

## Phase 5 — Canvas state stack

**Goal:** Support transforms and clipping for zoomed, panned, or clipped content.

### 5.1 Save/restore

```cpp
void save();     // push transform + clip + draw options
void restore();  // pop and restore
```

Internal stack of `{transform matrix, clip region, DrawOptions}`.

### 5.2 Transforms

```cpp
void translate(float dx, float dy);
void scale(float sx, float sy);
void rotate(float radians);
```

Each modifies the current 2D affine matrix. All subsequent `VisualCommand` coordinates are interpreted through the current transform.

### 5.3 Clipping

```cpp
void clipRect(const Core::Rect & rect);
void clipRoundedRect(const Core::RoundedRect & rect);
```

Intersect the current clip region. Applied at submit time or during compositing.

### Files touched

- `wtk/include/omegaWTK/Composition/Canvas.h` — save/restore, transform, clip methods
- `wtk/src/Composition/Canvas.cpp` — state stack implementation
- `wtk/src/Composition/backend/RenderTarget.cpp` — apply transform to vertex positions, apply clip

---

## Phase 6 — Future

These items are deferred. They are listed to confirm the Phase 0–5 designs are forward-compatible.

| Item | Depends on | Notes |
|------|------------|-------|
| `GradientSpace::Canvas` (gradient coords in canvas space, not shape-local) | Phase 5 transform stack | Without transforms, "canvas space" is just "shape space + offset" |
| Pattern brush (image/texture tiling) | Phase 1 gradient pipeline | Same texture render path; needs wrap-mode sampler |
| `BlendMode` enum on `DrawOptions` | Phase 3 DrawOptions | Extend fragment shader with blend equations |
| Gradient text | Phase 1 gradient pipeline | Backend generates gradient texture at glyph bounds |
| Image scale modes (aspect-fit/fill, tiling, source rect) | — | Extend UV generation in bitmap path |
| Text draw options (maxLines, truncation, underline) | — | Text layout engine changes |
| Effect bounds (subregion blur) | — | Compositor change |

---

## Dependency graph

```
Phase 0: Foundation cleanup
    └─→ Phase 1: Gradient pipeline (depends on type-based dispatch)
            ├─→ Phase 2: Gradient API extensions (depends on working pipeline)
            └─→ Phase 6: Pattern brush, gradient text (depend on texture pipeline)
    └─→ Phase 3: Canvas drawing extensions (independent of gradients)
            └─→ Phase 5: Canvas state stack (builds on DrawOptions infrastructure)

Phase 4: Color improvements (independent — can run in parallel with any phase)
```

Phases 3 and 4 can proceed in parallel with Phases 1–2. Phase 5 should follow Phase 3 since `save`/`restore` builds on the `DrawOptions` concept.

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/Composition/Brush.h` | 0, 2, 4 | Remove `isColor`/`isGradient`; add `LinearDef`, `RadialDef`, `GradientSpread`, new gradient factories; add `Color` statics and helpers |
| `wtk/src/Composition/Brush.cpp` | 0, 2, 4 | Remove boolean init; implement new gradient factories, color constants, HSL/HSV, lerp, withAlpha, lighter/darker |
| `wtk/include/omegaWTK/Composition/Canvas.h` | 3, 5 | `drawLine`, `drawPolyline`, `drawArc`, `DrawOptions`, const-ref brush params, save/restore/transform/clip |
| `wtk/src/Composition/Canvas.cpp` | 3, 5 | Implement new draw methods, state stack |
| `wtk/src/Composition/backend/RenderTarget.cpp` | 0, 1, 2, 3, 5 | Type-based dispatch; gradient compute shader; extended gradient params; opacity multiplier; transform application |
| `wtk/src/Composition/backend/RenderTarget.h` | 2 | Update `createGradientTexture` signature |

---

## References

- Current Brush/Color/Gradient: `wtk/include/omegaWTK/Composition/Brush.h`, `wtk/src/Composition/Brush.cpp`
- Current Canvas API: `wtk/include/omegaWTK/Composition/Canvas.h`
- Path API: `wtk/include/omegaWTK/Composition/Path.h`
- Backend rendering: `wtk/src/Composition/backend/RenderTarget.cpp`, `RenderTarget.h`
- UIView (general UI consumer): `wtk/src/UI/UIView.cpp`
- SVGView (vector rendering consumer): `wtk/src/UI/SVGView.cpp`
- Shader source: inline `librarySource` string in `RenderTarget.cpp` (lines 111–200)
