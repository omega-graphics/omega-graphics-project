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
| `Core/Core.h` includes `<OmegaGTE.h>` | `Core.h:3`, all downstream headers | Every TU that touches any WTK header transitively compiles the entire GTE surface (3D types, matrix templates, shader pipeline, etc.) even if it only needs `Rect` |
| Raw `OmegaGTE::` types in public API | `Path.h`, `Canvas.h`, `Animation.h` | `GPoint2D`, `GVectorPath2D`, `GETexture`, `FMatrix<4,4>` leak into public signatures; downstream modules can't avoid the GTE include |

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

## Phase 0A — Geometry type isolation

**Goal:** Stop exposing `<OmegaGTE.h>` through Core and Composition public headers. Define standalone geometry wrapper structs in the Composition submodule (which owns the Compositor — the only part that actually talks to GTE) so that OmegaGTE can be linked to `OmegaWTK_Composition` only instead of the top-level `OmegaWTK` target. The exception is `OmegaGTEView`, which talks to GTE directly by design.

### Problem

`Core/Core.h` does `#include <OmegaGTE.h>` at the top. Every header that includes `Core.h` — which is every Composition and UI header — pulls in the entire GTE surface: vector path templates, matrix types, 3D geometry, shader pipeline types, etc. This inflates translation units across all of WTK even though most modules only need a handful of 2D geometry PODs.

Additionally, several Composition headers expose raw `OmegaGTE::` types in public signatures:

| Type | Where exposed |
|------|---------------|
| `OmegaGTE::GPoint2D` | `Path.h` — `goTo()`, `addLine()`, constructor |
| `OmegaGTE::GVectorPath2D` | `Path.h` — `Segment` fields, constructor; `Canvas.h` — `VisualCommand::Data::pathParams` |
| `OmegaGTE::GETexture` / `OmegaGTE::GEFence` | `Canvas.h` — `drawGETexture()`, `VisualCommand::Data::bitmapParams`; `FontEngine.h` — `TextRect::BitmapRes` |
| `OmegaGTE::FMatrix<4,4>` | `Canvas.h` — `VisualCommand::Data::transformMatrix`, `setElementTransform()` |

### CMake context

The current build has six static submodule libraries (`OmegaWTK_Core`, `OmegaWTK_Native`, `OmegaWTK_Media`, `OmegaWTK_Composition`, `OmegaWTK_UI`, `OmegaWTK_Widgets`) linked into the `OmegaWTK` shared framework via whole-archive flags (`-Wl,-force_load` / `/WHOLEARCHIVE` / `--whole-archive`).

Because whole-archive linking bypasses CMake's transitive dependency resolution, every dependency that any submodule needs must be re-declared on the final `OmegaWTK` target. Currently OmegaGTE is linked at the framework level (lines 517–520) even though only the Composition backend actually calls GTE. After this phase, `OmegaWTK_Composition` links OmegaGTE directly and the framework-level OmegaGTE link can be dropped (or kept `PRIVATE` only for the whole-archive resolution).

### 0A.1 Create `Composition/Geometry.h` with standalone structs

A new lightweight header in the **Composition submodule** that defines WTK's geometry types — no GTE include. This is the Composition submodule's public contract for geometric primitives, consumed by UI, Widgets, and other submodules without pulling in GTE.

```cpp
// Composition/Geometry.h — no OmegaGTE dependency
#pragma once
#include "omegaWTK/Core/OmegaWTKExport.h"

namespace OmegaWTK::Composition {

struct OMEGAWTK_EXPORT Point2D {
    float x = 0.f, y = 0.f;
};

struct OMEGAWTK_EXPORT Rect {
    Point2D pos;
    float w = 0.f, h = 0.f;
};

struct OMEGAWTK_EXPORT RoundedRect {
    Point2D pos;
    float w = 0.f, h = 0.f, rad_x = 0.f, rad_y = 0.f;
};

struct OMEGAWTK_EXPORT Ellipse {
    float x = 0.f, y = 0.f;
    float rad_x = 0.f, rad_y = 0.f;
};

} // namespace OmegaWTK::Composition
```

These are layout-compatible with the GTE originals (same field order and types) so conversion is a `reinterpret_cast` or member-wise copy — no runtime cost.

Composition now owns these types. The old `Composition::Rect`, `Composition::Point2D`, `Composition::RoundedRect`, `Composition::Ellipse` are removed entirely — all callsites migrate to `Composition::` (see 0A.3).

