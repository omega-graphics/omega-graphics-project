# Brush API Extension Proposal

This document proposes extensions to the `Brush`, `Color`, and `Gradient` types in `omegaWTK/Composition/Brush.h` to complete the fill model for the composition system. It is a companion to the [Canvas API Extension Proposal](Canvas-API-Extension-Proposal.md), which covers the drawing surface; this document covers the *paint* that goes into it.

---

## 1. Current state

### 1.1 Brush

`Brush` is a tagged union over two fill types:

```cpp
struct Brush {
    bool isColor;
    bool isGradient;
    union { Color color; Gradient gradient; };
    Brush(const Color &);
    Brush(const Gradient &);
};
```

Factory helpers: `ColorBrush(const Color &)`, `GradientBrush(const Gradient &)`.

**Issues:**

| # | Issue | Impact |
|---|-------|--------|
| 1 | `GradientBrush()` header declares zero params; `.cpp` takes `const Gradient &`. Callers that include only the header get a linker error. | Build correctness |
| 2 | Two booleans (`isColor`, `isGradient`) encode a state that is always one-of-two. A single enum would be unambiguous and extensible. | Maintainability |
| 3 | No brush types beyond solid color and gradient (no image/pattern, no "none"/transparent sentinel). | Feature gap |
| 4 | No opacity or transform on the brush itself; callers must premultiply alpha or use layer effects. | Ergonomics |

### 1.2 Color

- Normalized `float r, g, b, a` storage.
- Factory methods for 8-bit, 16-bit, 32-bit channel construction.
- Packed-hex overloads for 8-bit and 16-bit.
- Named constant enums (`Eight`, `Sixteen`) for standard colors.
- `create32it` typo (missing `B` in `Bit`).
- No HSL/HSV/HSB construction, no color space tag, no interpolation helpers.

### 1.3 Gradient

- Linear (angle + stops) and Radial (radius + stops).
- Single `float arg` overloaded for angle or radius.
- Stops are `Vector<GradientStop>` with `float pos` and `Color color`.
- No start/end point for linear; no center/focus for radial; no elliptical radii.
- No spread mode (pad, reflect, repeat).
- Coordinate space is implicit (shape-local assumed by backend, but undocumented).

---

## 2. Proposed extensions

### 2.1 Brush type enum and tagged-union cleanup

Replace the two booleans with a `Type` enum and prepare for new brush kinds.

```cpp
struct Brush {
    enum class Type : uint8_t {
        Color,
        Gradient,
        Pattern,   // reserved (2.4)
        None       // transparent / no-fill sentinel
    };
    Type type;

    union {
        Composition::Color   color;
        Composition::Gradient gradient;
        // future: Pattern pattern;
    };

    Brush(const Color &);
    Brush(const Gradient &);
    static Brush transparent();   // Type::None

    ~Brush();
};
```

- Existing `isColor` / `isGradient` become `type == Type::Color` / `type == Type::Gradient`.
- Backend switch sites (`if(brush->isColor)`) migrate to `switch(brush->type)`.
- `Type::None` gives callers an explicit way to express "no fill" without constructing a transparent color brush — useful for stroke-only shapes once borders land (Canvas proposal 2.1).

**Migration:** Keep `isColor` / `isGradient` as deprecated inline getters during transition:

```cpp
[[deprecated("use type == Type::Color")]]
bool isColorBrush() const { return type == Type::Color; }
```

### 2.2 Fix header/source mismatch

The header currently declares:

```cpp
Core::SharedPtr<Brush> GradientBrush();  // zero params
```

The source defines:

```cpp
Core::SharedPtr<Brush> GradientBrush(const Gradient & gradient);
```

**Fix:** Update the header declaration to match the source:

```cpp
OMEGAWTK_EXPORT Core::SharedPtr<Brush> GradientBrush(const Gradient & gradient);
```

### 2.3 Color improvements

#### 2.3.1 Fix `create32it` typo

```cpp
// before
static Color create32it(uint32_t r, uint32_t g, uint32_t b, uint32_t a);

// after
static Color create32Bit(uint32_t r, uint32_t g, uint32_t b, uint32_t a);
```