### 0A.2 Create `Composition/GeometryConvert.h` (private/backend-only)

A backend-internal header (under `wtk/src/Composition/backend/` or a private include path) that includes both `Composition/Geometry.h` and `<OmegaGTE.h>` and provides conversion functions:

```cpp
// GeometryConvert.h — only include from Composition .cpp files
#pragma once
#include "omegaWTK/Composition/Geometry.h"
#include <OmegaGTE.h>

namespace OmegaWTK::Composition {

inline OmegaGTE::GRect toGTE(const Rect & r) {
    return {{r.pos.x, r.pos.y}, r.w, r.h};
}
inline Rect fromGTE(const OmegaGTE::GRect & r) {
    return {{r.pos.x, r.pos.y}, r.w, r.h};
}
// ... same pattern for RoundedRect, Point2D, Ellipse
}
```

This header lives in the Composition source tree, not in the public include directory. Only Composition `.cpp` files include it.

### 0A.3 Remove geometry types from `Core/Core.h`

- Remove `#include <OmegaGTE.h>` from `Core/Core.h`.
- Delete the existing typedefs (`typedef OmegaGTE::GRect Rect`, `typedef OmegaGTE::GPoint2D Position`, `typedef OmegaGTE::GRoundedRect RoundedRect`) and the `Ellipse` wrapper struct.
- **No aliases back into Core.** Core stops owning geometry types entirely. Composition is upstream of the geometric vocabulary.
- Move `extern OmegaGTE::GTE gte;` to a separate `Core/GTEHandle.h` that only Composition backend code includes.

### 0A.3a Migrate all `Core::` geometry callsites to `Composition::` [Done]

~978 occurrences across ~130 files. Mechanical rename:

| Old | New |
|-----|-----|
| `Composition::Rect` | `Composition::Rect` |
| `Composition::Point2D` | `Composition::Point2D` |
| `Composition::RoundedRect` | `Composition::RoundedRect` |
| `Composition::Ellipse` | `Composition::Ellipse` |

Files span all submodules: Composition headers/sources, UI, Widgets, Native, Media, tests, and docs. Since the rename is purely mechanical (no semantic change), it can be done in a single pass with search-and-replace tooling.

Headers that currently include `Core/Core.h` for geometry types should include `Composition/Geometry.h` instead (or in addition, if they still need other Core utilities). Since `Composition/Geometry.h` is a standalone header with no Core dependency, this does not introduce a circular include.

### 0A.4 Wrap remaining raw GTE types in Composition public API

**Path.h:**
- Public methods (`goTo`, `addLine`, constructor) change from `OmegaGTE::GPoint2D` to `Composition::Point2D`.
- `addArc` changes from `OmegaGTE::GRect` to `Composition::Rect`.
- The internal `Segment` struct keeps `OmegaGTE::GVectorPath2D` — it's `friend class Canvas` / private, not public API. But the `Path(GVectorPath2D &)` constructor should either become private/friend-only or take a WTK-level wrapper. For now, make it `private` with `friend class Canvas` since it's only used internally.

**Canvas.h:**
- `drawGETexture` stays — it's the explicit "talk to GTE" escape hatch, analogous to `OmegaGTEView`. Forward-declare `OmegaGTE::GETexture` and `OmegaGTE::GEFence` so the header compiles without the full GTE include.
- `setElementTransform(const OmegaGTE::FMatrix<4,4> &)` — replace with a WTK-level `Transform2D` or accept `float[16]` / a simple 3x2 matrix struct appropriate for 2D. This ties into Phase 5's transform stack; for now, a forward-declared opaque type or `float[16]` wrapper keeps GTE out of the header.
- `VisualCommand::Data` — internal fields like `transformMatrix` and `pathParams.path` can forward-declare GTE types rather than include them, or use `void*` + typed accessors in the backend. Prefer forward declarations.

### 0A.5 CMake: link OmegaGTE to `OmegaWTK_Composition` only

```cmake
# Before (line 421):
target_link_libraries(OmegaWTK_Composition PUBLIC OmegaCommon)

# After:
target_link_libraries(OmegaWTK_Composition
    PUBLIC  OmegaCommon
    PRIVATE OmegaGTE)
```

The `PRIVATE` link means OmegaGTE symbols are pulled into `OmegaWTK_Composition`'s object files but the dependency is not propagated to consumers. Since the whole-archive link embeds those object files into the final `OmegaWTK` shared library, the GTE symbols resolve at framework link time through the embedded objects.