Keep a deprecated forwarding alias if downstream code references the old name.

#### 2.3.2 Named color constants

Replace the enums with `static const Color` members so callers get `Color` values directly instead of raw integers that must be routed through a factory.

```cpp
// In addition to (or replacing) the Eight/Sixteen enums:
static const Color Black;    // create8Bit(0x000000)
static const Color White;    // create8Bit(0xFFFFFF)
static const Color Red;      // create8Bit(0xFF0000)
static const Color Green;    // create8Bit(0x00FF00)
static const Color Blue;     // create8Bit(0x0000FF)
static const Color Yellow;   // create8Bit(0xFFFF00)
static const Color Orange;   // create8Bit(0xFF8000)
static const Color Purple;   // create8Bit(0xFF00FF)
// Transparent already exists as Color::Transparent
```

This lets callers write `ColorBrush(Color::Red)` instead of `ColorBrush(Color::create8Bit(Color::Eight::Red8))`.

#### 2.3.3 HSL construction

```cpp
/// Hue in [0, 360), saturation and lightness in [0.0, 1.0].
static Color fromHSL(float h, float s, float l, float a = 1.0f);

/// Hue in [0, 360), saturation and value/brightness in [0.0, 1.0].
static Color fromHSV(float h, float s, float v, float a = 1.0f);
```

Useful for programmatic color generation (themes, data visualisation, animation interpolation).

#### 2.3.4 Arithmetic helpers

```cpp
/// Linear interpolation between two colors. t in [0.0, 1.0].
static Color lerp(const Color & a, const Color & b, float t);

/// Return a copy with the alpha channel replaced.
Color withAlpha(float a) const;

/// Return a copy with brightness scaled (clamped to [0, 1]).
Color lighter(float amount = 0.2f) const;
Color darker(float amount = 0.2f) const;
```

`lerp` is needed by animation and gradient evaluation. `withAlpha` avoids reconstructing colors just to change opacity. `lighter`/`darker` are common UI helpers.

### 2.4 Gradient extensions

#### 2.4.1 Linear gradient by start/end point

The current `Linear(stops, angle)` defines direction as a single angle with no anchor. Toolkits (CSS, SVG, CoreGraphics, Direct2D) define linear gradients with a start point and end point (or equivalently, a line segment in some coordinate space).

```cpp
struct Gradient {
    // ...existing...

    // New: explicit start/end points (in local shape coordinates, [0,0] = top-left, [1,1] = bottom-right)
    struct LinearDef {
        float startX = 0.f, startY = 0.f;
        float endX   = 0.f, endY   = 1.f;  // default: top-to-bottom
    };

    struct RadialDef {
        float centerX = 0.5f, centerY = 0.5f;  // center of ellipse
        float radiusX = 0.5f;                    // horizontal radius (in [0,1] shape-relative)
        float radiusY = 0.5f;                    // vertical radius (allows elliptical)
        float focusX  = 0.5f, focusY = 0.5f;    // focal point (optional; defaults to center)
    };

    union {
        float     arg;        // legacy (angle for linear, radius for radial)
        LinearDef linearDef;
        RadialDef radialDef;
    };

    // New factories (old ones remain for backward compatibility):
    static Gradient Linear(std::initializer_list<GradientStop> stops,
                           float startX, float startY,
                           float endX, float endY);

    static Gradient Radial(std::initializer_list<GradientStop> stops,
                           float centerX, float centerY,
                           float radiusX, float radiusY);

    static Gradient Radial(std::initializer_list<GradientStop> stops,
                           float centerX, float centerY,
                           float radiusX, float radiusY,
                           float focusX, float focusY);
};
```

The angle-based `Linear(stops, angle)` factory remains but internally converts to `LinearDef`. Backend reads `linearDef` / `radialDef` instead of `arg`.

#### 2.4.2 Spread mode

When a gradient is narrower than the shape it fills, what happens outside the defined range?