On the framework-level `OmegaWTK` target (lines 517–522):
- **Remove** the `PUBLIC OmegaGTE` link and the OmegaGTE `INCLUDE_DIRECTORIES` propagation. Other submodules and downstream consumers no longer need GTE headers or symbols.
- **Keep** `OmegaGTE` in `OMEGAWTK_MODULE_DEPS` (line 441) for build ordering, since `OmegaWTK_Composition` depends on it being built first.
- On macOS, `target_link_frameworks("OmegaWTK" OmegaGTE)` (line 517) stays if the framework needs to resolve GTE symbols at load time — but can potentially move to `PRIVATE` or be replaced by the whole-archive resolution. Evaluate at implementation time.

Similarly, remove the GTE include-directory propagation from `OmegaWTK_Composition`'s `PUBLIC` include dirs (line 419) — GTE headers are now only needed by Composition's own `.cpp` files, so `PRIVATE` suffices:

```cmake
target_include_directories(OmegaWTK_Composition
    PUBLIC  ${OMEGAWTK_PUBLIC_INCLUDE_DIR}
            $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
    PRIVATE $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>
            ${NATIVE_PRIVATE_INCLUDE_DIR})
```

### 0A.6 Update Composition `.cpp` files

Every `.cpp` file in the Composition submodule that actually constructs GTE objects (the backend, `Canvas.cpp`, `Path.cpp`, `RenderTarget.cpp`) adds `#include "GeometryConvert.h"` and converts at the WTK↔GTE boundary.

### 0A.7 Verify isolation

1. Grep all headers under `wtk/include/` — the only file allowed to include `<OmegaGTE.h>` should be `OmegaGTEView.h` (which has its own rules). All other GTE usage goes through `Composition/Geometry.h` types or forward declarations.
2. Build `OmegaWTK_Core`, `OmegaWTK_UI`, `OmegaWTK_Widgets` individually — they must compile without OmegaGTE headers on the include path.
3. Build the full `OmegaWTK` framework — GTE symbols resolve through the whole-archive'd `OmegaWTK_Composition` objects.

### Files touched

- `wtk/include/omegaWTK/Composition/Geometry.h` — **new** standalone geometry structs (no GTE dependency)
- `wtk/src/Composition/backend/GeometryConvert.h` — **new** conversion helpers (Composition-private)
- `wtk/include/omegaWTK/Core/Core.h` — remove `#include <OmegaGTE.h>`, delete geometry typedefs and `Ellipse` struct, move `extern GTE gte` to `GTEHandle.h`
- `wtk/include/omegaWTK/Core/GTEHandle.h` — **new** backend-only header for the GTE engine handle
- `wtk/include/omegaWTK/Composition/Path.h` — `GPoint2D` → `Composition::Point2D`, `GRect` → `Composition::Rect` in public API
- `wtk/include/omegaWTK/Composition/Canvas.h` — forward-declare GTE types for `drawGETexture`/`VisualCommand` internals; `Composition::Rect` → `Composition::Rect` etc.
- `wtk/include/omegaWTK/Composition/Animation.h` — `GPoint2D` → `Composition::Point2D` in public signatures
- ~130 files across all submodules — mechanical `Composition::Rect` → `Composition::Rect` (and Position/RoundedRect/Ellipse) rename
- `wtk/src/Composition/Canvas.cpp` — add `GeometryConvert.h`, convert at boundary
- `wtk/src/Composition/Path.cpp` — add `GeometryConvert.h`, convert at boundary
- `wtk/src/Composition/backend/RenderTarget.cpp` — add `GeometryConvert.h` (already includes GTE)
- `wtk/CMakeLists.txt` — link OmegaGTE `PRIVATE` to `OmegaWTK_Composition`; remove `PUBLIC` GTE from `OmegaWTK` framework target

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
void drawRect(Rect & rect, SharedPtr<Brush> & brush, ...);

// After:
void drawRect(Rect & rect, const SharedPtr<Brush> & brush, ...);
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
void clipRect(const Rect & rect);
void clipRoundedRect(const RoundedRect & rect);
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
            └─→ Future: Pattern brush, gradient text (depend on texture pipeline)
    └─→ Phase 3: Canvas drawing extensions (independent of gradients)
            └─→ Phase 5: Canvas state stack (builds on DrawOptions infrastructure)