```cpp
enum class GradientSpread : uint8_t {
    Pad,      // Extend the first/last stop color (default; current implicit behavior)
    Reflect,  // Mirror the gradient back and forth
    Repeat    // Tile the gradient
};

struct Gradient {
    // ...
    GradientSpread spread = GradientSpread::Pad;
};
```

Backend: the compute shader (or CPU fallback) checks `spread` when sampling positions outside `[0, 1]`.

#### 2.4.3 Coordinate space

Document and optionally make explicit whether gradient coordinates are shape-relative or canvas-absolute.

```cpp
enum class GradientSpace : uint8_t {
    ShapeLocal,  // [0,0]-[1,1] maps to the shape's bounding box (default)
    Canvas       // Coordinates are in canvas logical pixels
};

struct Gradient {
    // ...
    GradientSpace space = GradientSpace::ShapeLocal;
};
```

`ShapeLocal` matches current behavior. `Canvas` enables gradients that span multiple shapes with a single continuous sweep (common in SVG `gradientUnits="userSpaceOnUse"`).

### 2.5 Brush opacity

Per-brush opacity multiplier, independent of the color's own alpha.

```cpp
struct Brush {
    // ...
    float opacity = 1.0f;  // multiplied with fill alpha at render time
};
```

This avoids the need for a separate layer or `DrawOptions` just to fade a gradient brush. Backend multiplies `opacity` into the alpha channel during vertex or fragment generation.

### 2.6 Pattern brush (future)

Reserve the type slot and define the struct for image/texture tiling.

```cpp
struct Pattern {
    SharedHandle<Media::BitmapImage> image;  // or GETexture for GPU-resident
    enum class TileMode : uint8_t { Tile, TileX, TileY, Clamp } tileMode = TileMode::Tile;
    float scaleX = 1.f, scaleY = 1.f;
    float offsetX = 0.f, offsetY = 0.f;
};

struct Brush {
    // ...
    union {
        Color    color;
        Gradient gradient;
        Pattern  pattern;
    };
    Brush(const Pattern &);
};

OMEGAWTK_EXPORT Core::SharedPtr<Brush> PatternBrush(const Pattern & pattern);
```

Backend: generates a tiled texture from the image and renders it the same way gradient textures are rendered (textured pipeline, UV generation). The existing `textureRenderPipelineState` and `writeTexVertexToBuffer` paths handle this with a wrap-mode sampler.

**Note:** This is explicitly a future item. It is listed here to validate that the `Brush::Type` enum (2.1) and the union layout are forward-compatible with it.

---

## 3. Implementation priority

| Priority | Section | Change | Rationale | Status |
|----------|---------|--------|-----------|--------|
| ~~P0~~ | ~~2.2~~ | ~~Fix `GradientBrush` header signature~~ | ~~Correctness; one-line fix.~~ | Done |
| ~~P0~~ | ~~2.3.1~~ | ~~Fix `create32it` typo~~ | ~~Correctness; one-line rename.~~ | Done |
| ~~P0~~ | ~~2.1~~ | ~~`Brush::Type` enum, replace booleans~~ | ~~Extensibility foundation; required before new types.~~ | Done |
| P1 | 2.3.2 | Named `Color` constants | Ergonomics; small surface. | |
| P1 | 2.5 | `Brush::opacity` | Enables fade without layer effects; small backend surface. | |
| P1 | 2.4.1 | Linear start/end, radial center/radii | Unblocks SVG gradient import and CSS-style gradients. | |
| P2 | 2.4.2 | Gradient spread mode | Needed for full SVG compliance. | |
| P2 | 2.4.3 | Gradient coordinate space enum | Needed for `gradientUnits="userSpaceOnUse"` in SVG. | |
| P2 | 2.3.3 | HSL/HSV construction | Useful for animation/themes; pure CPU code. | |
| P2 | 2.3.4 | `lerp`, `withAlpha`, `lighter`/`darker` | Animation and UI conveniences; pure CPU code. | |
| P3 | 2.6 | Pattern brush | Requires sampler/wrap-mode work in each backend. | |

---

## 4. Backend impact

### 4.1 Brush type enum (2.1)

All three backends (Metal, DX12, Vulkan) touch `isColor`/`isGradient` through `RenderTarget.cpp::renderToTarget`. The change is mechanical: replace boolean checks with a switch on `Brush::Type`. No shader changes.

### 4.2 Gradient extensions (2.4)

- **Linear start/end:** The existing `GradientTextureConstParams` buffer passes a single `float arg` (angle). Extend it to pass `startX, startY, endX, endY` and update the compute shader to interpolate along the defined line segment instead of a global angle.
- **Radial elliptical:** Extend the compute shader to accept `radiusX, radiusY, focusX, focusY`. The current single-radius path becomes a special case where `radiusX == radiusY` and focus equals center.
- **Spread mode:** Add a `uint spreadMode` to the constant buffer. The shader uses it to remap out-of-range `t` values (clamp / fmod / mirror).

Shader changes are confined to a single compute function; vertex and fragment pipelines are unaffected.

### 4.3 Pattern brush (2.6)

Uses the existing texture render pipeline (`textureRenderPipelineState`). New work is limited to:
- Creating a GPU texture from `BitmapImage` (the `drawImage` path already does this).
- Setting sampler wrap mode to repeat/mirror based on `TileMode`.
- Generating UVs that account for `scale` and `offset`.

### 4.4 Opacity (2.5)

Multiply `brush->opacity` into the alpha component during vertex color write (`writeColorVertexToBuffer`) or into the fragment output for textured draws. One multiplication per vertex or one uniform, depending on path.

---

## 5. Interaction with Canvas API proposal

| Canvas proposal item | Brush dependency | Notes |
|----------------------|------------------|-------|
| 2.1 Borders | `Brush::Type::None` for fill-less bordered shapes | Border brush is already `SharedPtr<Brush>`; no change needed. |
| 2.5 DrawOptions (opacity, blend) | `Brush::opacity` stacks multiplicatively with `DrawOptions::opacity` | Document the multiplication order. |
| 2.8 Gradient text | Requires gradient brush to work with text rendering path | Backend must generate gradient texture at text glyph bounds. |
| 2.9 Gradient in canvas space | `GradientSpace::Canvas` (2.4.3) | This is the brush-side implementation of that Canvas proposal item. |

---

## 6. File change summary

| File | Changes |
|------|---------|
| `wtk/include/omegaWTK/Composition/Brush.h` | `Brush::Type` enum; `Gradient::LinearDef`, `RadialDef`, `GradientSpread`, `GradientSpace`; `Brush::opacity`; new `Color` statics and methods; `Pattern` struct (reserved). Fix `GradientBrush` signature and `create32it` typo. |
| `wtk/src/Composition/Brush.cpp` | Implement new `Color` factories (`fromHSL`, `fromHSV`, `lerp`, `withAlpha`, `lighter`, `darker`), named constants, new `Gradient` factories, `Brush::transparent()`. |
| `wtk/src/Composition/backend/RenderTarget.cpp` | Replace `isColor`/`isGradient` checks with `switch(brush->type)`. Multiply `brush->opacity` into alpha. Pass extended gradient params to shader. |
| `wtk/src/Composition/backend/RenderTarget.h` | Update `createGradientTexture` signature if gradient param struct changes. |
| Shader source (inline in `RenderTarget.cpp`) | Extend `GradientTextureConstParams` with start/end, radii, spread. |
| `wtk/src/UI/SVGView.cpp` | Adopt new gradient factories when parsing `<linearGradient>` / `<radialGradient>` elements. |

---

## 7. References

- Current Brush/Color/Gradient: `wtk/include/omegaWTK/Composition/Brush.h`, `wtk/src/Composition/Brush.cpp`
- Backend rendering: `wtk/src/Composition/backend/RenderTarget.cpp` (gradient texture generation, brush dispatch)
- Canvas API Extension Proposal: `wtk/docs/Canvas-API-Extension-Proposal.md`
- Shader source: inline `librarySource` string in `RenderTarget.cpp` (lines 108-197)