Phase 0A: Geometry type isolation (independent — can run in parallel with Phase 0)
    └─→ Phase 3: Canvas drawing extensions (new methods use Core:: geometry types)
    └─→ Phase 5: Canvas state stack (Transform2D replaces raw FMatrix in public API)

Phase 4: Color improvements (independent — can run in parallel with any phase)
```

Phases 0 and 0A can run in parallel. Phase 0A should complete before Phase 3 so that new Canvas methods use `Composition::Point2D` / `Composition::Rect` instead of GTE types from the start. Phases 3 and 4 can proceed in parallel with Phases 1–2. Phase 5 should follow both Phase 3 (builds on `DrawOptions`) and Phase 0A (transform types).

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/Composition/Geometry.h` | 0A | **New** — standalone `Point2D`, `Rect`, `RoundedRect`, `Ellipse` structs (no GTE dependency), owned by Composition submodule |
| `wtk/src/Composition/backend/GeometryConvert.h` | 0A | **New** — WTK↔GTE conversion helpers (Composition-private) |
| `wtk/include/omegaWTK/Core/Core.h` | 0A | Remove `#include <OmegaGTE.h>`; delete geometry typedefs and `Ellipse` struct; move GTE handle to `GTEHandle.h` |
| `wtk/include/omegaWTK/Core/GTEHandle.h` | 0A | **New** — backend-only header for `extern OmegaGTE::GTE gte` |
| ~130 files across all submodules | 0A | Mechanical rename: `Composition::Rect` → `Composition::Rect`, `Composition::Point2D` → `Composition::Point2D`, etc. |
| `wtk/CMakeLists.txt` | 0A | Link OmegaGTE `PRIVATE` to `OmegaWTK_Composition`; remove `PUBLIC` GTE link from `OmegaWTK` framework |
| `wtk/include/omegaWTK/Composition/Brush.h` | 0, 2, 4 | Remove `isColor`/`isGradient`; add `LinearDef`, `RadialDef`, `GradientSpread`, new gradient factories; add `Color` statics and helpers |
| `wtk/src/Composition/Brush.cpp` | 0, 2, 4 | Remove boolean init; implement new gradient factories, color constants, HSL/HSV, lerp, withAlpha, lighter/darker |
| `wtk/include/omegaWTK/Composition/Canvas.h` | 0A, 3, 5 | Forward-declare GTE types for internal fields; `drawLine`, `drawPolyline`, `drawArc`, `DrawOptions`, const-ref brush params, save/restore/transform/clip |
| `wtk/src/Composition/Canvas.cpp` | 0A, 3, 5 | Add `GeometryConvert.h`; implement new draw methods, state stack |
| `wtk/include/omegaWTK/Composition/Path.h` | 0A | `GPoint2D` → `Composition::Point2D`, `GRect` → `Composition::Rect` in public API; privatize `GVectorPath2D` constructor |
| `wtk/src/Composition/Path.cpp` | 0A | Add `GeometryConvert.h`, convert at boundary |
| `wtk/include/omegaWTK/Composition/Animation.h` | 0A | `GPoint2D` → `Core::Point2D` in public signatures |
| `wtk/src/Composition/backend/RenderTarget.cpp` | 0, 0A, 1, 2, 3, 5 | Type-based dispatch; `GeometryConvert.h`; gradient compute shader; extended gradient params; opacity multiplier; transform application |
| `wtk/src/Composition/backend/RenderTarget.h` | 2 | Update `createGradientTexture` signature |

---

## References

- Core type definitions: `wtk/include/omegaWTK/Core/Core.h` (typedefs at lines 101–122)
- GTE geometry originals: `gte/include/omegaGTE/GTEBase.h` (lines 319–363)
- Current Brush/Color/Gradient: `wtk/include/omegaWTK/Composition/Brush.h`, `wtk/src/Composition/Brush.cpp`
- Current Canvas API: `wtk/include/omegaWTK/Composition/Canvas.h`
- Path API: `wtk/include/omegaWTK/Composition/Path.h`
- Animation API: `wtk/include/omegaWTK/Composition/Animation.h`
- Backend rendering: `wtk/src/Composition/backend/RenderTarget.cpp`, `RenderTarget.h`
- UIView (general UI consumer): `wtk/src/UI/UIView.cpp`
- SVGView (vector rendering consumer): `wtk/src/UI/SVGView.cpp`
- Shader source: inline `librarySource` string in `RenderTarget.cpp` (lines 111–200)
